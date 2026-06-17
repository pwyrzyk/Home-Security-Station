#include "web.h"
#include "sensors.h"
#include "zones.h"
#include "alarm.h"
#include "hardware.h"
#include "mqtt.h"
#include "network.h"
#include <ArduinoJson.h>
#include <Update.h>

AsyncWebServer server(HTTP_PORT);

// ─── JSON API helpers ──────────────────────────────────────────────────────

static void apiStatus(AsyncWebServerRequest *req) {
  JsonDocument doc;
  JsonObject root = doc.to<JsonObject>();

  root["device"]    = deviceId;
  root["firmware"]  = FIRMWARE_VERSION;
  root["mqtt_base"] = mqttBase;
  root["wifi"]     = wifiConnected ? "connected" : (apMode ? "ap" : "disconnected");
  root["rssi"]     = wifiConnected ? WiFi.RSSI() : 0;
  root["apMode"]   = apMode;

  JsonArray zones = root["zones"].to<JsonArray>();
  for (int i = 0; i < MAX_ZONES; i++) {
    JsonObject z = zones.add<JsonObject>();
    z["id"]    = i + 1;
    z["name"]  = config.zones[i].name;
    z["armed"] = zoneStates[i].armed;
    z["state"] = zoneAlarmStateStr(zoneStates[i].alarmState);
    z["label"] = zoneAlarmStateLabel(zoneStates[i].alarmState);
    // Collect associated sensor labels
    String sensList;
    for (int s = 0; s < TOTAL_SENSORS; s++) {
      if (config.sensors[s].type != SENSOR_DISABLED && (config.sensors[s].zoneMask & (1U << i))) {
        if (sensList.length()) sensList += ", ";
        sensList += "T" + String(s + 1);
      }
    }
    for (int s = 0; s < MAX_EXT_SENSORS; s++) {
      if (config.extSensors[s].enabled && (config.extSensors[s].zoneMask & (1U << i))) {
        if (sensList.length()) sensList += ", ";
        sensList += "E" + String(s + 1);
      }
    }
    z["sensors"] = sensList;
  }

  JsonArray sensors = root["sensors"].to<JsonArray>();
  for (int i = 0; i < TOTAL_SENSORS; i++) {
    JsonObject s = sensors.add<JsonObject>();
    s["id"]     = i + 1;
    s["name"]   = config.sensors[i].name;
    s["type"]   = sensorTypeStr(config.sensors[i].type);
    s["state"]  = sensorStateStr(sensorStates[i].state);
    s["raw"]    = sensorStates[i].rawValue;
  }

  JsonArray relays = root["relays"].to<JsonArray>();
  for (int i = 0; i < MAX_RELAYS; i++) {
    JsonObject r = relays.add<JsonObject>();
    r["id"]    = i + 1;
    r["name"]  = config.relays[i].name;
    r["state"] = relayStates[i];
  }

  JsonArray dins = root["din"].to<JsonArray>();
  for (int i = 0; i < MAX_DINPUTS; i++) {
    JsonObject d = dins.add<JsonObject>();
    d["id"]    = i + 1;
    d["state"] = dinputStates[i];
  }

  JsonArray ext = root["ext_sensors"].to<JsonArray>();
  for (int i = 0; i < MAX_EXT_SENSORS; i++) {
    JsonObject e = ext.add<JsonObject>();
    e["id"]       = i + 1;
    e["name"]     = config.extSensors[i].name;
    e["enabled"]  = config.extSensors[i].enabled;
    e["active"]   = extSensorStates[i].active;
    e["zoneMask"] = config.extSensors[i].zoneMask;
  }

  String buf;
  serializeJson(doc, buf);
  req->send(200, "application/json", buf);
}

// ─── Sensors config API ────────────────────────────────────────────────────

static void apiSensorsConfig(AsyncWebServerRequest *req) {
  if (req->method() == HTTP_GET) {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (int i = 0; i < TOTAL_SENSORS; i++) {
      JsonObject s = arr.add<JsonObject>();
      s["id"]         = i + 1;
      s["name"]       = config.sensors[i].name;
      s["type"]       = (int)config.sensors[i].type;
      s["standbyMin"] = config.sensors[i].standbyMin;
      s["standbyMax"] = config.sensors[i].standbyMax;
      s["detectMin"]  = config.sensors[i].detectMin;
      s["detectMax"]  = config.sensors[i].detectMax;
      s["faultMin"]   = config.sensors[i].faultMin;
      s["faultMax"]   = config.sensors[i].faultMax;
      s["invert"]     = config.sensors[i].invert;
      s["debounceMs"] = config.sensors[i].debounceMs;
      s["onDelayMs"]  = config.sensors[i].onDelayMs;
      s["offDelayMs"] = config.sensors[i].offDelayMs;
      s["zoneMask"]   = config.sensors[i].zoneMask;
      s["raw"]        = sensorStates[i].rawValue;
      s["state"]      = sensorStateStr(sensorStates[i].state);
    }
    String buf;
    serializeJson(doc, buf);
    req->send(200, "application/json", buf);
  } else if (req->method() == HTTP_POST) {
    for (int i = 0; i < TOTAL_SENSORS; i++) {
      String prefix = "s" + String(i + 1);
      if (req->hasArg((prefix + "_name").c_str())) {
        String v = req->arg((prefix + "_name").c_str());
        strlcpy(config.sensors[i].name, v.c_str(), sizeof(config.sensors[i].name));
      }
      if (req->hasArg((prefix + "_type").c_str())) {
        config.sensors[i].type = (SensorType)req->arg((prefix + "_type").c_str()).toInt();
      }
      if (req->hasArg((prefix + "_standby_lo").c_str())) {
        config.sensors[i].standbyMin = req->arg((prefix + "_standby_lo").c_str()).toInt();
      }
      if (req->hasArg((prefix + "_standby").c_str())) {
        config.sensors[i].standbyMax = req->arg((prefix + "_standby").c_str()).toInt();
      }
      if (req->hasArg((prefix + "_detect").c_str())) {
        config.sensors[i].detectMin = req->arg((prefix + "_detect").c_str()).toInt();
      }
      if (req->hasArg((prefix + "_detect_hi").c_str())) {
        config.sensors[i].detectMax = req->arg((prefix + "_detect_hi").c_str()).toInt();
      }
      if (req->hasArg((prefix + "_fault").c_str())) {
        config.sensors[i].faultMin = req->arg((prefix + "_fault").c_str()).toInt();
      }
      if (req->hasArg((prefix + "_fault_hi").c_str())) {
        config.sensors[i].faultMax = req->arg((prefix + "_fault_hi").c_str()).toInt();
      }
      if (req->hasArg((prefix + "_invert").c_str())) {
        config.sensors[i].invert = (req->arg((prefix + "_invert").c_str()) == "1");
      }
      if (req->hasArg((prefix + "_debounce").c_str())) {
        config.sensors[i].debounceMs = req->arg((prefix + "_debounce").c_str()).toInt();
      }
      if (req->hasArg((prefix + "_ondelay").c_str())) {
        config.sensors[i].onDelayMs = req->arg((prefix + "_ondelay").c_str()).toInt();
      }
      if (req->hasArg((prefix + "_offdelay").c_str())) {
        config.sensors[i].offDelayMs = req->arg((prefix + "_offdelay").c_str()).toInt();
      }
      if (req->hasArg((prefix + "_zones").c_str())) {
        config.sensors[i].zoneMask = (uint16_t)req->arg((prefix + "_zones").c_str()).toInt();
      }
      if (req->hasArg((prefix + "_enabled").c_str())) {
        if (req->arg((prefix + "_enabled").c_str()) == "0") config.sensors[i].type = SENSOR_DISABLED;
        else if (config.sensors[i].type == SENSOR_DISABLED) config.sensors[i].type = SENSOR_CONTACTRON;
      }
    }
    saveConfig();
    req->send(200, "application/json", "{\"ok\":true,\"saved\":true}");
  }
}

