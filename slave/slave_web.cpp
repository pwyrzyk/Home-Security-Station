#include "slave_web.h"
#include "slave_keypad.h"
#include <WiFi.h>
#include <ArduinoJson.h>
#include <EEPROM.h>
#include <Update.h>

// ═══════════════════════════════════════════════════════════════════════════════
// Slave Keypad Web Server — Implementation
// ═══════════════════════════════════════════════════════════════════════════════

// ─── Web server on port 80 ──────────────────────────────────────────────────
static AsyncWebServer webServer(80);

// ─── WebSocket for real-time log streaming ──────────────────────────────────
AsyncWebSocket slaveWebSocket("/ws");

// ─── Runtime slave ID (can be changed via web UI) ───────────────────────────
static uint8_t runtimeSlaveId = 0;

// ─── WiFi STA state ─────────────────────────────────────────────────────────
static bool staConnected = false;
static bool staAttempted = false;

// ─── Log ring buffer ────────────────────────────────────────────────────────
static char logBuffer[WS_LOG_MAX_LINES][96];
static uint16_t logHead = 0;
static uint16_t logCount = 0;

static void logAdd(const char* direction, const char* msg) {
  uint32_t ms = millis() % 86400000;
  uint32_t h  = ms / 3600000;
  uint32_t m  = (ms % 3600000) / 60000;
  uint32_t s  = (ms % 60000) / 1000;
  uint32_t ms2 = ms % 1000;
  snprintf(logBuffer[logHead], sizeof(logBuffer[logHead]),
           "%02u:%02u:%02u.%03u [%s] %s", h, m, s, ms2, direction, msg);
  logHead = (logHead + 1) % WS_LOG_MAX_LINES;
  if (logCount < WS_LOG_MAX_LINES) logCount++;
}

static void wsBroadcastLog(const char* line) {
  if (slaveWebSocket.count() == 0) return;
  slaveWebSocket.textAll(line);
}

// ─── EEPROM WiFi credential helpers ─────────────────────────────────────────
static void eepromLoadWiFiCreds(char* ssid, size_t ssidLen, char* pass, size_t passLen) {
  uint8_t magic = EEPROM.read(EEPROM_WIFI_ADDR);
  if (magic != EEPROM_WIFI_MAGIC) {
    ssid[0] = '\0';
    pass[0] = '\0';
    return;
  }
  for (size_t i = 0; i < 32; i++) {
    ssid[i] = (char)EEPROM.read(EEPROM_WIFI_ADDR + 1 + i);
  }
  ssid[31] = '\0';
  for (size_t i = 0; i < 64; i++) {
    pass[i] = (char)EEPROM.read(EEPROM_WIFI_ADDR + 1 + 32 + i);
  }
  pass[63] = '\0';
}

static void eepromSaveWiFiCreds(const char* ssid, const char* pass) {
  EEPROM.write(EEPROM_WIFI_ADDR, EEPROM_WIFI_MAGIC);
  for (size_t i = 0; i < 32; i++) {
    EEPROM.write(EEPROM_WIFI_ADDR + 1 + i, (i < strlen(ssid)) ? ssid[i] : 0);
  }
  for (size_t i = 0; i < 64; i++) {
    EEPROM.write(EEPROM_WIFI_ADDR + 1 + 32 + i, (i < strlen(pass)) ? pass[i] : 0);
  }
  EEPROM.commit();
}

