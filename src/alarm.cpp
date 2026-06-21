#include "alarm.h"
#include "alarm_mode.h"
#include "sensors.h"
#include "zones.h"
#include "hardware.h"
#include "event_log.h"

void (*onZoneStateChanged)(uint8_t zoneId) = nullptr;
void (*onGlobalStateChanged)(AlarmState newState) = nullptr;

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

// ─── Build a sensor list for the zone (only active sensors) ────────────────
static void getTrippedSensorList(uint8_t zoneId, char* out, size_t outSize) {
  out[0] = 0;
  for (int s = 0; s < TOTAL_SENSORS; s++) {
    if (config.sensors[s].type == SENSOR_DISABLED) continue;
    if (!(config.sensors[s].zoneMask & (1U << (zoneId - 1)))) continue;
    if (sensorStates[s].state != SENSOR_ACTIVE) continue;
    const char* sn = config.sensors[s].name;
    if (strlen(sn) == 0) sn = "?";
    size_t cur = strlen(out);
    if (cur > 0) snprintf(out + cur, outSize - cur, ", ");
    cur = strlen(out);
    snprintf(out + cur, outSize - cur, "T%d '%s'", s + 1, sn);
  }
  for (int s = 0; s < MAX_EXT_SENSORS; s++) {
    if (!config.extSensors[s].enabled) continue;
    if (!(config.extSensors[s].zoneMask & (1U << (zoneId - 1)))) continue;
    if (!extSensorStates[s].active) continue;
    const char* en = config.extSensors[s].name;
    if (strlen(en) == 0) en = "?";
    size_t cur = strlen(out);
    if (cur > 0) snprintf(out + cur, outSize - cur, ", ");
    cur = strlen(out);
    snprintf(out + cur, outSize - cur, "E%d '%s'", s + 1, en);
  }
  if (out[0] == 0) strlcpy(out, "unknown sensor", outSize);
}

static void setZoneAlarmState(uint8_t zoneId, ZoneAlarmState newState) {
  uint8_t idx = zoneId - 1;
  if (zoneStates[idx].alarmState == newState) return;
  zoneStates[idx].alarmState = newState;
  if (onZoneStateChanged) onZoneStateChanged(zoneId);

  // ─── Event log ──────────────────────────────────────────────────────
  char buf[120];
  char sensors[64];
  const char* name = config.zones[idx].name;
  if (strlen(name) == 0) name = "?";
  switch (newState) {
    case ZONE_DISARMED: {
      const char* src = lastZoneCmdSource;
      if (src) snprintf(buf, sizeof(buf), "Zone %u '%s' disarmed by %s", zoneId, name, src);
      else     snprintf(buf, sizeof(buf), "Zone %u '%s' disarmed", zoneId, name);
      logAlarm(buf);
      break;
    }
    case ZONE_ARMING:
      snprintf(buf, sizeof(buf), "Zone %u '%s' arming (exit delay)", zoneId, name);
      logAlarm(buf);
      break;
    case ZONE_ARMED_IDLE: {
      const char* src = lastZoneCmdSource;
      if (src) snprintf(buf, sizeof(buf), "Zone %u '%s' armed by %s", zoneId, name, src);
      else     snprintf(buf, sizeof(buf), "Zone %u '%s' armed", zoneId, name);
      logAlarm(buf);
      break;
    }
    case ZONE_DISARMING:
      getTrippedSensorList(zoneId, sensors, sizeof(sensors));
      snprintf(buf, sizeof(buf), "Zone %u '%s' entry delay by %s", zoneId, name, sensors);
      logAlarm(buf);
      break;
    case ZONE_ALARM:
      getTrippedSensorList(zoneId, sensors, sizeof(sensors));
      snprintf(buf, sizeof(buf), "Zone %u '%s' ALARM triggered by %s", zoneId, name, sensors);
      logAlarm(buf);
      break;
    default:
      break;
  }
}