// ─── Network config API ────────────────────────────────────────────────────

static void apiNetworkConfig(AsyncWebServerRequest *req) {
  if (req->method() == HTTP_GET) {
    JsonDocument doc;
    doc["wifiSsid"]   = config.wifiSsid;
    doc["wifiPass"]   = config.wifiPass;
    doc["mqttServer"] = config.mqttServer;
    doc["mqttPort"]   = config.mqttPort;
    doc["mqttUser"]   = config.mqttUser;
    doc["mqttPass"]   = config.mqttPass;
    doc["apSsid"]     = String(AP_SSID_PREFIX) + deviceSuffix;
    String buf;
    serializeJson(doc, buf);
    req->send(200, "application/json", buf);
  } else if (req->method() == HTTP_POST) {
    if (req->hasArg("wifiSsid"))  strlcpy(config.wifiSsid, req->arg("wifiSsid").c_str(), sizeof(config.wifiSsid));
    if (req->hasArg("wifiPass"))  strlcpy(config.wifiPass, req->arg("wifiPass").c_str(), sizeof(config.wifiPass));
    if (req->hasArg("mqttServer")) strlcpy(config.mqttServer, req->arg("mqttServer").c_str(), sizeof(config.mqttServer));
    if (req->hasArg("mqttPort"))  config.mqttPort = req->arg("mqttPort").toInt();
    if (req->hasArg("mqttUser"))  strlcpy(config.mqttUser, req->arg("mqttUser").c_str(), sizeof(config.mqttUser));
    if (req->hasArg("mqttPass"))  strlcpy(config.mqttPass, req->arg("mqttPass").c_str(), sizeof(config.mqttPass));
    saveConfig();
    req->send(200, "application/json", "{\"ok\":true,\"saved\":true}");
  }
}

static void apiRestart(AsyncWebServerRequest *req) {
  req->send(200, "application/json", "{\"ok\":true}");
  delay(100);
  ESP.restart();
}

static void apiReconnect(AsyncWebServerRequest *req) {
  req->send(200, "application/json", "{\"ok\":true}");
  delay(100);
  WiFi.disconnect();
  delay(500);
  ensureWiFiMode();
}

static void apiZoneCommand(AsyncWebServerRequest *req) {
  String path = req->url();
  int lastSlash = path.lastIndexOf('/');
  String action = path.substring(lastSlash + 1);
  int prevSlash = path.lastIndexOf('/', lastSlash - 1);
  int zoneId    = path.substring(prevSlash + 1, lastSlash).toInt();
  if (zoneId >= 1 && zoneId <= MAX_ZONES) {
    if (action == "arm") zoneArm(zoneId);
    if (action == "disarm") zoneDisarm(zoneId);
    if (action == "toggle") zoneToggle(zoneId);
  }
  req->send(200, "application/json", "{\"ok\":true}");
}

static void apiRelayCommand(AsyncWebServerRequest *req) {
  String path = req->url();
  int lastSlash = path.lastIndexOf('/');
  int relayId   = path.substring(lastSlash + 1).toInt();
  String state  = req->arg("state");
  if (relayId >= 1 && relayId <= MAX_RELAYS) {
    bool on = (state == "ON" || state == "1" || state == "true");
    setRelay(relayId - 1, on);
  }
  req->send(200, "application/json", "{\"ok\":true}");
}

// ─── Dashboard HTML ────────────────────────────────────────────────────────