// ─── Shared CSS (included in both pages via <link>) ─────────────────────────
static const char SHARED_CSS[] PROGMEM = R"rawliteral(
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body {
    font-family: 'Courier New', monospace;
    background: #1a1a2e;
    color: #e0e0e0;
    padding: 12px;
    min-height: 100vh;
  }
  h1 { text-align: center; color: #00d4ff; font-size: 1.2em; margin-bottom: 12px; }
  .container { max-width: 520px; margin: 0 auto; display: flex; flex-direction: column; gap: 10px; }
  .panel {
    background: #16213e;
    border: 1px solid #0f3460;
    border-radius: 6px;
    padding: 10px;
  }
  .panel h2 {
    color: #00d4ff;
    font-size: 0.9em;
    margin-bottom: 8px;
    border-bottom: 1px solid #0f3460;
    padding-bottom: 6px;
    display: flex;
    justify-content: space-between;
    align-items: center;
  }
  .lcd-screen {
    background: #0a2a0a;
    border: 2px solid #1a4a1a;
    border-radius: 4px;
    padding: 8px 10px;
    margin: 0 auto;
    max-width: 400px;
    box-shadow: 0 0 8px rgba(0,255,136,0.15), inset 0 0 20px rgba(0,0,0,0.5);
  }
  .lcd-row {
    font-family: 'Courier New', monospace;
    font-size: 1em;
    color: #00ff88;
    letter-spacing: 1px;
    line-height: 1.5;
    height: 1.5em;
    white-space: pre;
    text-shadow: 0 0 4px rgba(0,255,136,0.4);
  }
  .lcd-row.dim { opacity: 0.65; }
  #log {
    background: #0d1117;
    border: 1px solid #30363d;
    border-radius: 4px;
    padding: 8px;
    height: 200px;
    overflow-y: auto;
    font-size: 0.75em;
    line-height: 1.4;
    white-space: pre-wrap;
    word-break: break-all;
  }
  #log .rx { color: #58a6ff; }
  #log .tx { color: #3fb950; }
  #log .wx { color: #d29922; }
  .status { font-size: 0.7em; color: #8b949e; text-align: center; }
  .status .ok { color: #3fb950; }
  .status .err { color: #f85149; }
  #clearBtn {
    padding: 4px 10px; font-size: 0.7em; font-family: 'Courier New', monospace;
    background: #484f58; color: #fff; border: none; border-radius: 4px; cursor: pointer;
  }
  #clearBtn:hover { background: #6e7681; }
  .id-row, .wifi-row {
    display: flex; align-items: center; gap: 8px; margin-bottom: 6px;
  }
  .id-row label, .wifi-row label { font-size: 0.85em; color: #8b949e; min-width: 80px; }
  #slaveIdInput, #textInput, #wifiSsid, #wifiPass {
    padding: 8px 12px; font-size: 1em; font-family: 'Courier New', monospace;
    background: #0d1117; border: 1px solid #30363d; border-radius: 4px; color: #e0e0e0;
    outline: none; flex: 1;
  }
  #slaveIdInput:focus, #textInput:focus, #wifiSsid:focus, #wifiPass:focus { border-color: #00d4ff; }
  .btn {
    padding: 8px 16px; font-size: 0.85em; font-family: 'Courier New', monospace;
    background: #238636; color: #fff; border: none; border-radius: 4px; cursor: pointer;
    font-weight: bold;
  }
  .btn:hover { background: #2ea043; }
  .btn-sm { padding: 6px 14px; font-size: 0.75em; }
  .send-row { display: flex; gap: 8px; }
  .keypad { display: grid; grid-template-columns: repeat(4, 1fr); gap: 4px; }
  .keypad button {
    padding: 14px 0; font-size: 1.2em; font-family: 'Courier New', monospace;
    background: #0d1117; border: 1px solid #30363d; border-radius: 4px;
    color: #e0e0e0; cursor: pointer; font-weight: bold;
    transition: background 0.1s;
  }
  .keypad button:hover { background: #1f2a3a; }
  .keypad button:active { background: #00d4ff; color: #000; }
  .keypad button.key-hash { color: #f85149; }
  .keypad button.key-star { color: #d29922; }
  .keypad button.key-mode { color: #00d4ff; }
  #otaProgress {
    width: 100%; height: 14px; margin-top: 8px; border-radius: 4px;
    appearance: none; -webkit-appearance: none; background: #0d1117; border: 1px solid #30363d;
  }
  #otaProgress::-webkit-progress-bar { background: #0d1117; border-radius: 4px; }
  #otaProgress::-webkit-progress-value { background: #238636; border-radius: 4px; }
  #otaProgress::-moz-progress-bar { background: #238636; border-radius: 4px; }
  .nav-bar {
    display: flex; justify-content: center; gap: 12px; margin-bottom: 4px;
  }
  .nav-bar a {
    color: #58a6ff; text-decoration: none; font-size: 0.8em;
    padding: 4px 12px; border: 1px solid #30363d; border-radius: 4px;
  }
  .nav-bar a:hover { background: #1f2a3a; }
  .nav-bar a.active { background: #0f3460; border-color: #00d4ff; }
)rawliteral";

// ═══════════════════════════════════════════════════════════════════════════════
// MAIN Page (/) — Virtual LCD + Virtual Keypad only
// ═══════════════════════════════════════════════════════════════════════════════
const char MAIN_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Keypad Slave</title>
<style>)rawliteral"  // CSS will be served separately
R"rawliteral(</style>
</head>
<body>
<div class="container">
  <h1>&#x1F4E1; Alarm Keypad Slave #<span id="slaveIdDisplay">?</span></h1>

  <!-- Navigation -->
  <div class="nav-bar">
    <a href="/" class="active">Keypad</a>
    <a href="/config">Config</a>
  </div>

  <!-- Virtual LCD Display -->
  <div class="panel">
    <h2>&#x1F4BB; LCD Display</h2>
    <div class="lcd-screen">
      <div class="lcd-row" id="lcdRow0">      DISARMED       </div>
      <div class="lcd-row dim" id="lcdRow1">    Enter PIN:      </div>
      <div class="lcd-row dim" id="lcdRow2">                     </div>
      <div class="lcd-row dim" id="lcdRow3">                     </div>
    </div>
  </div>

  <!-- Virtual 4x4 Keypad -->
  <div class="panel">
    <h2>&#x2328; Virtual Keypad</h2>
    <div class="keypad">
      <button onclick="sendKey('1')">1</button>
      <button onclick="sendKey('2')">2</button>
      <button onclick="sendKey('3')">3</button>
      <button class="key-mode" onclick="sendKey('A')">A</button>
      <button onclick="sendKey('4')">4</button>
      <button onclick="sendKey('5')">5</button>
      <button onclick="sendKey('6')">6</button>
      <button class="key-mode" onclick="sendKey('B')">B</button>
      <button onclick="sendKey('7')">7</button>
      <button onclick="sendKey('8')">8</button>
      <button onclick="sendKey('9')">9</button>
      <button class="key-mode" onclick="sendKey('C')">C</button>
      <button class="key-star" onclick="sendKey('*')">*</button>
      <button onclick="sendKey('0')">0</button>
      <button class="key-hash" onclick="sendKey('#')">#</button>
      <button class="key-mode" onclick="sendKey('D')">D</button>
    </div>
  </div>

  <div class="status">
    WebSocket: <span id="wsStatus" class="err">Disconnected</span>
  </div>
</div>

<script>
  var ws;
  function connect() {
    ws = new WebSocket('ws://' + window.location.host + '/ws');
    ws.onopen = function() {
      document.getElementById('wsStatus').textContent = 'Connected';
      document.getElementById('wsStatus').className = 'ok';
      fetch('/api/status').then(r=>r.json()).then(function(d){
        document.getElementById('slaveIdDisplay').textContent = d.slaveId;
      });
    };
    ws.onclose = function() {
      document.getElementById('wsStatus').textContent = 'Disconnected';
      document.getElementById('wsStatus').className = 'err';
      setTimeout(connect, 2000);
    };
    ws.onmessage = function(evt) {
      var msg = evt.data;
      if (msg.startsWith('[LCD]')) {
        try {
          var d = JSON.parse(msg.substring(5));
          document.getElementById('lcdRow0').textContent = d.r0 || '';
          document.getElementById('lcdRow1').textContent = d.r1 || '';
          document.getElementById('lcdRow2').textContent = d.r2 || '';
          document.getElementById('lcdRow3').textContent = d.r3 || '';
        } catch(e) {}
      }
    };
  }
  function sendKey(k) {
    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify({type:'key', value:k}));
    }
  }
  connect();
</script>
</body>
</html>
)rawliteral";

// ═══════════════════════════════════════════════════════════════════════════════
// CONFIG Page (/config) — WiFi, Slave ID, RS-485 Monitor, Send Key/Command, OTA
// ═══════════════════════════════════════════════════════════════════════════════
const char CONFIG_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Keypad Slave - Config</title>
<style>)rawliteral"  // CSS will be served separately
R"rawliteral(</style>
</head>
<body>
<div class="container">
  <h1>&#x1F4E1; Alarm Keypad Slave #<span id="slaveIdDisplay">?</span></h1>

  <!-- Navigation -->
  <div class="nav-bar">
    <a href="/">Keypad</a>
    <a href="/config" class="active">Config</a>
  </div>

  <!-- WiFi Config Panel -->
  <div class="panel">
    <h2>&#x1F4F6; WiFi Configuration</h2>
    <div class="wifi-row">
      <label>SSID:</label>
      <input type="text" id="wifiSsid" placeholder="WiFi network name" maxlength="31">
    </div>
    <div class="wifi-row">
      <label>Password:</label>
      <input type="password" id="wifiPass" placeholder="WiFi password" maxlength="63">
    </div>
    <div style="display:flex;align-items:center;gap:8px;margin-top:6px">
      <button class="btn btn-sm" onclick="saveWiFi()">SAVE & CONNECT</button>
      <span class="status" id="wifiStatus"></span>
    </div>
    <div class="status" style="margin-top:4px">WiFi info: <span id="wifiInfo">---</span></div>
  </div>

  <!-- Slave ID Panel -->
  <div class="panel">
    <h2>&#x2699; Slave Configuration</h2>
    <div class="id-row">
      <label>Slave ID (1-4):</label>
      <input type="number" id="slaveIdInput" min="1" max="4" value="1" style="width:70px">
      <button class="btn btn-sm" onclick="setSlaveId()">SET</button>
      <span class="status" id="idStatus"></span>
    </div>
  </div>

  <!-- RS-485 Log Monitor -->
  <div class="panel">
    <h2>RS-485 Monitor <button id="clearBtn" onclick="clearLog()">Clear</button></h2>
    <div id="log"></div>
  </div>

  <!-- Text Input -->
  <div class="panel">
    <h2>&#x2328; Send Key / Command</h2>
    <div class="send-row">
      <input type="text" id="textInput" placeholder="Type key or command..." maxlength="20" onkeydown="if(event.key==='Enter')sendText()">
      <button class="btn btn-sm" onclick="sendText()">Send</button>
    </div>
    <div class="status" style="margin-top:4px">Type single key (e.g. 1, A, #) or multi-char PIN</div>
  </div>

  <!-- OTA Firmware Upload Panel -->
  <div class="panel">
    <h2>&#x1F4E4; Firmware Update (OTA)</h2>
    <div style="display:flex;align-items:center;gap:8px;flex-wrap:wrap">
      <input type="file" id="otaFile" accept=".bin" style="flex:1;color:#e0e0e0;font-size:0.8em">
      <button class="btn btn-sm" onclick="uploadFirmware()">UPLOAD</button>
    </div>
    <progress id="otaProgress" value="0" max="100"></progress>
    <div class="status" id="otaStatus"></div>
  </div>

  <div class="status">
    WebSocket: <span id="wsStatus" class="err">Disconnected</span>
    &nbsp;|&nbsp; <span id="msgCount">0</span> messages
  </div>
</div>

<script>
  var ws;
  var logDiv = document.getElementById('log');
  var msgCount = 0;

  function connect() {
    ws = new WebSocket('ws://' + window.location.host + '/ws');
    ws.onopen = function() {
      document.getElementById('wsStatus').textContent = 'Connected';
      document.getElementById('wsStatus').className = 'ok';
      fetch('/api/status').then(r=>r.json()).then(function(d){
        document.getElementById('slaveIdDisplay').textContent = d.slaveId;
        document.getElementById('slaveIdInput').value = d.slaveId;
        document.getElementById('wifiInfo').textContent = d.wifiMode + ' | ' + d.wifiIp;
      });
    };
    ws.onclose = function() {
      document.getElementById('wsStatus').textContent = 'Disconnected';
      document.getElementById('wsStatus').className = 'err';
      setTimeout(connect, 2000);
    };
    ws.onmessage = function(evt) {
      var msg = evt.data;
      if (msg.startsWith('[LOG]')) {
        msg = msg.substring(5);
        var span = document.createElement('span');
        if (msg.indexOf('[TX]') >= 0) span.className = 'tx';
        else if (msg.indexOf('[RX]') >= 0) span.className = 'rx';
        else if (msg.indexOf('[WEB]') >= 0) span.className = 'wx';
        span.textContent = msg + '\n';
        logDiv.appendChild(span);
        msgCount++;
        document.getElementById('msgCount').textContent = msgCount;
        logDiv.scrollTop = logDiv.scrollHeight;
        while (logDiv.childElementCount > 500) logDiv.removeChild(logDiv.firstChild);
      }
      if (msg.startsWith('[WIFI]')) {
        document.getElementById('wifiStatus').textContent = msg.substring(6);
        document.getElementById('wifiStatus').className = 'ok';
        setTimeout(function(){ document.getElementById('wifiStatus').textContent = ''; }, 5000);
        fetch('/api/status').then(r=>r.json()).then(function(d){
          document.getElementById('wifiInfo').textContent = d.wifiMode + ' | ' + d.wifiIp;
        });
      }
    };
  }

  function sendText() {
    var inp = document.getElementById('textInput');
    var txt = inp.value.trim();
    if (!txt) return;
    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify({type:'text', value:txt}));
    }
    inp.value = '';
  }

  function saveWiFi() {
    var ssid = document.getElementById('wifiSsid').value.trim();
    var pass = document.getElementById('wifiPass').value.trim();
    if (!ssid) { alert('Enter SSID'); return; }
    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify({type:'wifi_save', ssid:ssid, pass:pass}));
    }
    document.getElementById('wifiStatus').textContent = 'Saving...';
    document.getElementById('wifiStatus').className = 'ok';
  }

  function setSlaveId() {
    var v = parseInt(document.getElementById('slaveIdInput').value);
    if (v < 1 || v > 4) { document.getElementById('idStatus').textContent = 'Invalid'; return; }
    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify({type:'slave_id', value:v}));
    }
    document.getElementById('slaveIdDisplay').textContent = v;
    document.getElementById('idStatus').textContent = 'OK';
    document.getElementById('idStatus').className = 'ok';
    setTimeout(function(){ document.getElementById('idStatus').textContent = ''; }, 2000);
  }

  function uploadFirmware() {
    var fileInput = document.getElementById('otaFile');
    var file = fileInput.files[0];
    if (!file) { alert('Select a firmware.bin file'); return; }
    if (!confirm('Upload firmware and restart device?')) return;

    var formData = new FormData();
    formData.append('firmware', file);

    var xhr = new XMLHttpRequest();
    xhr.open('POST', '/api/ota', true);
    xhr.upload.onprogress = function(e) {
      if (e.lengthComputable) {
        var pct = Math.round((e.loaded / e.total) * 100);
        document.getElementById('otaProgress').value = pct;
        document.getElementById('otaStatus').textContent = 'Uploading: ' + pct + '%';
      }
    };
    xhr.onload = function() {
      if (xhr.status === 200) {
        document.getElementById('otaStatus').textContent = 'OK - Restarting...';
        document.getElementById('otaProgress').value = 100;
      } else {
        document.getElementById('otaStatus').textContent = 'Error: ' + xhr.responseText;
      }
    };
    xhr.onerror = function() {
      document.getElementById('otaStatus').textContent = 'Upload failed';
    };
    xhr.send(formData);
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
// OTA firmware upload handler
// ═══════════════════════════════════════════════════════════════════════════════

static void handleOTAUpload(AsyncWebServerRequest *req, String filename, size_t index, uint8_t *data, size_t len, bool final) {
  if (!index) {
    Serial.printf("[OTA] Upload starting: %s (%u bytes)\n", filename.c_str(), req->contentLength());
    logAdd("WEB", "OTA upload started");
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
      Update.printError(Serial);
      req->send(500, "text/plain", "OTA begin failed");
      return;
    }
  }

  if (Update.write(data, len) != len) {
    Update.printError(Serial);
    req->send(500, "text/plain", "OTA write failed");
    return;
  }

  if (final) {
    if (Update.end(true)) {
      Serial.println("[OTA] Upload complete, restarting...");
      logAdd("WEB", "OTA successful - restarting");
      req->send(200, "text/plain", "OK");
      delay(500);
      ESP.restart();
    } else {
      Update.printError(Serial);
      req->send(500, "text/plain", "OTA end failed");
    }
  }
}

