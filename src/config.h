#pragma once

#include <Arduino.h>

// ─── Firmware ──────────────────────────────────────────────────────────────
#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "0.1.0"
#endif

// ─── Hardware constants ────────────────────────────────────────────────────
#define DEVICE_NAME "alarm-esp"

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
#define DEFAULT_WIFI_SSID  "Jasminowa_5"
#define DEFAULT_WIFI_PASS  "vader001"
#define DEFAULT_MQTT_SERVER "192.168.1.20"
#define DEFAULT_MQTT_PORT   1883
#define DEFAULT_MQTT_USER   ""
#define DEFAULT_MQTT_PASS   ""

#define AP_SSID_PREFIX "Alarm-AP-"
#define AP_PASS        "12345678"

#define OTA_PORT    3232
#define OTA_PASSWORD "admin"
#define OTA_HOSTNAME "alarm-esp"

#define NTP_SERVER      "pool.ntp.org"
#define TZ_OFFSET_SEC   7200   // UTC+2

#define WIFI_CONNECT_TIMEOUT_MS  15000
#define HTTP_PORT                80

// ─── EEPROM ────────────────────────────────────────────────────────────────
#define EEPROM_SIZE  4096
#define EEPROM_MAGIC 0xAC

// ─── Zone limits ───────────────────────────────────────────────────────────
#define MAX_EXT_SENSORS 16
#define MAX_ZONES 8
#define MAX_RELAYS 4
#define MAX_DINPUTS 2

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

// ─── Zone alarm states ─────────────────────────────────────────────────────
enum ZoneAlarmState : uint8_t {
  ZONE_DISARMED   = 0,
  ZONE_ARMED_IDLE = 1,
  ZONE_PREALARM   = 2,   // unused, kept for backward compatibility
  ZONE_ALARM      = 3,
  ZONE_ARMING     = 4,   // exit delay active, sensors ignored
  ZONE_DISARMING  = 5    // entry delay active, time to disarm before alarm
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
  INPUT_ACTION_DISARM_ALL = 5
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
  bool debounceTarget;
  uint32_t debounceStartMs;
};

extern SensorStateData sensorStates[TOTAL_SENSORS];

// ─── Zone runtime state ────────────────────────────────────────────────────
struct ZoneStateData {
  bool armed;
  ZoneAlarmState alarmState;
  uint32_t armedAtMs;
  uint32_t preAlarmStartMs;
  uint32_t sirenPhaseMs;    // start of current siren ON/OFF phase
  bool sirenOn;             // true = siren relay should be ON this phase
};

extern ZoneStateData zoneStates[MAX_ZONES];

// ─── Relay runtime ─────────────────────────────────────────────────────────
extern bool relayStates[MAX_RELAYS];
extern bool dinputStates[MAX_DINPUTS];
extern ExtSensorState extSensorStates[MAX_EXT_SENSORS];

// ─── Functions ─────────────────────────────────────────────────────────────
void setDefaults();
void saveConfig();
void loadConfig();
void computeDeviceIdentifiers();