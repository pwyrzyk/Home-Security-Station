#include "alarm_mode.h"
#include "zones.h"
#include "sensors.h"
#include "event_log.h"

// ─── Helper: check if a sensor is tripped for a given zone ────────────────
static bool sensorTrippedForZone(uint8_t zoneId, uint8_t& outSensorId, char* outSensorName, size_t nameSize) {
  for (int s = 0; s < TOTAL_SENSORS; s++) {
    if (config.sensors[s].type == SENSOR_DISABLED) continue;
    if (!(config.sensors[s].zoneMask & (1U << (zoneId - 1)))) continue;
    if (sensorStates[s].state == SENSOR_ACTIVE) {
      outSensorId = s + 1;
      strlcpy(outSensorName, config.sensors[s].name, nameSize);
      if (strlen(outSensorName) == 0) snprintf(outSensorName, nameSize, "T%d", s + 1);
      return true;
    }
  }
  for (int s = 0; s < MAX_EXT_SENSORS; s++) {
    if (!config.extSensors[s].enabled) continue;
    if (!(config.extSensors[s].zoneMask & (1U << (zoneId - 1)))) continue;
    if (extSensorStates[s].active) {
      outSensorId = s + 1 + TOTAL_SENSORS;  // offset to distinguish from local
      strlcpy(outSensorName, config.extSensors[s].name, nameSize);
      if (strlen(outSensorName) == 0) snprintf(outSensorName, nameSize, "E%d", s + 1);
      return true;
    }
  }
  return false;
}

// ─── armMode ───────────────────────────────────────────────────────────────

bool armMode(AlarmMode mode, const char* source) {
  uint8_t idx = (uint8_t)mode;
  if (idx == 0) return false;  // DISARMED is not an arm command
  if (idx >= (uint8_t)AlarmMode::NUM_MODES) return false;

  AlarmModeProfile &prof = config.modeProfiles[idx];
  if (!prof.defined) {
    char buf[80];
    snprintf(buf, sizeof(buf), "Arm rejected: mode '%s' has no profile configured", alarmModeToHaString(mode));
    logSystem(buf);
    return false;
  }
  if (prof.zoneMask == 0) {
    char buf[80];
    snprintf(buf, sizeof(buf), "Arm rejected: mode '%s' has no zones assigned", alarmModeToHaString(mode));
    logSystem(buf);
    return false;
  }

  // Compute effective mask: skip disabled zones
  uint8_t effectiveMask = prof.zoneMask;
  for (int z = 0; z < MAX_ZONES; z++) {
    if (!config.zones[z].enabled) {
      effectiveMask &= ~(1U << z);
    }
  }

  if (effectiveMask == 0) {
    char buf[80];
    snprintf(buf, sizeof(buf), "Arm rejected: all zones in mode '%s' are disabled", alarmModeToHaString(mode));
    logSystem(buf);
    return false;
  }

  lastZoneCmdSource = source;

  // Disarm zones not in the mode (only if currently armed)
  for (uint8_t z = 1; z <= MAX_ZONES; z++) {
    if (!(effectiveMask & (1U << (z - 1)))) {
      if (isZoneArmed(z)) {
        zoneDisarmNoSave(z);
      }
    }
  }

  // Arm zones in the mode
  for (uint8_t z = 1; z <= MAX_ZONES; z++) {
    if (effectiveMask & (1U << (z - 1))) {
      zoneArmNoSave(z);
    }
  }

  alarmCtx.activeMode = mode;
  alarmCtx.activeZoneMask = effectiveMask;
  saveArmedState();  // single persist after batch

  char buf[100];
  snprintf(buf, sizeof(buf), "Alarm mode set to '%s' by %s (mask=0x%02X)",
           alarmModeToHaString(mode), source, effectiveMask);
  logAlarm(buf);

  return true;
}

// ─── disarmMode ────────────────────────────────────────────────────────────

void disarmMode(const char* source) {
  lastZoneCmdSource = source;

  // Only disarm zones that are actually armed (idempotent)
  for (uint8_t z = 1; z <= MAX_ZONES; z++) {
    if (isZoneArmed(z)) {
      zoneDisarmNoSave(z);
    }
  }

  alarmCtx.activeMode = AlarmMode::DISARMED;
  alarmCtx.activeZoneMask = 0;
  saveArmedState();  // single persist after batch

  char buf[80];
  snprintf(buf, sizeof(buf), "Alarm disarmed by %s", source);
  logAlarm(buf);
}

// ─── deriveGlobalAlarmState ────────────────────────────────────────────────

