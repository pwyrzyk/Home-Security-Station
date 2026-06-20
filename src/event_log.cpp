#include "event_log.h"
#include <LittleFS.h>
#include <time.h>
#include <ArduinoJson.h>

// ─── Circular buffer ────────────────────────────────────────────────────────
static EventLogEntry events[EVENT_LOG_MAX_ENTRIES];
static uint16_t head = 0;    // next write position
static uint16_t count = 0;   // total entries stored (0..256)

// ─── Forward ────────────────────────────────────────────────────────────────
static uint32_t currentEpoch() {
  time_t t = time(nullptr);
  return (t > 0) ? (uint32_t)t : 0;
}

static void writeEntry(const EventLogEntry& entry) {
  events[head] = entry;
  head = (head + 1) % EVENT_LOG_MAX_ENTRIES;
  if (count < EVENT_LOG_MAX_ENTRIES) count++;
}

// ─── Persist a single entry to the end of the LittleFS file ────────────────
static void persistEntry(const EventLogEntry& entry) {
  File f = LittleFS.open(EVENT_LOG_FILE, "a");   // append
  if (!f) return;
  f.write((const uint8_t*)&entry, sizeof(EventLogEntry));
  f.close();
}

// ─── Load all entries from LittleFS on boot ─────────────────────────────────
static void loadFromLittleFS() {
  if (!LittleFS.exists(EVENT_LOG_FILE)) return;

  File f = LittleFS.open(EVENT_LOG_FILE, "r");
  if (!f) return;

  EventLogEntry buf;
  while (f.readBytes((char*)&buf, sizeof(EventLogEntry)) == sizeof(EventLogEntry)) {
    // Validate entry before accepting
    if (buf.type <= EVENT_RELAY && buf.timestamp > 0 && buf.description[0] != 0) {
      writeEntry(buf);
    }
  }
  f.close();
}

// ─── Public API ─────────────────────────────────────────────────────────────

void eventLogInit() {
  memset(events, 0, sizeof(events));
  head = 0;
  count = 0;
  loadFromLittleFS();
}

void logAlarm(const char* desc) {
  EventLogEntry entry;
  entry.type = EVENT_ALARM;
  entry.timestamp = currentEpoch();
  strlcpy(entry.description, desc, sizeof(entry.description));
  writeEntry(entry);
  persistEntry(entry);
}

void logSystem(const char* desc) {
  EventLogEntry entry;
  entry.type = EVENT_SYSTEM;
  entry.timestamp = currentEpoch();
  strlcpy(entry.description, desc, sizeof(entry.description));
  writeEntry(entry);
  persistEntry(entry);
}

void logRelay(const char* desc) {
  EventLogEntry entry;
  entry.type = EVENT_RELAY;
  entry.timestamp = currentEpoch();
  strlcpy(entry.description, desc, sizeof(entry.description));
  writeEntry(entry);
  persistEntry(entry);
}

void clearEventLog() {
  memset(events, 0, sizeof(events));
  head = 0;
  count = 0;
  // Truncate the persisted file
  if (LittleFS.exists(EVENT_LOG_FILE)) {
    LittleFS.remove(EVENT_LOG_FILE);
  }
}

String getEventLogJson() {
  uint32_t now = currentEpoch();
  uint32_t cutoff = (now > EVENT_LOG_RETENTION_SEC) ? (now - EVENT_LOG_RETENTION_SEC) : 0;

  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();

  // Iterate backwards from head (newest first), up to count entries
  uint16_t displayed = 0;
  for (uint16_t i = 0; i < count && displayed < EVENT_LOG_DISPLAY_COUNT; i++) {
    // Index in the circular buffer: newest = head-1, then head-2, ...
    uint16_t idx = (head >= i + 1) ? (head - i - 1) : (head + EVENT_LOG_MAX_ENTRIES - i - 1);

    // Skip entries outside retention window
    if (events[idx].timestamp < cutoff) continue;

    JsonObject e = arr.add<JsonObject>();
    e["type"] = events[idx].type;
    e["ts"]   = events[idx].timestamp;
    e["desc"] = events[idx].description;
    displayed++;
  }

  String buf;
  buf.reserve(3000);
  serializeJson(doc, buf);
  return buf;
}
