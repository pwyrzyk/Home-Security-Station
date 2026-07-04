#pragma once

#include "config.h"

// ─── Sensor loop ───────────────────────────────────────────────────────────
void sensorsLoop();            // runs every ~100ms, updates sensorStates[]
bool isSensorActive(uint8_t idx);

// ─── Query helpers ─────────────────────────────────────────────────────────
const char* sensorTypeStr(SensorType t);
const char* sensorStateStr(SensorState s);

// ─── Zone sensor activity cache ──────────────────────────────────────────
void updateZoneSensorCache();  // rebuild zoneSensorActiveCache from sensor states
