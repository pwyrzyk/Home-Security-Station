#include "config.h"
#include "auth.h"
#include "event_log.h"
#include <EEPROM.h>
#include <ESP.h>

Config config;

String deviceId;
String mqttBase;
String deviceSuffix;

bool wifiConnected = false;
bool apMode = false;

// Runtime state arrays
SensorStateData sensorStates[TOTAL_SENSORS];
ZoneStateData    zoneStates[MAX_ZONES];
bool relayStates[MAX_RELAYS];
bool relayManualOverride[MAX_RELAYS];
bool relayManualState[MAX_RELAYS];
bool dinputStates[MAX_DINPUTS];
ExtSensorState extSensorStates[MAX_EXT_SENSORS];
bool zoneSensorActiveCache[MAX_ZONES];  // dirty-flag cache for zoneSensorTripped()

AlarmContext alarmCtx;

unsigned long lastNtpTime = 0;

void computeDeviceIdentifiers() {
  uint64_t chipId = ESP.getEfuseMac();
  char buf[13];
  snprintf(buf, sizeof(buf), "%06llX", (unsigned long long)(chipId & 0xFFFFFF));
  deviceSuffix = String(buf);
  mqttBase     = "home-alarm-" + deviceSuffix;
  deviceId     = String(DEVICE_NAME) + "-" + deviceSuffix;
}