// ─── CSS route (serves shared CSS to both pages) ────────────────────────────
static void injectCss(String& html, const __FlashStringHelper* css) {
  html.replace(F("<style></style>"), String(F("<style>")) + String(css) + F("</style>"));
}

// ═══════════════════════════════════════════════════════════════════════════════
// WebSocket event handler
// ═══════════════════════════════════════════════════════════════════════════════

static void onWsEvent(AsyncWebSocket* server, AsyncWebSocketClient* client,
                      AwsEventType type, void* arg, uint8_t* data, size_t len) {
  switch (type) {
    case WS_EVT_CONNECT: {
      uint16_t total = logCount;
      uint16_t start = (logCount < WS_LOG_MAX_LINES) ? 0 : logHead;
      for (uint16_t i = 0; i < total; i++) {
        uint16_t idx = (start + i) % WS_LOG_MAX_LINES;
        client->text(logBuffer[idx]);
      }
      client->text("[INFO] Connected — " + String(total) + " log lines buffered");
      break;
    }

    case WS_EVT_DATA: {
      AwsFrameInfo* info = (AwsFrameInfo*)arg;
      if (info->final && info->index == 0 && info->len == len &&
          info->opcode == WS_TEXT) {
        data[len] = '\0';
        String msg = String((char*)data);

        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, msg);
        if (err) return;

        const char* msgType = doc["type"];
        if (!msgType) return;

        if (strcmp(msgType, "key") == 0) {
          const char* keyStr = doc["value"];
          if (keyStr && strlen(keyStr) == 1) {
            extern void slaveInjectKey(char key);
            slaveInjectKey(keyStr[0]);
            char logMsg[64];
            snprintf(logMsg, sizeof(logMsg), "Web key pressed: '%c'", keyStr[0]);
            logAdd("WEB", logMsg);
            wsBroadcastLog(logBuffer[(logHead == 0) ? WS_LOG_MAX_LINES - 1 : logHead - 1]);
          }
        } else if (strcmp(msgType, "text") == 0) {
          const char* text = doc["value"];
          if (text) {
            extern void slaveInjectKey(char key);
            for (size_t i = 0; i < strlen(text); i++) {
              slaveInjectKey(text[i]);
            }
            char logMsg[64];
            snprintf(logMsg, sizeof(logMsg), "Web text sent: '%s' (%d chars)", text, (int)strlen(text));
            logAdd("WEB", logMsg);
            wsBroadcastLog(logBuffer[(logHead == 0) ? WS_LOG_MAX_LINES - 1 : logHead - 1]);
          }
        } else if (strcmp(msgType, "slave_id") == 0) {
          int newId = doc["value"] | 0;
          if (newId >= 1 && newId <= 4) {
            runtimeSlaveId = (uint8_t)newId;
            char logMsg[48];
            snprintf(logMsg, sizeof(logMsg), "Slave ID set to %d (until reboot)", newId);
            logAdd("WEB", logMsg);
            client->text("[LOG]" + String(logBuffer[(logHead == 0) ? WS_LOG_MAX_LINES - 1 : logHead - 1]));
          }
        } else if (strcmp(msgType, "wifi_save") == 0) {
          const char* ssid = doc["ssid"];
          const char* pass = doc["pass"];
          if (ssid && strlen(ssid) > 0) {
            eepromSaveWiFiCreds(ssid, pass ? pass : "");
            char logMsg[64];
            snprintf(logMsg, sizeof(logMsg), "WiFi saved: %s. Connecting...", ssid);
            logAdd("WEB", logMsg);
            wsBroadcastLog(logBuffer[(logHead == 0) ? WS_LOG_MAX_LINES - 1 : logHead - 1]);
            client->text("[WIFI]Connecting to " + String(ssid) + "...");

            WiFi.disconnect(true);
            delay(200);
            WiFi.mode(WIFI_AP_STA);
            WiFi.begin(ssid, pass ? pass : "");

            uint32_t start = millis();
            while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_STA_TIMEOUT_MS) {
              delay(200);
            }

            if (WiFi.status() == WL_CONNECTED) {
              staConnected = true;
              IPAddress ip = WiFi.localIP();
              char ipMsg[64];
              snprintf(ipMsg, sizeof(ipMsg), "Connected! IP: %s", ip.toString().c_str());
              client->text("[WIFI]" + String(ipMsg));
              logAdd("WEB", ipMsg);
              wsBroadcastLog(logBuffer[(logHead == 0) ? WS_LOG_MAX_LINES - 1 : logHead - 1]);
            } else {
              client->text("[WIFI]Connection failed. AP mode active.");
              logAdd("WEB", "WiFi STA connection failed, AP active");
              wsBroadcastLog(logBuffer[(logHead == 0) ? WS_LOG_MAX_LINES - 1 : logHead - 1]);
            }
          }
        }
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

void slaveWebInit(uint8_t slaveId) {
  runtimeSlaveId = slaveId;

  EEPROM.begin(EEPROM_SIZE);

  char storedSsid[33] = "";
  char storedPass[65] = "";
  eepromLoadWiFiCreds(storedSsid, sizeof(storedSsid), storedPass, sizeof(storedPass));

  char apSsid[32];
  snprintf(apSsid, sizeof(apSsid), "%s%d", WIFI_AP_SSID_PREFIX, slaveId);

  if (strlen(storedSsid) > 0) {
    Serial.printf("[WEB] Trying STA: %s\n", storedSsid);
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(apSsid, WIFI_AP_PASSWORD, WIFI_AP_CHANNEL, 0, WIFI_AP_MAX_CLIENTS);
    WiFi.begin(storedSsid, storedPass);

    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_STA_TIMEOUT_MS) {
      delay(200);
    }

    if (WiFi.status() == WL_CONNECTED) {
      staConnected = true;
      Serial.printf("[WEB] STA connected: %s\n", WiFi.localIP().toString().c_str());
      logAdd("WEB", (String("STA connected: ") + WiFi.localIP().toString()).c_str());
    } else {
      Serial.println("[WEB] STA failed, AP only");
      logAdd("WEB", "STA failed, AP only");
    }
    staAttempted = true;
  } else {
    Serial.printf("[WEB] No stored WiFi, AP only: %s\n", apSsid);
    WiFi.mode(WIFI_AP);
    WiFi.softAP(apSsid, WIFI_AP_PASSWORD, WIFI_AP_CHANNEL, 0, WIFI_AP_MAX_CLIENTS);
  }

  IPAddress apIp = WiFi.softAPIP();
  Serial.printf("[WEB] AP IP: %s\n", apIp.toString().c_str());

  slaveWebSocket.onEvent(onWsEvent);
  webServer.addHandler(&slaveWebSocket);

  // ─── Routes ──────────────────────────────────────────────────────────────

  // Main page: virtual LCD + virtual keypad
  webServer.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
    String html = FPSTR(MAIN_HTML);
    injectCss(html, FPSTR(SHARED_CSS));
    req->send(200, "text/html", html);
  });

  // Config page: WiFi, Slave ID, RS-485 Monitor, Send Key/Command, OTA
  webServer.on("/config", HTTP_GET, [](AsyncWebServerRequest* req) {
    String html = FPSTR(CONFIG_HTML);
    injectCss(html, FPSTR(SHARED_CSS));
    req->send(200, "text/html", html);
  });

  webServer.on("/api/status", HTTP_GET, [](AsyncWebServerRequest* req) {
    JsonDocument doc;
    doc["slaveId"] = runtimeSlaveId;
    doc["firmware"] = FIRMWARE_VERSION;
    doc["uptimeMs"] = millis();
    doc["heapFree"] = ESP.getFreeHeap();
    doc["wsClients"] = slaveWebSocket.count();
    doc["wifiMode"] = staConnected ? "STA+AP" : "AP";
    doc["wifiIp"] = staConnected ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
    doc["apSsid"] = String(WIFI_AP_SSID_PREFIX) + String(runtimeSlaveId);
    String json;
    serializeJson(doc, json);
    req->send(200, "application/json", json);
  });

  webServer.on("/api/ota", HTTP_POST,
    [](AsyncWebServerRequest* req) {},
    handleOTAUpload
  );

  webServer.begin();
  Serial.println("[WEB] HTTP server started on port 80");
}

