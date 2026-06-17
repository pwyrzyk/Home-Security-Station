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
  for (int r = 0; r < MAX_RELAYS; r++) {
    RelayConfig &rc = config.relays[r];
    if (!rc.enabled) {
      setRelay(r, false);
      continue;
    }
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
          uint8_t zidx = rc.zoneId - 1;
          if (zoneStates[zidx].alarmState == ZONE_ALARM) {
            active = zoneStates[zidx].sirenOn;
          }
        }
        setRelay(r, active);
        break;
      }
      case RELAY_PULSE_MODE: {
        // "Alarm" relay: cycles 10s ON / 60s OFF while any zone is in ALARM state
        bool anyAlarm = false;
        for (int z = 0; z < MAX_ZONES; z++) {
          if (config.zones[z].enabled && zoneStates[z].alarmState == ZONE_ALARM) {
            anyAlarm = true;
            break;
          }
        }
        static uint32_t pulsePhaseMs[MAX_RELAYS] = {0};
        static bool     pulseOn[MAX_RELAYS] = {false};
        if (anyAlarm) {
          uint32_t now = millis();
          if (pulseOn[r]) {
            // Currently ON — check if 10s elapsed
            if (now - pulsePhaseMs[r] >= 10000UL) {
              pulseOn[r] = false;
              pulsePhaseMs[r] = now;
            }
          } else {
            // Currently OFF — check if 60s elapsed
            if (now - pulsePhaseMs[r] >= 60000UL || pulsePhaseMs[r] == 0) {
              pulseOn[r] = true;
              pulsePhaseMs[r] = now;
            }
          }
          setRelay(r, pulseOn[r]);
        } else {
          // No alarm — reset
          pulseOn[r] = false;
          pulsePhaseMs[r] = 0;
          setRelay(r, false);
        }
        break;
      }
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

      // ─── ARMING (exit delay active) ────────────────────────────────────
      case ZONE_ARMING:
        if (now - zs.armedAtMs >= ((uint32_t)zc.exitDelayS * 1000UL)) {
          setZoneAlarmState(zoneId, ZONE_ARMED_IDLE);
          zs.armedAtMs = now;  // reset timestamp 2s settle grace period start
        }
        break;

      // ─── ARMED_IDLE ───────────────────────────────────────────────────
      case ZONE_ARMED_IDLE: {
        // 500ms settle grace period after arm completes — ignore sensors
        if (now - zs.armedAtMs < 500) break;
        if (zoneSensorTripped(zoneId)) {
          if (zc.entryDelayS > 0) {
            setZoneAlarmState(zoneId, ZONE_DISARMING);
            zs.preAlarmStartMs = now;
          } else {
            setZoneAlarmState(zoneId, ZONE_ALARM);
            zs.sirenOn = true;
            zs.sirenPhaseMs = now;
          }
        }
        break;
      }

      // ─── DISARMING (entry delay active) ───────────────────────────────
      case ZONE_DISARMING:
        // Sensor clearing does NOT cancel — must disarm manually
        if (now - zs.preAlarmStartMs >= ((uint32_t)zc.entryDelayS * 1000UL)) {
          setZoneAlarmState(zoneId, ZONE_ALARM);
          zs.sirenOn = true;
          zs.sirenPhaseMs = now;
        }
        break;

      // ─── ALARM ────────────────────────────────────────────────────────
      case ZONE_ALARM: {
        // Siren cycling: ON for sirenOnS, OFF for sirenOffS, repeat
        if (zc.sirenOnS > 0 && zc.sirenOffS > 0) {
          uint32_t phaseDuration = zs.sirenOn ? ((uint32_t)zc.sirenOnS * 1000UL) : ((uint32_t)zc.sirenOffS * 1000UL);
          if (now - zs.sirenPhaseMs >= phaseDuration) {
            zs.sirenOn = !zs.sirenOn;
            zs.sirenPhaseMs = now;
          }
        } else {
          zs.sirenOn = true;   // continuous ON
        }
        break;
      }

      default:
        break;
    }
  }

  // Sync relay outputs
  syncRelays();
}