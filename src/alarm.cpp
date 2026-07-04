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
  if (zoneId < 1 || zoneId > MAX_ZONES) return false;
  return zoneSensorActiveCache[zoneId - 1];
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
  // Fast-path exit: when global state is DISARMED and no manual overrides
  // are active, all alarm-driven relays should already be OFF.
  // Skip the full scan to save CPU cycles every 100ms iteration.
  static bool relaysKnownOff = true;
  bool anyManualOverride = false;
  for (int r = 0; r < MAX_RELAYS; r++) {
    if (relayManualOverride[r]) { anyManualOverride = true; break; }
  }
  if (alarmCtx.globalState == AlarmState::DISARMED && !anyManualOverride) {
    if (relaysKnownOff) return;  // still disarmed, relays already OFF — nothing to do
    // otherwise fall through to sync (first call after state change)
  }
  for (int r = 0; r < MAX_RELAYS; r++) {
    RelayConfig &rc = config.relays[r];

    // Manual override takes priority regardless of enabled/armed state
    if (relayManualOverride[r]) {
      setRelay(r, relayManualState[r]);
      continue;
    }

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
        // Siren relay — self-contained timing, mirrors PULSE_MODE.
        // Independent of zoneStates.sirenOn flag.
        bool anyAlarm = false;
        uint8_t onSecs = 0, offSecs = 0;
        uint32_t oldestMs = UINT32_MAX;

        if (rc.zoneId == 0) {
          // Follow the zone that entered alarm earliest
          for (int z = 0; z < MAX_ZONES; z++) {
            if (config.zones[z].enabled && config.zones[z].sirenEnabled &&
                zoneStates[z].alarmState == ZONE_ALARM) {
              anyAlarm = true;
              if (zoneStates[z].alarmEnteredMs < oldestMs) {
                oldestMs = zoneStates[z].alarmEnteredMs;
                onSecs  = config.zones[z].sirenOnS;
                offSecs = config.zones[z].sirenOffS;
              }
            }
          }
        } else if (rc.zoneId >= 1 && rc.zoneId <= MAX_ZONES) {
          uint8_t zidx = rc.zoneId - 1;
          if (config.zones[zidx].enabled && config.zones[zidx].sirenEnabled &&
              zoneStates[zidx].alarmState == ZONE_ALARM) {
            anyAlarm = true;
            onSecs  = config.zones[zidx].sirenOnS;
            offSecs = config.zones[zidx].sirenOffS;
          }
        }

        static uint32_t sirenPhaseMs[MAX_RELAYS]    = {0};
        static bool     sirenPulseOn[MAX_RELAYS]     = {false};
        static bool     sirenOneShotDone[MAX_RELAYS] = {false};

        // Diagnostic: log when siren should fire on new alarm
        static bool sirenNewAlarmLogged = false;
        if (anyAlarm && !sirenNewAlarmLogged && !sirenOneShotDone[r]) {
          char dbg[80];
          snprintf(dbg, sizeof(dbg), "Siren start ON=%ds OFF=%ds", onSecs, offSecs);
          logRelay(dbg);
          sirenNewAlarmLogged = true;
        }
        if (!anyAlarm) {
          sirenNewAlarmLogged = false;
          sirenPulseOn[r] = false;
          sirenPhaseMs[r] = 0;
          sirenOneShotDone[r] = false;
          setRelay(r, false);
        } else if (onSecs == 0) {
          // Continuous ON — stays on until disarmed
          sirenPulseOn[r] = false;
          sirenPhaseMs[r] = 0;
          setRelay(r, true);
        } else if (offSecs == 0) {
          // One-shot: ON for onSecs, then OFF permanently
          if (!sirenOneShotDone[r]) {
            if (sirenPhaseMs[r] == 0) sirenPhaseMs[r] = millis();
            if (millis() - sirenPhaseMs[r] >= ((uint32_t)onSecs * 1000UL)) {
              sirenPulseOn[r] = false;
              sirenOneShotDone[r] = true;
              setRelay(r, false);
            } else {
              setRelay(r, true);
            }
          }
        } else {
          // Cycling: ON for onSecs, OFF for offSecs, repeat
          uint32_t tnow = millis();
          if (sirenPulseOn[r]) {
            if (tnow - sirenPhaseMs[r] >= ((uint32_t)onSecs * 1000UL)) {
              sirenPulseOn[r] = false;
              sirenPhaseMs[r] = tnow;
            }
          } else {
            if (tnow - sirenPhaseMs[r] >= ((uint32_t)offSecs * 1000UL) || sirenPhaseMs[r] == 0) {
              sirenPulseOn[r] = true;
              sirenPhaseMs[r] = tnow;
            }
          }
          setRelay(r, sirenPulseOn[r]);
        }
        break;
      }
      case RELAY_PULSE_MODE: {
        // Alarm relay with configurable on/off timing per zone.
        // Uses the timing from the zone that entered alarm earliest.
        bool anyAlarm = false;
        uint8_t onSecs = 0, offSecs = 0;
        uint32_t oldestMs = UINT32_MAX;
        for (int z = 0; z < MAX_ZONES; z++) {
          if (config.zones[z].enabled && config.zones[z].alarmRelayEnabled &&
              zoneStates[z].alarmState == ZONE_ALARM) {
            anyAlarm = true;
            if (zoneStates[z].alarmEnteredMs < oldestMs) {
              oldestMs = zoneStates[z].alarmEnteredMs;
              onSecs  = config.zones[z].alarmRelayOnS;
              offSecs = config.zones[z].alarmRelayOffS;
            }
          }
        }
        static uint32_t pulsePhaseMs[MAX_RELAYS]   = {0};
        static bool     pulseOn[MAX_RELAYS]         = {false};
        static bool     pulseOneShotDone[MAX_RELAYS] = {false};

        if (!anyAlarm) {
          pulseOn[r] = false;
          pulsePhaseMs[r] = 0;
          pulseOneShotDone[r] = false;
          setRelay(r, false);
        } else if (onSecs == 0) {
          // Continuous ON — stays on until disarmed
          pulseOn[r] = false;
          pulsePhaseMs[r] = 0;
          setRelay(r, true);
        } else if (offSecs == 0) {
          // One-shot: ON for onSecs, then OFF permanently
          if (!pulseOneShotDone[r]) {
            if (pulsePhaseMs[r] == 0) pulsePhaseMs[r] = millis();
            if (millis() - pulsePhaseMs[r] >= ((uint32_t)onSecs * 1000UL)) {
              pulseOn[r] = false;
              pulseOneShotDone[r] = true;
              setRelay(r, false);
            } else {
              setRelay(r, true);
            }
          }
        } else {
          // Cycling: ON for onSecs, OFF for offSecs, repeat
          uint32_t tnow = millis();
          if (pulseOn[r]) {
            if (tnow - pulsePhaseMs[r] >= ((uint32_t)onSecs * 1000UL)) {
              pulseOn[r] = false;
              pulsePhaseMs[r] = tnow;
            }
          } else {
            if (tnow - pulsePhaseMs[r] >= ((uint32_t)offSecs * 1000UL) || pulsePhaseMs[r] == 0) {
              pulseOn[r] = true;
              pulsePhaseMs[r] = tnow;
            }
          }
          setRelay(r, pulseOn[r]);
        }
        break;
      }
      default:
        break;
    }
  }

  // Update the fast-exit flag based on actual relay states after processing.
  // Must reflect reality: only set true if all relays are OFF and we're disarmed.
  relaysKnownOff = (alarmCtx.globalState == AlarmState::DISARMED);
  for (int r = 0; r < MAX_RELAYS && relaysKnownOff; r++) {
    if (relayManualOverride[r]) {
      relaysKnownOff = !relayManualState[r];  // only OK if override is OFF
    } else if (relayStates[r]) {
      relaysKnownOff = false;
    }
  }
}

