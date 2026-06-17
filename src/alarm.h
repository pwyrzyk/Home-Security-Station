#pragma once

#include "config.h"

// ─── Alarm engine ──────────────────────────────────────────────────────────
void alarmLoop();     // runs each main loop iteration, evaluates zones

// Zone state change callback (set by mqtt module for HA publish)
extern void (*onZoneStateChanged)(uint8_t zoneId);