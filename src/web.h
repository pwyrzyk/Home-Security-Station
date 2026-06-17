#pragma once

#include <ESPAsyncWebServer.h>
#include "config.h"

// ─── Web server ────────────────────────────────────────────────────────────
extern AsyncWebServer server;   // on port HTTP_PORT

void initWebServer();