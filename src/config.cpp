#include "config.h"
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
bool dinputStates[MAX_DINPUTS];
ExtSensorState extSensorStates[MAX_EXT_SENSORS];

unsigned long lastNtpTime = 0;

void computeDeviceIdentifiers() {
  uint64_t chipId = ESP.getEfuseMac();
  char buf[13];
  snprintf(buf, sizeof(buf), "%06llX", (unsigned long long)(chipId & 0xFFFFFF));
  deviceSuffix = String(buf);
  mqttBase     = "alarm-" + deviceSuffix;
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
    config.zones[i].enabled     = true;
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

  // Initialize runtime state
  for (int i = 0; i < TOTAL_SENSORS; i++) {
    sensorStates[i].state  = SENSOR_IDLE;
    sensorStates[i].rawValue = 0;
  }
  for (int i = 0; i < MAX_ZONES; i++) {
    zoneStates[i].armed         = false;
    zoneStates[i].alarmState    = ZONE_DISARMED;
    zoneStates[i].sirenPhaseMs  = 0;
    zoneStates[i].sirenOn       = false;
  }
  for (int i = 0; i < MAX_RELAYS; i++) {
    relayStates[i] = false;
  }
  for (int i = 0; i < MAX_DINPUTS; i++) {
    dinputStates[i] = false;
  }
}