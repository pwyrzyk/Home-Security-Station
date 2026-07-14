#pragma once

#include <ESPAsyncWebServer.h>
#include "config.h"

// ─── Web server ────────────────────────────────────────────────────────────
extern AsyncWebServer server;   // on port HTTP_PORT

void initWebServer();

// ─── Deferred actions (set by async handlers, executed in loop) ───────────
// Async handlers must not block (delay/WiFi.disconnect). They set these flags
// and webLoop() performs the actual work from the main loop context.
extern bool pendingRestart;
extern bool pendingReconnect;

// Deferred external-sensor trigger (set by /api/extsensors/trigger handler).
// Mutating extSensorStates[] from the async TCP task races with sensorsLoop();
// webLoop() applies the change from the main loop context instead.
struct PendingExtSensorTrigger {
  bool     pending;
  uint8_t  id;     // 1..MAX_EXT_SENSORS
  bool     active;
};
extern PendingExtSensorTrigger pendingExtTrigger;

// Deferred relay command (set by /api/relay/<id> handler).
// Mutating relayManualOverride/relayManualState + setRelay() from the async
// TCP task races with syncRelays() in alarmLoop(); webLoop() applies it.
struct PendingRelayCommand {
  bool     pending;
  uint8_t  id;     // 1..MAX_RELAYS
  bool     on;
};
extern PendingRelayCommand pendingRelayCmd;

// Called from main loop() to execute deferred web-request actions.
void webLoop();