AlarmState deriveGlobalAlarmState() {
  bool anyTriggered = false;
  bool anyPending = false;
  bool anyArmed = false;

  for (int z = 0; z < MAX_ZONES; z++) {
    if (!config.zones[z].enabled) continue;
    switch (zoneStates[z].alarmState) {
      case ZONE_ALARM:
        anyTriggered = true;
        break;
      case ZONE_ARMING:
      case ZONE_DISARMING:
        anyPending = true;
        break;
      case ZONE_ARMED_IDLE:
        anyArmed = true;
        break;
      default:
        break;
    }
  }

  if (anyTriggered) return AlarmState::TRIGGERED;
  if (anyPending)   return AlarmState::PENDING;
  if (!anyArmed)    return AlarmState::DISARMED;

  // Map active mode to HA state
  switch (alarmCtx.activeMode) {
    case AlarmMode::ARMED_HOME:          return AlarmState::ARMED_HOME;
    case AlarmMode::ARMED_AWAY:          return AlarmState::ARMED_AWAY;
    case AlarmMode::ARMED_NIGHT:         return AlarmState::ARMED_NIGHT;
    case AlarmMode::ARMED_VACATION:      return AlarmState::ARMED_VACATION;
    case AlarmMode::ARMED_CUSTOM_BYPASS: return AlarmState::ARMED_CUSTOM_BYPASS;
    default:                             return AlarmState::DISARMED;
  }
}

// ─── String conversions ────────────────────────────────────────────────────

const char* alarmStateToHaString(AlarmState s) {
  switch (s) {
    case AlarmState::DISARMED:            return "disarmed";
    case AlarmState::ARMED_HOME:          return "armed_home";
    case AlarmState::ARMED_AWAY:          return "armed_away";
    case AlarmState::ARMED_NIGHT:         return "armed_night";
    case AlarmState::ARMED_VACATION:      return "armed_vacation";
    case AlarmState::ARMED_CUSTOM_BYPASS: return "armed_custom_bypass";
    case AlarmState::PENDING:             return "pending";
    case AlarmState::TRIGGERED:           return "triggered";
    default:                              return "disarmed";
  }
}

const char* alarmModeToHaString(AlarmMode m) {
  switch (m) {
    case AlarmMode::DISARMED:            return "disarmed";
    case AlarmMode::ARMED_HOME:          return "armed_home";
    case AlarmMode::ARMED_AWAY:          return "armed_away";
    case AlarmMode::ARMED_NIGHT:         return "armed_night";
    case AlarmMode::ARMED_VACATION:      return "armed_vacation";
    case AlarmMode::ARMED_CUSTOM_BYPASS: return "armed_custom_bypass";
    default:                             return "disarmed";
  }
}

AlarmMode haCommandToMode(const char* cmd) {
  if (strcmp(cmd, "DISARM") == 0)            return AlarmMode::DISARMED;
  if (strcmp(cmd, "ARM_HOME") == 0)          return AlarmMode::ARMED_HOME;
  if (strcmp(cmd, "ARM_AWAY") == 0)          return AlarmMode::ARMED_AWAY;
  if (strcmp(cmd, "ARM_NIGHT") == 0)         return AlarmMode::ARMED_NIGHT;
  if (strcmp(cmd, "ARM_VACATION") == 0)      return AlarmMode::ARMED_VACATION;
  if (strcmp(cmd, "ARM_CUSTOM_BYPASS") == 0) return AlarmMode::ARMED_CUSTOM_BYPASS;
  // Unknown commands default to DISARMED (no-op for arm)
  return AlarmMode::DISARMED;
}

// ─── updateLastTrigger ─────────────────────────────────────────────────────

void updateLastTrigger(uint8_t zoneId) {
  if (zoneId < 1 || zoneId > MAX_ZONES) return;

  uint8_t zidx = zoneId - 1;
  alarmCtx.lastTriggerZoneId = zoneId;
  alarmCtx.lastTriggerTimeMs = millis();

  // Copy zone name
  strlcpy(alarmCtx.lastTriggerZoneName, config.zones[zidx].name,
          sizeof(alarmCtx.lastTriggerZoneName));
  if (strlen(alarmCtx.lastTriggerZoneName) == 0) {
    snprintf(alarmCtx.lastTriggerZoneName, sizeof(alarmCtx.lastTriggerZoneName),
             "Zone %u", zoneId);
  }

  // Find the tripped sensor for this zone
  uint8_t sensorId = 0;
  char sensorName[24] = "";
  if (sensorTrippedForZone(zoneId, sensorId, sensorName, sizeof(sensorName))) {
    alarmCtx.lastTriggerSensorId = sensorId;
    strlcpy(alarmCtx.lastTriggerSensorName, sensorName,
            sizeof(alarmCtx.lastTriggerSensorName));
  } else {
    alarmCtx.lastTriggerSensorId = 0;
    strlcpy(alarmCtx.lastTriggerSensorName, "unknown",
            sizeof(alarmCtx.lastTriggerSensorName));
  }

  char buf[100];
  snprintf(buf, sizeof(buf), "Last trigger: zone='%s' sensor='%s'",
           alarmCtx.lastTriggerZoneName, alarmCtx.lastTriggerSensorName);
  logSensor(buf);
}