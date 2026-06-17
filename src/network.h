#pragma once

#include "config.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>

// ─── WiFi management ───────────────────────────────────────────────────────
bool connectWiFiStation();
void startConfigAP();
void ensureWiFiMode();

// ─── OTA setup ─────────────────────────────────────────────────────────────
void initOTA();

// ─── NTP sync ──────────────────────────────────────────────────────────────
void syncNTP();           // call periodically in loop
bool ntpSynced();