void setDefaults() {
  memset(&config, 0, sizeof(config));
  config.magic = EEPROM_MAGIC;

  strlcpy(config.wifiSsid,   DEFAULT_WIFI_SSID,   sizeof(config.wifiSsid));
  strlcpy(config.wifiPass,   DEFAULT_WIFI_PASS,   sizeof(config.wifiPass));
  strlcpy(config.mqttServer, DEFAULT_MQTT_SERVER,  sizeof(config.mqttServer));
  config.mqttPort = DEFAULT_MQTT_PORT;
  strlcpy(config.mqttUser, DEFAULT_MQTT_USER, sizeof(config.mqttUser));
  strlcpy(config.mqttPass, DEFAULT_MQTT_PASS, sizeof(config.mqttPass));

  // ─── Default sensor settings ──────────────────────────────────────────
  for (int i = 0; i < TOTAL_SENSORS; i++) {
    snprintf(config.sensors[i].name, sizeof(config.sensors[i].name), "T%d", i + 1);
    config.sensors[i].type        = SENSOR_DISABLED;
    config.sensors[i].standbyMin  = 0;
    config.sensors[i].standbyMax  = 2000;
    config.sensors[i].detectMin   = 8000;
    config.sensors[i].detectMax   = 65535;   // no upper bound
    config.sensors[i].faultMin    = 30000;
    config.sensors[i].faultMax    = 65535;   // no upper bound
    config.sensors[i].invert      = false;
    config.sensors[i].debounceMs  = 100;
    config.sensors[i].onDelayMs   = 200;
    config.sensors[i].offDelayMs  = 500;
    config.sensors[i].zoneMask    = 0;
  }

  // ─── Default zones ────────────────────────────────────────────────────
  for (int i = 0; i < MAX_ZONES; i++) {
    snprintf(config.zones[i].name, sizeof(config.zones[i].name), "Zone %d", i + 1);
    config.zones[i].entryDelayS = 10;
    config.zones[i].exitDelayS  = 5;
    config.zones[i].sirenOnS    = 0;
    config.zones[i].sirenOffS   = 0;
    config.zones[i].relayMask   = 0;
    config.zones[i].enabled     = false;
    config.zones[i].sirenEnabled      = true;
    config.zones[i].alarmRelayEnabled  = true;
    config.zones[i].alarmRelayOnS      = 0;
    config.zones[i].alarmRelayOffS     = 0;
  }

  // ─── Default relays (active LOW) ──────────────────────────────────────
  const char* relayNames[MAX_RELAYS] = { "Siren", "Alarm", "Tamper", "No-Power" };
  const RelayMode defaultModes[MAX_RELAYS] = { RELAY_FOLLOW_ZONE, RELAY_PULSE_MODE, RELAY_OFF, RELAY_OFF };
  const uint8_t  defaultZoneIds[MAX_RELAYS] = { 0, 0, 0, 0 };
  for (int i = 0; i < MAX_RELAYS; i++) {
    strlcpy(config.relays[i].name, relayNames[i], sizeof(config.relays[i].name));
    config.relays[i].mode    = defaultModes[i];
    config.relays[i].zoneId  = defaultZoneIds[i];
    config.relays[i].enabled = true;
  }

  // ─── Default external MQTT sensors ────────────────────────────────────
  for (int i = 0; i < MAX_EXT_SENSORS; i++) {
    snprintf(config.extSensors[i].name, sizeof(config.extSensors[i].name), "Ext %d", i + 1);
    config.extSensors[i].enabled  = false;
    config.extSensors[i].zoneMask = 0;
  }

  // ─── Default digital inputs ───────────────────────────────────────────
  config.dinputs[0].action    = INPUT_ACTION_ARM_ZONE;
  config.dinputs[0].zoneId    = 1;
  config.dinputs[0].pin       = DIN_ARM_ZONE;
  config.dinputs[0].activeLow = true;
  config.dinputs[0].debounceMs = 50;

  config.dinputs[1].action    = INPUT_ACTION_DISARM_ALL;
  config.dinputs[1].zoneId    = 0;
  config.dinputs[1].pin       = DIN_DISARM_ALL;
  config.dinputs[1].activeLow = true;
  config.dinputs[1].debounceMs = 50;

  // ─── Default Alarm Mode Profiles ──────────────────────────────────────
  // DISARMED: always empty mask, always defined (read-only in UI)
  config.modeProfiles[(uint8_t)AlarmMode::DISARMED].zoneMask = 0;
  config.modeProfiles[(uint8_t)AlarmMode::DISARMED].defined  = true;

  // Sensible factory defaults (all zones disabled → user must configure)
  for (int m = 1; m < 6; m++) {
    config.modeProfiles[m].zoneMask = 0;
    config.modeProfiles[m].defined  = false;
  }

  // ─── Default HA Discovery settings ────────────────────────────────────
  config.haDiscoveryEnabled = true;
  strlcpy(config.haDiscoveryPrefix, "homeassistant", sizeof(config.haDiscoveryPrefix));

  // ─── Default user accounts ────────────────────────────────────────────
  memset(config.users, 0, sizeof(config.users));
  String defaultHash = hashPassword("admin");
  String apiHash = hashPassword("api_user");
  // Admin account
  strlcpy(config.users[0].username, "admin", sizeof(config.users[0].username));
  strlcpy(config.users[0].passwordHash, defaultHash.c_str(), sizeof(config.users[0].passwordHash));
  strlcpy(config.users[0].pin, "0000", sizeof(config.users[0].pin));
  config.users[0].role   = USER_ROLE_ADMIN;
  config.users[0].active = true;
  // API user account
  strlcpy(config.users[1].username, "api_user", sizeof(config.users[1].username));
  strlcpy(config.users[1].passwordHash, apiHash.c_str(), sizeof(config.users[1].passwordHash));
  strlcpy(config.users[1].pin, "0001", sizeof(config.users[1].pin));
  config.users[1].role   = USER_ROLE_API;
  config.users[1].active = true;
  config.userCount = 2;
  config.authMigrated = EEPROM_AUTH_MIGRATED_FLAG;

  // Clear legacy fields
  memset(config.adminPasswordHash, 0, sizeof(config.adminPasswordHash));
  config.forcePasswordChange = true;
}

// ─── Deferred config save (dirty-flag pattern) ────────────────────────────
// Async web handlers call requestSaveConfig() instead of saveConfig() to
// avoid blocking the async TCP thread with a multi-hundred-ms EEPROM write.
// configSaveLoop() (called from main loop) performs the actual write.
static bool configSavePending = false;

void requestSaveConfig() {
  configSavePending = true;
}

void configSaveLoop() {
  if (!configSavePending) return;
  configSavePending = false;
  saveConfig();
}

void saveConfig() {
  EEPROM.put(0, config);
  EEPROM.commit();
}

// ─── Power-fail state persistence ──────────────────────────────────────────

