#include "rs485_web.h"
#include "keypad_comm.h"
#include "web.h"
#include <ESPAsyncWebServer.h>

// ─── RS-485 Monitor HTML Page (PROGMEM) ─────────────────────────────────────

const char RS485_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>RS-485 Bus Monitor</title>
<style>
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body {
    font-family: 'Courier New', monospace;
    background: #1a1a2e;
    color: #e0e0e0;
    padding: 20px;
    min-height: 100vh;
  }
  h1 {
    text-align: center;
    color: #00d4ff;
    margin-bottom: 20px;
    font-size: 1.4em;
  }
  .container {
    max-width: 900px;
    margin: 0 auto;
    display: flex;
    flex-direction: column;
    gap: 20px;
  }
  .panel {
    background: #16213e;
    border: 1px solid #0f3460;
    border-radius: 8px;
    padding: 15px;
  }
  .panel h2 {
    color: #00d4ff;
    font-size: 1em;
    margin-bottom: 10px;
    border-bottom: 1px solid #0f3460;
    padding-bottom: 8px;
    display: flex;
    justify-content: space-between;
    align-items: center;
  }
  #log {
    background: #0d1117;
    border: 1px solid #30363d;
    border-radius: 4px;
    padding: 10px;
    height: 350px;
    overflow-y: auto;
    font-size: 0.85em;
    line-height: 1.5;
    white-space: pre-wrap;
    word-wrap: break-word;
  }
  #log .rx { color: #58a6ff; }
  #log .tx { color: #3fb950; }
  #log .info { color: #d29922; }
  #log .sys { color: #8b949e; }
  .send-area {
    display: flex;
    gap: 10px;
  }
  #msgInput {
    flex: 1;
    padding: 10px 14px;
    font-size: 1em;
    font-family: 'Courier New', monospace;
    background: #0d1117;
    border: 1px solid #30363d;
    border-radius: 4px;
    color: #e0e0e0;
    outline: none;
  }
  #msgInput:focus { border-color: #00d4ff; }
  #sendBtn {
    padding: 10px 24px;
    font-size: 1em;
    font-family: 'Courier New', monospace;
    background: #238636;
    color: #fff;
    border: none;
    border-radius: 4px;
    cursor: pointer;
    font-weight: bold;
  }
  #sendBtn:hover { background: #2ea043; }
  #sendBtn:active { background: #1a7f37; }
  .status {
    text-align: center;
    font-size: 0.8em;
    color: #8b949e;
    margin-top: 10px;
  }
  .status .connected { color: #3fb950; }
  .status .disconnected { color: #f85149; }
  #clearBtn {
    padding: 6px 14px;
    font-size: 0.8em;
    font-family: 'Courier New', monospace;
    background: #484f58;
    color: #fff;
    border: none;
    border-radius: 4px;
    cursor: pointer;
  }
  #clearBtn:hover { background: #6e7681; }
  .back-link {
    display: block;
    text-align: center;
    color: #58a6ff;
    text-decoration: none;
    font-size: 0.85em;
    margin-top: 10px;
  }
  .back-link:hover { text-decoration: underline; }
  .slave-grid {
    display: flex;
    gap: 10px;
    flex-wrap: wrap;
    margin-bottom: 10px;
  }
  .slave-box {
    flex: 1;
    min-width: 80px;
    background: #0d1117;
    border: 1px solid #30363d;
    border-radius: 6px;
    padding: 10px;
    text-align: center;
    font-size: 0.8em;
  }
  .slave-box.online { border-color: #3fb950; }
  .slave-box.offline { border-color: #30363d; opacity: 0.5; }
  .slave-box .slave-id { font-weight: bold; color: #00d4ff; }
  .slave-box .slave-status { font-size: 0.85em; margin-top: 4px; }
  .slave-box.online .slave-status { color: #3fb950; }
  .slave-box.offline .slave-status { color: #8b949e; }
</style>
</head>
<body>
<div class="container">
  <h1>&#x1F4E1; RS-485 Bus Monitor</h1>

  <div class="panel">
    <h2>Keypad Slaves <button id="clearBtn" onclick="clearLog()">Clear Log</button></h2>
    <div class="slave-grid" id="slaveGrid">Loading...</div>
  </div>

  <div class="panel">
    <h2>RS-485 Communication Log</h2>
    <div id="log"></div>
  </div>

  <div class="panel">
    <h2>Send Raw Message to RS-485 Bus</h2>
    <div class="send-area">
      <input type="text" id="msgInput" placeholder="Type message to send..." onkeydown="if(event.key==='Enter')sendMsg()">
      <button id="sendBtn" onclick="sendMsg()">SEND</button>
    </div>
  </div>

  <div class="status">
    WebSocket: <span id="wsStatus" class="disconnected">Disconnected</span>
    &nbsp;|&nbsp; <span id="msgCount">0</span> messages
  </div>

  <a class="back-link" href="/">&larr; Back to Dashboard</a>
</div>

<script>
  var ws;
  var logDiv = document.getElementById('log');
  var msgCount = 0;

  function connect() {
    ws = new WebSocket('ws://' + window.location.host + '/ws-rs485');
    ws.onopen = function() {
      document.getElementById('wsStatus').textContent = 'Connected';
      document.getElementById('wsStatus').className = 'connected';
    };
    ws.onclose = function() {
      document.getElementById('wsStatus').textContent = 'Disconnected';
      document.getElementById('wsStatus').className = 'disconnected';
      setTimeout(connect, 2000);
    };
    ws.onmessage = function(evt) {
      var msg = evt.data;
      var span = document.createElement('span');

      // Check for slave status update
      if (msg.startsWith('[SLAVES]')) {
        try {
          var data = JSON.parse(msg.substring(8));
          updateSlaves(data);
        } catch(e) {}
        return;
      }

      if (msg.startsWith('[RX]')) span.className = 'rx';
      else if (msg.startsWith('[TX]')) span.className = 'tx';
      else if (msg.startsWith('[INFO]')) span.className = 'info';
      else span.className = 'sys';

      span.textContent = msg + '\n';
      logDiv.appendChild(span);
      msgCount++;
      document.getElementById('msgCount').textContent = msgCount;

      // Auto-scroll to bottom
      logDiv.scrollTop = logDiv.scrollHeight;

      // Limit log lines
      while (logDiv.childElementCount > 500) {
        logDiv.removeChild(logDiv.firstChild);
      }
    };
  }

  function updateSlaves(data) {
    var grid = document.getElementById('slaveGrid');
    var h = '';
    for (var i = 0; i < data.length; i++) {
      var s = data[i];
      var cls = s.online ? 'online' : 'offline';
      var status = s.online ? 'ONLINE' : 'offline';
      var lastHb = s.lastHbSec !== null ? s.lastHbSec + 's ago' : '---';
      h += '<div class="slave-box ' + cls + '">';
      h += '<div class="slave-id">Keypad ' + s.id + '</div>';
      h += '<div class="slave-status">' + status + '</div>';
      h += '<div style="font-size:0.75em;color:#8b949e">' + lastHb + '</div>';
      h += '</div>';
    }
    grid.innerHTML = h;
  }

  function sendMsg() {
    var input = document.getElementById('msgInput');
    var msg = input.value.trim();
    if (msg && ws && ws.readyState === WebSocket.OPEN) {
      ws.send(msg);
      input.value = '';
    }
  }

  function clearLog() {
    logDiv.innerHTML = '';
    msgCount = 0;
    document.getElementById('msgCount').textContent = '0';
  }

  connect();
</script>
</body>
</html>
)rawliteral";

// ═══════════════════════════════════════════════════════════════════════════════
// WebSocket for RS-485 monitor
// ═══════════════════════════════════════════════════════════════════════════════

static AsyncWebSocket wsRS485("/ws-rs485");

static RS485TrafficCallback trafficCb = nullptr;

// ─── Slave status broadcast timer ───────────────────────────────────────────
static uint32_t lastSlaveStatusMs = 0;
#define SLAVE_STATUS_INTERVAL_MS 3000  // broadcast slave status every 3s

static void broadcastSlaveStatus() {
  if (wsRS485.count() == 0) return;  // no clients connected

  String json = "[";
  for (uint8_t i = 0; i < KEYPAD_MAX_SLAVES; i++) {
    bool online = keypadSlaveOnline(i);
    uint32_t lastHb = keypadSlaveLastHB(i);
    uint32_t ago = 0;
    if (lastHb > 0) {
      uint32_t now = millis();
      ago = (now >= lastHb) ? ((now - lastHb) / 1000) : 0;
    }

    if (i > 0) json += ",";
    json += "{\"id\":" + String(i + 1);
    json += ",\"online\":" + String(online ? "true" : "false");
    json += ",\"lastHbSec\":" + String(lastHb > 0 ? (int)ago : -1);
    json += "}";
  }
  json += "]";

  wsRS485.textAll("[SLAVES]" + json);
}

// ─── Periodic loop — must be called regularly from main loop() ─────────────
void rs485WebLoop() {
  uint32_t now = millis();
  if (now - lastSlaveStatusMs >= SLAVE_STATUS_INTERVAL_MS) {
    lastSlaveStatusMs = now;
    broadcastSlaveStatus();
  }
}

// ─── Forward traffic to all connected WebSocket clients ────────────────────
static void onRS485Traffic(RS485Direction dir, const char* msg) {
  if (wsRS485.count() == 0) return;

  String prefix = (dir == RS485_DIR_RX) ? "[RX] " : "[TX] ";
  String line = prefix + String(msg);

  // Timestamp prefix
  uint32_t ms = millis() % 86400000;  // time-of-day ms
  uint32_t h  = ms / 3600000;
  uint32_t m  = (ms % 3600000) / 60000;
  uint32_t s  = (ms % 60000) / 1000;
  uint32_t ms2 = ms % 1000;
  char ts[16];
  snprintf(ts, sizeof(ts), "%02u:%02u:%02u.%03u ", h, m, s, ms2);

  wsRS485.textAll(String(ts) + line);
}

// ─── WebSocket event handler ────────────────────────────────────────────────
static void onWsRS485Event(AsyncWebSocket* server, AsyncWebSocketClient* client,
                           AwsEventType type, void* arg, uint8_t* data, size_t len) {
  switch (type) {
    case WS_EVT_CONNECT:
      client->printf("[INFO] Connected to RS-485 monitor\n");
      break;

    case WS_EVT_DATA: {
      // Client wants to send raw data to the RS-485 bus
      AwsFrameInfo* info = (AwsFrameInfo*)arg;
      if (info->final && info->index == 0 && info->len == len &&
          info->opcode == WS_TEXT) {
        data[len] = '\0';
        String msg = String((char*)data);

      // Forward to RS-485 bus via keypad_comm TX
      extern void rs485SendRaw(const char* msg);
      rs485SendRaw(msg.c_str());

      client->printf("{\"status\":\"ok\",\"sent\":%u}", msg.length());

      // Also log locally as TX
      onRS485Traffic(RS485_DIR_TX, msg.c_str());
      }
      break;
    }

    case WS_EVT_DISCONNECT:
      break;

    default:
      break;
  }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Public API
// ═══════════════════════════════════════════════════════════════════════════════

void initRS485Web() {
  // Register WebSocket endpoint
  wsRS485.onEvent(onWsRS485Event);
  server.addHandler(&wsRS485);

  // Serve the monitor HTML page (auth required, same as dashboard)
  server.on("/rs485", HTTP_GET, [](AsyncWebServerRequest *req) {
    extern bool requireAuth(AsyncWebServerRequest*);
    if (!requireAuth(req)) return;
    req->send_P(200, "text/html", RS485_HTML);
  });

  // Register traffic callback with this module
  rs485RegisterTrafficCallback(onRS485Traffic);
}

void rs485RegisterTrafficCallback(RS485TrafficCallback cb) {
  trafficCb = cb;
}

void rs485NotifyRX(const char* msg) {
  if (trafficCb) {
    trafficCb(RS485_DIR_RX, msg);
  }
}

void rs485NotifyTX(const char* msg) {
  if (trafficCb) {
    trafficCb(RS485_DIR_TX, msg);
  }
}