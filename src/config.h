#pragma once

#include <Arduino.h>

// ─── Firmware ──────────────────────────────────────────────────────────────
#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "0.1.0"
#endif

// ─── Hardware constants ────────────────────────────────────────────────────
#define DEVICE_NAME "home-alarm"

// I2C
#define I2C_SDA 0
#define I2C_SCL 3

// ADS1115 addresses (ADDR pin strapped)
#define ADS_ADDR_1 0x48
#define ADS_ADDR_2 0x49
#define ADS_ADDR_3 0x4A
#define ADS_ADDR_4 0x4B

#define ADS_COUNT      4
#define ADS_CHANNELS   4
#define TOTAL_SENSORS  (ADS_COUNT * ADS_CHANNELS)  // 16

// Relay outputs (active LOW: LOW=ON, HIGH=OFF)
#define RELAY_SIREN        5
#define RELAY_PULSE        6
#define RELAY_TAMPER       7
#define RELAY_NOPOWER      10

// Digital inputs (active LOW, external pull-up to 3.3V)
#define DIN_ARM_ZONE       8
#define DIN_DISARM_ALL     9

// Digital output
#define DOUT_PREALARM      20

// Status LED
#ifndef LED_BUILTIN
#define LED_BUILTIN 2
#endif

// ─── Network defaults ──────────────────────────────────────────────────────
// NOTE: These are factory fallbacks only. Real credentials are configured via
// the web UI and persisted to EEPROM. Do NOT hardcode real secrets here.
#define DEFAULT_WIFI_SSID  ""
#define DEFAULT_WIFI_PASS  ""
#define DEFAULT_MQTT_SERVER ""
#define DEFAULT_MQTT_PORT   1883
#define DEFAULT_MQTT_USER   ""
#define DEFAULT_MQTT_PASS   ""

#define AP_SSID_PREFIX "Alarm-AP-"
#define AP_PASS        "12345678"   // 8-char minimum for WPA2; change via UI

#define OTA_PORT    3232
#define OTA_PASSWORD ""             // empty = OTA disabled; set via UI/build flags
#define OTA_HOSTNAME "alarm"

#define NTP_SERVER      "pool.ntp.org"
#define TZ_OFFSET_SEC   7200   // UTC+2

#define WIFI_CONNECT_TIMEOUT_MS  15000
#define WIFI_RETRY_MAX_ATTEMPTS  5
#define WIFI_RETRY_INTERVAL_MS   30000    // 30s between retries
#define WIFI_AP_SCAN_INTERVAL_MS 60000    // 60s between AP→STA scan checks
#define HTTP_PORT                80

// ─── EEPROM ────────────────────────────────────────────────────────────────
#define EEPROM_SIZE  4096
#define EEPROM_MAGIC 0xAE

// ─── Zone limits ───────────────────────────────────────────────────────────
#define MAX_EXT_SENSORS 16
#define MAX_ZONES 8
#define MAX_RELAYS 4
#define MAX_DINPUTS 2
#define MAX_USERS  8

// ─── User roles ──────────────────────────────────────────────────────────────
enum UserRole : uint8_t {
  USER_ROLE_ADMIN     = 0,
  USER_ROLE_OPERATOR  = 1,
  USER_ROLE_API       = 2
};

// ─── Keypad PIN mode suffixes ──────────────────────────────────────────────
#define KEYPAD_SUFFIX_ARM_HOME     'A'
#define KEYPAD_SUFFIX_ARM_AWAY     'B'
#define KEYPAD_SUFFIX_ARM_NIGHT    'C'
#define KEYPAD_SUFFIX_ARM_VACATION 'D'
#define KEYPAD_SUFFIX_DISARM       '#'

// ─── Per-user entry ────────────────────────────────────────────────────────
struct UserEntry {
  char     username[16];
  char     passwordHash[65];   // SHA-256 hex for web login
  char     pin[5];             // 4-digit PIN for keypad (null-terminated)
  uint8_t  role;               // 0 = admin, 1 = operator
  bool     active;
};

// ─── EEPROM migration tracking ─────────────────────────────────────────────
#define EEPROM_AUTH_MIGRATED_FLAG 0xBB

// ─── Sensor types ──────────────────────────────────────────────────────────
enum SensorType : uint8_t {
  SENSOR_DISABLED  = 0,
  SENSOR_PIR       = 1,
  SENSOR_CONTACTRON = 2
};

enum SensorState : uint8_t {
  SENSOR_IDLE   = 0,
  SENSOR_ACTIVE = 1,
  SENSOR_FAULT  = 2
};

// ─── Zone alarm states (internal engine) ────────────────────────────────────
enum ZoneAlarmState : uint8_t {
  ZONE_DISARMED   = 0,
  ZONE_ARMED_IDLE = 1,
  ZONE_PREALARM   = 2,   // unused, kept for backward compatibility
  ZONE_ALARM      = 3,
  ZONE_ARMING     = 4,   // exit delay active, sensors ignored
  ZONE_DISARMING  = 5    // entry delay active, time to disarm before alarm
};