// ─── Armed-state EEPROM region (dedicated, small — avoids wearing Config area) ──
// Uses the last EEPROM page (offset = EEPROM_SIZE - sizeof(ArmedStateBlob))
// to avoid overlap with the Config struct at offset 0.
#define ARMED_STATE_MAGIC 0x5A
struct ArmedStateBlob {
  uint8_t  magic;
  uint8_t  activeMode;
  uint8_t  zoneArmedMask;
  bool     stateRestoreValid;
};
static_assert(sizeof(ArmedStateBlob) <= 16, "ArmedStateBlob too large for dedicated EEPROM page");

static int armedStateOffset() {
  return EEPROM_SIZE - 16;  // last 16 bytes of EEPROM
}

void saveArmedState() {
  // Build bitmask of currently armed zones
  uint8_t mask = 0;
  for (int z = 0; z < MAX_ZONES; z++) {
    if (zoneStates[z].armed && zoneStates[z].alarmState != ZONE_DISARMED) {
      mask |= (1U << z);
    }
  }

  ArmedStateBlob blob;
  blob.magic            = ARMED_STATE_MAGIC;
  blob.activeMode       = (uint8_t)alarmCtx.activeMode;
  blob.zoneArmedMask    = mask;
  blob.stateRestoreValid = (mask != 0);

  // Write only the small blob — avoids full 4K EEPROM erase/write cycle
  int offset = armedStateOffset();
  EEPROM.put(offset, blob);
  EEPROM.commit();

  // Also sync to Config struct (for backward compatibility with saved fields)
  config.savedActiveMode   = blob.activeMode;
  config.zoneArmedMask     = blob.zoneArmedMask;
  config.stateRestoreValid = blob.stateRestoreValid;

  char buf[64];
  snprintf(buf, sizeof(buf), "Armed state saved: mask=0x%02X mode=%d", mask, blob.activeMode);
  logSystem(buf);
}

void restoreArmedState() {
  // First, try to read from the dedicated armed-state EEPROM blob (newer, more reliable)
  int offset = armedStateOffset();
  ArmedStateBlob blob;
  EEPROM.get(offset, blob);

  bool hasBlob = (blob.magic == ARMED_STATE_MAGIC && blob.stateRestoreValid && blob.zoneArmedMask != 0);

  uint8_t  effectiveMask  = hasBlob ? blob.zoneArmedMask     : config.zoneArmedMask;
  uint8_t  effectiveMode  = hasBlob ? blob.activeMode        : config.savedActiveMode;
  bool     hasValid       = hasBlob ? blob.stateRestoreValid : config.stateRestoreValid;

  Serial.printf("[BOT] restoreArmedState: blob=%d valid=%d mask=0x%02X mode=%d\n",
                hasBlob, hasValid, effectiveMask, effectiveMode);

  if (!hasValid || effectiveMask == 0) {
    Serial.println("[BOT] restoreArmedState: nothing to restore, returning");
    return;
  }

  // Re-arm zones from saved bitmask (use effective values from blob or config)
  for (int z = 0; z < MAX_ZONES; z++) {
    if (effectiveMask & (1U << z)) {
      zoneStates[z].armed = true;
      zoneStates[z].armedAtMs = millis();
      zoneStates[z].alarmState = ZONE_ARMED_IDLE;
      Serial.printf("[BOT] restoreArmedState: re-armed zone %d\n", z+1);
    }
  }

  // Restore active mode
  alarmCtx.activeMode = (AlarmMode)effectiveMode;
  alarmCtx.activeZoneMask = effectiveMask;
  Serial.printf("[BOT] restoreArmedState: done, mode=%d\n", effectiveMode);
}

