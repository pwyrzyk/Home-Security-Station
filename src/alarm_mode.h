#pragma once

#include "config.h"

// ─── Alarm Mode operations ─────────────────────────────────────────────────
// Arm the controller into a specific HA alarm mode.
// Returns true on success, false if the mode profile is invalid (no zones).
bool armMode(AlarmMode mode, const char* source);

// Disarm the controller (all zones off).
void disarmMode(const char* source);

// Derive the global HA-compatible alarm state from per-zone states.
AlarmState deriveGlobalAlarmState();

// ─── String conversions ────────────────────────────────────────────────────
const char* alarmStateToHaString(AlarmState s);
const char* alarmModeToHaString(AlarmMode m);
AlarmMode haCommandToMode(const char* cmd);

// ─── Last trigger tracking ─────────────────────────────────────────────────
// Called by the alarm engine when a zone enters ZONE_ALARM.
void updateLastTrigger(uint8_t zoneId);