// ─── HA-compatible global alarm states ──────────────────────────────────────
// These are the ONLY values published to HA via the state topic.
// ARMING/DISARMING from ZoneAlarmState map to PENDING — never published directly.
enum class AlarmState : uint8_t {
  DISARMED            = 0,
  ARMED_HOME          = 1,
  ARMED_AWAY          = 2,
  ARMED_NIGHT         = 3,
  ARMED_VACATION      = 4,
  ARMED_CUSTOM_BYPASS = 5,
  PENDING             = 6,   // exit delay or entry delay active
  TRIGGERED           = 7    // any zone in ZONE_ALARM
};

// ─── Alarm Mode identifiers (HA-compatible) ─────────────────────────────────
enum class AlarmMode : uint8_t {
  DISARMED            = 0,
  ARMED_HOME          = 1,
  ARMED_AWAY          = 2,
  ARMED_NIGHT         = 3,
  ARMED_VACATION      = 4,
  ARMED_CUSTOM_BYPASS = 5,
  NUM_MODES           = 6
};

// ─── Alarm Mode Profile (persisted per mode) ────────────────────────────────
struct AlarmModeProfile {
  uint8_t zoneMask;         // bitmask: bit 0 = Zone1 … bit 7 = Zone8
  bool    defined;          // false = this mode has no profile configured
};

// ─── Relay modes ───────────────────────────────────────────────────────────
enum RelayMode : uint8_t {
  RELAY_OFF        = 0,
  RELAY_ON         = 1,
  RELAY_FOLLOW_ZONE = 2,  // follow zone alarm state
  RELAY_PULSE_MODE = 3    // 1s pulse
};

// ─── Digital input actions ─────────────────────────────────────────────────
enum InputAction : uint8_t {
  INPUT_ACTION_NONE       = 0,
  INPUT_ACTION_ARM_ZONE   = 1,
  INPUT_ACTION_DISARM_ZONE= 2,
  INPUT_ACTION_TOGGLE_ZONE= 3,
  INPUT_ACTION_ARM_ALL    = 4,
  INPUT_ACTION_DISARM_ALL = 5,
  INPUT_ACTION_PANIC      = 6    // triggers panic alarm via E16 always-armed zone
};

// ─── Per-sensor configuration ──────────────────────────────────────────────
struct SensorConfig {
  SensorType type;
  char name[24];
  uint16_t standbyMin;      // raw ≥ this AND raw ≤ standbyMax = STANDBY (idle)
  uint16_t standbyMax;
  uint16_t detectMin;       // raw ≥ this AND raw ≤ detectMax = DETECTED (active)
  uint16_t detectMax;       // 65535 = no upper bound
  uint16_t faultMin;        // raw ≥ this AND raw ≤ faultMax = FAULT
  uint16_t faultMax;        // 65535 = no upper bound, 0 = disabled
  bool invert;
  uint16_t debounceMs;      // 20..5000
  uint16_t onDelayMs;       // must stay active this long to trigger
  uint16_t offDelayMs;      // must stay inactive this long to clear
  uint16_t zoneMask;        // bitmask of zones this sensor belongs to
};

// ─── Per-zone configuration ────────────────────────────────────────────────
struct ZoneConfig {
  char name[24];
  uint8_t entryDelayS;      // 0..120, entry delay before alarm
  uint8_t exitDelayS;       // 0..120, exit delay after arming
  uint8_t sirenOnS;         // 0..255, siren ON time (0=continuous)
  uint8_t sirenOffS;        // 0..255, siren OFF time between cycles (0=no cycling)
  uint8_t relayMask;        // bitmask of relays to activate on alarm
  bool enabled;             // zone enabled/disabled
  bool sirenEnabled;        // zone can trigger siren relay (FOLLOW_ZONE mode)
  bool alarmRelayEnabled;   // zone can trigger alarm/pulse relay (PULSE_MODE)
  uint8_t alarmRelayOnS;    // 0..255, alarm relay ON time (0=use hardcoded 10s)
  uint8_t alarmRelayOffS;   // 0..255, alarm relay OFF time (0=use hardcoded 60s)
  bool alwaysArmed;         // zone cannot be disarmed — used for panic/24h zones
};

// ─── Relay configuration ───────────────────────────────────────────────────
struct RelayConfig {
  char name[24];
  RelayMode mode;
  uint8_t zoneId;           // if FOLLOW_ZONE: which zone
  bool enabled;             // false = relay stays OFF regardless of mode
};

// ─── External MQTT sensor configuration ────────────────────────────────────
struct ExtSensorConfig {
  char name[16];
  bool enabled;
  uint16_t zoneMask;        // bitmask of zones (1..8)
};

// ─── External MQTT sensor runtime state ───────────────────────────────────
struct ExtSensorState {
  bool active;
  uint32_t lastChangeMs;
};