static const char HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Alarm ESP</title><style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:Arial,sans-serif;background:#1a1a2e;color:#eee;min-height:100vh}
nav{display:flex;background:#0f3460;padding:0}
nav button{padding:12px 24px;background:none;border:none;color:#aaa;cursor:pointer;font-size:15px;border-bottom:2px solid transparent}
nav button:hover{color:#fff}nav button.active{color:#e94560;border-bottom-color:#e94560}
.page{display:none;padding:20px;max-width:1100px;margin:0 auto}
.page.active{display:block}h1{color:#e94560;margin-bottom:16px}h2{color:#ccc;margin-bottom:12px}
.card{background:#16213e;border-radius:8px;padding:16px;margin:12px 0}
.zone-grid{display:flex;gap:6px;justify-content:space-between}
.zone-box{width:110px;border-radius:10px;padding:12px 6px;text-align:center;display:flex;flex-direction:column;align-items:center;gap:6px;transition:all 0.3s;flex-shrink:0}
.zone-box.disarmed{border:1px solid #1a1a2e;background:#16213e}
.zone-box.armed_idle{border:1px solid #2ecc71;background:#142820}
.zone-box.prealarm{border:1px solid #f39c12;background:#2d2600}
.zone-box.alarm{border:1px solid #e74c3c;background:#2d1010}
.zone-dot{width:10px;height:10px;border-radius:50%}
.zone-dot.disarmed{background:#666}
.zone-dot.armed_idle{background:#2ecc71}
.zone-dot.prealarm{background:#f39c12}
.zone-dot.alarm{background:#e74c3c}
.zone-name{font-size:11px;font-weight:bold;line-height:1.2}
.zone-label{font-size:10px;color:#aaa}
.zone-label.has-state{color:#2ecc71}
.zone-label.prealarm-state{color:#f39c12}
.zone-label.alarm-state{color:#e74c3c}
.toggle{position:relative;display:inline-block;width:44px;height:24px}
.toggle input{opacity:0;width:0;height:0}
.toggle-slider{position:absolute;cursor:pointer;top:0;left:0;right:0;bottom:0;background:#444;border-radius:24px;transition:0.3s}
.toggle-slider:before{position:absolute;content:"";height:18px;width:18px;left:3px;bottom:3px;background:#eee;border-radius:50%;transition:0.3s}
.toggle input:checked+.toggle-slider{background:#2ecc71}
.toggle input:checked+.toggle-slider:before{transform:translateX(20px)}
.toggle.disarm-slider input+.toggle-slider{background:#555}
.toggle.alarm-slider input+.toggle-slider{background:#e74c3c}
.btn{padding:6px 14px;margin:2px;border:none;border-radius:4px;cursor:pointer;font-size:13px;color:#fff}
.btn-arm{background:#2ecc71}.btn-disarm{background:#e74c3c}
.btn-save{background:#3498db}.btn-danger{background:#c0392b}
.sensor-grid{display:flex;flex-direction:column;gap:6px}
.sensor-row{display:flex;gap:6px;justify-content:space-between}
.sensor-box{width:110px;border-radius:10px;padding:10px 6px;text-align:center;display:flex;flex-direction:column;align-items:center;gap:4px;transition:all 0.3s;flex-shrink:0}
.sensor-box.idle{border:1px solid #1a1a2e;background:#16213e}
.sensor-box.active{border:1px solid #e74c3c;background:#2d1010}
.sensor-box.fault{border:1px solid #f39c12;background:#2d2600}
.sensor-box.disabled{border:1px solid #1a1a2e;background:#0d1020;opacity:0.5}
.sensor-box .sdot{width:10px;height:10px;border-radius:50%}
.sensor-box .sdot.idle{background:#2ecc71}
.sensor-box .sdot.active{background:#e74c3c}
.sensor-box .sdot.fault{background:#f39c12}
.sensor-box .sdot.disabled{background:#444}
.sensor-box .slabel{font-size:11px;font-weight:bold;line-height:1.2}
.sensor-box .sraw{font-size:12px;font-weight:bold}
.sensor-box .sraw.idle{color:#2ecc71}
.sensor-box .sraw.active{color:#e74c3c}
.sensor-box .sraw.fault{color:#f39c12}
.sensor-box .sraw.disabled{color:#666}
.sensor-box .sstate{font-size:10px;color:#aaa}
.sensor-box .sstate.active-state{color:#e74c3c}
.sensor-box .sstate.fault-state{color:#f39c12}
.sensor{display:inline-block;padding:4px 10px;margin:2px;border-radius:4px;font-size:13px}
.sensor.active{background:#e74c3c}.sensor.fault{background:#f39c12}.sensor.idle{background:#2c3e50}
.relay-grid{display:flex;gap:6px;justify-content:center}
.relay-box{width:110px;border-radius:10px;padding:10px 6px;text-align:center;display:flex;flex-direction:column;align-items:center;gap:4px;transition:all 0.3s;flex-shrink:0}
.relay-box.off{border:1px solid #1a1a2e;background:#16213e}
.relay-box.on{border:1px solid #e94560;background:#2d1018}
.relay-box .rdot{width:10px;height:10px;border-radius:50%}
.relay-box .rdot.off{background:#666}
.relay-box .rdot.on{background:#e94560}
.relay-box .rlabel{font-size:11px;font-weight:bold;line-height:1.2}
.relay-box .rstate{font-size:12px;font-weight:bold}
.relay-box .rstate.off{color:#888}
.relay-box .rstate.on{color:#e94560}
label{display:block;margin-top:10px;font-size:13px;color:#aaa}
input,select{width:100%;padding:8px;margin-top:3px;border:1px solid #333;border-radius:4px;background:#0f3460;color:#eee;font-size:14px}
input[type=number]{width:80px;display:inline;margin:0 2px;-moz-appearance:textfield}
input[type=number]::-webkit-outer-spin-button,input[type=number]::-webkit-inner-spin-button{-webkit-appearance:none;margin:0}
input[type=checkbox]{width:16px;height:16px;margin:0 2px;accent-color:#3498db}
.ztoggle{display:inline-flex;align-items:center;margin-left:auto;flex-shrink:0}
.ztoggle input{width:36px;height:20px;margin:0;padding:0;cursor:pointer;accent-color:#2ecc71}
.form-row{display:grid;grid-template-columns:1fr 1fr;gap:12px}
@media(max-width:600px){.form-row{grid-template-columns:1fr}}
small{color:#888}
#sensorCards,#zoneCards,#extCards{display:grid;grid-template-columns:1fr 1fr;gap:8px}
@media(max-width:750px){#sensorCards,#zoneCards,#extCards{grid-template-columns:1fr}}
.sens-card{border-radius:8px;padding:8px 10px;font-size:12px;border:1px solid #1a1a2e;background:#16213e}
.sens-card.idle{border:1px solid #1a1a2e;background:#16213e}
.sens-card.active{border:1px solid #e74c3c;background:#2d1010}
.sens-card.fault{border:1px solid #f39c12;background:#2d2600}
.sens-card.disabled{border:1px solid #1a1a2e;background:#0d1020;opacity:0.5}
.sens-top{display:flex;align-items:center;gap:6px;margin-bottom:4px;flex-wrap:wrap}
.sens-top .sid{color:#e94560;font-weight:bold;font-size:13px;min-width:22px}
.sens-top input[type=text]{width:80px;padding:3px 6px;font-size:12px;margin-top:0}
.sens-top select{width:75px;padding:3px 4px;font-size:11px;margin-top:0}
.sens-live{display:flex;align-items:center;gap:5px;font-size:11px;margin-left:auto}
.sens-live .sdot{width:10px;height:10px;border-radius:50%;flex-shrink:0}
.sdot.idle{background:#2ecc71}.sdot.active{background:#e74c3c}.sdot.fault{background:#f39c12}.sdot.none{background:#444}
.srange-row{display:flex;align-items:center;gap:3px;margin:1px 0;font-size:11px}
.srange-bar{width:6px;height:18px;border-radius:3px;flex-shrink:0}
.srange-bar.standby{background:#2ecc71}
.srange-bar.detected{background:#e74c3c}
.srange-bar.faultbar{background:#f39c12}
.srange-row input[type=number]{width:52px;padding:2px 4px;font-size:11px;margin-top:0}
.srange-label{width:60px;color:#aaa;font-size:10px;text-align:right}
.stiming-row{display:flex;align-items:center;gap:4px;margin:2px 0;font-size:11px;flex-wrap:wrap}
.stiming-row span{color:#aaa;font-size:10px}
.stiming-row input[type=number]{width:46px;padding:2px 4px;font-size:11px;margin-top:0}
.szone-row{display:flex;align-items:center;gap:2px;margin:2px 0;font-size:11px;flex-wrap:wrap}
.szone-row span{color:#aaa;font-size:10px;margin-right:2px}
.szone-row label{display:inline-flex!important;align-items:center;font-size:10px!important;color:#888!important;margin-top:0!important;margin-right:1px}
.szone-row input[type=checkbox]{width:13px;height:13px;margin:0 1px}
.ext-box{width:110px;border-radius:10px;padding:10px 6px;text-align:center;display:flex;flex-direction:column;align-items:center;gap:4px;transition:all 0.3s;flex-shrink:0}
.ext-box.off{border:1px solid #1a1a2e;background:#16213e}
.ext-box.on{border:1px solid #f0a030;background:#2d2010}
.ext-box .edot{width:10px;height:10px;border-radius:50%}
.ext-box .edot.off{background:#666}
.ext-box .edot.on{background:#f0a030}
.ext-box .elabel{font-size:11px;font-weight:bold;line-height:1.2}
.ext-box .estate{font-size:12px;font-weight:bold}
.ext-box .estate.off{color:#888}
.ext-box .estate.on{color:#f0a030}
.sens-foot{display:flex;justify-content:flex-end;margin-top:4px}
.sens-upd{background:#3498db;padding:3px 12px;border:none;border-radius:4px;cursor:pointer;font-size:11px;color:#fff}
.dot.idle{background:#2ecc71}.dot.active{background:#e74c3c}.dot.fault{background:#f39c12}.dot.none{background:#444}
.range-row{display:flex;align-items:center;gap:6px;margin:4px 0;font-size:12px}
.range-bar{width:8px;height:24px;border-radius:4px;flex-shrink:0}
.range-bar.standby{background:#2ecc71}
.range-bar.detected{background:#e74c3c}
.range-bar.faultbar{background:#f39c12}
.range-row input[type=number]{width:62px;padding:3px 6px;font-size:12px}
.range-label{width:70px;color:#aaa;font-size:12px;text-align:right}
.timing-row{display:flex;align-items:center;gap:8px;margin:4px 0;font-size:12px;flex-wrap:wrap}
.timing-row span{color:#aaa}
.timing-row input[type=number]{width:58px;padding:3px 6px;font-size:12px}
.zone-row{display:flex;align-items:center;gap:4px;margin:4px 0;font-size:12px}
.zone-row span{color:#aaa;margin-right:4px}
</style></head>
<body>
<nav>
<button onclick="showTab('dashboard')" id="tab-dashboard" class="active">Dashboard</button>
<button onclick="showTab('sensors')" id="tab-sensors">Sensors</button>
<button onclick="showTab('extsensors')" id="tab-extsensors">External Sensors</button>
<button onclick="showTab('zones')" id="tab-zones">Zones</button>
<button onclick="showTab('config')" id="tab-config">Config</button>
</nav>
<div id="page-dashboard" class="page active"><h1>Alarm ESP</h1>
<div id="sysInfo" style="font-size:13px;color:#888;margin-bottom:12px">Loading...</div>
<div class="card"><h2>Zones</h2><div id="zones">Loading...</div></div>
<div class="card"><h2>Sensors</h2><div id="sensors">Loading...</div></div>
<div class="card"><h2>Relays</h2><div id="relays">Loading...</div></div>
<div class="card"><h2>External Sensors</h2><div id="extensors">Loading...</div></div>
</div>
<div id="page-extsensors" class="page"><h1>External Sensor Configuration</h1>
<div id="extSubtitle" style="font-size:11px;color:#888;margin-bottom:16px">Loading...</div>
<div id="extCards">Loading...</div>
<div style="margin-top:12px">
<button class="btn btn-save" onclick="saveExtSensors()">Save All External</button>
<span id="extMsg" style="font-size:13px;margin-left:12px"></span>
</div>
</div>
<div id="page-zones" class="page"><h1>Zone Configuration</h1>
<div id="zoneCards">Loading...</div>
<div style="margin-top:12px">
<button class="btn btn-save" onclick="saveZones()">Save All Zones</button>
<span id="zoneMsg" style="font-size:13px;margin-left:12px"></span>
</div>
</div>
<div id="page-sensors" class="page"><h1>Sensor Configuration</h1>
<div id="sensorCards">Loading...</div>
<div style="margin-top:12px">
<button class="btn btn-save" onclick="saveSensors()">Save Sensors</button>
<span id="sensMsg" style="font-size:13px;margin-left:12px"></span>
</div>
</div>
<div id="page-config" class="page"><h1>Network Config</h1>
<div class="card"><h2>WiFi</h2>
<div class="form-row"><div><label>SSID</label><input id="cfgSsid"></div>
<div><label>Password</label><input id="cfgPass" type="password"></div></div></div>
<div class="card"><h2>MQTT</h2>
<div class="form-row"><div><label>Server</label><input id="cfgMqttSrv"></div>
<div><label>Port</label><input id="cfgMqttPort" type="number"></div></div>
<div class="form-row"><div><label>User</label><input id="cfgMqttUser"></div>
<div><label>Password</label><input id="cfgMqttPass" type="password"></div></div></div>
<div class="card" id="apInfo" style="display:none"><small>AP Mode active. Connect to <strong id="apSsid"></strong> (password: 12345678) then open http://192.168.4.1</small></div>
<div class="card"><h2>Firmware Update</h2>
<div style="margin:8px 0"><small>Select a compiled firmware.bin file to upload via OTA. The device will restart automatically after a successful update.</small></div>
<div style="display:flex;align-items:center;gap:8px;flex-wrap:wrap">
<input type="file" id="otaFile" accept=".bin" style="flex:1;min-width:200px">
<button class="btn btn-save" onclick="uploadFirmware()">Upload & Flash</button>
</div>
<div id="otaMsg" style="margin-top:8px;font-size:13px"></div>
</div>
<div style="margin-top:12px;display:flex;gap:8px">
<button class="btn btn-save" onclick="saveConfig()">Save Config</button>
<button class="btn btn-save" onclick="reconnect()">Reconnect WiFi</button>
<button class="btn btn-danger" onclick="restart()">Restart</button></div>
<div id="cfgMsg" style="margin-top:8px;font-size:13px"></div></div>
<script>
let data={};

function rangeClass(raw, lo, hi){
  if(raw>=lo && raw<=hi) return 'in-range';
  return '';
}

function stateColor(state){ return state=='active'?'#e74c3c':state=='fault'?'#f39c12':'#2ecc71'; }
function dotClass(state){ return state=='active'?'active':state=='fault'?'fault':'idle'; }

function cardClass(s){
  if(s.type==0) return 'disabled';
  return s.state;
}

function rangeBar(raw, lo, hi, cls){
  // Show colored bar only if raw falls within this range
  if(raw>=lo && raw<=hi && (lo>0 || hi<65535)) return '<span class="range-bar '+cls+'"></span>';
  if(lo===0 && hi===65535 && cls==='standby') return '<span class="range-bar standby"></span>';
  return '<span class="range-bar" style="background:#222"></span>';
}

async function load(){
  const r=await fetch('/api/status');
  data=await r.json();
  document.getElementById('sysInfo').textContent=
    data.device+' | '+data.firmware+' | WiFi: '+data.wifi+' | RSSI: '+data.rssi+' dBm';
  renderZones(data.zones);
  renderSensors(data.sensors);
  renderRelays(data.relays);
  renderExtSensors(data.ext_sensors);
  if(data.apMode) document.getElementById('apInfo').style.display='block';
}

function renderExtSensors(a){
  let h='';
  for(let row=0;row<2;row++){
    h+='<div class="sensor-row">';
    for(let i=row*8;i<Math.min(row*8+8,a.length);i++){
      let e=a[i];
      let on=e.active;
      h+=`<div class="ext-box ${on?'on':'off'}">
<span class="edot ${on?'on':'off'}"></span>
<span class="elabel">E${e.id}</span>
<span class="estate ${on?'on':'off'}">${on?'ON':'OFF'}</span>
</div>`;
    }
    h+='</div>';
  }
  document.getElementById('extensors').innerHTML=h;
}

function renderRelays(a){
  let h='<div class="relay-grid">';
  a.forEach(r=>{
    let on=r.state;
    h+=`<div class="relay-box ${on?'on':'off'}">
<span class="rdot ${on?'on':'off'}"></span>
<span class="rlabel">${r.name}</span>
<span class="rstate ${on?'on':'off'}">${on?'ON':'OFF'}</span>
</div>`;
  });
  h+='</div>';
  document.getElementById('relays').innerHTML=h;
}

function renderZones(a){
  let h='<div class="zone-grid">';
  a.forEach(z=>{
    let s=z.state;
    let armed=(s!=='disarmed');
    let toggleCls=(s==='alarm')?'alarm-slider':((!armed)?'disarm-slider':'');
    let labelCls=(s==='armed_idle')?'has-state':((s==='prealarm')?'prealarm-state':((s==='alarm')?'alarm-state':''));
    let icon=(s==='alarm')?'🚨':(s==='prealarm')?'⏳':(armed?'🔒':'');
    let sensors=z.sensors||'';
    let sensHtml=sensors?'<span style="font-size:9px;color:#666;line-height:1.2">'+sensors+'</span>':'';
    h+='<div class="zone-box '+s+'">'+
'<span class="zone-dot '+s+'"></span>'+
'<span class="zone-name">'+icon+' '+z.name+'</span>'+
'<span class="zone-label '+labelCls+'">'+z.label+'</span>'+
sensHtml+
'<label class="toggle '+toggleCls+'">'+
'<input type="checkbox" '+(armed?'checked':'')+' onchange="cmd(\'/api/zone/'+z.id+'/'+(armed?'disarm':'arm')+'\')">'+
'<span class="toggle-slider"></span>'+
'</label>'+
'</div>';
  });
  h+='</div>';
  document.getElementById('zones').innerHTML=h;
}

function renderSensors(a){
  let h='<div class="sensor-grid">';
  for(let row=0;row<2;row++){
    h+='<div class="sensor-row">';
    for(let i=row*8;i<Math.min(row*8+8,a.length);i++){
      let s=a[i];
      let st=s.type==0?'disabled':s.state;
      let rawStr=s.type==0?'---':(s.raw+' mV');
      let stateLabel=s.type==0?'off':s.state;
      let stateCls=(st==='active')?'active-state':((st==='fault')?'fault-state':'');
      h+=`<div class="sensor-box ${st}">
<span class="sdot ${st}"></span>
<span class="slabel">T${s.id}</span>
<span class="sraw ${st}">${rawStr}</span>
<span class="sstate ${stateCls}">${stateLabel}</span>
</div>`;
    }
    h+='</div>';
  }
  h+='</div>';
  document.getElementById('sensors').innerHTML=h;
}

function srangeBar(raw,lo,hi,cls){
  if(raw>=lo && raw<=hi && (lo>0 || hi<65535)) return '<span class="srange-bar '+cls+'"></span>';
  if(lo===0 && hi===65535 && cls==='standby') return '<span class="srange-bar standby"></span>';
  return '<span class="srange-bar" style="background:#222"></span>';
}

function renderSensorCards(a){
  let h='';
  a.forEach(s=>{
    let cls=cardClass(s);
    let standbyLo=s.standbyMin||0;
    let standbyHi=s.standbyMax||2000;
    let detectLo=s.detectMin||8000;
    let detectHi=(s.detectMax===65535||s.detectMax===0)?'max':s.detectMax;
    let faultLo=s.faultMin||30000;
    let faultHi=(s.faultMax===65535||s.faultMax===0)?'max':s.faultMax;
    h+=`<div class="sens-card ${cls}" id="card_${s.id}">
<div class="sens-top">
<span class="sid">T${s.id}</span>
<input type="text" id="s${s.id}_name" value="${s.name||''}" placeholder="Name">
<select id="s${s.id}_type">
<option value="0" ${s.type==0?'selected':''}>off</option>
<option value="1" ${s.type==1?'selected':''}>PIR</option>
<option value="2" ${s.type==2?'selected':''}>Contact</option>
</select>
<div class="sens-live">
<span class="sdot ${s.type==0?'none':dotClass(s.state)}" id="dot_${s.id}"></span>
<span id="state_${s.id}">${s.type==0?'disabled':s.state}</span>
<span id="raw_${s.id}" style="color:${s.type==0?'#888':stateColor(s.state)}">${s.raw} mV</span>
</div>
</div>
<div class="srange-row">
<span id="bar_sb_${s.id}">${srangeBar(s.raw,standbyLo,standbyHi,'standby')}</span>
<span class="srange-label">Standby:</span>
<input type="number" id="s${s.id}_standby_lo" value="${standbyLo}" placeholder="0" min="0" max="65535">
<span>–</span>
<input type="number" id="s${s.id}_standby" value="${standbyHi}" placeholder="2000" min="0" max="65535">
<span>mV</span>
</div>
<div class="srange-row">
<span id="bar_dt_${s.id}">${srangeBar(s.raw,detectLo,typeof detectHi==='number'?detectHi:65535,'detected')}</span>
<span class="srange-label">Detected:</span>
<input type="number" id="s${s.id}_detect" value="${detectLo}" placeholder="8000" min="0" max="65535">
<span>–</span>
<input type="number" id="s${s.id}_detect_hi" value="${detectHi==='max'?'65535':detectHi}" placeholder="max" min="0" max="65535">
<span>mV</span>
</div>
<div class="srange-row">
<span id="bar_ft_${s.id}">${srangeBar(s.raw,faultLo,typeof faultHi==='number'?faultHi:65535,'faultbar')}</span>
<span class="srange-label">Fault:</span>
<input type="number" id="s${s.id}_fault" value="${faultLo}" placeholder="30000" min="0" max="65535">
<span>–</span>
<input type="number" id="s${s.id}_fault_hi" value="${faultHi==='max'?'65535':faultHi}" placeholder="max" min="0" max="65535">
<span>mV</span>
</div>
<div class="stiming-row">
<span>Db:</span><input type="number" id="s${s.id}_debounce" value="${s.debounceMs}" min="0" max="5000"><span>ms</span>
<span>On:</span><input type="number" id="s${s.id}_ondelay" value="${s.onDelayMs}" min="0" max="60000"><span>ms</span>
<span>Off:</span><input type="number" id="s${s.id}_offdelay" value="${s.offDelayMs}" min="0" max="60000"><span>ms</span>
</div>
<div class="szone-row">
<span>Zones:</span>`;
    for(let z=1;z<=8;z++) h+=`<label><input type="checkbox" id="s${s.id}_z${z}" ${(s.zoneMask&(1<<(z-1)))?'checked':''}>Z${z}</label>`;
    h+=`</div>
<div class="sens-foot"><button class="sens-upd" onclick="saveSensors()">Update</button></div>
</div>`;
  });
  document.getElementById('sensorCards').innerHTML=h;
}

async function renderZoneCards(a){
  let h='';
  a.forEach(z=>{
    let sensors=z.sensors||'';
    h+=`<div class="sens-card">
<div class="sens-top">
<span class="sid">Z${z.id}</span>
<input type="text" id="z${z.id}_name" value="${z.name||''}" placeholder="Name">
<span class="ztoggle"><input type="checkbox" id="z${z.id}_enabled" ${z.enabled?'checked':''}><span class="toggle-on"></span></span>
</div>
<div class="stiming-row">
<span>Exit:</span><input type="number" id="z${z.id}_exit" value="${z.exitDelayS}" min="0" max="120"><span>s</span>
<span>Entry:</span><input type="number" id="z${z.id}_entry" value="${z.entryDelayS}" min="0" max="120"><span>s</span>
</div>
${sensors?`<div style="font-size:10px;color:#888;margin:2px 0">Sensors: ${sensors}</div>`:''}
<div class="sens-foot"><button class="sens-upd" onclick="saveZones()">Update</button></div>
</div>`;
  });
  document.getElementById('zoneCards').innerHTML=h;
}
async function loadZones(){const r=await fetch('/api/zones');renderZoneCards(await r.json());}
async function saveZones(){
  const body=new URLSearchParams();
  for(let i=1;i<=8;i++){
    body.set('z'+i+'_name',document.getElementById('z'+i+'_name')?.value||'');
    body.set('z'+i+'_exit',document.getElementById('z'+i+'_exit')?.value||'5');
    body.set('z'+i+'_entry',document.getElementById('z'+i+'_entry')?.value||'10');
    body.set('z'+i+'_enabled',document.getElementById('z'+i+'_enabled')?.checked?'1':'0');
  }
  const r=await fetch('/api/zones',{method:'POST',body});
  const d=await r.json();
  document.getElementById('zoneMsg').textContent=d.saved?'Saved.':d.error||'Error.';
  loadZones();
}

async function renderExtCards(a){
  let h='';
  a.forEach(e=>{
    h+=`<div class="sens-card">
<div class="sens-top">
<span class="sid">E${e.id}</span>
<input type="text" id="e${e.id}_name" value="${e.name||''}" placeholder="Name">
<span class="ztoggle"><input type="checkbox" id="e${e.id}_enabled" ${e.enabled?'checked':''}><span class="toggle-on"></span></span>
</div>
<div class="szone-row"><span>Zones:</span>`;
    for(let z=1;z<=8;z++) h+=`<label><input type="checkbox" id="e${e.id}_z${z}" ${(e.zoneMask&(1<<(z-1)))?'checked':''}>Z${z}</label>`;
    h+=`</div>
<div class="sens-foot"><button class="sens-upd" onclick="saveExtSensors()">Update</button></div>
</div>`;
  });
  document.getElementById('extCards').innerHTML=h;
}
async function loadExtSensors(){
  const r=await fetch('/api/extsensors');
  renderExtCards(await r.json());
  if(data.mqtt_base) document.getElementById('extSubtitle').textContent='Topic: '+data.mqtt_base+'/ext_sensor/1..16 | Payload: active/idle (on/off)';
}
async function saveExtSensors(){
  const body=new URLSearchParams();
  for(let i=1;i<=16;i++){
    body.set('e'+i+'_name',document.getElementById('e'+i+'_name')?.value||'');
    body.set('e'+i+'_enabled',document.getElementById('e'+i+'_enabled')?.checked?'1':'0');
    let zones=0;
    for(let z=1;z<=8;z++) if(document.getElementById('e'+i+'_z'+z)?.checked) zones|=(1<<(z-1));
    body.set('e'+i+'_zones',zones);
  }
  const r=await fetch('/api/extsensors',{method:'POST',body});
  const d=await r.json();
  document.getElementById('extMsg').textContent=d.saved?'Saved.':d.error||'Error.';
  loadExtSensors();
}

async function loadSensors(){const r=await fetch('/api/sensors');renderSensorCards(await r.json());}

let sensorRefreshTimer=null;
async function refreshSensorLive(){
  const r=await fetch('/api/sensors');
  const a=await r.json();
  a.forEach(s=>{
    let card=document.getElementById('card_'+s.id);
    if(!card) return;
    let cls=s.type==0?'disabled':s.state;
    card.className='sens-card '+cls;
    let dot=document.getElementById('dot_'+s.id);
    if(dot){ dot.className='sdot '+(s.type==0?'none':dotClass(s.state)); }
    let st=document.getElementById('state_'+s.id);
    if(st){ st.textContent=s.type==0?'disabled':s.state; }
    let rw=document.getElementById('raw_'+s.id);
    if(rw){ rw.textContent=s.raw+' mV'; rw.style.color=s.type==0?'#888':stateColor(s.state); }
    let sLo=s.standbyMin||0, sHi=s.standbyMax||2000;
    let dLo=s.detectMin||8000, dHi=(s.detectMax===65535||s.detectMax===0)?65535:s.detectMax;
    let fLo=s.faultMin||30000, fHi=(s.faultMax===65535||s.faultMax===0)?65535:s.faultMax;
    let u=document.getElementById('bar_sb_'+s.id);
    if(u) u.innerHTML=srangeBar(s.raw,sLo,sHi,'standby');
    u=document.getElementById('bar_dt_'+s.id);
    if(u) u.innerHTML=srangeBar(s.raw,dLo,dHi,'detected');
    u=document.getElementById('bar_ft_'+s.id);
    if(u) u.innerHTML=srangeBar(s.raw,fLo,fHi,'faultbar');
  });
}
async function saveSensors(){
  const body=new URLSearchParams();
  for(let i=1;i<=16;i++){
    body.set('s'+i+'_name',document.getElementById('s'+i+'_name')?.value||'');
    body.set('s'+i+'_type',document.getElementById('s'+i+'_type')?.value||'0');
    body.set('s'+i+'_standby_lo',document.getElementById('s'+i+'_standby_lo')?.value||'0');
    body.set('s'+i+'_standby',document.getElementById('s'+i+'_standby')?.value||'2000');
    body.set('s'+i+'_detect',document.getElementById('s'+i+'_detect')?.value||'8000');
    body.set('s'+i+'_detect_hi',document.getElementById('s'+i+'_detect_hi')?.value||'65535');
    body.set('s'+i+'_fault',document.getElementById('s'+i+'_fault')?.value||'30000');
    body.set('s'+i+'_fault_hi',document.getElementById('s'+i+'_fault_hi')?.value||'65535');
    body.set('s'+i+'_debounce',document.getElementById('s'+i+'_debounce')?.value||'100');
    body.set('s'+i+'_ondelay',document.getElementById('s'+i+'_ondelay')?.value||'200');
    body.set('s'+i+'_offdelay',document.getElementById('s'+i+'_offdelay')?.value||'500');
    let zones=0;
    for(let z=1;z<=8;z++) if(document.getElementById('s'+i+'_z'+z)?.checked) zones|=(1<<(z-1));
    body.set('s'+i+'_zones',zones);
    body.set('s'+i+'_enabled',document.getElementById('s'+i+'_type')?.value!='0'?'1':'0');
  }
  const r=await fetch('/api/sensors',{method:'POST',body});
  const d=await r.json();
  document.getElementById('sensMsg').textContent=d.saved?'Saved.':d.error||'Error.';
  loadSensors();
}
async function loadNetCfg(){
  const r=await fetch('/api/network');
  const d=await r.json();
  document.getElementById('cfgSsid').value=d.wifiSsid||'';
  document.getElementById('cfgPass').value=d.wifiPass||'';
  document.getElementById('cfgMqttSrv').value=d.mqttServer||'';
  document.getElementById('cfgMqttPort').value=d.mqttPort||1883;
  document.getElementById('cfgMqttUser').value=d.mqttUser||'';
  document.getElementById('cfgMqttPass').value=d.mqttPass||'';
  document.getElementById('apSsid').textContent=d.apSsid||'';
}
async function saveConfig(){
  const body=new URLSearchParams();
  body.set('wifiSsid',document.getElementById('cfgSsid').value);
  body.set('wifiPass',document.getElementById('cfgPass').value);
  body.set('mqttServer',document.getElementById('cfgMqttSrv').value);
  body.set('mqttPort',document.getElementById('cfgMqttPort').value);
  body.set('mqttUser',document.getElementById('cfgMqttUser').value);
  body.set('mqttPass',document.getElementById('cfgMqttPass').value);
  const r=await fetch('/api/network',{method:'POST',body});
  const d=await r.json();
  document.getElementById('cfgMsg').textContent=d.saved?'Config saved.':'Error.';
}
function showTab(t){
  if(sensorRefreshTimer){clearInterval(sensorRefreshTimer);sensorRefreshTimer=null;}
  document.querySelectorAll('.page').forEach(p=>p.classList.remove('active'));
  document.querySelectorAll('nav button').forEach(b=>b.classList.remove('active'));
  document.getElementById('page-'+t).classList.add('active');
  document.getElementById('tab-'+t).classList.add('active');
  if(t=='config')loadNetCfg();
  if(t=='sensors'){loadSensors();sensorRefreshTimer=setInterval(refreshSensorLive,2000);}
  if(t=='zones')loadZones();
  if(t=='extsensors')loadExtSensors();
}
async function uploadFirmware(){
  const file=document.getElementById('otaFile').files[0];
  if(!file){ document.getElementById('otaMsg').textContent='Please select a .bin file.'; return; }
  if(!file.name.endsWith('.bin')){ document.getElementById('otaMsg').textContent='File must be a .bin firmware image.'; return; }
  document.getElementById('otaMsg').textContent='Uploading... '+Math.round(file.size/1024)+' kB';
  const r=await fetch('/api/ota',{method:'POST',body:file});
  if(r.ok){
    document.getElementById('otaMsg').textContent='Firmware flashed successfully. Device is restarting...';
  } else {
    document.getElementById('otaMsg').textContent='Upload failed: '+(await r.text());
  }
}
async function reconnect(){await fetch('/api/reconnect');load();}
async function restart(){if(confirm('Restart?'))await fetch('/api/restart');}
async function cmd(url){await fetch(url);load();}
load();setInterval(load,3000);
</script></body></html>)rawliteral";

// ─── External sensors config API ───────────────────────────────────────────

static void apiExtSensorsConfig(AsyncWebServerRequest *req) {
  if (req->method() == HTTP_GET) {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (int i = 0; i < MAX_EXT_SENSORS; i++) {
      JsonObject e = arr.add<JsonObject>();
      e["id"]       = i + 1;
      e["name"]     = config.extSensors[i].name;
      e["enabled"]  = config.extSensors[i].enabled;
      e["zoneMask"] = config.extSensors[i].zoneMask;
    }
    String buf;
    serializeJson(doc, buf);
    req->send(200, "application/json", buf);
  } else if (req->method() == HTTP_POST) {
    for (int i = 0; i < MAX_EXT_SENSORS; i++) {
      String prefix = "e" + String(i + 1);
      if (req->hasArg((prefix + "_name").c_str())) {
        String v = req->arg((prefix + "_name").c_str());
        strlcpy(config.extSensors[i].name, v.c_str(), sizeof(config.extSensors[i].name));
      }
      config.extSensors[i].enabled = (req->arg((prefix + "_enabled").c_str()) != "0");
      if (req->hasArg((prefix + "_zones").c_str())) {
        config.extSensors[i].zoneMask = (uint16_t)req->arg((prefix + "_zones").c_str()).toInt();
      }
    }
    saveConfig();
    req->send(200, "application/json", "{\"ok\":true,\"saved\":true}");
  }
}

// ─── Zones config API ──────────────────────────────────────────────────────

static void apiZonesConfig(AsyncWebServerRequest *req) {
  if (req->method() == HTTP_GET) {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (int i = 0; i < MAX_ZONES; i++) {
      JsonObject z = arr.add<JsonObject>();
      z["id"]          = i + 1;
      z["name"]        = config.zones[i].name;
      z["entryDelayS"] = config.zones[i].entryDelayS;
      z["exitDelayS"]  = config.zones[i].exitDelayS;
      z["enabled"]     = config.zones[i].enabled;
    // Collect associated sensor labels
    String sensList;
    for (int s = 0; s < TOTAL_SENSORS; s++) {
      if (config.sensors[s].type != SENSOR_DISABLED && (config.sensors[s].zoneMask & (1U << i))) {
        if (sensList.length()) sensList += ", ";
        sensList += "T" + String(s + 1);
      }
    }
    for (int s = 0; s < MAX_EXT_SENSORS; s++) {
      if (config.extSensors[s].enabled && (config.extSensors[s].zoneMask & (1U << i))) {
        if (sensList.length()) sensList += ", ";
        sensList += "E" + String(s + 1);
      }
    }
    z["sensors"] = sensList;
    }
    String buf;
    serializeJson(doc, buf);
    req->send(200, "application/json", buf);
  } else if (req->method() == HTTP_POST) {
    for (int i = 0; i < MAX_ZONES; i++) {
      String prefix = "z" + String(i + 1);
      if (req->hasArg((prefix + "_name").c_str())) {
        String v = req->arg((prefix + "_name").c_str());
        strlcpy(config.zones[i].name, v.c_str(), sizeof(config.zones[i].name));
      }
      if (req->hasArg((prefix + "_entry").c_str())) {
        config.zones[i].entryDelayS = req->arg((prefix + "_entry").c_str()).toInt();
      }
      if (req->hasArg((prefix + "_exit").c_str())) {
        config.zones[i].exitDelayS = req->arg((prefix + "_exit").c_str()).toInt();
      }
      config.zones[i].enabled = (req->arg((prefix + "_enabled").c_str()) != "0");
    }
    saveConfig();
    req->send(200, "application/json", "{\"ok\":true,\"saved\":true}");
  }
}

// ─── OTA firmware upload handler ───────────────────────────────────────────

static void handleOTAUpload(AsyncWebServerRequest *req, String filename, size_t index, uint8_t *data, size_t len, bool final) {
  if (!index) {
    Serial.printf("[OTA] Upload starting: %s (%u bytes)\n", filename.c_str(), req->contentLength());
    if (!Update.begin(req->contentLength(), U_FLASH)) {
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
      req->send(200, "text/plain", "OK");
      delay(500);
      ESP.restart();
    } else {
      Update.printError(Serial);
      req->send(500, "text/plain", "OTA end failed");
    }
  }
}

void initWebServer() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send(200, "text/html", HTML);
  });

  server.on("/api/ota", HTTP_POST, [](AsyncWebServerRequest *req) {
    req->send(200, "text/plain", "OK");
  }, handleOTAUpload);

  server.on("/api/extsensors", HTTP_GET, apiExtSensorsConfig);
  server.on("/api/extsensors", HTTP_POST, apiExtSensorsConfig);
  server.on("/api/zones", HTTP_GET, apiZonesConfig);
  server.on("/api/zones", HTTP_POST, apiZonesConfig);
  server.on("/api/status", HTTP_GET, apiStatus);
  server.on("/api/sensors", HTTP_GET, apiSensorsConfig);
  server.on("/api/sensors", HTTP_POST, apiSensorsConfig);
  server.on("/api/network", HTTP_GET, apiNetworkConfig);
  server.on("/api/network", HTTP_POST, apiNetworkConfig);
  server.on("/api/restart", HTTP_GET, apiRestart);
  server.on("/api/reconnect", HTTP_GET, apiReconnect);

  for (int z = 1; z <= MAX_ZONES; z++) {
    String base = "/api/zone/" + String(z);
    server.on((base + "/arm").c_str(),    HTTP_GET, apiZoneCommand);
    server.on((base + "/disarm").c_str(), HTTP_GET, apiZoneCommand);
    server.on((base + "/toggle").c_str(), HTTP_GET, apiZoneCommand);
  }

  for (int r = 1; r <= MAX_RELAYS; r++) {
    server.on(("/api/relay/" + String(r)).c_str(), HTTP_GET, apiRelayCommand);
  }

  server.begin();
}