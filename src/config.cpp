#include "config.h"
#include "auth.h"
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

  // ─── Default auth settings ─────────────────────────────────────────────
  String defaultHash = hashPassword("admin");
  strlcpy(config.adminPasswordHash, defaultHash.c_str(), sizeof(config.adminPasswordHash));
  config.forcePasswordChange = true;
}

void saveConfig() {
  EEPROM.put(0, config);
  EEPROM.commit();
}

void loadConfig() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(0, config);

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

  // ─── Migrate: sirenEnabled / alarmRelayEnabled flags (added later) ─────
  // These bool fields were added after initial EEPROM layout.
  // Old EEPROM bytes may be 0 (false) or garbage (true).  Migrate each flag
  // independently: if an enabled zone has a flag as false, set it to true
  // (matching factory defaults in setDefaults()).
  {
    bool migrated = false;
    for (int z = 0; z < MAX_ZONES; z++) {
      if (!config.zones[z].enabled) continue;
      if (!config.zones[z].sirenEnabled) {
        config.zones[z].sirenEnabled = true;
        migrated = true;
      }
      if (!config.zones[z].alarmRelayEnabled) {
        config.zones[z].alarmRelayEnabled = true;
        migrated = true;
      }
    }
    if (migrated) saveConfig();
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

  // ─── Migrate: Auth settings (added later) ────────────────────────────
  // Check if adminPasswordHash is empty (old EEPROM without auth fields)
  if (config.adminPasswordHash[0] == '\0') {
    String defaultHash = hashPassword("admin");
    strlcpy(config.adminPasswordHash, defaultHash.c_str(), sizeof(config.adminPasswordHash));
    config.forcePasswordChange = true;
    saveConfig();
    Serial.println("[BOT] Auth: initialized default admin password");
  } else {
    // Validate hash contains only valid hex chars
    bool hashValid = true;
    for (size_t i = 0; i < strlen(config.adminPasswordHash); i++) {
      char c = config.adminPasswordHash[i];
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
        hashValid = false;
        break;
      }
    }
    if (!hashValid || strlen(config.adminPasswordHash) != 64) {
      String defaultHash = hashPassword("admin");
      strlcpy(config.adminPasswordHash, defaultHash.c_str(), sizeof(config.adminPasswordHash));
      config.forcePasswordChange = true;
      saveConfig();
      Serial.println("[BOT] Auth: reset corrupted password hash to default");
    }
  }

  // Validate forcePasswordChange is 0 or 1
  if (config.forcePasswordChange != 0 && config.forcePasswordChange != 1) {
    config.forcePasswordChange = true;
    saveConfig();
  }

  // ─── Self-heal: if hash doesn't match current binary's hash("admin")
  //     but forcePasswordChange is true (meaning user hasn't set their own
  //     password yet), recompute the hash with the current SHA-256 code.
  //     This prevents "admin/admin" from breaking after OTA updates.
  {
    String currentAdminHash = hashPassword("admin");
    if (strcmp(config.adminPasswordHash, currentAdminHash.c_str()) != 0 && config.forcePasswordChange) {
      strlcpy(config.adminPasswordHash, currentAdminHash.c_str(), sizeof(config.adminPasswordHash));
      saveConfig();
      Serial.println("[BOT] Auth: self-healed admin password hash after firmware update");
    }
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
    zoneStates[i].sirenPhaseMs  = 0;
    zoneStates[i].sirenOn       = false;
    zoneStates[i].sirenOneShotDone = false;
  }
  for (int i = 0; i < MAX_RELAYS; i++) {
    relayStates[i] = false;
    relayManualOverride[i] = false;
    relayManualState[i] = false;
  }
  for (int i = 0; i < MAX_DINPUTS; i++) {
    dinputStates[i] = false;
  }
}