// ─── Sync relay outputs based on zone states and relay config ─────────────
static void syncRelays() {
  // Auto-clear manual overrides when any zone is armed
  bool anyArmed = false;
  for (int z = 0; z < MAX_ZONES; z++) {
    if (config.zones[z].enabled && zoneStates[z].alarmState != ZONE_DISARMED) {
      anyArmed = true;
      break;
    }
  }
  if (anyArmed) {
    for (int r = 0; r < MAX_RELAYS; r++) {
      relayManualOverride[r] = false;
    }
  }

  for (int r = 0; r < MAX_RELAYS; r++) {
    RelayConfig &rc = config.relays[r];
    if (!rc.enabled) {
      setRelay(r, false);
      continue;
    }
    // Manual override takes priority when all zones are disarmed
    if (relayManualOverride[r]) {
      setRelay(r, relayManualState[r]);
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
        if (rc.zoneId == 0) {
          // Follow the zone that entered alarm earliest
          uint32_t oldestMs = UINT32_MAX;
          int bestZ = -1;
          for (int z = 0; z < MAX_ZONES; z++) {
            if (config.zones[z].enabled && config.zones[z].sirenEnabled &&
                zoneStates[z].alarmState == ZONE_ALARM && zoneStates[z].sirenOn) {
              if (zoneStates[z].alarmEnteredMs < oldestMs) {
                oldestMs = zoneStates[z].alarmEnteredMs;
                bestZ = z;
              }
            }
          }
          if (bestZ >= 0) {
            active = zoneStates[bestZ].sirenOn;
          }
        } else if (rc.zoneId >= 1 && rc.zoneId <= MAX_ZONES) {
          uint8_t zidx = rc.zoneId - 1;
          if (config.zones[zidx].sirenEnabled &&
              zoneStates[zidx].alarmState == ZONE_ALARM) {
            active = zoneStates[zidx].sirenOn;
          }
        }
        setRelay(r, active);
        break;
      }
      case RELAY_PULSE_MODE: {
        // "Alarm" relay: cycles ON/OFF from the zone that entered alarm earliest
        bool anyAlarm = false;
        uint8_t onSecs = 0, offSecs = 0;
        uint32_t oldestMs = UINT32_MAX;
        for (int z = 0; z < MAX_ZONES; z++) {
          if (config.zones[z].enabled && config.zones[z].alarmRelayEnabled &&
              zoneStates[z].alarmState == ZONE_ALARM) {
            if (zoneStates[z].alarmEnteredMs < oldestMs) {
              oldestMs = zoneStates[z].alarmEnteredMs;
              anyAlarm = true;
              if (config.zones[z].alarmRelayOnS > 0 && config.zones[z].alarmRelayOffS > 0) {
                onSecs = config.zones[z].alarmRelayOnS;
                offSecs = config.zones[z].alarmRelayOffS;
              }
              // if 0/0, leave onSecs/offSecs at 0 for continuous-ON below
            }
          }
        }
        static uint32_t pulsePhaseMs[MAX_RELAYS] = {0};
        static bool     pulseOn[MAX_RELAYS] = {false};
        if (anyAlarm) {
          if (onSecs == 0 || offSecs == 0) {
            // Continuous ON when both are 0
            pulseOn[r] = false;
            pulsePhaseMs[r] = 0;
            setRelay(r, true);
          } else {
            uint32_t now = millis();
            if (pulseOn[r]) {
              if (now - pulsePhaseMs[r] >= ((uint32_t)onSecs * 1000UL)) {
                pulseOn[r] = false;
                pulsePhaseMs[r] = now;
              }
            } else {
              if (now - pulsePhaseMs[r] >= ((uint32_t)offSecs * 1000UL) || pulsePhaseMs[r] == 0) {
                pulseOn[r] = true;
                pulsePhaseMs[r] = now;
              }
            }
            setRelay(r, pulseOn[r]);
          }
        } else {
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

    char buf[100];
    snprintf(buf, sizeof(buf), "Digital input %d pressed (action=%d zone=%d)", i+1, (int)cfg.action, cfg.zoneId);
    logSystem(buf);

    lastZoneCmdSource = "digital input";
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
            zs.alarmEnteredMs = now;
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
          zs.alarmEnteredMs = now;
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

  // ─── Evaluate global alarm state and publish transitions ──────────────
  static AlarmState lastGlobalState = AlarmState::DISARMED;
  AlarmState currentState = deriveGlobalAlarmState();

  if (currentState != lastGlobalState) {
    // On entering TRIGGERED, capture last trigger info
    if (currentState == AlarmState::TRIGGERED && lastGlobalState != AlarmState::TRIGGERED) {
      // Find the zone that just entered ALARM
      for (uint8_t z = 1; z <= MAX_ZONES; z++) {
        uint8_t idx = z - 1;
        if (config.zones[idx].enabled && zoneStates[idx].alarmState == ZONE_ALARM) {
          updateLastTrigger(z);
          break;
        }
      }
    }

    lastGlobalState = currentState;
    alarmCtx.globalState = currentState;

    if (onGlobalStateChanged) {
      onGlobalStateChanged(currentState);
    }
  }
}