// ─── Digital input configuration ───────────────────────────────────────────
struct DigitalInputConfig {
  InputAction action;
  uint8_t zoneId;           // target zone (1..8)
  uint8_t pin;              // actual GPIO pin
  bool activeLow;           // true = LOW means pressed
  uint16_t debounceMs;
};

// ─── Main config struct (persisted in EEPROM) ──────────────────────────────
struct Config {
  uint8_t magic;

  char wifiSsid[32];
  char wifiPass[64];
  char mqttServer[40];
  uint16_t mqttPort;
  char mqttUser[32];
  char mqttPass[32];

  ExtSensorConfig extSensors[MAX_EXT_SENSORS];
  SensorConfig sensors[TOTAL_SENSORS];
  ZoneConfig   zones[MAX_ZONES];
  RelayConfig  relays[MAX_RELAYS];
  DigitalInputConfig dinputs[MAX_DINPUTS];

  // ─── Alarm Mode Profiles ──────────────────────────────────────────────
  // Indexed by AlarmMode (0=DISARMED .. 5=ARMED_CUSTOM_BYPASS)
  AlarmModeProfile modeProfiles[6];

  // ─── HA Discovery settings ─────────────────────────────────────────────
  bool haDiscoveryEnabled;
  char haDiscoveryPrefix[40];

  // ─── Power-fail safety — saved armed state ───────────────────────────
  uint8_t  savedActiveMode;        // AlarmMode to restore on boot (0-5)
  uint8_t  zoneArmedMask;          // bitmask of armed zones
  bool     stateRestoreValid;      // true = valid armed state to restore

  // ─── User accounts ───────────────────────────────────────────────────
  uint8_t    authMigrated;          // EEPROM_AUTH_MIGRATED_FLAG when users[] is primary
  UserEntry  users[MAX_USERS];      // user accounts (replaces adminPasswordHash)
  uint8_t    userCount;            // number of active users

  // ── Legacy fields (kept in struct for EEPROM compatibility, unused in code) ──
  char adminPasswordHash[65];       // DEPRECATED — migrated to users[]
  bool forcePasswordChange;         // DEPRECATED — migrated to users[]
};

// ─── Globals ───────────────────────────────────────────────────────────────
extern Config config;
extern String deviceId;
extern String mqttBase;
extern String deviceSuffix;

extern bool wifiConnected;
extern bool apMode;
extern unsigned long lastNtpTime;

// ─── Sensor runtime state ──────────────────────────────────────────────────
struct SensorStateData {
  SensorState state;
  uint16_t rawValue;
  uint32_t lastChangeMs;
  uint32_t activeSinceMs;
  uint32_t idleSinceMs;
  bool debouncePending;
  uint8_t debounceTarget;  // 0=idle, 1=active, 2=fault (was bool — only tracked active)
  uint32_t debounceStartMs;
};

extern SensorStateData sensorStates[TOTAL_SENSORS];

// ─── Zone runtime state ────────────────────────────────────────────────────
struct ZoneStateData {
  bool armed;
  ZoneAlarmState alarmState;
  uint32_t armedAtMs;
  uint32_t preAlarmStartMs;
  uint32_t alarmEnteredMs;  // timestamp when zone entered ZONE_ALARM
  // Siren timing is handled exclusively by syncRelays() in alarm.cpp
  // (relay-level static state machines — no per-zone duplication)
};

extern ZoneStateData zoneStates[MAX_ZONES];

// ─── Zone sensor activity cache (set by sensorsLoop, consumed by alarm engine) ───
extern bool zoneSensorActiveCache[MAX_ZONES];  // true = at least one sensor tripped in zone

// ─── Relay runtime ─────────────────────────────────────────────────────────
extern bool relayStates[MAX_RELAYS];
extern bool relayManualOverride[MAX_RELAYS];
extern bool relayManualState[MAX_RELAYS];
extern bool dinputStates[MAX_DINPUTS];
extern ExtSensorState extSensorStates[MAX_EXT_SENSORS];

// ─── Global alarm context (runtime only — NOT persisted) ───────────────────
struct AlarmContext {
  AlarmMode activeMode;           // currently selected mode (runtime only)
  AlarmState globalState;         // derived global state → published to HA
  uint8_t   activeZoneMask;       // resolved from activeMode profile

  // Last trigger tracking
  uint8_t   lastTriggerZoneId;    // 1-based zone that last triggered
  uint8_t   lastTriggerSensorId;  // 1-based sensor that last triggered
  uint32_t  lastTriggerTimeMs;
  char      lastTriggerSensorName[24];
  char      lastTriggerZoneName[24];
};
extern AlarmContext alarmCtx;

// ─── Functions ─────────────────────────────────────────────────────────────
void setDefaults();
void saveConfig();           // immediate EEPROM write (blocking — use sparingly)
void requestSaveConfig();   // deferred — sets dirty flag, flushed in loop()
void configSaveLoop();      // called from loop() to flush pending save
void loadConfig();
void computeDeviceIdentifiers();
void saveArmedState();
void restoreArmedState();
