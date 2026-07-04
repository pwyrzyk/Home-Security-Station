#pragma once

#include "config.h"

// ─── Zone commands ─────────────────────────────────────────────────────────
void zoneArm(uint8_t zoneId);        // 1-based
void zoneDisarm(uint8_t zoneId);
void zoneToggle(uint8_t zoneId);
void armAllZones();
void disarmAllZones();

// ─── Internal (no EEPROM save) — for batch operations ──────────────────────
// These update zone state without calling saveArmedState(). The caller is
// responsible for calling saveArmedState() once after the batch completes.
void zoneArmNoSave(uint8_t zoneId);
void zoneDisarmNoSave(uint8_t zoneId);

// ─── Command source (for event log) ────────────────────────────────────────
extern const char* lastZoneCmdSource;    // "web user", "MQTT", "digital input", "system"

// ─── Zone query ────────────────────────────────────────────────────────────
bool isZoneArmed(uint8_t zoneId);
ZoneAlarmState getZoneState(uint8_t zoneId);
const char* zoneAlarmStateStr(ZoneAlarmState s);
const char* zoneAlarmStateLabel(ZoneAlarmState s);