// ─── Handle digital input actions ─────────────────────────────────────────
static void processDigitalInputs() {
  static bool    startupGraceActive = true;
  static uint32_t bootStartMs        = millis();  // captured once at first call
  if (startupGraceActive) {
    // Ignore digital inputs for first 3 seconds after boot
    // (prevents spurious active-low triggers from disarming on restart)
    uint32_t now = millis();
    if (now - bootStartMs < 3000) {
      for (int i = 0; i < MAX_DINPUTS; i++) dinputStates[i] = false;
      return;
    }
    startupGraceActive = false;
  }

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
          zs.armedAtMs = now;  // reset timestamp for settle grace period
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
          zs.alarmEnteredMs = now;
        }
        break;

      // ─── ALARM ────────────────────────────────────────────────────────
      case ZONE_ALARM:
        // Siren timing handled entirely by syncRelays() — nothing to do here
        break;

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
    // ─── Clear manual relay overrides on any state transition ───────────
    // This ensures automatic siren/alarm relay control resumes for the new
    // alarm cycle. Without this, a manual relay toggle (web/MQTT) would
    // permanently bypass syncRelays() automatic control for that relay.
    bool anyOverrideCleared = false;
    for (int r = 0; r < MAX_RELAYS; r++) {
      if (relayManualOverride[r]) {
        relayManualOverride[r] = false;
        anyOverrideCleared = true;
      }
    }
    if (anyOverrideCleared) {
      logRelay("Manual override cleared on state transition");
    }

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