void slaveWebLoop() {
  slaveWebSocket.cleanupClients();

  static uint32_t lastLcdBroadcast = 0;
  if (millis() - lastLcdBroadcast >= 300) {
    lastLcdBroadcast = millis();
    if (slaveWebSocket.count() > 0) {
      char row0[21], row1[21], row2[21], row3[21];
      extern void slaveGetLcdContent(char*,size_t,char*,size_t,char*,size_t,char*,size_t);
      slaveGetLcdContent(row0, sizeof(row0), row1, sizeof(row1),
                         row2, sizeof(row2), row3, sizeof(row3));
      char json[160];
      snprintf(json, sizeof(json),
               "[LCD]{\"r0\":\"%s\",\"r1\":\"%s\",\"r2\":\"%s\",\"r3\":\"%s\"}",
               row0, row1, row2, row3);
      slaveWebSocket.textAll(json);
    }
  }

  static uint32_t lastStaCheck = 0;
  if (staAttempted && millis() - lastStaCheck >= 30000) {
    lastStaCheck = millis();
    if (!staConnected && WiFi.status() == WL_CONNECTED) {
      staConnected = true;
      logAdd("WEB", (String("STA reconnected: ") + WiFi.localIP().toString()).c_str());
      wsBroadcastLog(logBuffer[(logHead == 0) ? WS_LOG_MAX_LINES - 1 : logHead - 1]);
    } else if (staConnected && WiFi.status() != WL_CONNECTED) {
      staConnected = false;
      logAdd("WEB", "STA disconnected");
      wsBroadcastLog(logBuffer[(logHead == 0) ? WS_LOG_MAX_LINES - 1 : logHead - 1]);
    }
  }
}

void slaveWebNotifyTX(const char* msg) {
  logAdd("TX", msg);
  char line[100];
  uint32_t idx = (logHead == 0) ? WS_LOG_MAX_LINES - 1 : logHead - 1;
  snprintf(line, sizeof(line), "[LOG]%s", logBuffer[idx]);
  wsBroadcastLog(line);
}

void slaveWebNotifyRX(const char* msg) {
  logAdd("RX", msg);
  char line[100];
  uint32_t idx = (logHead == 0) ? WS_LOG_MAX_LINES - 1 : logHead - 1;
  snprintf(line, sizeof(line), "[LOG]%s", logBuffer[idx]);
  wsBroadcastLog(line);
}

uint8_t slaveWebGetRuntimeId() {
  return runtimeSlaveId;
}