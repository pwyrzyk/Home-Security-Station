#include "zones.h"
#include "hardware.h"
#include "sensors.h"

const char* lastZoneCmdSource = nullptr;

// ─── Internal change notification ─────────────────────────────────────────
// Set by alarm engine when zone state changes programmatically
extern void (*onZoneStateChanged)(uint8_t zoneId);
static void notifyZoneChange(uint8_t zoneId);

static void notifyZoneChange(uint8_t zoneId) {
  if (onZoneStateChanged) onZoneStateChanged(zoneId);
}

void zoneArm(uint8_t zoneId) {
  if (zoneId < 1 || zoneId > MAX_ZONES) return;
  uint8_t idx = zoneId - 1;
  if (zoneStates[idx].alarmState == ZONE_DISARMED) {
    zoneStates[idx].armed = true;
    zoneStates[idx].armedAtMs = millis();
    if (config.zones[idx].exitDelayS > 0) {
      zoneStates[idx].alarmState = ZONE_ARMING;
    } else {
      zoneStates[idx].alarmState = ZONE_ARMED_IDLE;
    }
    notifyZoneChange(zoneId);
  }
}

void zoneDisarm(uint8_t zoneId) {
  if (zoneId < 1 || zoneId > MAX_ZONES) return;
  uint8_t idx = zoneId - 1;
  zoneStates[idx].armed = false;
  zoneStates[idx].alarmState = ZONE_DISARMED;
  zoneStates[idx].preAlarmStartMs = 0;
  zoneStates[idx].sirenPhaseMs = 0;
  zoneStates[idx].sirenOn = false;
  zoneStates[idx].sirenOneShotDone = false;
  notifyZoneChange(zoneId);
}

void zoneToggle(uint8_t zoneId) {
  if (zoneId < 1 || zoneId > MAX_ZONES) return;
  if (isZoneArmed(zoneId)) zoneDisarm(zoneId);
  else zoneArm(zoneId);
}

void armAllZones() {
  lastZoneCmdSource = "system";
  for (uint8_t i = 1; i <= MAX_ZONES; i++) zoneArm(i);
}

void disarmAllZones() {
  lastZoneCmdSource = "system";
  for (uint8_t i = 1; i <= MAX_ZONES; i++) zoneDisarm(i);
}

bool isZoneArmed(uint8_t zoneId) {
  if (zoneId < 1 || zoneId > MAX_ZONES) return false;
  uint8_t idx = zoneId - 1;
  return zoneStates[idx].armed && zoneStates[idx].alarmState != ZONE_DISARMED;
}

ZoneAlarmState getZoneState(uint8_t zoneId) {
  if (zoneId < 1 || zoneId > MAX_ZONES) return ZONE_DISARMED;
  return zoneStates[zoneId - 1].alarmState;
}

const char* zoneAlarmStateStr(ZoneAlarmState s) {
  switch (s) {
    case ZONE_ARMED_IDLE: return "armed_idle";
    case ZONE_PREALARM:   return "prealarm";
    case ZONE_ALARM:      return "alarm";
    case ZONE_ARMING:     return "arming";
    case ZONE_DISARMING:  return "disarming";
    default:              return "disarmed";
  }
}

const char* zoneAlarmStateLabel(ZoneAlarmState s) {
  switch (s) {
    case ZONE_ARMED_IDLE: return "Armed";
    case ZONE_PREALARM:   return "Pre-alarm";
    case ZONE_ALARM:      return "Alarm";
    case ZONE_ARMING:     return "Pending Exit";
    case ZONE_DISARMING:  return "Pending Entry";
    default:              return "Disarmed";
  }
}
