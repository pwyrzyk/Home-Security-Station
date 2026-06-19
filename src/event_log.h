#pragma once

#include <Arduino.h>

// ─── Event types ────────────────────────────────────────────────────────────
enum EventType : uint8_t {
  EVENT_ALARM  = 0,
  EVENT_SYSTEM = 1,
  EVENT_RELAY  = 2
};

// ─── Single event entry ─────────────────────────────────────────────────────
struct EventLogEntry {
  EventType type;
  uint32_t  timestamp;        // Unix epoch (seconds)
  char      description[80];  // null-terminated
};

// ─── Constants ──────────────────────────────────────────────────────────────
#define EVENT_LOG_MAX_ENTRIES  256
#define EVENT_LOG_RETENTION_SEC (30UL * 86400UL)   // 30 days
#define EVENT_LOG_DISPLAY_COUNT 30                 // shown in web UI

// ─── File path for LittleFS persistence ─────────────────────────────────────
#define EVENT_LOG_FILE "/eventlog.bin"

// ─── API ────────────────────────────────────────────────────────────────────
void eventLogInit();
void logAlarm(const char* desc);
void logSystem(const char* desc);
void logRelay(const char* desc);
void clearEventLog();
String getEventLogJson();           // last 30 entries within retention window