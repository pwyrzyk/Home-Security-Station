#pragma once

#include <Arduino.h>
#include <ESPAsyncWebServer.h>

// ═══════════════════════════════════════════════════════════════════════════════
// Slave Keypad Web Server — WiFi AP/STA + WebUI
// ═══════════════════════════════════════════════════════════════════════════════

// ─── WiFi AP Configuration (fallback) ───────────────────────────────────────
#define WIFI_AP_SSID_PREFIX  "Keypad-Slave-"
#define WIFI_AP_PASSWORD     "12345678"
#define WIFI_AP_CHANNEL      6
#define WIFI_AP_MAX_CLIENTS  3

// ─── STA connection timeout ─────────────────────────────────────────────────
#define WIFI_STA_TIMEOUT_MS  15000

// ─── EEPROM layout ─────────────────────────────────────────────────────────
#define EEPROM_SIZE          512
#define EEPROM_WIFI_MAGIC    0xA5
#define EEPROM_WIFI_ADDR     0
// Offset 0: magic byte
// Offset 1: ssid[32] + '\0'
// Offset 33: pass[64] + '\0'

// ─── WebSocket log max lines in ring buffer ─────────────────────────────────
#define WS_LOG_MAX_LINES  200

// ─── HTML pages (PROGMEM) ───────────────────────────────────────────────────
extern const char MAIN_HTML[] PROGMEM;
extern const char CONFIG_HTML[] PROGMEM;

// ─── Public API ─────────────────────────────────────────────────────────────

// Initialize WiFi (AP or STA) and web server. Call once in setup().
void slaveWebInit(uint8_t slaveId);

// Process web server & WebSocket events. Call every loop iteration.
void slaveWebLoop();

// Notify web clients of RS-485 TX traffic
void slaveWebNotifyTX(const char* msg);

// Notify web clients of RS-485 RX traffic
void slaveWebNotifyRX(const char* msg);

// Get current runtime slave ID (may differ from compile-time SLAVE_ID)
uint8_t slaveWebGetRuntimeId();

// Extern reference to the WebSocket server
extern AsyncWebSocket slaveWebSocket;