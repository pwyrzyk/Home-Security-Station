#pragma once

#include "config.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>

// ─── WiFi management ───────────────────────────────────────────────────────
bool connectWiFiStation();
void startConfigAP();
void ensureWiFiMode();         // boot-time WiFi connect with retry scheduling
void wifiStationRetryLoop();   // runtime watchdog: retries, AP→STA recovery

// ─── OTA setup ─────────────────────────────────────────────────────────────
void initOTA();

// ─── NTP sync ──────────────────────────────────────────────────────────────
void syncNTP();           // call periodically in loop
bool ntpSynced();