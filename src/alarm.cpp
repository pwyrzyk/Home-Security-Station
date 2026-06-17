#include "alarm.h"
#include "sensors.h"
#include "zones.h"
#include "hardware.h"

void (*onZoneStateChanged)(uint8_t zoneId) = nullptr;

static uint32_t lastAlarmLoopMs = 0;

// ─── Check if any sensor in a zone is active ──────────────────────────────
static bool zoneSensorTripped(uint8_t zoneId) {
  for (int s = 0; s < TOTAL_SENSORS; s++) {
    if (config.sensors[s].type == SENSOR_DISABLED) continue;
    if (!(config.sensors[s].zoneMask & (1U << (zoneId - 1)))) continue;
    if (sensorStates[s].state == SENSOR_ACTIVE) return true;
  }
  // Check external MQTT sensors
  for (int s = 0; s < MAX_EXT_SENSORS; s++) {
    if (!config.extSensors[s].enabled) continue;
    if (!(config.extSensors[s].zoneMask & (1U << (zoneId - 1)))) continue;
    if (extSensorStates[s].active) return true;
  }
  return false;
}

static void setZoneAlarmState(uint8_t zoneId, ZoneAlarmState newState) {
  uint8_t idx = zoneId - 1;
  if (zoneStates[idx].alarmState == newState) return;
  zoneStates[idx].alarmState = newState;
  if (onZoneStateChanged) onZoneStateChanged(zoneId);
}

// ─── Sync relay outputs based on zone states and relay config ─────────────
static void syncRelays() {
  bool zoneAlarmActive[MAX_ZONES] = {false};
  for (int z = 0; z < MAX_ZONES; z++) {
    zoneAlarmActive[z] = (zoneStates[z].alarmState == ZONE_ALARM);
  }

  for (int r = 0; r < MAX_RELAYS; r++) {
    RelayConfig &rc = config.relays[r];
    switch (rc.mode) {
      case RELAY_OFF:
        setRelay(r, false);
        break;
      case RELAY_ON:
        setRelay(r, true);
        break;
      case RELAY_FOLLOW_ZONE: {
        bool active = false;
        if (rc.zoneId >= 1 && rc.zoneId <= MAX_ZONES) {
          active = zoneAlarmActive[rc.zoneId - 1];
        }
        setRelay(r, active);
        break;
      }
      case RELAY_PULSE_MODE:
        // Pulse handled separately — keep off unless actively pulsing
        break;
      default:
        break;
    }
  }
}

// ─── Handle digital input actions ─────────────────────────────────────────
static void processDigitalInputs() {
  for (int i = 0; i < MAX_DINPUTS; i++) {
    DigitalInputConfig &cfg = config.dinputs[i];
    if (cfg.action == INPUT_ACTION_NONE) continue;

    bool pressed = dinputStates[i];
    if (!pressed) continue;

    // Mark as handled (edge-triggered)
    dinputStates[i] = false;

    switch (cfg.action) {
      case INPUT_ACTION_ARM_ZONE:
        zoneArm(cfg.zoneId);
        break;
      case INPUT_ACTION_DISARM_ZONE:
        zoneDisarm(cfg.zoneId);
        break;
      case INPUT_ACTION_TOGGLE_ZONE:
        zoneToggle(cfg.zoneId);
        break;
      case INPUT_ACTION_ARM_ALL:
        armAllZones();
        break;
      case INPUT_ACTION_DISARM_ALL:
        disarmAllZones();
        break;
      default:
        break;
    }
  }
}

void alarmLoop() {
  uint32_t now = millis();
  // Run at roughly 100ms cadence
  if (now - lastAlarmLoopMs < 100) return;
  lastAlarmLoopMs = now;

  processDigitalInputs();

  bool anyPrealarm = false;

  for (uint8_t zoneId = 1; zoneId <= MAX_ZONES; zoneId++) {
    uint8_t idx = zoneId - 1;
    ZoneStateData &zs = zoneStates[idx];
    ZoneConfig    &zc = config.zones[idx];

    if (!zc.enabled) continue;

    switch (zs.alarmState) {
      // ─── DISARMED ─────────────────────────────────────────────────────
      case ZONE_DISARMED:
        // Nothing to evaluate; stay disarmed
        break;

      // ─── ARMED_IDLE ───────────────────────────────────────────────────
      case ZONE_ARMED_IDLE: {
        // Exit delay: ignore sensors until exit delay expires
        if (zc.exitDelayS > 0 &&
            (now - zs.armedAtMs) < ((uint32_t)zc.exitDelayS * 1000UL)) {
          break;  // still in exit delay
        }
        // Check for sensor trip
        if (zoneSensorTripped(zoneId)) {
          if (zc.entryDelayS > 0) {
            setZoneAlarmState(zoneId, ZONE_PREALARM);
            zs.preAlarmStartMs = now;
          } else {
            setZoneAlarmState(zoneId, ZONE_ALARM);
          }
        }
        break;
      }

      // ─── PREALARM ─────────────────────────────────────────────────────
      case ZONE_PREALARM: {
        anyPrealarm = true;
        if (now - zs.preAlarmStartMs >= ((uint32_t)zc.entryDelayS * 1000UL)) {
          // Entry delay expired → full alarm
          setZoneAlarmState(zoneId, ZONE_ALARM);
        }
        // If sensor clears during prealarm, go back to armed-idle
        if (!zoneSensorTripped(zoneId)) {
          setZoneAlarmState(zoneId, ZONE_ARMED_IDLE);
        }
        break;
      }

      // ─── ALARM ────────────────────────────────────────────────────────
      case ZONE_ALARM:
        // Stay in alarm until manually disarmed
        break;

      default:
        break;
    }
  }

  // Drive prealarm output (GPIO20)
  digitalWrite(DOUT_PREALARM, anyPrealarm ? HIGH : LOW);

  // Sync relay outputs
  syncRelays();
}