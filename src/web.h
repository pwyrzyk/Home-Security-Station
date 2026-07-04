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

// Called from main loop() to execute deferred web-request actions.
void webLoop();
