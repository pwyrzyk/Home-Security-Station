#include "event_log.h"
#include <LittleFS.h>
#include <time.h>
#include <ArduinoJson.h>

// ─── Circular buffer (in-memory) ────────────────────────────────────────────
static EventLogEntry events[EVENT_LOG_MAX_ENTRIES];
static uint16_t head = 0;    // next write position
static uint16_t count = 0;   // total entries stored (0..256)

// ─── Write-through batch buffer (reduces LittleFS open/close cycles) ────────
// Instead of open/append/close per entry (which wears flash), we buffer up to
// 10 entries and flush them together. ALARM events flush immediately.
#define FLUSH_BATCH_SIZE 10
static EventLogEntry batchBuf[FLUSH_BATCH_SIZE];
static uint8_t       batchCount = 0;
static uint32_t      lastFlushMs = 0;

static uint32_t currentEpoch() {
  time_t t = time(nullptr);
  return (t > 0) ? (uint32_t)t : 0;
}

static void writeEntry(const EventLogEntry& entry) {
  events[head] = entry;
  head = (head + 1) % EVENT_LOG_MAX_ENTRIES;
  if (count < EVENT_LOG_MAX_ENTRIES) count++;
}

// ─── Flush buffered entries to LittleFS (single open/append/close) ─────────
static void flushBatch() {
  if (batchCount == 0) return;
  File f = LittleFS.open(EVENT_LOG_FILE, "a");
  if (!f) return;
  f.write((const uint8_t*)batchBuf, batchCount * sizeof(EventLogEntry));
  f.close();
  batchCount = 0;
  lastFlushMs = millis();
}

// ─── Buffer a single entry; flush if batch full or alarm event ─────────────
static void bufferEntry(const EventLogEntry& entry) {
  batchBuf[batchCount++] = entry;
  // Flush immediately on alarm events (critical — must survive crash)
  // or when batch is full
  if (entry.type == EVENT_ALARM || batchCount >= FLUSH_BATCH_SIZE) {
    flushBatch();
  }
}

// ─── Called periodically from main loop (every ~10ms) to flush stale ───────
void eventLogFlushIfNeeded() {
  if (batchCount > 0 && millis() - lastFlushMs >= 10000) {
    flushBatch();
  }
}

// ─── Load all entries from LittleFS on boot ─────────────────────────────────
static void loadFromLittleFS() {
  if (!LittleFS.exists(EVENT_LOG_FILE)) return;

  File f = LittleFS.open(EVENT_LOG_FILE, "r");
  if (!f) return;

  EventLogEntry buf;
  while (f.readBytes((char*)&buf, sizeof(EventLogEntry)) == sizeof(EventLogEntry)) {
    // Validate entry before accepting
    if (buf.type <= EVENT_SENSOR && buf.timestamp > 0 && buf.description[0] != 0) {
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
  batchCount = 0;
  lastFlushMs = 0;
  loadFromLittleFS();
}

void logAlarm(const char* desc) {
  EventLogEntry entry;
  entry.type = EVENT_ALARM;
  entry.timestamp = currentEpoch();
  strlcpy(entry.description, desc, sizeof(entry.description));
  writeEntry(entry);
  bufferEntry(entry);  // will flush immediately (EVENT_ALARM)
}

void logSystem(const char* desc) {
  EventLogEntry entry;
  entry.type = EVENT_SYSTEM;
  entry.timestamp = currentEpoch();
  strlcpy(entry.description, desc, sizeof(entry.description));
  writeEntry(entry);
  bufferEntry(entry);
}

void logRelay(const char* desc) {
  EventLogEntry entry;
  entry.type = EVENT_RELAY;
  entry.timestamp = currentEpoch();
  strlcpy(entry.description, desc, sizeof(entry.description));
  writeEntry(entry);
  bufferEntry(entry);
}

void logSensor(const char* desc) {
  EventLogEntry entry;
  entry.type = EVENT_SENSOR;
  entry.timestamp = currentEpoch();
  strlcpy(entry.description, desc, sizeof(entry.description));
  writeEntry(entry);
  bufferEntry(entry);
}

void clearEventLog() {
  flushBatch();  // flush any pending entries first
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