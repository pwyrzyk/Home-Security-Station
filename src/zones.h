#pragma once

#include "config.h"

// ─── Zone commands ─────────────────────────────────────────────────────────
void zoneArm(uint8_t zoneId);        // 1-based
void zoneDisarm(uint8_t zoneId);
void zoneToggle(uint8_t zoneId);
void armAllZones();
void disarmAllZones();

// ─── Zone query ────────────────────────────────────────────────────────────
bool isZoneArmed(uint8_t zoneId);
ZoneAlarmState getZoneState(uint8_t zoneId);
const char* zoneAlarmStateStr(ZoneAlarmState s);
const char* zoneAlarmStateLabel(ZoneAlarmState s);