void loadConfig() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(0, config);

  // Snapshot power-fail armed state BEFORE migrations (they may saveConfig and overwrite)
  uint8_t  _savedMask = config.zoneArmedMask;
  uint8_t  _savedMode = config.savedActiveMode;
  bool     _savedValid = config.stateRestoreValid;

  if (config.magic != EEPROM_MAGIC) {
    setDefaults();
    saveConfig();
  }

  // Sanitize network defaults
  if (strlen(config.wifiSsid) == 0) {
    strlcpy(config.wifiSsid, DEFAULT_WIFI_SSID, sizeof(config.wifiSsid));
  }
  if (strlen(config.mqttServer) == 0) {
    strlcpy(config.mqttServer, DEFAULT_MQTT_SERVER, sizeof(config.mqttServer));
  }
  if (config.mqttPort == 0 || config.mqttPort > 65535) {
    config.mqttPort = DEFAULT_MQTT_PORT;
  }

  // Sanitize sensor voltage ranges
  for (int i = 0; i < TOTAL_SENSORS; i++) {
    if (config.sensors[i].standbyMax == 0) config.sensors[i].standbyMax = 2000;
    if (config.sensors[i].detectMin == 0)  config.sensors[i].detectMin  = 8000;
    if (config.sensors[i].faultMin == 0)   config.sensors[i].faultMin   = 30000;
    // Cap upper bounds: 0 means "use open bound"
    if (config.sensors[i].detectMax == 0) config.sensors[i].detectMax = 65535;
    if (config.sensors[i].faultMax == 0)  config.sensors[i].faultMax = 65535;
    // Swap if min > max
    if (config.sensors[i].standbyMin > config.sensors[i].standbyMax) {
      uint16_t t = config.sensors[i].standbyMin;
      config.sensors[i].standbyMin = config.sensors[i].standbyMax;
      config.sensors[i].standbyMax = t;
    }
    if (config.sensors[i].detectMin > config.sensors[i].detectMax) {
      uint16_t t = config.sensors[i].detectMin;
      config.sensors[i].detectMin = config.sensors[i].detectMax;
      config.sensors[i].detectMax = t;
    }
    if (config.sensors[i].faultMin > config.sensors[i].faultMax) {
      uint16_t t = config.sensors[i].faultMin;
      config.sensors[i].faultMin = config.sensors[i].faultMax;
      config.sensors[i].faultMax = t;
    }
  }

  // ─── Migrate: force Siren relay zoneId to 0 (follow all zones) ────────
  if (config.relays[0].mode == RELAY_FOLLOW_ZONE && config.relays[0].zoneId != 0) {
    config.relays[0].zoneId = 0;
    saveConfig();
  }

  // ─── EEPROM struct stability: force bool flags to safe defaults ───────
  // bool fields added after original EEPROM layout can shift byte offset
  // across firmware updates. Force-set on every boot to prevent silent
  // failures (siren/alarm relay not firing, alarm mode profiles broken).
  {
    bool fixed = false;
    for (int z = 0; z < MAX_ZONES; z++) {
      if (!config.zones[z].enabled) continue;
      if (!config.zones[z].sirenEnabled) {
        config.zones[z].sirenEnabled = true;
        fixed = true;
      }
      if (!config.zones[z].alarmRelayEnabled) {
        config.zones[z].alarmRelayEnabled = true;
        fixed = true;
      }
    }
    // Alarm mode profiles: once defined in UI, always force to defined=true
    for (int m = 1; m < 6; m++) {
      if (config.modeProfiles[m].zoneMask != 0 && !config.modeProfiles[m].defined) {
        config.modeProfiles[m].defined = true;
        fixed = true;
      }
    }
    if (fixed) {
      saveConfig();
      Serial.println("[BOT] EEPROM: corrected corrupted bool fields");
    }
  }

  // ─── Migrate: alarmRelayOnS / alarmRelayOffS (added later) ──────────────
  // These uint8_t fields were added after initial EEPROM layout. On old
  // configs the bytes contain adjacent struct data (e.g. zone name chars
  // like 'Z'=90, 'o'=111, 'n'=110, space=32). These fall in the printable
  // ASCII range 32–122 and would cause rapid relay cycling (e.g. 90s ON /
  // 110s OFF). Real users would never intentionally set these to ASCII-
  // range values. Reset to 0 (continuous-ON, factory default).
  {
    bool migrated = false;
    for (int z = 0; z < MAX_ZONES; z++) {
      if (!config.zones[z].enabled) continue;
      uint8_t onS = config.zones[z].alarmRelayOnS;
      uint8_t offS = config.zones[z].alarmRelayOffS;
      // Printable ASCII range or zero: 0 is valid (continuous-ON), but
      // 32–122 are almost certainly name-string spillover from old EEPROM.
      if (onS >= 32 && onS <= 122) {
        config.zones[z].alarmRelayOnS = 0;
        migrated = true;
      }
      if (offS >= 32 && offS <= 122) {
        config.zones[z].alarmRelayOffS = 0;
        migrated = true;
      }
    }
    if (migrated) saveConfig();
  }

  // ─── Migrate: sirenOnS / sirenOffS garbage values ───────────────────────
  // These are part of the original ZoneConfig layout, but on older configs
  // that predate the UI they may have been set to arbitrary values.
  // Clamp to 0 if they look like ASCII spillover (32–122).
  {
    bool migrated = false;
    for (int z = 0; z < MAX_ZONES; z++) {
      if (!config.zones[z].enabled) continue;
      uint8_t onS  = config.zones[z].sirenOnS;
      uint8_t offS = config.zones[z].sirenOffS;
      if (onS >= 32 && onS <= 122) {
        config.zones[z].sirenOnS = 0;
        migrated = true;
      }
      if (offS >= 32 && offS <= 122) {
        config.zones[z].sirenOffS = 0;
        migrated = true;
      }
    }
    if (migrated) saveConfig();
  }

  // ─── Migrate: populate mode profiles if uninitialized (old config) ────
  bool modeMigrationNeeded = false;
  for (int m = 0; m < 6; m++) {
    if (config.modeProfiles[m].defined && config.modeProfiles[m].zoneMask == 0 && m == 0) {
      // DISARMED with defined=true and zoneMask=0 is valid
      continue;
    }
    // Check if profile appears uninitialized (garbage from old EEPROM)
    if (config.modeProfiles[m].zoneMask > 0xFF || config.modeProfiles[m].defined > 1) {
      modeMigrationNeeded = true;
      break;
    }
  }
  // If all profiles have defined=false and zero masks, this is a migration from old config
  bool allUndefined = true;
  for (int m = 0; m < 6; m++) {
    if (config.modeProfiles[m].defined) { allUndefined = false; break; }
  }
  if (allUndefined || modeMigrationNeeded) {
    config.modeProfiles[(uint8_t)AlarmMode::DISARMED].zoneMask = 0;
    config.modeProfiles[(uint8_t)AlarmMode::DISARMED].defined  = true;
    for (int m = 1; m < 6; m++) {
      config.modeProfiles[m].zoneMask = 0;
      config.modeProfiles[m].defined  = false;
    }
    saveConfig();
  }

  // ─── Migrate: Auth from single-user to multi-user system ─────────────
  if (config.authMigrated != EEPROM_AUTH_MIGRATED_FLAG) {
    // Migrate old admin password to new users array
    memset(config.users, 0, sizeof(config.users));

    if (config.adminPasswordHash[0] != '\0' && strlen(config.adminPasswordHash) == 64) {
      // Old single-user system had a password hash — migrate it to users[0]
      strlcpy(config.users[0].username, "admin", sizeof(config.users[0].username));
      strlcpy(config.users[0].passwordHash, config.adminPasswordHash, sizeof(config.users[0].passwordHash));
    } else {
      // No valid old hash — create fresh defaults
      String defaultHash = hashPassword("admin");
      strlcpy(config.users[0].username, "admin", sizeof(config.users[0].username));
      strlcpy(config.users[0].passwordHash, defaultHash.c_str(), sizeof(config.users[0].passwordHash));
    }

    strlcpy(config.users[0].pin, "0000", sizeof(config.users[0].pin));
    config.users[0].role   = USER_ROLE_ADMIN;
    config.users[0].active = true;
    config.userCount = 1;
    config.authMigrated = EEPROM_AUTH_MIGRATED_FLAG;

    // Clear legacy fields
    memset(config.adminPasswordHash, 0, sizeof(config.adminPasswordHash));
    config.forcePasswordChange = true;
    saveConfig();
    Serial.println("[BOT] Auth: migrated to multi-user system");
  }

  // ─── Self-heal: if user[0] hash doesn't match current binary
  //     and user hasn't changed password yet (legacy flag or pin=0000+hash=stale),
  //     recompute with current SHA-256 code. Only self-heals the first admin.
  {
    String currentAdminHash = hashPassword("admin");
    if (config.users[0].active &&
        strcmp(config.users[0].passwordHash, currentAdminHash.c_str()) != 0 &&
        config.forcePasswordChange) {
      strlcpy(config.users[0].passwordHash, currentAdminHash.c_str(), sizeof(config.users[0].passwordHash));
      saveConfig();
      Serial.println("[BOT] Auth: self-healed admin password hash after firmware update");
    }
  }

  // Validate forcePasswordChange is 0 or 1
  if (config.forcePasswordChange != 0 && config.forcePasswordChange != 1) {
    config.forcePasswordChange = true;
    saveConfig();
  }

  // ─── Migrate: ensure api_user account exists (added in firmware 1.0.1) ──
  {
    bool hasApiUser = false;
    for (int i = 0; i < MAX_USERS; i++) {
      if (config.users[i].active && strcmp(config.users[i].username, "api_user") == 0) {
        hasApiUser = true;
        break;
      }
    }
    if (!hasApiUser && countActiveUsers() < MAX_USERS) {
      int slot = -1;
      for (int i = 0; i < MAX_USERS; i++) {
        if (!config.users[i].active) { slot = i; break; }
      }
      if (slot >= 0) {
        String apiHash = hashPassword("api_user");
        strlcpy(config.users[slot].username, "api_user", sizeof(config.users[slot].username));
        strlcpy(config.users[slot].passwordHash, apiHash.c_str(), sizeof(config.users[slot].passwordHash));
        strlcpy(config.users[slot].pin, "0001", sizeof(config.users[slot].pin));
        config.users[slot].role   = USER_ROLE_API;
        config.users[slot].active = true;
        config.userCount++;
        saveConfig();
        Serial.println("[BOT] Auth: migrated — added api_user account");
      }
    }
  }

  // Sanitize user array: ensure at least one active admin
  bool hasAdmin = false;
  for (int i = 0; i < MAX_USERS; i++) {
    if (config.users[i].active && config.users[i].role == USER_ROLE_ADMIN) {
      hasAdmin = true;
      break;
    }
  }
  if (!hasAdmin && config.userCount == 0) {
    // Accidentally no users — recreate default admin
    String defaultHash = hashPassword("admin");
    strlcpy(config.users[0].username, "admin", sizeof(config.users[0].username));
    strlcpy(config.users[0].passwordHash, defaultHash.c_str(), sizeof(config.users[0].passwordHash));
    strlcpy(config.users[0].pin, "0000", sizeof(config.users[0].pin));
    config.users[0].role   = USER_ROLE_ADMIN;
    config.users[0].active = true;
    config.userCount = 1;
    config.forcePasswordChange = true;
    saveConfig();
  }

  // ─── Migrate: HA discovery settings ───────────────────────────────────
  // Validate haDiscoveryPrefix contains only printable ASCII (EPROM corruption guard)
  bool prefixValid = true;
  if (config.haDiscoveryPrefix[0] == 0) {
    prefixValid = false;
  } else {
    for (int i = 0; i < (int)sizeof(config.haDiscoveryPrefix); i++) {
      char c = config.haDiscoveryPrefix[i];
      if (c == 0) break;
      if (c < 32 || c > 126) { prefixValid = false; break; }
    }
  }
  // Ensure null termination
  config.haDiscoveryPrefix[sizeof(config.haDiscoveryPrefix) - 1] = 0;

  if (!prefixValid) {
    strlcpy(config.haDiscoveryPrefix, "homeassistant", sizeof(config.haDiscoveryPrefix));
    config.haDiscoveryEnabled = true;
    saveConfig();
  }

  // Validate haDiscoveryEnabled is 0 or 1
  if (config.haDiscoveryEnabled != 0 && config.haDiscoveryEnabled != 1) {
    config.haDiscoveryEnabled = true;
    saveConfig();
  }

  // Initialize runtime state
  for (int i = 0; i < TOTAL_SENSORS; i++) {
    sensorStates[i].state  = SENSOR_IDLE;
    sensorStates[i].rawValue = 0;
  }
  for (int i = 0; i < MAX_ZONES; i++) {
    zoneStates[i].armed         = false;
    zoneStates[i].alarmState    = ZONE_DISARMED;
    zoneStates[i].alarmEnteredMs  = 0;
  }
  for (int i = 0; i < MAX_RELAYS; i++) {
    relayStates[i] = false;
    relayManualOverride[i] = false;
    relayManualState[i] = false;
  }
  for (int i = 0; i < MAX_DINPUTS; i++) {
    dinputStates[i] = false;
  }

  // Restore snapshotted power-fail armed state (migrations may have overwritten it)
  config.zoneArmedMask = _savedMask;
  config.savedActiveMode = _savedMode;
  config.stateRestoreValid = _savedValid;
}
