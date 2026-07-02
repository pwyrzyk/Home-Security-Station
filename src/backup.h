#pragma once

#include <Arduino.h>

// ─── API ────────────────────────────────────────────────────────────────────
String   buildBackupJson();                              // returns full backup as JSON string
bool     applyRestore(const char *json, String &error);  // applies backup, returns false + error msg on failure