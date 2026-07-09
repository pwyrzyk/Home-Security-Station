#include "web.h"
#include "sensors.h"
#include "zones.h"
#include "alarm.h"
#include "alarm_mode.h"
#include "hardware.h"
#include "mqtt.h"
#include "network.h"
#include "event_log.h"
#include "auth.h"
#include "backup.h"
#include <ArduinoJson.h>
#include <Update.h>

AsyncWebServer server(HTTP_PORT);

// ─── Deferred action flags (set by async handlers, executed in webLoop) ───
volatile bool pendingRestart  = false;
volatile bool pendingReconnect = false;
PendingExtSensorTrigger pendingExtTrigger = {false, 0, false};
PendingRelayCommand     pendingRelayCmd   = {false, 0, false};

void webLoop() {
  if (pendingRestart) {
    pendingRestart = false;
    saveConfig();   // force flush any pending config changes before reboot
    delay(100);     // allow HTTP response to flush
    ESP.restart();
  }
  if (pendingReconnect) {
    pendingReconnect = false;
    if (apMode) {
      WiFi.softAPdisconnect(true);  // turn off AP before trying STA
      apMode = false;
    }
    WiFi.disconnect();
    delay(500);
    ensureWiFiMode();
  }

  // ─── Deferred external-sensor trigger (SHOULD #7) ─────────────────────
  // Applied from main loop to avoid racing sensorsLoop()/alarmLoop().
  if (pendingExtTrigger.pending) {
    uint8_t id = pendingExtTrigger.id;
    bool active = pendingExtTrigger.active;
    pendingExtTrigger.pending = false;
    if (id >= 1 && id <= MAX_EXT_SENSORS) {
      bool wasActive = extSensorStates[id - 1].active;
      extSensorStates[id - 1].active = active;
      extSensorStates[id - 1].lastChangeMs = millis();
      if (active != wasActive) {
        char buf[80];
        snprintf(buf, sizeof(buf), "E%d '%s' -> %s (API)", id,
                 config.extSensors[id - 1].name, active ? "ACTIVE" : "IDLE");
        logSensor(buf);
        updateZoneSensorCache();  // keep zone tripped cache in sync
      }
    }
  }

  // ─── Deferred relay command (SHOULD #7) ───────────────────────────────
  // Applied from main loop to avoid racing syncRelays() in alarmLoop().
  if (pendingRelayCmd.pending) {
    uint8_t id = pendingRelayCmd.id;
    bool on = pendingRelayCmd.on;
    pendingRelayCmd.pending = false;
    if (id >= 1 && id <= MAX_RELAYS) {
      uint8_t idx = id - 1;
      relayManualOverride[idx] = true;
      relayManualState[idx] = on;
      setRelay(idx, on);
    }
  }
}

// ─── Heap guard ────────────────────────────────────────────────────────────
#define HEAP_SAFETY_THRESHOLD 25000   // 25 KB minimum free heap

static bool checkHeap(AsyncWebServerRequest *req) {
  if (ESP.getFreeHeap() < HEAP_SAFETY_THRESHOLD) {
    req->send(503, "application/json", "{\"error\":\"low_memory\",\"retry_ms\":5000}");
    return false;
  }
  return true;
}

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
  root["uptime"]   = millis() / 1000;
  root["heapFree"] = ESP.getFreeHeap();
  root["localIP"]  = WiFi.localIP().toString();
  root["ssid"]     = config.wifiSsid;
  root["mqttConnected"] = mqtt.connected();
  root["haEnabled"]     = config.haDiscoveryEnabled;

  // ─── Sensor summary ──────────────────────────────────────────────────
  {
    JsonObject ss = root["sensorSummary"].to<JsonObject>();
    int idle = 0, active = 0, fault = 0;
    for (int i = 0; i < TOTAL_SENSORS; i++) {
      if (config.sensors[i].type == SENSOR_DISABLED) continue;
      switch (sensorStates[i].state) {
        case SENSOR_IDLE:   idle++;   break;
        case SENSOR_ACTIVE: active++; break;
        case SENSOR_FAULT:  fault++;  break;
      }
    }
    // Include external sensors in the active count
    int extActive = 0;
    for (int i = 0; i < MAX_EXT_SENSORS; i++) {
      if (config.extSensors[i].enabled && extSensorStates[i].active) extActive++;
    }
    ss["idle"] = idle; ss["active"] = active + extActive; ss["fault"] = fault;
    ss["intActive"] = active; ss["extActive"] = extActive;
  }

  // ─── Last trigger info ───────────────────────────────────────────────
  {
    JsonObject lt = root["lastTrigger"].to<JsonObject>();
    lt["zoneName"]   = alarmCtx.lastTriggerZoneName;
    lt["sensorName"] = alarmCtx.lastTriggerSensorName;
    lt["zoneId"]     = alarmCtx.lastTriggerZoneId;
    lt["sensorId"]   = alarmCtx.lastTriggerSensorId;
    uint32_t ago = 0;
    if (alarmCtx.lastTriggerTimeMs > 0) {
      ago = (millis() - alarmCtx.lastTriggerTimeMs) / 1000;
    }
    lt["timeAgoSec"] = ago;
  }

  // ─── Global alarm state ──────────────────────────────────────────────
  root["activeMode"]  = alarmModeToHaString(alarmCtx.activeMode);
  root["globalState"] = alarmStateToHaString(alarmCtx.globalState);

  JsonArray zones = root["zones"].to<JsonArray>();
  for (int i = 0; i < MAX_ZONES; i++) {
    yield();
    JsonObject z = zones.add<JsonObject>();
    z["id"]    = i + 1;
    z["name"]  = config.zones[i].name;
    z["armed"] = zoneStates[i].armed;
    z["enabled"] = config.zones[i].enabled;
    z["state"] = zoneAlarmStateStr(zoneStates[i].alarmState);
    z["label"] = zoneAlarmStateLabel(zoneStates[i].alarmState);
    String sensList;
    sensList.reserve(200);
    for (int s = 0; s < TOTAL_SENSORS; s++) {
      if (config.sensors[s].type != SENSOR_DISABLED && (config.sensors[s].zoneMask & (1U << i))) {
        if (sensList.length()) sensList += ", ";
        sensList += "T" + String(s + 1);
      }
    }
    yield();
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
    yield();
    JsonObject s = sensors.add<JsonObject>();
    s["id"]     = i + 1;
    s["name"]   = config.sensors[i].name;
    s["type"]   = sensorTypeStr(config.sensors[i].type);
    s["state"]  = sensorStateStr(sensorStates[i].state);
    s["raw"]    = sensorStates[i].rawValue;
  }

  JsonArray relays = root["relays"].to<JsonArray>();
  for (int i = 0; i < MAX_RELAYS; i++) {
    yield();
    JsonObject r = relays.add<JsonObject>();
    r["id"]      = i + 1;
    r["name"]    = config.relays[i].name;
    r["state"]   = relayStates[i];
    r["enabled"] = config.relays[i].enabled;
    r["mode"]    = (int)config.relays[i].mode;
    r["zoneId"]  = config.relays[i].zoneId;
  }

  JsonArray dins = root["din"].to<JsonArray>();
  for (int i = 0; i < MAX_DINPUTS; i++) {
    yield();
    JsonObject d = dins.add<JsonObject>();
    d["id"]    = i + 1;
    d["state"] = dinputStates[i];
  }

  JsonArray ext = root["ext_sensors"].to<JsonArray>();
  for (int i = 0; i < MAX_EXT_SENSORS; i++) {
    yield();
    JsonObject e = ext.add<JsonObject>();
    e["id"]       = i + 1;
    e["name"]     = config.extSensors[i].name;
    e["enabled"]  = config.extSensors[i].enabled;
    e["active"]   = extSensorStates[i].active;
    e["zoneMask"] = config.extSensors[i].zoneMask;
  }

  String buf;
  buf.reserve(3000);
  serializeJson(doc, buf);
  req->send(200, "application/json", buf);
}

// ─── Sensors config API ────────────────────────────────────────────────────

static void apiSensorsConfig(AsyncWebServerRequest *req) {
  if (req->method() == HTTP_GET) {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (int i = 0; i < TOTAL_SENSORS; i++) {
      yield();
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
    requestSaveConfig();
    req->send(200, "application/json", "{\"ok\":true,\"saved\":true}");
  }
}

// ─── Network config API ────────────────────────────────────────────────────

static void apiNetworkConfig(AsyncWebServerRequest *req) {
  if (req->method() == HTTP_GET) {
    JsonDocument doc;
    doc["wifiSsid"]   = config.wifiSsid;
    // Redact WiFi password — never expose in plaintext over HTTP
    if (strlen(config.wifiPass) > 0) {
      doc["wifiPass"] = "****";
    } else {
      doc["wifiPass"] = "";
    }
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
    if (req->hasArg("wifiPass")) {
      String pass = req->arg("wifiPass");
      // Don't overwrite real password with the masked placeholder "****"
      if (pass != "****") {
        strlcpy(config.wifiPass, pass.c_str(), sizeof(config.wifiPass));
      }
    }
    if (req->hasArg("mqttServer")) strlcpy(config.mqttServer, req->arg("mqttServer").c_str(), sizeof(config.mqttServer));
    if (req->hasArg("mqttPort"))  config.mqttPort = req->arg("mqttPort").toInt();
    if (req->hasArg("mqttUser"))  strlcpy(config.mqttUser, req->arg("mqttUser").c_str(), sizeof(config.mqttUser));
    if (req->hasArg("mqttPass"))  strlcpy(config.mqttPass, req->arg("mqttPass").c_str(), sizeof(config.mqttPass));
    saveConfig();  // synchronous — WiFi creds must persist before restart
    req->send(200, "application/json", "{\"ok\":true,\"saved\":true}");
  }
}

static void apiRestart(AsyncWebServerRequest *req) {
  req->send(200, "application/json", "{\"ok\":true}");
  pendingRestart = true;   // deferred — executed in webLoop()
}

static void apiReconnect(AsyncWebServerRequest *req) {
  req->send(200, "application/json", "{\"ok\":true}");
  pendingReconnect = true;  // deferred — executed in webLoop()
}

static void apiZoneCommand(AsyncWebServerRequest *req) {
  String path = req->url();
  int lastSlash = path.lastIndexOf('/');
  String action = path.substring(lastSlash + 1);
  int prevSlash = path.lastIndexOf('/', lastSlash - 1);
  int zoneId    = path.substring(prevSlash + 1, lastSlash).toInt();
  if (zoneId >= 1 && zoneId <= MAX_ZONES) {
    lastZoneCmdSource = "web user";
    if (action == "arm") zoneArm(zoneId);
    if (action == "disarm") zoneDisarm(zoneId);
    if (action == "toggle") zoneToggle(zoneId);
  }
  req->send(200, "application/json", "{\"ok\":true}");
}

static void apiModeSet(AsyncWebServerRequest *req) {
  String modeArg = req->arg("mode");
  if (modeArg.length() == 0) {
    req->send(400, "application/json", "{\"error\":\"missing mode parameter\"}");
    return;
  }

  // Convert the HA-style string to AlarmMode and apply via the shared helper.
  // This replaces 6 duplicated armMode/disarmMode + publish blocks.
  AlarmMode mode = haStringToMode(modeArg.c_str());
  if (mode == AlarmMode::DISARMED && modeArg != "disarmed") {
    // haStringToMode returns DISARMED for unknown strings too — reject them
    req->send(400, "application/json", "{\"error\":\"unknown mode\"}");
    return;
  }

  bool ok = applyModeAndPublish(mode, "web user");
  if (!ok) {
    // armMode rejected (e.g. mode profile not defined / no zones)
    req->send(400, "application/json", "{\"error\":\"arm_rejected\"}");
    return;
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
    // SHOULD #7: defer the mutation to webLoop() to avoid racing syncRelays()
    // in alarmLoop() on the async TCP task.
    pendingRelayCmd.pending = true;
    pendingRelayCmd.id      = (uint8_t)relayId;
    pendingRelayCmd.on      = on;
  }
  req->send(200, "application/json", "{\"ok\":true}");
}

// ─── Dashboard HTML ────────────────────────────────────────────────────────

static const char HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Home Alarm System</title><style>
:root{--bg:#0d1117;--fg:#c9d1d9;--card:#161b22;--border:#30363d;--muted:#8b949e;--blue:#58a6ff;--green:#3fb950;--red:#f85149;--yellow:#d2991d;--card-hover:#1c2129}
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,BlinkMacSystemFont,"SF Pro Display","Helvetica Neue",sans-serif;background:var(--bg);color:var(--fg);min-height:100vh;-webkit-font-smoothing:antialiased;-moz-osx-font-smoothing:grayscale}
nav{display:flex;background:rgba(22,27,34,0.85);backdrop-filter:blur(16px);-webkit-backdrop-filter:blur(16px);padding:0;position:sticky;top:0;z-index:100;border-bottom:1px solid var(--border)}
nav button{padding:12px 18px;background:none;border:none;color:var(--muted);cursor:pointer;font-size:13px;font-weight:500;letter-spacing:0.01em;border-bottom:2px solid transparent;transition:color 0.15s,border-color 0.15s}
nav button:hover{color:var(--fg)}nav button.active{color:var(--blue);border-bottom-color:var(--blue)}
.page{display:none;padding:32px 24px;max-width:1100px;margin:0 auto}
.page.active{display:block}h1{font-size:28px;font-weight:700;color:var(--fg);margin-bottom:4px;letter-spacing:-0.02em}h2{font-size:16px;font-weight:600;color:var(--fg);margin-bottom:12px;letter-spacing:-0.01em}
.card{background:var(--card);border:1px solid var(--border);border-radius:12px;padding:20px;margin:14px 0}
.zone-grid{display:flex;gap:10px;justify-content:space-between;flex-wrap:wrap}
.zone-box{flex:1;min-width:100px;max-width:130px;border-radius:12px;padding:16px 8px 12px;text-align:center;display:flex;flex-direction:column;align-items:center;gap:8px;transition:all 0.2s ease;flex-shrink:0;background:var(--card);border:1px solid var(--border)}
.zone-box.disarmed{border-color:var(--border);background:var(--card)}
.zone-box.armed_idle{border-color:var(--green);background:rgba(63,185,80,0.08)}
.zone-box.arming{border-color:var(--blue);background:rgba(88,166,255,0.08)}
.zone-box.prealarm{border-color:var(--yellow);background:rgba(210,153,29,0.08)}
.zone-box.disarming{border-color:var(--yellow);background:rgba(210,153,29,0.08)}
.zone-box.alarm{border-color:var(--red);background:rgba(248,81,73,0.08)}
.zone-dot{width:12px;height:12px;border-radius:50%}
.zone-dot.disarmed{background:var(--muted)}
.zone-dot.armed_idle{background:var(--green)}
.zone-dot.arming{background:var(--blue)}
.zone-dot.prealarm{background:var(--yellow)}
.zone-dot.disarming{background:var(--yellow)}
.zone-dot.alarm{background:var(--red)}
.zone-name{font-size:12px;font-weight:600;line-height:1.2;color:var(--fg);letter-spacing:-0.01em}
.zone-label{font-size:11px;color:var(--muted);font-weight:500}
.zone-label.has-state{color:var(--green)}
.zone-label.arming-state{color:var(--blue)}
.zone-label.prealarm-state{color:var(--yellow)}
.zone-label.disarming-state{color:var(--yellow)}
.zone-label.alarm-state{color:var(--red)}
.toggle{position:relative;display:inline-block;width:51px;height:31px}
.toggle input{opacity:0;width:0;height:0}
.toggle-slider{position:absolute;cursor:pointer;top:0;left:0;right:0;bottom:0;background:var(--border);border-radius:31px;transition:0.25s}
.toggle-slider:before{position:absolute;content:"";height:27px;width:27px;left:2px;bottom:2px;background:var(--fg);border-radius:50%;transition:0.25s}
.toggle input:checked+.toggle-slider{background:var(--green)}
.toggle input:checked+.toggle-slider:before{transform:translateX(20px)}
.toggle.disarm-slider input+.toggle-slider{background:var(--muted)}
.toggle.alarm-slider input+.toggle-slider{background:var(--red)}
.btn{padding:8px 18px;margin:2px;border:none;border-radius:8px;cursor:pointer;font-size:13px;font-weight:500;color:#fff;letter-spacing:-0.01em;transition:opacity 0.2s,transform 0.1s}
.btn:hover{opacity:0.88}.btn:active{transform:scale(0.97)}
.btn-arm{background:var(--green)}.btn-disarm{background:var(--red)}
.btn-save{background:var(--blue)}.btn-danger{background:var(--red)}
.sensor-grid{display:flex;flex-direction:column;gap:10px}
.sensor-row{display:flex;gap:10px;justify-content:space-between;flex-wrap:wrap}
.sensor-box{flex:1;min-width:100px;max-width:130px;border-radius:12px;padding:14px 8px;text-align:center;display:flex;flex-direction:column;align-items:center;gap:6px;transition:all 0.2s ease;flex-shrink:0;background:var(--card);border:1px solid var(--border)}
.sensor-box.idle{border-color:var(--border);background:var(--card)}
.sensor-box.active{border-color:var(--red);background:rgba(248,81,73,0.08)}
.sensor-box.fault{border-color:var(--yellow);background:rgba(210,153,29,0.08)}
.sensor-box.disabled{border-color:var(--border);background:var(--card);opacity:0.5}
.sensor-box .sdot{width:12px;height:12px;border-radius:50%}
.sensor-box .sdot.idle{background:var(--green)}
.sensor-box .sdot.active{background:var(--red)}
.sensor-box .sdot.fault{background:var(--yellow)}
.sensor-box .sdot.disabled{background:var(--muted)}
.sensor-box .slabel{font-size:12px;font-weight:600;line-height:1.2;color:var(--fg);letter-spacing:-0.01em}
.sensor-box .sraw{font-size:13px;font-weight:600}
.sensor-box .sraw.idle{color:var(--green)}
.sensor-box .sraw.active{color:var(--red)}
.sensor-box .sraw.fault{color:var(--yellow)}
.sensor-box .sraw.disabled{color:var(--muted)}
.sensor-box .sstate{font-size:10px;color:var(--muted);font-weight:500;text-transform:uppercase;letter-spacing:0.02em}
.sensor-box .sstate.active-state{color:var(--red)}
.sensor-box .sstate.fault-state{color:var(--yellow)}
.sensor{display:inline-block;padding:4px 10px;margin:2px;border-radius:8px;font-size:12px;font-weight:500}
.sensor.active{background:var(--red);color:#fff}.sensor.fault{background:var(--yellow);color:#fff}.sensor.idle{background:var(--border);color:var(--fg)}
.relay-grid{display:flex;gap:10px;justify-content:center;flex-wrap:wrap}
.relay-box{flex:1;min-width:100px;max-width:140px;border-radius:12px;padding:14px 8px;text-align:center;display:flex;flex-direction:column;align-items:center;gap:6px;transition:all 0.2s ease;flex-shrink:0;background:var(--card);border:1px solid var(--border)}
.relay-box.off{border-color:var(--border);background:var(--card)}
.relay-box.on{border-color:var(--blue);background:rgba(88,166,255,0.08)}
.relay-box .rdot{width:12px;height:12px;border-radius:50%}
.relay-box .rdot.off{background:var(--muted)}
.relay-box .rdot.on{background:var(--blue)}
.relay-box .rlabel{font-size:12px;font-weight:600;line-height:1.2;color:var(--fg);letter-spacing:-0.01em}
.relay-box .rstate{font-size:13px;font-weight:600}
.relay-box .rstate.off{color:var(--muted)}
.relay-box .rstate.on{color:var(--blue)}
label{display:block;margin-top:8px;font-size:12px;color:var(--muted);font-weight:500;letter-spacing:-0.01em}
input:not([type=checkbox]):not([type=file]),select{width:100%;padding:10px 12px;margin-top:4px;border:1px solid var(--border);border-radius:8px;background:var(--bg);color:var(--fg);font-size:14px;font-family:inherit;transition:border-color 0.15s}
input:focus,select:focus{outline:none;border-color:var(--blue);background:var(--bg)}
input[type=number]{width:70px;display:inline;margin:0 2px;-moz-appearance:textfield}
input[type=number]::-webkit-outer-spin-button,input[type=number]::-webkit-inner-spin-button{-webkit-appearance:none;margin:0}
input[type=checkbox]{width:18px;height:18px;margin:0 2px;accent-color:var(--blue);-webkit-appearance:checkbox}
.ztoggle{display:inline-flex;align-items:center;margin-left:auto;flex-shrink:0}
.ztoggle input{width:40px;height:22px;margin:0;padding:0;cursor:pointer;accent-color:var(--green)}
.form-row{display:grid;grid-template-columns:1fr 1fr;gap:16px}
@media(max-width:600px){.form-row{grid-template-columns:1fr}}
small{color:var(--muted);font-size:12px}
#sensorCards,#zoneCards,#extCards{display:grid;grid-template-columns:1fr 1fr;gap:12px}
@media(max-width:750px){#sensorCards,#zoneCards,#extCards{grid-template-columns:1fr}}
.sens-card{border-radius:12px;padding:14px;font-size:12px;border:1px solid var(--border);background:var(--card);transition:border-color 0.2s}
.sens-card.idle{border-color:var(--border);background:var(--card)}
.sens-card.active{border-color:var(--red);background:rgba(248,81,73,0.06)}
.sens-card.fault{border-color:var(--yellow);background:rgba(210,153,29,0.06)}
.sens-card.disabled{border-color:var(--border);background:var(--card);opacity:0.5}
.sens-top{display:flex;align-items:center;gap:8px;margin-bottom:6px;flex-wrap:wrap}
.sens-top .sid{color:var(--blue);font-weight:700;font-size:14px;min-width:24px;letter-spacing:-0.01em}
.sens-top input[type=text]{width:90px;padding:6px 8px;font-size:12px;margin-top:0}
.sens-top select{width:80px;padding:6px;font-size:12px;margin-top:0}
.sens-live{display:flex;align-items:center;gap:6px;font-size:12px;margin-left:auto;font-weight:500}
.sens-live .sdot{width:10px;height:10px;border-radius:50%;flex-shrink:0}
.sdot.idle{background:var(--green)}.sdot.active{background:var(--red)}.sdot.fault{background:var(--yellow)}.sdot.none{background:var(--muted)}
.srange-row{display:flex;align-items:center;gap:4px;margin:2px 0;font-size:11px}
.srange-bar{width:8px;height:20px;border-radius:4px;flex-shrink:0}
.srange-bar.standby{background:var(--green)}
.srange-bar.detected{background:var(--red)}
.srange-bar.faultbar{background:var(--yellow)}
.srange-row input[type=number]{width:50px;padding:4px 6px;font-size:11px;margin-top:0}
.srange-label{width:60px;color:var(--muted);font-size:10px;text-align:right;font-weight:500}
.stiming-row{display:flex;align-items:center;gap:4px;margin:3px 0;font-size:11px;flex-wrap:wrap}
.stiming-row span{color:var(--muted);font-size:10px;font-weight:500}
.stiming-row input[type=number]{width:46px;padding:4px 6px;font-size:11px;margin-top:0}
.szone-row{display:flex;align-items:center;gap:3px;margin:3px 0;font-size:11px;flex-wrap:wrap}
.szone-row span{color:var(--muted);font-size:10px;margin-right:3px;font-weight:500}
.szone-row label{display:inline-flex!important;align-items:center;font-size:10px!important;color:var(--muted)!important;margin-top:0!important;margin-right:2px;font-weight:500}
.szone-row input[type=checkbox]{width:14px;height:14px;margin:0 1px}
.ext-box{flex:1;min-width:100px;max-width:130px;border-radius:12px;padding:14px 8px;text-align:center;display:flex;flex-direction:column;align-items:center;gap:6px;transition:all 0.2s ease;flex-shrink:0;background:var(--card);border:1px solid var(--border)}
.ext-box.idle{border-color:var(--border);background:var(--card)}
.ext-box.active{border-color:var(--red);background:rgba(248,81,73,0.08)}
.ext-box .edot{width:12px;height:12px;border-radius:50%}
.ext-box .edot.idle{background:var(--green)}
.ext-box .edot.active{background:var(--red)}
.ext-box .edot.disabled{background:var(--muted)}
.ext-box .elabel{font-size:12px;font-weight:600;line-height:1.2;color:var(--fg);letter-spacing:-0.01em}
.ext-box .estate{font-size:10px;color:var(--muted);font-weight:500;text-transform:uppercase;letter-spacing:0.02em}
.ext-box .estate.active-state{color:var(--red)}
.ext-box.disabled{border-color:var(--border);background:var(--card);opacity:0.5}
.sens-foot{display:flex;justify-content:flex-end;margin-top:8px}
.sens-upd{background:var(--blue);padding:6px 16px;border:none;border-radius:8px;cursor:pointer;font-size:12px;color:#fff;font-weight:500;letter-spacing:-0.01em;transition:opacity 0.2s,transform 0.1s}
.sens-upd:hover{opacity:0.88}.sens-upd:active{transform:scale(0.97)}
.dot.idle{background:var(--green)}.dot.active{background:var(--red)}.dot.fault{background:var(--yellow)}.dot.none{background:var(--muted)}
.range-row{display:flex;align-items:center;gap:6px;margin:4px 0;font-size:12px}
.range-bar{width:8px;height:24px;border-radius:4px;flex-shrink:0}
.range-bar.standby{background:var(--green)}
.range-bar.detected{background:var(--red)}
.range-bar.faultbar{background:var(--yellow)}
.range-row input[type=number]{width:62px;padding:4px 8px;font-size:12px}
.range-label{width:70px;color:var(--muted);font-size:12px;text-align:right;font-weight:500}
.timing-row{display:flex;align-items:center;gap:8px;margin:4px 0;font-size:12px;flex-wrap:wrap}
.timing-row span{color:var(--muted);font-weight:500}
.timing-row input[type=number]{width:58px;padding:4px 8px;font-size:12px}
.zone-row{display:flex;align-items:center;gap:4px;margin:4px 0;font-size:12px}
.zone-row span{color:var(--muted);margin-right:4px;font-weight:500}
.sens-params-row{display:grid;grid-template-columns:1fr 1fr;gap:10px}
.zone-params{display:grid;grid-template-columns:1fr 1fr;gap:10px 20px;background:rgba(255,255,255,0.03);border-radius:8px;padding:12px 14px;margin:0;border:1px solid var(--border)}
.zone-params-stack{grid-template-columns:1fr}
.zone-params-field{display:flex;align-items:center;gap:6px}
.zone-params-field span{font-size:11px;color:var(--muted);font-weight:500;white-space:nowrap}
.zone-params-field input[type=number]{width:56px;padding:5px 8px;font-size:12px;margin-top:0}
.zone-params-title{grid-column:1/-1;font-size:10px;font-weight:600;color:var(--fg);text-transform:uppercase;letter-spacing:0.04em;margin-bottom:-4px}
.log-row{display:flex;align-items:flex-start;gap:12px;padding:12px 16px;margin:0;border-left:4px solid var(--border);transition:border-color 0.2s,background 0.2s}
.log-row:hover{background:var(--card-hover)}
.log-row.alarm{border-left-color:var(--red)}
.log-row.system{border-left-color:var(--blue)}
.log-row.relay{border-left-color:var(--yellow)}
.log-row.sensor{border-left-color:var(--green)}
.log-badge{display:inline-flex;align-items:center;padding:3px 10px;border-radius:8px;font-size:11px;font-weight:600;color:#fff;white-space:nowrap;min-width:64px;justify-content:center;letter-spacing:0.02em}
.log-badge.alarm{background:var(--red)}
.log-badge.system{background:var(--blue)}
.log-badge.relay{background:var(--yellow)}
.log-badge.sensor{background:var(--green)}
.log-time{font-size:12px;color:var(--muted);white-space:nowrap;min-width:140px;font-variant-numeric:tabular-nums}
.log-desc{font-size:13px;color:var(--fg);line-height:1.4;word-break:break-word}
.log-empty{text-align:center;padding:60px 20px;color:var(--muted);font-size:15px}
.log-empty div{font-size:40px;margin-bottom:12px}
.log-header{display:flex;align-items:center;justify-content:space-between;flex-wrap:wrap;gap:10px}
.log-header h1{margin-bottom:0}
@keyframes fadeIn{from{opacity:0;transform:translateY(-4px)}to{opacity:1;transform:translateY(0)}}
.log-footer{display:flex;align-items:center;justify-content:space-between;padding:8px 0 0;font-size:11px;color:var(--muted)}
.filter-btn{padding:6px 14px;border:1px solid var(--border);border-radius:8px;background:var(--card);color:var(--muted);cursor:pointer;font-size:12px;font-weight:500;transition:all 0.2s}
.filter-btn:hover{border-color:var(--blue);color:var(--blue)}
.filter-btn.active{border-color:var(--blue);background:rgba(88,166,255,0.1);color:var(--blue);font-weight:600}
.mode-grid-row{display:flex;gap:8px;flex-wrap:wrap}
.mode-btn{border:1px solid var(--border);border-radius:10px;padding:10px 14px;background:var(--card);cursor:pointer;font-size:13px;font-weight:500;color:var(--fg);transition:all 0.15s;white-space:nowrap}
.mode-btn:hover{border-color:var(--blue);background:rgba(88,166,255,0.08)}
.mode-btn.active{border-color:var(--green);background:rgba(63,185,80,0.1);font-weight:600;color:var(--green)}
.mode-btn.triggered{border-color:var(--red);background:rgba(248,81,73,0.1);color:var(--red)}
.mode-btn.pending{border-color:var(--yellow);background:rgba(210,153,29,0.1);color:var(--yellow)}
.modal-overlay{display:none;position:fixed;top:0;left:0;right:0;bottom:0;background:rgba(0,0,0,0.6);backdrop-filter:blur(8px);-webkit-backdrop-filter:blur(8px);z-index:200;align-items:center;justify-content:center}
.modal-overlay.show{display:flex}
.modal-card{background:var(--card);border:1px solid var(--border);border-radius:14px;padding:36px 32px;width:100%;max-width:380px;animation:fadeIn 0.3s ease}
.modal-card h2{font-size:22px;font-weight:700;margin-bottom:4px;letter-spacing:-0.02em;color:var(--fg)}
.modal-card .sub{font-size:13px;color:var(--muted);margin-bottom:20px}
.modal-card label{display:block;margin-top:10px;font-size:12px;color:var(--muted);font-weight:500}
.modal-card input{width:100%;padding:10px 14px;margin-top:4px;border:1px solid var(--border);border-radius:8px;background:var(--bg);color:var(--fg);font-size:14px;font-family:inherit;transition:border-color 0.15s}
.modal-card input:focus{outline:none;border-color:var(--blue);background:var(--bg)}
.modal-card .btn{width:100%;margin-top:16px;padding:12px}
.modal-card .msg{margin-top:10px;font-size:13px;text-align:center;font-weight:500}
.logout-btn{padding:6px 14px;background:none;border:1px solid var(--red);border-radius:8px;color:var(--red);cursor:pointer;font-size:12px;font-weight:500;transition:all 0.15s}
.logout-btn:hover{background:var(--red);color:#fff}
.nav-right{margin-left:auto;display:flex;align-items:center;gap:10px;padding-right:14px}
.nav-user{display:flex;align-items:center;gap:5px;font-size:13px;font-weight:500;color:var(--fg);white-space:nowrap}
.nav-user .user-icon{font-size:15px;opacity:0.6}
/* ─── Hero status card (modernized status-bar) ─────────────────────────── */
.hero-status{border-radius:14px;padding:18px 22px;margin-bottom:16px;display:flex;align-items:center;gap:16px;transition:all 0.3s ease;border:1px solid var(--border);background:var(--card)}
.hero-status.ready{border-color:var(--green);background:linear-gradient(135deg,rgba(63,185,80,0.12),rgba(63,185,80,0.03))}
.hero-status.pending{border-color:var(--yellow);background:linear-gradient(135deg,rgba(210,153,29,0.12),rgba(210,153,29,0.03))}
.hero-status.alarm{border-color:var(--red);background:linear-gradient(135deg,rgba(248,81,73,0.15),rgba(248,81,73,0.03))}
.hero-status.down{border-color:var(--red);background:linear-gradient(135deg,rgba(248,81,73,0.1),rgba(248,81,73,0.02))}
.hero-status .hero-icon{font-size:32px;flex-shrink:0;width:48px;height:48px;display:flex;align-items:center;justify-content:center;border-radius:12px;background:rgba(255,255,255,0.06)}
.hero-status .hero-text{flex:1;min-width:0}
.hero-status .hero-title{font-size:18px;font-weight:700;color:var(--fg);letter-spacing:-0.02em;line-height:1.3}
.hero-status .hero-sub{font-size:12px;color:var(--muted);font-weight:500;margin-top:2px}
.hero-status .hero-dot{width:10px;height:10px;border-radius:50%;flex-shrink:0;animation:pulse 2s ease-in-out infinite}
.hero-status.ready .hero-dot{background:var(--green)}
.hero-status.pending .hero-dot{background:var(--yellow)}
.hero-status.alarm .hero-dot{background:var(--red)}
.hero-status.down .hero-dot{background:var(--red)}
@keyframes pulse{0%,100%{opacity:1;transform:scale(1)}50%{opacity:0.5;transform:scale(0.85)}}

/* ─── Stat tiles (modernized info-cards) ────────────────────────────────── */
.stat-grid{display:grid;grid-template-columns:repeat(5,1fr);gap:12px;margin-bottom:16px}
@media(max-width:800px){.stat-grid{grid-template-columns:repeat(2,1fr)}}
.stat-tile{background:var(--card);border:1px solid var(--border);border-radius:12px;padding:14px 16px;display:flex;flex-direction:column;gap:6px;transition:all 0.2s ease;position:relative;overflow:hidden}
.stat-tile:hover{background:var(--card-hover);transform:translateY(-1px)}
.stat-tile::before{content:'';position:absolute;left:0;top:0;bottom:0;width:3px;border-radius:0 2px 2px 0;transition:background 0.2s}
.stat-tile.accent-green::before{background:var(--green)}
.stat-tile.accent-red::before{background:var(--red)}
.stat-tile.accent-yellow::before{background:var(--yellow)}
.stat-tile.accent-blue::before{background:var(--blue)}
.stat-tile.accent-muted::before{background:var(--muted)}
.stat-tile .st-header{display:flex;align-items:center;gap:8px}
.stat-tile .st-icon{width:28px;height:28px;border-radius:8px;display:flex;align-items:center;justify-content:center;font-size:16px;background:rgba(255,255,255,0.06);flex-shrink:0}
.stat-tile .st-title{font-size:11px;color:var(--muted);font-weight:600;text-transform:uppercase;letter-spacing:0.03em}
.stat-tile .st-value{font-size:14px;font-weight:600;color:var(--fg);line-height:1.4}
.stat-tile .st-sub{font-size:11px;color:var(--muted);line-height:1.4}
.stat-tile .st-row{display:flex;align-items:center;gap:6px;font-size:12px;font-weight:500}
.stat-tile .st-mini-dot{width:8px;height:8px;border-radius:50%;flex-shrink:0}
.stat-tile .st-mini-dot.on{background:var(--red)}
.stat-tile .st-mini-dot.off{background:var(--muted)}

/* ─── Mode cards (modernized mode-grid) ─────────────────────────────────── */
.mode-cards{display:grid;grid-template-columns:repeat(3,1fr);gap:10px}
@media(max-width:600px){.mode-cards{grid-template-columns:repeat(2,1fr)}}
.mode-card{border:1px solid var(--border);border-radius:12px;padding:16px 12px;background:var(--card);cursor:pointer;transition:all 0.2s ease;display:flex;flex-direction:column;align-items:center;gap:8px;text-align:center}
.mode-card:hover{border-color:var(--blue);background:rgba(88,166,255,0.06);transform:translateY(-1px)}
.mode-card.active{border-color:var(--green);background:rgba(63,185,80,0.08)}
.mode-card.triggered{border-color:var(--red);background:rgba(248,81,73,0.1)}
.mode-card.pending{border-color:var(--yellow);background:rgba(210,153,29,0.08)}
.mode-card .mc-icon{font-size:24px;width:40px;height:40px;display:flex;align-items:center;justify-content:center;border-radius:10px;background:rgba(255,255,255,0.06)}
.mode-card .mc-label{font-size:13px;font-weight:600;color:var(--fg);letter-spacing:-0.01em}
.mode-card.active .mc-label{color:var(--green)}
.mode-card.triggered .mc-label{color:var(--red)}
.mode-card.pending .mc-label{color:var(--yellow)}
.mode-card .mc-dot{width:8px;height:8px;border-radius:50%;background:transparent}
.mode-card.active .mc-dot{background:var(--green)}
.mode-card.triggered .mc-dot{background:var(--red);animation:pulse 1.5s ease-in-out infinite}
.mode-card.pending .mc-dot{background:var(--yellow)}

/* ─── New dashboard widgets ─────────────────────────────────────────────── */
.zone-summary{display:flex;align-items:center;gap:12px;background:var(--card);border:1px solid var(--border);border-radius:12px;padding:14px 18px;margin-bottom:16px}
.zone-summary .zs-label{font-size:13px;font-weight:600;color:var(--fg);white-space:nowrap}
.zone-summary .zs-bar{flex:1;height:8px;border-radius:4px;background:var(--border);overflow:hidden}
.zone-summary .zs-fill{height:100%;border-radius:4px;transition:width 0.3s ease,background 0.3s ease}
.zone-summary .zs-count{font-size:13px;font-weight:600;color:var(--muted);white-space:nowrap}

.alert-banner{border-radius:12px;padding:12px 18px;margin-bottom:16px;display:flex;align-items:center;gap:10px;font-size:13px;font-weight:500;transition:all 0.3s ease}
.alert-banner.warn{background:rgba(210,153,29,0.1);border:1px solid var(--yellow);color:var(--yellow)}
.alert-banner.danger{background:rgba(248,81,73,0.1);border:1px solid var(--red);color:var(--red)}
.alert-banner .ab-icon{font-size:18px;flex-shrink:0}
.alert-banner .ab-text{flex:1}
.alert-banner .ab-list{font-size:11px;opacity:0.8;margin-top:2px}

.quick-actions{display:flex;gap:10px;margin-bottom:16px;flex-wrap:wrap}
.qa-btn{flex:1;min-width:120px;padding:14px;border:none;border-radius:12px;cursor:pointer;font-size:14px;font-weight:600;color:#fff;transition:all 0.2s ease;display:flex;align-items:center;justify-content:center;gap:8px}
.qa-btn:hover{opacity:0.88;transform:translateY(-1px)}
.qa-btn:active{transform:scale(0.97)}
.qa-btn.qa-arm{background:var(--green)}
.qa-btn.qa-disarm{background:var(--red)}
.qa-btn.qa-panic{background:var(--yellow);color:#1a1a1a}

.conn-quality{display:flex;align-items:center;gap:10px}
.cq-bar{flex:1;height:6px;border-radius:3px;background:var(--border);overflow:hidden;display:flex;gap:2px;padding:0}
.cq-seg{flex:1;height:100%;border-radius:1px;transition:background 0.3s ease}
.cq-seg.active.excellent{background:var(--green)}
.cq-seg.active.good{background:var(--green)}
.cq-seg.active.fair{background:var(--yellow)}
.cq-seg.active.weak{background:var(--red)}
.cq-label{font-size:11px;font-weight:600;color:var(--muted);white-space:nowrap}

.recent-events{margin-bottom:16px}
.recent-events .re-item{display:flex;align-items:center;gap:10px;padding:8px 0;border-bottom:1px solid var(--border)}
.recent-events .re-item:last-child{border-bottom:none}
.recent-events .re-badge{font-size:10px;font-weight:600;padding:2px 8px;border-radius:6px;color:#fff;white-space:nowrap}
.recent-events .re-badge.alarm{background:var(--red)}
.recent-events .re-badge.system{background:var(--blue)}
.recent-events .re-badge.relay{background:var(--yellow)}
.recent-events .re-badge.sensor{background:var(--green)}
.recent-events .re-time{font-size:11px;color:var(--muted);white-space:nowrap}
.recent-events .re-desc{font-size:12px;color:var(--fg);flex:1;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
</style></head>
<body>
<nav>
<button onclick="showTab('dashboard')" id="tab-dashboard" class="active">Dashboard</button>
<button onclick="showTab('sensors')" id="tab-sensors">Sensors</button>
<button onclick="showTab('extsensors')" id="tab-extsensors">External Sensors</button>
<button onclick="showTab('zones')" id="tab-zones">Zones</button>
<button onclick="showTab('alarmmodes')" id="tab-alarmmodes">Alarm Modes</button>
<button onclick="showTab('config')" id="tab-config">Config</button>
<button onclick="showTab('eventlog')" id="tab-eventlog">Event Log</button>
<button onclick="showTab('users')" id="tab-users" style="display:none">Users</button>
<div class="nav-right">
<span class="nav-user"><span class="user-icon">👤</span> <span id="navUser"></span></span>
<button class="logout-btn" onclick="doLogout()">Logout</button>
</div>
</nav>
<div id="page-dashboard" class="page active"><h1>Home Alarm System</h1>
<div class="hero-status" id="heroStatus">Loading...</div>
<div class="alert-banner" id="alertBanner" style="display:none"></div>
<div class="zone-summary" id="zoneSummary" style="display:none"></div>
<div class="quick-actions" id="quickActions" style="display:none">
<button class="qa-btn qa-panic" id="panicBtn" onclick="quickPanic()">🆘 Panic</button>
<button class="qa-btn qa-disarm" onclick="quickDisarmAll()">🔓 Disarm All</button>
</div>
<div class="stat-grid" id="statGrid">Loading...</div>
<div class="card"><h2>Alarm Mode</h2><div class="mode-cards" id="modeGrid">Loading...</div></div>
<div class="card" id="recentEventsCard" style="display:none"><h2>Recent Events</h2><div class="recent-events" id="recentEvents"></div></div>
<div class="card"><h2>Zones</h2><div id="zones">Loading...</div></div>
<div class="card"><h2>Relays</h2><div id="relays">Loading...</div></div>
<div class="card"><h2>Sensors</h2><div id="sensors">Loading...</div></div>
<div class="card"><h2>External Sensors</h2><div id="extensors">Loading...</div></div>
</div>
<div id="page-extsensors" class="page"><h1>External Sensor Configuration</h1>
<div id="extSubtitle" style="font-size:11px;color:#86868b;margin-bottom:16px">Loading...</div>
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
<div id="page-alarmmodes" class="page"><h1>Alarm Mode Configuration</h1>
<div style="font-size:11px;color:#86868b;margin-bottom:16px">Configure which zones are active in each Home Assistant-compatible alarm mode.</div>
<div class="card" id="alarmModesMatrix">Loading...</div>
<div style="margin-top:12px">
<button class="btn btn-save" onclick="saveAlarmModes()">Save Alarm Modes</button>
<span id="alarmModeMsg" style="font-size:13px;margin-left:12px"></span>
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
<div class="card"><h2>Backup & Restore</h2>
<div style="margin:8px 0"><small>Download full system backup (config + event log).</small></div>
<button class="btn btn-save" onclick="downloadBackup()">Download Backup</button>
<div style="margin-top:14px"><small>Restore all configuration from a backup file. <strong>Device will reboot after restore.</strong></small></div>
<div style="display:flex;align-items:center;gap:8px;flex-wrap:wrap;margin-top:4px">
<input type="file" id="restoreFile" accept=".json" style="flex:1;min-width:200px">
<button class="btn btn-save" onclick="restoreBackup()">Upload & Restore</button>
</div>
<div id="bkMsg" style="margin-top:8px;font-size:13px"></div>
</div>
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
<button class="btn btn-save" onclick="window.open('/docs','_blank')">📖 Documentation</button>
<button class="btn btn-danger" onclick="restart()">Restart</button></div>
<div id="cfgMsg" style="margin-top:8px;font-size:13px"></div></div>
<div id="page-eventlog" class="page">
<div class="log-header"><h1>Event Log</h1>
<div style="display:flex;align-items:center;gap:6px;flex-wrap:wrap">
<button class="filter-btn active" id="filterAll" onclick="setEventLogFilter(-1)">All</button>
<button class="filter-btn" id="filter0" onclick="setEventLogFilter(0)">🔴 Alarm</button>
<button class="filter-btn" id="filter1" onclick="setEventLogFilter(1)">🔵 System</button>
<button class="filter-btn" id="filter2" onclick="setEventLogFilter(2)">🟠 Relay</button>
<button class="filter-btn" id="filter3" onclick="setEventLogFilter(3)">🟢 Sensor</button>
</div>
<button class="btn btn-danger" onclick="clearEventLog()">Clear Log</button>
</div>
<div class="card" id="eventLogContainer" style="padding:0;overflow:hidden">Loading...</div>
<div class="log-footer" id="logFooter"></div>
</div>
<div id="page-users" class="page"><h1>User Management</h1>
<div style="font-size:11px;color:#86868b;margin-bottom:16px">Manage users who can access the system.</div>
<div class="card"><h2>Add User</h2>
<div class="form-row"><div><label>Username</label><input id="uUser" placeholder="Username (min 2 chars)"></div>
<div><label>Password</label><input id="uPass" type="password" placeholder="At least 4 characters"></div></div>
<div class="form-row"><div><label>Role</label><select id="uRole"><option value="0">Admin (full access)</option><option value="1">Operator (arm/disarm only)</option><option value="2">API (sensor trigger only)</option></select></div>
<div><label>PIN (4 digits for keypad)</label><input id="uPin" type="text" placeholder="0000" maxlength="4" pattern="[0-9]{4}"></div></div>
<button class="btn btn-save" onclick="addUser()">Add User</button>
<span id="uMsg" style="font-size:13px;margin-left:12px"></span>
</div>
<div class="card"><h2>Current Users</h2>
<div id="userTable">Loading...</div>
</div>
</div>
<div class="modal-overlay" id="pwModal">
<div class="modal-card">
<h2>🔐 Change Password</h2>
<div class="sub">You must change the default password before continuing.</div>
<label>Current Password</label>
<input type="password" id="pwCurrent" placeholder="Enter current password">
<label>New Password</label>
<input type="password" id="pwNew" placeholder="At least 4 characters">
<label>Confirm New Password</label>
<input type="password" id="pwConfirm" placeholder="Repeat new password">
<button class="btn btn-save" id="pwBtn" onclick="changePassword()">Change Password</button>
<div id="pwMsg" class="msg"></div>
</div>
</div>
<script>
let data={};
let _pendingLoad = false;
let _pendingRefresh = false;
let _pendingEventLog = false;

function rangeClass(raw, lo, hi){
  if(raw>=lo && raw<=hi) return 'in-range';
  return '';
}

function stateColor(state){ return state=='active'?'#ff3b30':state=='fault'?'#ff9500':'#34c759'; }
function dotClass(state){ return state=='active'?'active':state=='fault'?'fault':'idle'; }

function cardClass(s){
  if(s.type==0) return 'disabled';
  return s.state;
}

function rangeBar(raw, lo, hi, cls){
  if(raw>=lo && raw<=hi && (lo>0 || hi<65535)) return '<span class="range-bar '+cls+'"></span>';
  if(lo===0 && hi===65535 && cls==='standby') return '<span class="range-bar standby"></span>';
  return '<span class="range-bar" style="background:#e5e5ea"></span>';
}

let _authChecked = false;

let _authRole = 1;
async function checkAuth(){
  try{
    const r=await fetch('/api/auth-status');
    const d=await r.json();
    if(!d.authenticated){
      window.location.href='/login.html';
      return false;
    }
    _authRole = (d.role !== undefined) ? d.role : 1;
    // Display username and role
    document.getElementById('navUser').textContent = (d.username || 'User');
    // Admin tab visibility
    let isAdmin = (_authRole===0);
    document.getElementById('tab-sensors').style.display = isAdmin ? '' : 'none';
    document.getElementById('tab-extsensors').style.display = isAdmin ? '' : 'none';
    document.getElementById('tab-zones').style.display = isAdmin ? '' : 'none';
    document.getElementById('tab-alarmmodes').style.display = isAdmin ? '' : 'none';
    document.getElementById('tab-config').style.display = isAdmin ? '' : 'none';
    document.getElementById('tab-eventlog').style.display = isAdmin ? '' : 'none';
    document.getElementById('tab-users').style.display = isAdmin ? '' : 'none';
    // API users get no tabs — dashboard is hidden, redirect
    if(_authRole===2){
      document.getElementById('tab-dashboard').style.display = 'none';
      document.getElementById('page-dashboard').innerHTML='<div style="padding:40px;text-align:center;color:var(--muted)"><div style="font-size:48px;margin-bottom:16px">🔌</div><h2>API Only Account</h2><p style="margin:8px 0">This account can only trigger external sensors via REST API.</p><p style="font-size:13px">Use curl or another HTTP client to call /api/extsensors/trigger</p></div>';
      document.getElementById('page-dashboard').classList.add('active');
    }
    if(d.forcePasswordChange){
      document.getElementById('pwModal').classList.add('show');
      document.getElementById('pwCurrent').focus();
      return true;
    }
  }catch(e){}
  return true;
}

async function loadUsers(){
  try{
    const r=await fetch('/api/users');
    const users=await r.json();
    let h='<table style="width:100%;border-collapse:collapse">';
    h+='<tr><th style="text-align:left;padding:8px 12px;font-size:12px;color:#86868b;font-weight:600">Username</th><th style="text-align:left;padding:8px 12px;font-size:12px;color:#86868b;font-weight:600">Role</th><th style="text-align:left;padding:8px 12px;font-size:12px;color:#86868b;font-weight:600">PIN</th><th style="text-align:right;padding:8px 12px;font-size:12px;color:#86868b;font-weight:600">Actions</th></tr>';
    users.forEach(u=>{
      h+='<tr style="border-top:1px solid #f0f0f5"><td style="padding:10px 12px;font-size:14px;font-weight:500">'+u.username+'</td>';
      h+='<td style="padding:10px 12px;font-size:13px;color:#86868b">'+(u.role==0?'Admin':u.role==2?'API':'Operator')+'</td>';
      h+='<td style="padding:10px 12px;font-size:13px;font-family:monospace;color:#86868b">'+u.pin+'</td>';
      h+='<td style="text-align:right;padding:10px 12px"><button class="btn btn-danger" onclick="deleteUser('+u.id+',\''+u.username+'\')" style="font-size:11px;padding:4px 12px">Delete</button></td></tr>';
    });
    h+='</table>';
    if(users.length===0) h='<div style="text-align:center;padding:20px;color:#aeaeb2">No users found.</div>';
    document.getElementById('userTable').innerHTML=h;
  }catch(e){}
}

async function addUser(){
  let u=document.getElementById('uUser').value.trim();
  let p=document.getElementById('uPass').value;
  let r=document.getElementById('uRole').value;
  let n=document.getElementById('uPin').value;
  if(!u||!p||!n||n.length!==4){document.getElementById('uMsg').textContent='All fields required (PIN=4 digits).';return;}
  let rsp=await fetch('/api/users/add',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body:'username='+encodeURIComponent(u)+'&password='+encodeURIComponent(p)+'&role='+r+'&pin='+n});
  let d=await rsp.json();
  if(d.ok){
    document.getElementById('uMsg').textContent='User added.';
    document.getElementById('uUser').value='';document.getElementById('uPass').value='';
    document.getElementById('uPin').value='';
    loadUsers();
  } else {
    document.getElementById('uMsg').textContent=d.error||'Error.';
  }
}

async function doLogout(){
  await fetch('/api/logout',{method:'POST'});
  window.location.href='/login.html';
}

async function deleteUser(id,name){
  if(!confirm('Delete user '+name+'?')) return;
  let rsp=await fetch('/api/users/delete',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'id='+id});
  let d=await rsp.json();
  if(d.ok) loadUsers(); else alert(d.error||'Error');
}

async function changePassword(){
  let btn=document.getElementById('pwBtn');
  let msg=document.getElementById('pwMsg');
  let cur=document.getElementById('pwCurrent').value;
  let pw=document.getElementById('pwNew').value;
  let cf=document.getElementById('pwConfirm').value;
  msg.className='msg';
  if(!cur||!pw||!cf){msg.textContent='All fields are required.';return;}
  if(pw.length<4){msg.textContent='Password must be at least 4 characters.';return;}
  if(pw!==cf){msg.textContent='Passwords do not match.';return;}
  if(cur===pw){msg.textContent='New password must differ from current.';return;}
  btn.disabled=true;
  btn.textContent='Changing...';
  try{
    let r=await fetch('/api/change-password',{
      method:'POST',
      headers:{'Content-Type':'application/x-www-form-urlencoded'},
      body:'current='+encodeURIComponent(cur)+'&new='+encodeURIComponent(pw)
    });
    let d=await r.json();
    if(d.ok){
      msg.className='msg success';
      msg.textContent='Password changed! Loading dashboard...';
      setTimeout(()=>{
        document.getElementById('pwModal').classList.remove('show');
        document.getElementById('pwCurrent').value='';
        document.getElementById('pwNew').value='';
        document.getElementById('pwConfirm').value='';
        msg.textContent='';
        msg.className='msg';
        btn.disabled=false;
        btn.textContent='Change Password';
        load();
      },800);
    }else{
      msg.textContent=d.message||'Failed to change password.';
      btn.disabled=false;
      btn.textContent='Change Password';
    }
  }catch(e){
    msg.textContent='Connection error.';
    btn.disabled=false;
    btn.textContent='Change Password';
  }
}

function fmDur(s){
  if(s<60) return s+'s';
  if(s<3600) return Math.floor(s/60)+'m '+(s%60)+'s';
  return Math.floor(s/3600)+'h '+Math.floor((s%3600)/60)+'m';
}

function renderHeroStatus(){
  let d=data;
  let wifiOk = d.wifi==='connected';
  let state = d.globalState||'disarmed';
  let cls = wifiOk ? (state==='triggered'?'alarm':(state==='disarmed'?'ready':(state==='pending'?'pending':'ready'))) : 'down';
  let icon = wifiOk ? (state==='triggered'?'🚨':(state==='disarmed'?'✅':(state==='pending'?'⏳':'🔒'))) : '❌';
  let title = wifiOk ? (state==='triggered'?'Alarm Active':(state==='disarmed'?'System Ready':(state==='pending'?'Arming...':'System Armed'))) : 'WiFi Down';
  let sub = wifiOk ? (d.activeMode ? modeLabels[d.activeMode] : '') : 'Check network connection';
  let h='<div class="hero-icon">'+icon+'</div>';
  h+='<div class="hero-text"><div class="hero-title">'+title+'</div><div class="hero-sub">'+sub+'</div></div>';
  h+='<div class="hero-dot"></div>';
  let el=document.getElementById('heroStatus');
  el.className='hero-status '+cls;
  el.innerHTML=h;
}

function renderStatTiles(){
  let d=data, ss=d.sensorSummary||{}, lt=d.lastTrigger||{};
  let tiles=[];
  let ltRecent = lt.timeAgoSec && lt.timeAgoSec < 3600;
  tiles.push({icon:'🚨',title:'Last Trigger',accent:ltRecent?'red':'muted',
    value:(lt.zoneName?('Z'+lt.zoneId+': '+lt.sensorName):'None'),
    sub:(lt.timeAgoSec?fmDur(lt.timeAgoSec)+' ago':'No triggers yet')});
  let sAccent = ss.active>0?'red':(ss.fault>0?'yellow':'green');
  tiles.push({icon:'📊',title:'Sensors',accent:sAccent,
    value:'<div class="st-row"><span class="st-mini-dot off"></span>'+ss.idle+' Idle</div><div class="st-row"><span class="st-mini-dot on"></span>'+ss.active+' Active</div><div class="st-row"><span class="st-mini-dot off"></span>'+ss.fault+' Fault</div>',
    sub:''});
  let relayRows='';
  if(d.relays) d.relays.forEach((r,i)=>{
    let on=r.state;
    relayRows+='<div class="st-row"><span class="st-mini-dot '+(on?'on':'off')+'"></span>'+r.name+'</div>';
  });
  tiles.push({icon:'🔌',title:'Relays',accent:'blue',value:relayRows,sub:''});
  let heapIcon = d.heapFree>80000?'🟢':d.heapFree>50000?'🟡':d.heapFree>25000?'🟠':'🔴';
  tiles.push({icon:'🔧',title:'System',accent:'muted',
    value:'<div class="st-row">'+d.firmware+'</div><div class="st-row">Up: '+fmDur(d.uptime)+'</div><div class="st-row">'+heapIcon+' '+(d.heapFree/1024).toFixed(0)+' KB free</div>',
    sub:d.device});
  let nAccent = d.mqttConnected?'green':(d.wifi==='connected'?'yellow':'red');
  let rssiQ = d.rssi>=-50?'excellent':d.rssi>=-65?'good':d.rssi>=-75?'fair':'weak';
  let cqHtml='<div class="conn-quality"><div class="cq-bar">';
  for(let i=0;i<4;i++) cqHtml+='<div class="cq-seg '+(i<=(['weak','fair','good','excellent'].indexOf(rssiQ))?'active '+rssiQ:'')+'"></div>';
  cqHtml+='</div><span class="cq-label">'+d.rssi+' dBm</span></div>';
  tiles.push({icon:'🌐',title:'Network',accent:nAccent,
    value:'<div class="st-row">'+(d.ssid||'WiFi')+'</div>'+cqHtml+'<div class="st-row">MQTT '+(d.mqttConnected?'✅':'❌')+' · HA '+(d.haEnabled?'✅':'❌')+'</div>',
    sub:d.localIP||''});
  let h='';
  tiles.forEach(t=>{
    h+='<div class="stat-tile accent-'+t.accent+'"><div class="st-header"><div class="st-icon">'+t.icon+'</div><div class="st-title">'+t.title+'</div></div><div class="st-value">'+t.value+'</div>'+(t.sub?'<div class="st-sub">'+t.sub+'</div>':'')+'</div>';
  });
  document.getElementById('statGrid').innerHTML=h;
}

function renderAlertBanner(){
  let d=data, ss=d.sensorSummary||{};
  let el=document.getElementById('alertBanner');
  if(ss.active>0){
    let activeList=[];
    if(d.sensors) d.sensors.forEach(s=>{ if(s.state==='active') activeList.push(s.name||('T'+s.id)); });
    el.className='alert-banner danger';
    el.style.display='flex';
    el.innerHTML='<div class="ab-icon">🚨</div><div class="ab-text"><strong>'+ss.active+' Active Sensor'+(ss.active>1?'s':'')+'</strong><div class="ab-list">'+activeList.join(', ')+'</div></div>';
  } else if(ss.fault>0){
    el.className='alert-banner warn';
    el.style.display='flex';
    el.innerHTML='<div class="ab-icon">⚠️</div><div class="ab-text"><strong>'+ss.fault+' Sensor Fault'+(ss.fault>1?'s':'')+'</strong></div>';
  } else {
    el.style.display='none';
  }
}

function renderZoneSummary(){
  let d=data;
  if(!d.zones){ document.getElementById('zoneSummary').style.display='none'; return; }
  let enabled=d.zones.filter(z=>z.enabled!==false);
  let armed=enabled.filter(z=>z.armed).length;
  let total=enabled.length;
  if(total===0){ document.getElementById('zoneSummary').style.display='none'; return; }
  let pct=Math.round(armed/total*100);
  let fillColor = armed===total?'var(--green)':(armed>0?'var(--yellow)':'var(--muted)');
  document.getElementById('zoneSummary').style.display='flex';
  document.getElementById('zoneSummary').innerHTML='<div class="zs-label">🔒 Zones</div><div class="zs-bar"><div class="zs-fill" style="width:'+pct+'%;background:'+fillColor+'"></div></div><div class="zs-count">'+armed+' / '+total+' armed</div>';
}

function renderQuickActions(){
  let el=document.getElementById('quickActions');
  el.style.display='flex';
}

async function quickPanic(){
  // Toggle E16 sensor: if currently active → deactivate; if idle → activate
  let isActive = false;
  if (data.ext_sensors && data.ext_sensors.length >= 16) {
    isActive = data.ext_sensors[15].active;
  }
  let newState = isActive ? 'off' : 'on';
  await fetch('/api/extsensors/trigger?id=16&state=' + newState);
  // Update button label immediately (optimistic, before reload)
  let btn = document.getElementById('panicBtn');
  if (btn) btn.innerHTML = newState === 'on' ? '🛑 Panic OFF' : '🆘 Panic';
  load();
}
async function quickDisarmAll(){
  await fetch('/api/mode/set?mode=disarmed');
  load();
}

async function renderRecentEvents(){
  try{
    const r=await fetch('/api/eventlog');
    const a=await r.json();
    if(!a||!a.length){ document.getElementById('recentEventsCard').style.display='none'; return; }
    let top3=a.slice(0,3);
    let h='';
    top3.forEach(e=>{
      const cls=e.type===0?'alarm':e.type===2?'relay':e.type===3?'sensor':'system';
      const badge=e.type===0?'ALARM':e.type===2?'RELAY':e.type===3?'SENSOR':'SYS';
      const d=new Date(e.ts*1000);
      const ts=d.toLocaleTimeString([],{hour:'2-digit',minute:'2-digit'});
      h+='<div class="re-item"><span class="re-badge '+cls+'">'+badge+'</span><span class="re-desc">'+(e.desc||'')+'</span><span class="re-time">'+ts+'</span></div>';
    });
    document.getElementById('recentEvents').innerHTML=h;
    document.getElementById('recentEventsCard').style.display='block';
  }catch(e){}
}

async function load(){
  if(_pendingLoad) return;
  _pendingLoad=true;
  if(!_authChecked){
    _authChecked=true;
    let ok=await checkAuth();
    if(!ok){_pendingLoad=false;return;}
  }
  try{
    const r=await fetch('/api/status');
    if(r.status===401){window.location.href='/login.html';_pendingLoad=false;return;}
    data=await r.json();
  renderHeroStatus();
  renderAlertBanner();
  renderZoneSummary();
  renderQuickActions();
  renderStatTiles();
  renderModeGrid();
  renderRecentEvents();
  if(_authRole===0){
    renderZones(data.zones);
    renderSensors(data.sensors);
    renderRelays(data.relays);
    renderExtSensors(data.ext_sensors);
  }
    if(data.apMode) document.getElementById('apInfo').style.display='block';
  }catch(e){}
  _pendingLoad=false;
}

function renderModeGrid() {
  const modes = [
    { id: 'disarmed',            icon: '🔓', label: 'Disarmed' },
    { id: 'armed_home',          icon: '🏠', label: 'Armed Home' },
    { id: 'armed_away',          icon: '🚗', label: 'Armed Away' },
    { id: 'armed_night',         icon: '🌙', label: 'Armed Night' },
    { id: 'armed_vacation',      icon: '✈️', label: 'Armed Vac.' },
    { id: 'armed_custom_bypass', icon: '⚙️', label: 'Cust. Bypass' }
  ];
  const active = data.activeMode || 'disarmed';
  const gs = data.globalState || 'disarmed';
  let h = '';
  modes.forEach(m => {
    let cls = 'mode-card';
    if (m.id === active) cls += ' active';
    if (gs === 'triggered' && m.id === active) cls += ' triggered';
    if (gs === 'pending' && m.id === active) cls += ' pending';
    h += '<div class="' + cls + '" onclick="setMode(\'' + m.id + '\')"><div class="mc-icon">' + m.icon + '</div><div class="mc-label">' + m.label + '</div><div class="mc-dot"></div></div>';
  });
  document.getElementById('modeGrid').innerHTML = h;
}

async function setMode(mode) {
  await fetch('/api/mode/set?mode=' + encodeURIComponent(mode));
  load();
}

function renderExtSensors(a){
  let h='<div class="sensor-grid">';
  for(let row=0;row<2;row++){
    h+='<div class="sensor-row">';
    for(let i=row*8;i<Math.min(row*8+8,a.length);i++){
      let e=a[i];
      let en=e.enabled!==false;
      let st=en?(e.active?'active':'idle'):'disabled';
      let stateCls=en?(e.active?'active-state':''):'';
      h+=`<div class="ext-box ${st}">
<span class="edot ${en?(e.active?'active':'idle'):'disabled'}"></span>
<span class="elabel">E${e.id}</span>
<span class="estate ${stateCls}">${en?(e.active?'ACTIVE':'IDLE'):'OFF'}</span>
</div>`;
    }
    h+='</div>';
  }
  h+='</div>';
  document.getElementById('extensors').innerHTML=h;
}

function renderRelays(a){
  let h='<div class="relay-grid">';
  a.forEach(r=>{
    let on=r.state;
    let en=r.enabled!==false;
    h+=`<div class="relay-box ${on?'on':'off'}" style="${en?'':'opacity:0.5'}">
<span class="rdot ${on?'on':'off'}"></span>
<span class="rlabel">${r.name}</span>
<label class="toggle">
<input type="checkbox" ${on?'checked':''} onchange="toggleRelay(${r.id},this)">
<span class="toggle-slider"></span>
</label>
<label style="margin-top:4px;font-size:10px;display:inline-flex;align-items:center;gap:4px;color:#86868b">
<span>On</span>
<input type="checkbox" ${en?'checked':''} onchange="fetch('/api/relays/config',{method:'POST',body:new URLSearchParams({r${r.id}_enabled:this.checked?'1':'0'})}).then(()=>load())" style="width:14px;height:14px;margin:0;accent-color:#3498db">
</label>
</div>`;
  });
  h+='</div>';
  document.getElementById('relays').innerHTML=h;
}

function renderZones(a){
  let h='<div class="zone-grid">';
  a.forEach(z=>{
    if(z.enabled===false) return;
    let s=z.state;
    let armed=(s!=='disarmed');
    let toggleCls=(s==='alarm')?'alarm-slider':((!armed)?'disarm-slider':'');
    let labelCls=(s==='armed_idle')?'has-state':((s==='arming')?'arming-state':((s==='prealarm')?'prealarm-state':((s==='disarming')?'disarming-state':((s==='alarm')?'alarm-state':''))));
    let icon=(s==='alarm')?'🚨':(s==='prealarm'||s=='arming'||s=='disarming')?'⏳':(armed?'🔒':'');
    let sensors=z.sensors||'';
    let sensHtml=sensors?'<span style="font-size:9px;color:#aeaeb2;line-height:1.2">'+sensors+'</span>':'';
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
      let st=s.type==='disabled'?'disabled':s.state;
      let rawStr=s.type==='disabled'?'---':(s.raw+' mV');
      let stateLabel=s.type==='disabled'?'off':s.state;
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
  return '<span class="srange-bar" style="background:#e5e5ea"></span>';
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
<span id="raw_${s.id}" style="color:${s.type==0?'#aeaeb2':stateColor(s.state)}">${s.raw} mV</span>
</div>
</div>
<div class="sens-params-row">
<div class="zone-params zone-params-stack">
<div class="zone-params-title">Voltage thresholds</div>
<div class="zone-params-field"><span id="bar_sb_${s.id}">${srangeBar(s.raw,standbyLo,standbyHi,'standby')}</span><span>Idle:</span><input type="number" id="s${s.id}_standby_lo" value="${standbyLo}" placeholder="0" min="0" max="65535"><span>–</span><input type="number" id="s${s.id}_standby" value="${standbyHi}" placeholder="2000" min="0" max="65535"><span>mV</span></div>
<div class="zone-params-field"><span id="bar_dt_${s.id}">${srangeBar(s.raw,detectLo,typeof detectHi==='number'?detectHi:65535,'detected')}</span><span>Active:</span><input type="number" id="s${s.id}_detect" value="${detectLo}" placeholder="8000" min="0" max="65535"><span>–</span><input type="number" id="s${s.id}_detect_hi" value="${detectHi==='max'?'65535':detectHi}" placeholder="max" min="0" max="65535"><span>mV</span></div>
<div class="zone-params-field"><span id="bar_ft_${s.id}">${srangeBar(s.raw,faultLo,typeof faultHi==='number'?faultHi:65535,'faultbar')}</span><span>Fault:</span><input type="number" id="s${s.id}_fault" value="${faultLo}" placeholder="30000" min="0" max="65535"><span>–</span><input type="number" id="s${s.id}_fault_hi" value="${faultHi==='max'?'65535':faultHi}" placeholder="max" min="0" max="65535"><span>mV</span></div>
</div>
<div class="zone-params zone-params-stack">
<div class="zone-params-title">Timing</div>
<div class="zone-params-field"><span>Debouncing:</span><input type="number" id="s${s.id}_debounce" value="${s.debounceMs}" min="0" max="5000"><span>ms</span></div>
<div class="zone-params-field"><span>Active after:</span><input type="number" id="s${s.id}_ondelay" value="${s.onDelayMs}" min="0" max="60000"><span>ms</span></div>
<div class="zone-params-field"><span>Idle after:</span><input type="number" id="s${s.id}_offdelay" value="${s.offDelayMs}" min="0" max="60000"><span>ms</span></div>
</div>
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
<input type="text" id="z${z.id}_name" value="${z.name||''}" placeholder="Name" style="width:120px">
<span class="ztoggle"><input type="checkbox" id="z${z.id}_enabled" ${z.enabled?'checked':''}><span class="toggle-on"></span><span style="font-size:10px;color:#86868b;margin-left:4px">Enable</span></span>
</div>
<div class="zone-params">
<div class="zone-params-field"><span>Exit Delay:</span><input type="number" id="z${z.id}_exit" value="${z.exitDelayS}" min="0" max="120"><span>s</span></div>
<div class="zone-params-field"><span>Entry Delay:</span><input type="number" id="z${z.id}_entry" value="${z.entryDelayS}" min="0" max="120"><span>s</span></div>
<div class="zone-params-title">Siren cycle</div>
<div class="zone-params-field"><span>ON for:</span><input type="number" id="z${z.id}_sirenOn" value="${z.sirenOnS||0}" min="0" max="255"><span>s</span></div>
<div class="zone-params-field"><span>OFF for:</span><input type="number" id="z${z.id}_sirenOff" value="${z.sirenOffS||0}" min="0" max="255"><span>s</span></div>
<div class="zone-params-title">Alarm relay cycle</div>
<div class="zone-params-field"><span>ON for:</span><input type="number" id="z${z.id}_alarmRelayOnS" value="${z.alarmRelayOnS||0}" min="0" max="255"><span>s</span></div>
<div class="zone-params-field"><span>OFF for:</span><input type="number" id="z${z.id}_alarmRelayOffS" value="${z.alarmRelayOffS||0}" min="0" max="255"><span>s</span></div>
</div>
<div style="font-size:10px;color:#86868b;margin:4px 0;display:flex;gap:12px;flex-wrap:wrap">
<label style="display:inline-flex;align-items:center;gap:3px;margin-top:0;font-size:10px;color:#86868b">
<input type="checkbox" id="z${z.id}_sirenEnabled" ${z.sirenEnabled!==false?'checked':''}>Siren
</label>
<label style="display:inline-flex;align-items:center;gap:3px;margin-top:0;font-size:10px;color:#86868b">
<input type="checkbox" id="z${z.id}_alarmRelayEnabled" ${z.alarmRelayEnabled!==false?'checked':''}>Alarm Relay
</label>
<label style="display:inline-flex;align-items:center;gap:3px;margin-top:0;font-size:10px;color:var(--red);font-weight:600">
<input type="checkbox" id="z${z.id}_alwaysArmed" ${z.alwaysArmed?'checked':''} onchange="if(this.checked){if(!confirm('Always Armed zones cannot be disarmed. Siren/Alarm relay will fire on any sensor trigger in this zone. Continue?')){this.checked=false}}">⚠️ Always Armed
</label>
</div>
${sensors?`<div style="font-size:10px;color:#86868b;margin:2px 0">Sensors: ${sensors}</div>`:''}
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
    body.set('z'+i+'_sirenOn',document.getElementById('z'+i+'_sirenOn')?.value||'0');
    body.set('z'+i+'_sirenOff',document.getElementById('z'+i+'_sirenOff')?.value||'0');
    body.set('z'+i+'_enabled',document.getElementById('z'+i+'_enabled')?.checked?'1':'0');
    body.set('z'+i+'_sirenEnabled',document.getElementById('z'+i+'_sirenEnabled')?.checked?'1':'0');
    body.set('z'+i+'_alarmRelayEnabled',document.getElementById('z'+i+'_alarmRelayEnabled')?.checked?'1':'0');
    body.set('z'+i+'_alarmRelayOnS',document.getElementById('z'+i+'_alarmRelayOnS')?.value||'0');
    body.set('z'+i+'_alarmRelayOffS',document.getElementById('z'+i+'_alarmRelayOffS')?.value||'0');
    body.set('z'+i+'_alwaysArmed',document.getElementById('z'+i+'_alwaysArmed')?.checked?'1':'0');
  }
  const r=await fetch('/api/zones',{method:'POST',body});
  const d=await r.json();
  document.getElementById('zoneMsg').textContent=d.saved?'Saved.':d.error||'Error.';
  loadZones();
}

async function renderExtCards(a){
  let h='';
  a.forEach(e=>{
    let st=e.enabled?(e.active?'active':'idle'):'disabled';
    let stateLabel=e.enabled?(e.active?'Active':'Idle'):'disabled';
    h+=`<div class="sens-card ${st}" id="ecard_${e.id}">
<div class="sens-top">
<span class="sid">E${e.id}</span>
<input type="text" id="e${e.id}_name" value="${e.name||''}" placeholder="Name">
<span class="ztoggle"><input type="checkbox" id="e${e.id}_enabled" ${e.enabled?'checked':''}><span class="toggle-on"></span></span>
<div class="sens-live">
<span class="sdot ${e.enabled?(e.active?'active':'idle'):'none'}" id="edot_${e.id}"></span>
<span id="estate_${e.id}">${stateLabel}</span>
</div>
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
  if(data.mqtt_base) document.getElementById('extSubtitle').innerHTML='<strong>MQTT Topic:</strong> '+data.mqtt_base+'/ext_sensor/1..16 | Payload: active/idle (on/off)<br><strong>REST API:</strong><br><span style="font-family:monospace;font-size:10px">COOKIE_JAR=$(mktemp)</span><br><span style="font-family:monospace;font-size:10px">curl -s -c "$COOKIE_JAR" -X POST -d "user=api_user&pass=api_user" http://alarm.local/api/login</span><br><span style="font-family:monospace;font-size:10px">curl -s -b "$COOKIE_JAR" "http://alarm.local/api/extsensors/trigger?id=1&state=on"</span><br><span style="font-family:monospace;font-size:10px">curl -s -b "$COOKIE_JAR" "http://alarm.local/api/extsensors/trigger?id=1&state=off"</span>';
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

async function refreshExtLive(){
  if(_pendingRefresh) return;
  _pendingRefresh=true;
  try{
    const r=await fetch('/api/extsensors');
    const a=await r.json();
    a.forEach(e=>{
      let card=document.getElementById('ecard_'+e.id);
      if(!card) return;
      let st=e.enabled?(e.active?'active':'idle'):'disabled';
      card.className='sens-card '+st;
      let dot=document.getElementById('edot_'+e.id);
      if(dot){ dot.className='sdot '+(e.enabled?(e.active?'active':'idle'):'none'); }
      let stEl=document.getElementById('estate_'+e.id);
      if(stEl){ stEl.textContent=e.enabled?(e.active?'Active':'Idle'):'disabled'; }
    });
  }catch(e){}
  _pendingRefresh=false;
}

let sensorRefreshTimer=null;
async function refreshSensorLive(){
  if(_pendingRefresh) return;
  _pendingRefresh=true;
  try{
    const r=await fetch('/api/sensors');
    const a=await r.json();
    a.forEach(s=>{
      let card=document.getElementById('card_'+s.id);
      if(!card) return;
      let cls=s.type==='disabled'?'disabled':s.state;
      card.className='sens-card '+cls;
      let dot=document.getElementById('dot_'+s.id);
      if(dot){ dot.className='sdot '+(s.type==='disabled'?'none':dotClass(s.state)); }
      let st=document.getElementById('state_'+s.id);
      if(st){ st.textContent=s.type==='disabled'?'disabled':s.state; }
      let rw=document.getElementById('raw_'+s.id);
      if(rw){ rw.textContent=s.raw+' mV'; rw.style.color=s.type==='disabled'?'#aeaeb2':stateColor(s.state); }
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
  }catch(e){}
  _pendingRefresh=false;
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
let eventLogTimer=null;
let lastEventLogText='';
let eventLogFilter=-1;
function setEventLogFilter(f){
  eventLogFilter=f;
  lastEventLogText='';
  document.querySelectorAll('.filter-btn').forEach(b=>b.classList.remove('active'));
  const id=f===-1?'filterAll':'filter'+f;
  document.getElementById(id).classList.add('active');
  loadEventLog();
}
async function loadEventLog(){
  if(_pendingEventLog) return;
  _pendingEventLog=true;
  try{
    const r=await fetch('/api/eventlog');
    const txt=await r.text();
  const cacheKey=eventLogFilter+':'+txt;
  if(cacheKey===lastEventLogText){ _pendingEventLog=false; return; }
  lastEventLogText=cacheKey;
  let a=JSON.parse(txt);
  if(eventLogFilter>=0) a=a.filter(e=>e.type===eventLogFilter);
  const c=document.getElementById('eventLogContainer');
  if(!a.length){
    c.innerHTML='<div class="log-empty"><div>📋</div>No events recorded yet.</div>';
    document.getElementById('logFooter').textContent='';
    return;
  }
  let h='';
  a.forEach(e=>{
    const cls=e.type===0?'alarm':e.type===2?'relay':e.type===3?'sensor':'system';
    const badge=e.type===0?'🔴 ALARM':e.type===2?'🟠 RELAY':e.type===3?'🟢 SENSOR':'🔵 SYSTEM';
    const d=new Date(e.ts*1000);
    const ts=d.toLocaleDateString()+' '+d.toLocaleTimeString([],{hour:'2-digit',minute:'2-digit',second:'2-digit'});
    h+=`<div class="log-row ${cls}">
<span class="log-badge ${cls}">${badge}</span>
<span class="log-time">${ts}</span>
<span class="log-desc">${e.desc||''}</span>
</div>`;
  });
  c.innerHTML=h;
    document.getElementById('logFooter').textContent=`Showing ${a.length} events · 30-day retention · persisted in LittleFS`;
  }catch(e){}
  _pendingEventLog=false;
}
async function clearEventLog(){
  if(!confirm('Permanently clear all event logs?')) return;
  await fetch('/api/eventlog/clear',{method:'POST'});
  loadEventLog();
}
const modeLabels = {
  'disarmed':            '🔓 Disarmed',
  'armed_home':          '🏠 Armed Home',
  'armed_away':          '🚗 Armed Away',
  'armed_night':         '🌙 Armed Night',
  'armed_vacation':      '✈️ Armed Vacation',
  'armed_custom_bypass': '⚙️ Custom Bypass'
};

function renderAlarmModes(modes) {
  let h = '<div style="overflow-x:auto">';
  h += '<table style="width:100%;border-collapse:collapse">';
  h += '<thead><tr>';
  h += '<th style="text-align:left;padding:10px 12px;font-size:13px;color:#86868b;font-weight:600;width:200px">Alarm Mode</th>';
  for (let z = 1; z <= 8; z++) {
    let zn = 'Z' + z;
    // try to get zone name from the data if loaded
    if (data.zones && data.zones[z-1] && data.zones[z-1].name) {
      zn = data.zones[z-1].name;
    }
    h += '<th style="text-align:center;padding:10px 8px;font-size:12px;color:#86868b;font-weight:500">' + zn + '</th>';
  }
  h += '</tr></thead><tbody>';

  modes.forEach((m, idx) => {
    const isDisarmed = m.mode === 'disarmed';
    const label = modeLabels[m.mode] || m.mode;
    h += '<tr style="' + (isDisarmed ? 'opacity:0.5;background:#f9f9fb' : '') + '">';
    h += '<td style="padding:14px 12px;font-weight:600;font-size:14px;white-space:nowrap;border-bottom:1px solid #f0f0f5">' + label + '</td>';
    for (let z = 0; z < 8; z++) {
      const checked = m.zones[z] ? 'checked' : '';
      const disabled = isDisarmed ? 'disabled' : '';
      h += '<td style="text-align:center;padding:12px 8px;border-bottom:1px solid #f0f0f5">';
      h += '<input type="checkbox" id="m' + idx + '_z' + (z+1) + '" ' + checked + ' ' + disabled;
      h += ' style="width:22px;height:22px;accent-color:#0071e3;cursor:' + (isDisarmed ? 'not-allowed' : 'pointer') + '"';
      if (!isDisarmed) h += ' onchange="validateAlarmModes()"';
      h += '>';
      h += '</td>';
    }
    h += '</tr>';
  });
  h += '</tbody></table></div>';
  document.getElementById('alarmModesMatrix').innerHTML = h;
}

function validateAlarmModes() {
  let warnings = [];
  for (let m = 1; m <= 5; m++) {
    let anyChecked = false;
    for (let z = 1; z <= 8; z++) {
      const el = document.getElementById('m' + m + '_z' + z);
      if (el && el.checked) { anyChecked = true; break; }
    }
    if (!anyChecked) {
      const keys = Object.keys(modeLabels);
      const name = keys[m] || ('Mode ' + m);
      warnings.push(name + ' has no zones assigned');
    }
  }
  const el = document.getElementById('alarmModeMsg');
  if (warnings.length) {
    el.innerHTML = '<span style="color:#ff9500">⚠️ ' + warnings.join('<br>') + '</span>';
  } else {
    el.textContent = '';
  }
}

async function loadAlarmModes() {
  const r = await fetch('/api/alarmmodes');
  const modes = await r.json();
  renderAlarmModes(modes);
}

async function saveAlarmModes() {
  const body = new URLSearchParams();
  for (let m = 0; m < 6; m++) {
    for (let z = 1; z <= 8; z++) {
      const el = document.getElementById('m' + m + '_z' + z);
      body.set('m' + m + '_z' + z, el && el.checked ? '1' : '0');
    }
  }
  const r = await fetch('/api/alarmmodes', { method: 'POST', body });
  const d = await r.json();
  document.getElementById('alarmModeMsg').textContent = d.saved ? 'Saved.' : (d.error || 'Error.');
  loadAlarmModes();
}

function showTab(t){
  if(sensorRefreshTimer){clearInterval(sensorRefreshTimer);sensorRefreshTimer=null;}
  if(eventLogTimer){clearInterval(eventLogTimer);eventLogTimer=null;}
  document.querySelectorAll('.page').forEach(p=>p.classList.remove('active'));
  document.querySelectorAll('nav button').forEach(b=>b.classList.remove('active'));
  document.getElementById('page-'+t).classList.add('active');
  document.getElementById('tab-'+t).classList.add('active');
  if(t=='config')loadNetCfg();
  if(t=='sensors'){loadSensors();sensorRefreshTimer=setInterval(refreshSensorLive,2000);}
  if(t=='zones')loadZones();
  if(t=='extsensors'){loadExtSensors();sensorRefreshTimer=setInterval(refreshExtLive,2000);}
  if(t=='eventlog'){loadEventLog();eventLogTimer=setInterval(loadEventLog,10000);}
  if(t=='alarmmodes')loadAlarmModes();
  if(t=='users')loadUsers();
}
async function downloadBackup(){
  try{
    let r=await fetch('/api/backup');
    if(r.status===403){document.getElementById('bkMsg').textContent='Admin access required.';return;}
    let blob=await r.blob();
    let url=URL.createObjectURL(blob);
    let a=document.createElement('a');
    a.href=url;a.download='alarm-backup.json';
    document.body.appendChild(a);a.click();a.remove();
    URL.revokeObjectURL(url);
    document.getElementById('bkMsg').textContent='Backup downloaded.';
  }catch(e){document.getElementById('bkMsg').textContent='Download failed.';}
}

async function restoreBackup(){
  let file=document.getElementById('restoreFile').files[0];
  if(!file){document.getElementById('bkMsg').textContent='Select a .json backup file.';return;}
  if(!confirm('Restore will overwrite ALL settings and reboot. Continue?')) return;
  document.getElementById('bkMsg').textContent='Restoring...';
  try{
    let text=await file.text();
    let r=await fetch('/api/restore',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'data='+encodeURIComponent(text)});
    let d=await r.json();
    if(d.ok){
      document.getElementById('bkMsg').textContent='Restore complete. Restarting...';
      setTimeout(()=>fetch('/api/restart'),1000);
    }else{
      document.getElementById('bkMsg').textContent='Restore failed: '+(d.error||'Unknown error');
    }
  }catch(e){document.getElementById('bkMsg').textContent='Restore failed.';}
}

async function uploadFirmware(){
  const file=document.getElementById('otaFile').files[0];
  if(!file){ document.getElementById('otaMsg').textContent='Please select a .bin file.'; return; }
  if(!file.name.endsWith('.bin')){ document.getElementById('otaMsg').textContent='File must be a .bin firmware image.'; return; }
  document.getElementById('otaMsg').textContent='Uploading... '+Math.round(file.size/1024)+' kB';
  // Use multipart/form-data — ESPAsyncWebServer's upload handler requires it
  // (a raw body never triggers the multipart parser, so handleOTAUpload is
  // never called and the request hangs until timeout).
  const fd=new FormData();
  fd.append('firmware',file,file.name);
  const r=await fetch('/api/ota',{method:'POST',body:fd});
  if(r.ok){
    document.getElementById('otaMsg').textContent='Firmware flashed successfully. Device is restarting...';
  } else {
    document.getElementById('otaMsg').textContent='Upload failed: '+(await r.text());
  }
}
async function reconnect(){await fetch('/api/reconnect');load();}
async function restart(){if(confirm('Restart?'))await fetch('/api/restart');}
async function cmd(url){await fetch(url);loadFast();setTimeout(load,500);}

// ─── Optimistic relay toggle — updates UI immediately, fires API in background
async function toggleRelay(id, el) {
  let on = el.checked;
  let box = el.closest('.relay-box');
  if (box) {
    box.className = 'relay-box ' + (on ? 'on' : 'off');
    let dot = box.querySelector('.rdot');
    if (dot) dot.className = 'rdot ' + (on ? 'on' : 'off');
  }
  fetch('/api/relay/' + id + '?state=' + (on ? 'ON' : 'OFF'));
}

// ─── Fast poll: relays, zones, alarm state (~200 bytes JSON) at 1s ──────
async function loadFast() {
  try {
    const r = await fetch('/api/status-light');
    if (r.status === 401) return;
    const d = await r.json();
    data.wifi = d.wifi;
    data.activeMode = d.activeMode;
    data.globalState = d.globalState;
    data.mqttConnected = d.mqttConnected;
    if (d.relays && data.relays) {
    d.relays.forEach(r => {
      const e = data.relays.find(x => x.id === r.id);
      if (e) e.state = r.state;
    });
  }
    if (d.zones && data.zones) {
      d.zones.forEach(z => {
        const e = data.zones.find(x => x.id === z.id);
        if (e) { e.state = z.state; e.armed = z.armed; e.label = z.label; }
      });
    }
    renderHeroStatus();
    renderAlertBanner();
    renderZoneSummary();
    renderQuickActions();
    renderStatTiles();
    renderModeGrid();
    if (_authRole === 0) { renderRelays(data.relays); renderZones(data.zones); }
  } catch (e) {}
}
let _fastPollId = setInterval(loadFast, 1000);
load();let _fullPollId = setInterval(load, 3000);
</script></body></html>)rawliteral";

// ─── Documentation page HTML ────────────────────────────────────────────────

static const char DOCS_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Documentation - Home Alarm</title><style>
:root{--bg:#0d1117;--fg:#c9d1d9;--card:#161b22;--border:#30363d;--muted:#8b949e;--blue:#58a6ff;--green:#3fb950;--red:#f85149;--yellow:#d2991d}
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:-apple-system,BlinkMacSystemFont,"SF Pro Display","Helvetica Neue",sans-serif;background:var(--bg);color:var(--fg);min-height:100vh;-webkit-font-smoothing:antialiased}
.container{max-width:900px;margin:0 auto;padding:32px 24px}
h1{font-size:28px;font-weight:700;margin-bottom:4px;letter-spacing:-0.02em}
h2{font-size:18px;font-weight:600;margin:28px 0 12px;padding-bottom:8px;border-bottom:1px solid var(--border);letter-spacing:-0.01em}
h3{font-size:14px;font-weight:600;margin:18px 0 8px;color:var(--blue)}
p,li{font-size:14px;line-height:1.6;color:var(--fg);margin:6px 0}
ul,ol{margin:4px 0 12px 20px}
a{color:var(--blue);text-decoration:none}
a:hover{text-decoration:underline}
table{width:100%;border-collapse:collapse;margin:12px 0;font-size:13px}
th,td{padding:10px 12px;text-align:left;border-bottom:1px solid var(--border)}
th{font-weight:600;color:var(--muted);font-size:11px;text-transform:uppercase;letter-spacing:0.03em}
td{font-family:"SF Mono","Menlo","Monaco",monospace;font-size:13px}
td:first-child{font-weight:600;color:var(--blue)}
pre{background:var(--card);border:1px solid var(--border);border-radius:8px;padding:14px 18px;overflow-x:auto;font-size:12px;line-height:1.5;margin:10px 0;font-family:"SF Mono","Menlo","Monaco",monospace}
pre .c{color:var(--muted)}
code{font-family:"SF Mono","Menlo","Monaco",monospace;font-size:12px;background:var(--card);padding:2px 6px;border-radius:4px}
.card{background:var(--card);border:1px solid var(--border);border-radius:12px;padding:20px;margin:14px 0}
.badge{display:inline-block;padding:2px 8px;border-radius:6px;font-size:11px;font-weight:600}
.badge.green{background:var(--green);color:#000}
.badge.red{background:var(--red);color:#fff}
.badge.blue{background:var(--blue);color:#000}
.badge.yellow{background:var(--yellow);color:#000}
.ip-highlight{color:var(--green);font-weight:700}
nav{padding:16px 24px}
nav a{color:var(--muted);font-size:14px;font-weight:500}
@media(max-width:600px){.container{padding:16px 12px}pre{font-size:10px;padding:10px}td{font-size:11px}}
</style></head>
<body>
<nav><a href="/">← Back to Dashboard</a></nav>
<div class="container">
<h1>📖 Home Alarm System Documentation</h1>
<p style="color:var(--muted);margin-bottom:24px">Firmware <span id="fwVer">—</span> &middot; Device <span id="devId">—</span> &middot; IP <span class="ip-highlight" id="devIp">—</span></p>

<!-- ─── 1. HARDWARE ─────────────────────────────────────────────────────── -->
<h2>1. Hardware Description</h2>
<div class="card">
<h3>ESP32-C3-DevKitM-1</h3>
<table>
<tr><th>Spec</th><th>Value</th></tr>
<tr><td>MCU</td><td>ESP32-C3 RISC-V</td></tr>
<tr><td>Clock</td><td>160 MHz</td></tr>
<tr><td>RAM</td><td>400 KB SRAM</td></tr>
<tr><td>Flash</td><td>4 MB</td></tr>
<tr><td>WiFi</td><td>2.4 GHz 802.11 b/g/n</td></tr>
<tr><td>Filesystem</td><td>LittleFS</td></tr>
</table>

<h3>GPIO Pin Mapping</h3>
<table>
<tr><th>GPIO</th><th>Function</th><th>Notes</th></tr>
<tr><td>0</td><td>I2C SDA</td><td>ADS1115 bus</td></tr>
<tr><td>2</td><td>Status LED</td><td>Built-in (LOW=off, HIGH=on)</td></tr>
<tr><td>3</td><td>I2C SCL</td><td>ADS1115 bus</td></tr>
<tr><td>5</td><td>Relay Siren</td><td>Active LOW (LOW=ON)</td></tr>
<tr><td>6</td><td>Relay Pulse</td><td>Active LOW</td></tr>
<tr><td>7</td><td>Relay Tamper</td><td>Active LOW</td></tr>
<tr><td>8</td><td>Digital In ARM ZONE</td><td>Active LOW, ext. pull-up to 3.3V</td></tr>
<tr><td>9</td><td>Digital In DISARM ALL</td><td>Active LOW, ext. pull-up to 3.3V</td></tr>
<tr><td>10</td><td>Relay No-Power</td><td>Active LOW</td></tr>
<tr><td>20</td><td>Prealarm Output</td><td>Digital OUT</td></tr>
</table>

<h3>ADS1115 16-bit ADC (×4, I2C)</h3>
<table>
<tr><th>Address</th><th>Channels</th><th>Range</th></tr>
<tr><td>0x48</td><td>T1 – T4</td><td>±4.096 V (1 mV/LSB)</td></tr>
<tr><td>0x49</td><td>T5 – T8</td><td>±4.096 V</td></tr>
<tr><td>0x4A</td><td>T9 – T12</td><td>±4.096 V</td></tr>
<tr><td>0x4B</td><td>T13 – T16</td><td>±4.096 V</td></tr>
</table>
<p><strong>16 analog sensor inputs total.</strong> Each channel reads raw voltage in mV. Thresholds define detection ranges — idle (standby), active (detect), and fault. Each sensor can be assigned to one or more zones.</p>
</div>

<!-- ─── 2. CONFIGURATION GUIDE ──────────────────────────────────────────── -->
<h2>2. Configuration Guide</h2>

<div class="card">
<h3>2.1 First Boot &mdash; Access Point Mode</h3>
<ol>
<li>Power on the device. If no WiFi is configured, it starts in <strong>AP mode</strong>.</li>
<li>Connect to the WiFi network <code>Alarm-AP-XXXX</code> (password: <code>12345678</code>).</li>
<li>Open <code>http://192.168.4.1</code> in a browser.</li>
<li>Log in with default credentials: <code>admin</code> / <code>admin</code>.</li>
<li>You will be prompted to change the password on first login.</li>
</ol>
</div>

<div class="card">
<h3>2.2 Network Setup</h3>
<ol>
<li>Go to <strong>Config</strong> tab.</li>
<li>Enter your WiFi SSID and password.</li>
<li>Enter MQTT broker address (IP or hostname), port (default 1883), and credentials if required.</li>
<li>Click <strong>Save Config</strong>. The device will reconnect to your WiFi.</li>
<li>After reconnection, access the dashboard at the new LAN IP (check your router, or use <code>http://alarm.local</code> if mDNS is supported).</li>
</ol>
</div>

<div class="card">
<h3>2.3 Sensor Configuration</h3>
<ol>
<li>Go to <strong>Sensors</strong> tab.</li>
<li>For each sensor (T1–T16), set:<ul>
<li><strong>Type:</strong> PIR, Contactron (reed switch), or Off (disabled)</li>
<li><strong>Name:</strong> Descriptive label (e.g. "Front Door")</li>
<li><strong>Idle range:</strong> Voltage range considered normal/standby (e.g. 0–2000 mV). The LED bar goes <span style="color:var(--green)">green</span>.</li>
<li><strong>Active range:</strong> Voltage range that triggers detection (e.g. 4000–6000 mV). The LED bar goes <span style="color:var(--red)">red</span>. Set Max to 65535 for "no upper bound".</li>
<li><strong>Fault range:</strong> Voltage range that indicates a wiring fault or tamper (e.g. 30000–33000 mV). The LED bar goes <span style="color:var(--yellow)">yellow</span>. Set both min and max to 0 to disable.</li>
<li><strong>Debounce:</strong> Time (ms) the voltage must be stable before state change is accepted.</li>
<li><strong>Active after / Idle after:</strong> On/off delay timers (ms). Sensor must stay in the new range for this long before the state actually transitions.</li>
<li><strong>Zones:</strong> Check which zones this sensor belongs to.</li>
</ul></li>
<li>Click <strong>Update</strong> to save each sensor, or <strong>Save Sensors</strong> to save all.</li>
</ol>
<p style="color:var(--muted);font-size:12px;margin-top:6px">💡 The live voltage value (mV) and colored bar show you whether the current reading falls within each threshold range. Use this to calibrate your thresholds.</p>
</div>

<div class="card">
<h3>2.4 Zone Configuration</h3>
<ol>
<li>Go to <strong>Zones</strong> tab.</li>
<li>For each zone (Z1–Z8), configure:<ul>
<li><strong>Name:</strong> Descriptive label (e.g. "Perimeter")</li>
<li><strong>Enable:</strong> Toggle the zone on/off</li>
<li><strong>Exit Delay:</strong> Seconds after arming before sensors are monitored (0–120). Gives you time to leave.</li>
<li><strong>Entry Delay:</strong> Seconds after sensor triggers before alarm sounds (0–120). Gives you time to disarm.</li>
<li><strong>Siren cycle:</strong> ON/OFF times in seconds for the siren relay. Set ON=0 for continuous siren.</li>
<li><strong>Alarm relay cycle:</strong> ON/OFF times for the pulse relay during alarm.</li>
<li><strong>Siren checkbox:</strong> Enable siren relay for this zone.</li>
<li><strong>Alarm Relay checkbox:</strong> Enable pulse relay for this zone.</li>
</ul></li>
<li>Click <strong>Update</strong> to save each zone, or <strong>Save All Zones</strong> to save all.</li>
</ol>
</div>

<div class="card">
<h3>2.5 Alarm Mode Profiles</h3>
<ol>
<li>Go to <strong>Alarm Modes</strong> tab.</li>
<li>For each mode (Armed Home, Armed Away, Armed Night, Armed Vacation, Custom Bypass), check which zones should be active.</li>
<li>Disarmed mode is fixed — no zones are monitored.</li>
<li>Click <strong>Save Alarm Modes</strong>.</li>
</ol>
<p style="color:var(--muted);font-size:12px;margin-top:6px">💡 If a mode has no zones assigned, arming it will be rejected.</p>
</div>

<div class="card">
<h3>2.6 User Management</h3>
<ol>
<li>Go to <strong>Users</strong> tab (admin only).</li>
<li>Three roles available:<ul>
<li><strong>Admin:</strong> Full access to all settings and tabs.</li>
<li><strong>Operator:</strong> Can arm/disarm via dashboard but cannot change configuration.</li>
<li><strong>API:</strong> Can only trigger external sensors via REST API — no dashboard access.</li>
</ul></li>
<li>Each user has a username, password, and 4-digit PIN for keypad arming.</li>
<li>The system ships with <code>admin/admin</code> (Admin) and <code>api_user/api_user</code> (API).</li>
</ol>
</div>

<div class="card">
<h3>2.7 Backup & Restore</h3>
<ol>
<li>Go to <strong>Config</strong> tab → <strong>Backup & Restore</strong>.</li>
<li><strong>Download Backup:</strong> Exports all configuration + event log as a JSON file.</li>
<li><strong>Upload & Restore:</strong> Select a backup file and click restore. The device will reboot after restoring. <strong>All current settings will be overwritten.</strong></li>
<li>Backup/restore covers: WiFi, MQTT, users, sensors, zones, relays, external sensors, digital inputs, alarm mode profiles, and event log.</li>
</ol>
</div>

<!-- ─── 3. HOME ASSISTANT ───────────────────────────────────────────────── -->
<h2>3. Home Assistant Integration</h2>

<div class="card">
<h3>3.1 Enable Auto-Discovery</h3>
<ol>
<li>Go to <strong>Config</strong> tab.</li>
<li>Ensure MQTT is configured and connected (check the Dashboard status tile shows MQTT ✅).</li>
<li>Auto-discovery is enabled by default. If disabled, toggle <strong>HA Discovery</strong> in config and save.</li>
<li>The device publishes MQTT discovery messages on connect. In Home Assistant, go to <strong>Settings → Devices & Services → MQTT</strong> — you should see the alarm device with all its entities.</li>
</ol>
</div>

<div class="card">
<h3>3.2 Auto-Discovered Entities</h3>
<table>
<tr><th>Entity</th><th>Type</th><th>Description</th></tr>
<tr><td>alarm_control_panel.home_alarm</td><td>Alarm Panel</td><td>Arm/disarm with modes: disarmed, armed_home, armed_away, armed_night, armed_vacation, armed_custom_bypass</td></tr>
<tr><td>binary_sensor.*</td><td>Binary Sensor (×16)</td><td>One per wired sensor input (T1–T16). State: on/off</td></tr>
<tr><td>binary_sensor.*</td><td>Binary Sensor (×16)</td><td>One per external MQTT sensor (E1–E16)</td></tr>
<tr><td>switch.*</td><td>Switch (×4)</td><td>Relay control (Siren, Pulse, Tamper, No-Power)</td></tr>
<tr><td>sensor.*</td><td>Sensor</td><td>WiFi RSSI, uptime, heap free</td></tr>
</table>
<p style="color:var(--muted);font-size:12px;margin-top:6px">💡 Use the Alarm Panel card in your HA dashboard for arm/disarm control. Sensor entities update in real-time via MQTT.</p>
</div>

<div class="card">
<h3>3.3 Manual MQTT Topics</h3>
<p>All topics are prefixed with <code id="mqttBase">homealarm/XXXXXX</code> (shown on the dashboard).</p>
<table>
<tr><th>Topic</th><th>Direction</th><th>Payload</th></tr>
<tr><td>cmd/mode</td><td>HA → Device</td><td>DISARM, ARM_HOME, ARM_AWAY, ARM_NIGHT, ARM_VACATION, ARM_CUSTOM_BYPASS</td></tr>
<tr><td>state</td><td>Device → HA</td><td>disarmed, armed_home, armed_away, armed_night, armed_vacation, armed_custom_bypass, pending, triggered</td></tr>
<tr><td>cmd/relay/1..4</td><td>HA → Device</td><td>ON / OFF</td></tr>
<tr><td>ext_sensor/1..16</td><td>HA → Device</td><td>active / idle / on / off / 1 / 0</td></tr>
<tr><td>zones/1..8/state</td><td>Device → HA</td><td>disarmed, armed_idle, arming, disarming, alarm</td></tr>
<tr><td>sensors/1..16/state</td><td>Device → HA</td><td>idle, active, fault</td></tr>
<tr><td>status/relay/1..4</td><td>Device → HA</td><td>ON / OFF</td></tr>
<tr><td>status/wifi</td><td>Device → HA</td><td>connected / ap / disconnected</td></tr>
<tr><td>status/rssi</td><td>Device → HA</td><td>dBm value</td></tr>
</table>
<p style="color:var(--muted);font-size:12px;margin-top:6px">💡 For reliable arm/disarm via HA, publish <code>cmd/mode</code> with <code>retain: true</code> so the command survives MQTT reconnections.</p>
</div>

<!-- ─── 4. API & MQTT REFERENCE ─────────────────────────────────────────── -->
<h2>4. API & MQTT Reference</h2>

<div class="card">
<h3>4.1 MQTT — External Sensor Trigger</h3>
<p>Publish to the device's MQTT topic to activate an external sensor. No authentication required beyond MQTT broker credentials. This is the preferred method for zero-latency triggers when the device is already connected.</p>

<p><strong>Topic:</strong> <code id="mqttExt">homealarm/XXXXXX/ext_sensor/1</code></p>
<p><strong>Payload:</strong> <code>active</code>, <code>idle</code>, <code>on</code>, <code>off</code>, <code>1</code>, or <code>0</code></p>

<pre><span class="c"># Activate external sensor E1 via MQTT (using mosquitto_pub)</span>
mosquitto_pub -h MQTT_BROKER_IP -t "<span id="mqttExt2">homealarm/XXXXXX/ext_sensor/1</span>" \
  -m "active"

<span class="c"># Deactivate</span>
mosquitto_pub -h MQTT_BROKER_IP -t "<span id="mqttExt3">homealarm/XXXXXX/ext_sensor/1</span>" \
  -m "idle"</pre>

<p style="color:var(--muted);font-size:12px;margin-top:6px">💡 Sensors 1–16 are available. Replace the number in the topic path.</p>
</div>

<div class="card">
<h3>4.2 REST API — External Sensor Trigger</h3>
<p>Use the API user (<code>api_user</code> / <code>api_user</code> by default) to trigger external sensors from scripts or other systems.</p>

<p><strong>Step 1: Authenticate (get session cookie)</strong></p>
<pre><span class="c"># Replace DEVICE_IP with the actual IP shown at the top of this page</span>
COOKIE_JAR=$(mktemp)
curl -s -c "$COOKIE_JAR" -X POST \
  -d "user=api_user&pass=api_user" \
  http://<span class="ip-highlight" id="ipLogin">DEVICE_IP</span>/api/login</pre>

<p><strong>Step 2: Activate external sensor</strong></p>
<pre><span class="c"># Trigger sensor E1 as active</span>
curl -s -b "$COOKIE_JAR" \
  "http://<span class="ip-highlight" id="ipOn">DEVICE_IP</span>/api/extsensors/trigger?id=1&state=on"</pre>

<p><strong>Step 3: Deactivate external sensor</strong></p>
<pre><span class="c"># Trigger sensor E1 as idle</span>
curl -s -b "$COOKIE_JAR" \
  "http://<span class="ip-highlight" id="ipOff">DEVICE_IP</span>/api/extsensors/trigger?id=1&state=off"</pre>

<p style="color:var(--muted);font-size:12px;margin-top:6px">💡 Replace <code>?id=1</code> with the external sensor number (1–16). Replace <code>state=on</code> with <code>state=off</code> to deactivate.<br>
💡 The session cookie expires after 30 minutes of inactivity. Re-authenticate if you get a 401.</p>
</div>

<div class="card">
<h3>4.3 REST API Endpoints</h3>
<table>
<tr><th>Method</th><th>Endpoint</th><th>Auth</th><th>Description</th></tr>
<tr><td>POST</td><td>/api/login</td><td>—</td><td>Authenticate (form: user, pass)</td></tr>
<tr><td>POST</td><td>/api/logout</td><td>Session</td><td>Destroy session</td></tr>
<tr><td>GET</td><td>/api/auth-status</td><td>—</td><td>Check auth state</td></tr>
<tr><td>GET</td><td>/api/status</td><td>Session</td><td>Full system status JSON</td></tr>
<tr><td>GET</td><td>/api/status-light</td><td>Session</td><td>Lightweight status (relays, zones, alarm state)</td></tr>
<tr><td>GET/POST</td><td>/api/sensors</td><td>Admin</td><td>Sensor configuration</td></tr>
<tr><td>GET/POST</td><td>/api/zones</td><td>Session</td><td>Zone configuration</td></tr>
<tr><td>GET/POST</td><td>/api/extsensors</td><td>Session</td><td>External sensor configuration</td></tr>
<tr><td>GET/POST</td><td>/api/alarmmodes</td><td>Admin</td><td>Alarm mode profiles</td></tr>
<tr><td>GET/POST</td><td>/api/network</td><td>Admin</td><td>WiFi/MQTT configuration</td></tr>
<tr><td>GET</td><td>/api/mode/set?mode=MODE</td><td>Session</td><td>Arm/disarm (see mode table below)</td></tr>
<tr><td>GET</td><td>/api/extsensors/trigger?id=N&state=S</td><td>Session</td><td>Trigger external sensor (API role or higher)</td></tr>
<tr><td>GET</td><td>/api/relay/N?state=S</td><td>Session</td><td>Manual relay control</td></tr>
<tr><td>GET/POST</td><td>/api/relays/config</td><td>Admin</td><td>Relay configuration</td></tr>
<tr><td>GET</td><td>/api/eventlog</td><td>Session</td><td>Event log (JSON)</td></tr>
<tr><td>POST</td><td>/api/eventlog/clear</td><td>Admin</td><td>Clear event log</td></tr>
<tr><td>GET</td><td>/api/backup</td><td>Admin</td><td>Download backup JSON</td></tr>
<tr><td>POST</td><td>/api/restore</td><td>Admin</td><td>Restore from backup</td></tr>
<tr><td>POST</td><td>/api/ota</td><td>Admin</td><td>Firmware upload (multipart form)</td></tr>
<tr><td>GET</td><td>/api/restart</td><td>Admin</td><td>Soft restart</td></tr>
<tr><td>GET</td><td>/api/reconnect</td><td>Admin</td><td>Reconnect WiFi</td></tr>
<tr><td>GET</td><td>/api/users</td><td>Session</td><td>List users</td></tr>
<tr><td>POST</td><td>/api/users/add</td><td>Admin</td><td>Add user</td></tr>
<tr><td>POST</td><td>/api/users/delete</td><td>Admin</td><td>Delete user</td></tr>
<tr><td>POST</td><td>/api/change-password</td><td>Session</td><td>Change own password</td></tr>
</table>
</div>

<div class="card">
<h3>4.4 Alarm Mode Values</h3>
<table>
<tr><th>Mode String</th><th>Description</th></tr>
<tr><td>disarmed</td><td>All zones disarmed</td></tr>
<tr><td>armed_home</td><td>Perimeter zones armed (interior excluded)</td></tr>
<tr><td>armed_away</td><td>All zones armed</td></tr>
<tr><td>armed_night</td><td>Bedroom perimeter armed</td></tr>
<tr><td>armed_vacation</td><td>All zones armed (extended away)</td></tr>
<tr><td>armed_custom_bypass</td><td>Custom zone selection</td></tr>
</table>
<p style="color:var(--muted);font-size:12px;margin-top:6px">💡 State values published by the device also include <code>pending</code> (arming/disarming in progress) and <code>triggered</code> (alarm active).</p>
</div>

</div>
<script>
fetch('/api/status').then(r=>r.json()).then(d=>{
  document.getElementById('fwVer').textContent = d.firmware||'—';
  document.getElementById('devId').textContent = d.device||'—';
  let ip = d.localIP||'DEVICE_IP';
  document.getElementById('devIp').textContent = ip;
  document.getElementById('ipLogin').textContent = ip;
  document.getElementById('ipOn').textContent = ip;
  document.getElementById('ipOff').textContent = ip;
  if(d.mqtt_base){
    document.getElementById('mqttBase').textContent = d.mqtt_base;
    document.getElementById('mqttExt').textContent = d.mqtt_base+'/ext_sensor/1';
    document.getElementById('mqttExt2').textContent = d.mqtt_base+'/ext_sensor/1';
    document.getElementById('mqttExt3').textContent = d.mqtt_base+'/ext_sensor/1';
  }
});
</script>
</body></html>)rawliteral";

// ─── Login page HTML ───────────────────────────────────────────────────────

static const char LOGIN_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Login - Home Alarm</title><style>
:root{--bg:#0d1117;--fg:#c9d1d9;--card:#161b22;--border:#30363d;--muted:#8b949e;--blue:#58a6ff;--green:#3fb950;--red:#f85149;--yellow:#d2991d}
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,BlinkMacSystemFont,"SF Pro Display","Helvetica Neue",sans-serif;background:var(--bg);color:var(--fg);display:flex;align-items:center;justify-content:center;min-height:100vh;-webkit-font-smoothing:antialiased}
.login-card{background:var(--card);border:1px solid var(--border);border-radius:14px;padding:40px 36px;width:100%;max-width:380px}
.login-card h1{font-size:28px;font-weight:700;text-align:center;margin-bottom:6px;letter-spacing:-0.02em;color:var(--fg)}
.login-card .sub{font-size:13px;color:var(--muted);text-align:center;margin-bottom:24px}
.login-card label{display:block;margin-top:12px;font-size:12px;color:var(--muted);font-weight:500;letter-spacing:-0.01em}
.login-card input{width:100%;padding:12px 14px;margin-top:4px;border:1px solid var(--border);border-radius:8px;background:var(--bg);color:var(--fg);font-size:15px;font-family:inherit;transition:border-color 0.15s}
.login-card input:focus{outline:none;border-color:var(--blue);background:var(--bg)}
.login-card button{width:100%;padding:12px;margin-top:20px;border:none;border-radius:10px;background:var(--blue);color:#fff;font-size:15px;font-weight:600;cursor:pointer;letter-spacing:-0.01em;transition:opacity 0.2s,transform 0.1s}
.login-card button:hover{opacity:0.88}
.login-card button:active{transform:scale(0.97)}
.login-card button:disabled{opacity:0.5;cursor:not-allowed}
.msg{margin-top:12px;font-size:13px;text-align:center;color:var(--red);font-weight:500}
.msg.success{color:var(--green)}
.msg.warn{color:var(--yellow)}
.pw-toggle{position:relative}
.pw-toggle input{padding-right:44px}
.pw-toggle .eye{position:absolute;right:12px;top:50%;transform:translateY(0%);cursor:pointer;font-size:18px;user-select:none;opacity:0.5;transition:opacity 0.2s}
.pw-toggle .eye:hover{opacity:1}
.subtext{text-align:center;margin-top:20px;font-size:11px;color:var(--muted)}
</style></head>
<body>
<div class="login-card">
<h1>🔒 Home Alarm</h1>
<div class="sub">Sign in to access the dashboard</div>
<label>Username</label>
<input type="text" id="user" placeholder="Username" value="admin" autofocus>
<label>Password</label>
<div class="pw-toggle">
<input type="password" id="pass" placeholder="Enter password" autofocus>
<span class="eye" onclick="togglePw()">👁</span>
</div>
<button id="loginBtn" onclick="doLogin()">Sign In</button>
<div id="msg" class="msg"></div>
<div class="subtext">Default: admin / admin</div>
</div>
<script>
let lastUser = localStorage.getItem('lastUser');
if (lastUser) document.getElementById('user').value = lastUser;

function togglePw(){
  let e=document.getElementById('pass');
  e.type=e.type==='password'?'text':'password';
}
async function doLogin(){
  let btn=document.getElementById('loginBtn');
  let msg=document.getElementById('msg');
  btn.disabled=true;
  btn.textContent='Signing in...';
  msg.textContent='';
  try{
    let r=await fetch('/api/login',{
      method:'POST',
      headers:{'Content-Type':'application/x-www-form-urlencoded'},
      body:'user='+encodeURIComponent(document.getElementById('user').value)+
           '&pass='+encodeURIComponent(document.getElementById('pass').value)
    });
    let d=await r.json();
    if(d.ok){
      localStorage.setItem('lastUser', document.getElementById('user').value);
      msg.className='msg success';
      msg.textContent='Logged in. Redirecting...';
      setTimeout(()=>window.location.href='/',500);
    } else if(d.error==='locked'){
      msg.className='msg warn';
      msg.textContent='Too many failed attempts. Try again in '+d.retry_after_sec+' seconds.';
      btn.disabled=false;
      btn.textContent='Sign In';
    } else {
      msg.className='msg';
      msg.textContent=d.message||'Wrong password.';
      btn.disabled=false;
      btn.textContent='Sign In';
      document.getElementById('pass').value='';
      document.getElementById('pass').focus();
    }
  }catch(e){
    msg.className='msg';
    msg.textContent='Connection error. Is the device reachable?';
    btn.disabled=false;
    btn.textContent='Sign In';
  }
}
document.getElementById('pass').addEventListener('keydown',function(e){
  if(e.key==='Enter') doLogin();
});
</script>
</body></html>)rawliteral";

// ─── Auth API handlers ────────────────────────────────────────────────────

static void apiLogin(AsyncWebServerRequest *req) {
  String user = req->arg("user");
  String pass = req->arg("pass");

  if (user.length() == 0 || pass.length() == 0) {
    req->send(400, "application/json", "{\"error\":\"missing_fields\",\"message\":\"Username and password required\"}");
    return;
  }

  // Rate limit check
  String ip = getClientIP(req);
  int retryAfter = checkRateLimit(ip);
  if (retryAfter > 0) {
    String resp;
    resp.reserve(64);
    resp = "{\"error\":\"locked\",\"retry_after_sec\":" + String(retryAfter) + ",\"message\":\"Too many failed attempts. Try again later.\"}";
    req->send(429, "application/json", resp);
    return;
  }

  // Verify credentials against users array
  UserEntry* u = verifyCredentials(user.c_str(), pass.c_str());
  if (u) {
    resetFailedAttempts(ip);
    String session = createSession(u->username, u->role);

    String setCookie = String(SESSION_COOKIE_NAME) + "=" + session +
                       "; Path=/; HttpOnly; SameSite=Lax; Max-Age=" + String(SESSION_TIMEOUT_SEC);
    AsyncWebServerResponse *resp = req->beginResponse(200, "application/json", "{\"ok\":true}");
    resp->addHeader("Set-Cookie", setCookie);
    req->send(resp);

    char logBuf[80];
    snprintf(logBuf, sizeof(logBuf), "Login %s from %s", u->username, ip.c_str());
    logSystem(logBuf);
  } else {
    recordFailedAttempt(ip);

    char logBuf[80];
    snprintf(logBuf, sizeof(logBuf), "Failed login %s from %s", user.c_str(), ip.c_str());
    logSystem(logBuf);

    req->send(401, "application/json", "{\"error\":\"wrong_password\",\"message\":\"Invalid username or password\"}");
  }
}

static void apiLogout(AsyncWebServerRequest *req) {
  String token;
  if (req->hasHeader("Cookie")) {
    String cookie = req->getHeader("Cookie")->value();
    String search = String(SESSION_COOKIE_NAME) + "=";
    int start = cookie.indexOf(search);
    if (start >= 0) {
      start += search.length();
      int end = cookie.indexOf(';', start);
      if (end < 0) end = cookie.length();
      token = cookie.substring(start, end);
    }
  }
  if (token.length() > 0) {
    destroySession(token.c_str());
  }

  // Clear cookie
  String clearCookie = String(SESSION_COOKIE_NAME) + "=; Path=/; HttpOnly; SameSite=Lax; Max-Age=0";
  AsyncWebServerResponse *resp = req->beginResponse(200, "application/json", "{\"ok\":true}");
  resp->addHeader("Set-Cookie", clearCookie);
  req->send(resp);
  logSystem("Logout");
}

static void apiAuthStatus(AsyncWebServerRequest *req) {
  JsonDocument doc;
  bool authenticated = false;
  uint8_t role = USER_ROLE_OPERATOR;
  bool forcePwChange = false;
  const char* username = "";

  String token;
  if (req->hasHeader("Cookie")) {
    String cookie = req->getHeader("Cookie")->value();
    String search = String(SESSION_COOKIE_NAME) + "=";
    int start = cookie.indexOf(search);
    if (start >= 0) {
      start += search.length();
      int end = cookie.indexOf(';', start);
      if (end < 0) end = cookie.length();
      token = cookie.substring(start, end);
    }
  }

  if (token.length() > 0 && validateSession(token.c_str())) {
    authenticated = true;
    role = getSessionRole(token.c_str());
    touchSession(token.c_str());
    // Extract username from session
    username = getSessionUsername(token.c_str());
    // Check if the logged-in user's hash matches default "admin" hash
    // This serves as the forcePasswordChange indicator
    String defaultHash = hashPassword("admin");
    for (int i = 0; i < MAX_USERS; i++) {
      if (config.users[i].active && strcmp(config.users[i].passwordHash, defaultHash.c_str()) == 0) {
        forcePwChange = true;
        break;
      }
    }
  }

  doc["authenticated"]       = authenticated;
  doc["role"]                = role;
  doc["username"]            = username;
  doc["forcePasswordChange"] = forcePwChange;

  String buf;
  serializeJson(doc, buf);
  req->send(200, "application/json", buf);
}

static void apiChangePassword(AsyncWebServerRequest *req) {
  String current = req->arg("current");
  String newPass = req->arg("new");

  if (current.length() == 0 || newPass.length() == 0) {
    req->send(400, "application/json", "{\"error\":\"missing_fields\",\"message\":\"Current and new password required\"}");
    return;
  }

  if (newPass.length() < 4) {
    req->send(400, "application/json", "{\"error\":\"password_too_short\",\"message\":\"Password must be at least 4 characters\"}");
    return;
  }

  if (newPass.length() > 32) {
    req->send(400, "application/json", "{\"error\":\"password_too_long\",\"message\":\"Password must be at most 32 characters\"}");
    return;
  }

  // Find the user whose password matches the current password
  for (int i = 0; i < MAX_USERS; i++) {
    if (!config.users[i].active) continue;
    if (verifyPassword(current.c_str(), config.users[i].passwordHash)) {
      // Found the user — update their password
      String newHash = hashPassword(newPass.c_str());
      strlcpy(config.users[i].passwordHash, newHash.c_str(), sizeof(config.users[i].passwordHash));
      config.forcePasswordChange = false;
      saveConfig();  // immediate — password change must persist before response
      logSystem("Password changed");
      req->send(200, "application/json", "{\"ok\":true,\"message\":\"Password changed successfully\"}");
      return;
    }
  }

  req->send(401, "application/json", "{\"error\":\"wrong_password\",\"message\":\"Current password is incorrect\"}");
}

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
      e["active"]   = extSensorStates[i].active;
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
    requestSaveConfig();
    req->send(200, "application/json", "{\"ok\":true,\"saved\":true}");
  }
}

// ─── Alarm Modes config API ─────────────────────────────────────────────────

static void apiAlarmModesConfig(AsyncWebServerRequest *req) {
  if (req->method() == HTTP_GET) {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    const char* modeNames[6] = {
      "disarmed", "armed_home", "armed_away",
      "armed_night", "armed_vacation", "armed_custom_bypass"
    };
    for (int m = 0; m < 6; m++) {
      JsonObject obj = arr.add<JsonObject>();
      obj["mode"]     = modeNames[m];
      obj["zoneMask"] = config.modeProfiles[m].zoneMask;
      obj["defined"]  = config.modeProfiles[m].defined;
      JsonArray zones = obj["zones"].to<JsonArray>();
      for (int z = 0; z < MAX_ZONES; z++) {
        zones.add((bool)(config.modeProfiles[m].zoneMask & (1U << z)));
      }
    }
    String buf;
    serializeJson(doc, buf);
    req->send(200, "application/json", buf);
  } else if (req->method() == HTTP_POST) {
    for (int m = 0; m < 6; m++) {
      uint8_t mask = 0;
      for (int z = 0; z < MAX_ZONES; z++) {
        String key = "m" + String(m) + "_z" + String(z + 1);
        if (req->arg(key) == "1") {
          mask |= (1U << z);
        }
      }
      config.modeProfiles[m].zoneMask = mask;
      config.modeProfiles[m].defined = true;
    }
    // DISARMED always has empty mask regardless of form input
    config.modeProfiles[(uint8_t)AlarmMode::DISARMED].zoneMask = 0;
    config.modeProfiles[(uint8_t)AlarmMode::DISARMED].defined  = true;

    requestSaveConfig();
    req->send(200, "application/json", "{\"ok\":true,\"saved\":true}");
  }
}

// ─── Zones config API ──────────────────────────────────────────────────────

static void apiZonesConfig(AsyncWebServerRequest *req) {
  if (req->method() == HTTP_GET) {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (int i = 0; i < MAX_ZONES; i++) {
      yield();
      JsonObject z = arr.add<JsonObject>();
      z["id"]          = i + 1;
      z["name"]        = config.zones[i].name;
      z["entryDelayS"] = config.zones[i].entryDelayS;
      z["exitDelayS"]  = config.zones[i].exitDelayS;
      z["sirenOnS"]    = config.zones[i].sirenOnS;
      z["sirenOffS"]   = config.zones[i].sirenOffS;
      z["enabled"]     = config.zones[i].enabled;
      z["sirenEnabled"]       = config.zones[i].sirenEnabled;
      z["alarmRelayEnabled"]  = config.zones[i].alarmRelayEnabled;
      z["alarmRelayOnS"]      = config.zones[i].alarmRelayOnS;
      z["alarmRelayOffS"]     = config.zones[i].alarmRelayOffS;
      z["alwaysArmed"]        = config.zones[i].alwaysArmed;
    // Collect associated sensor labels
    String sensList;
    sensList.reserve(200);
    for (int s = 0; s < TOTAL_SENSORS; s++) {
      if (config.sensors[s].type != SENSOR_DISABLED && (config.sensors[s].zoneMask & (1U << i))) {
        if (sensList.length()) sensList += ", ";
        sensList += "T" + String(s + 1);
      }
    }
    yield();
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
      if (req->hasArg((prefix + "_sirenOn").c_str())) {
        config.zones[i].sirenOnS = req->arg((prefix + "_sirenOn").c_str()).toInt();
      }
      if (req->hasArg((prefix + "_sirenOff").c_str())) {
        config.zones[i].sirenOffS = req->arg((prefix + "_sirenOff").c_str()).toInt();
      }
      config.zones[i].enabled = (req->arg((prefix + "_enabled").c_str()) != "0");
      config.zones[i].sirenEnabled = (req->arg((prefix + "_sirenEnabled").c_str()) != "0");
      config.zones[i].alarmRelayEnabled = (req->arg((prefix + "_alarmRelayEnabled").c_str()) != "0");
      config.zones[i].alarmRelayOnS = (uint8_t)req->arg((prefix + "_alarmRelayOnS").c_str()).toInt();
      config.zones[i].alarmRelayOffS = (uint8_t)req->arg((prefix + "_alarmRelayOffS").c_str()).toInt();
      config.zones[i].alwaysArmed = (req->arg((prefix + "_alwaysArmed").c_str()) == "1");
    }
    requestSaveConfig();
    req->send(200, "application/json", "{\"ok\":true,\"saved\":true}");
  }
}

// ─── OTA firmware upload handler ───────────────────────────────────────────

static void handleOTAUpload(AsyncWebServerRequest *req, String filename, size_t index, uint8_t *data, size_t len, bool final) {
  static bool otaFailed = false;
  if (otaFailed) return;  // silently discard chunks after a failure

  if (!index) {
    otaFailed = false;
    size_t totalSize = req->contentLength();
    if (totalSize == 0) totalSize = UPDATE_SIZE_UNKNOWN;
    Serial.printf("[OTA] Upload starting: %s (%u bytes)\n", filename.c_str(), totalSize);
    if (!Update.begin(totalSize, U_FLASH)) {
      Update.printError(Serial);
      req->send(500, "text/plain", "OTA begin failed");
      otaFailed = true;
      return;
    }
  }

  // Progress logging every ~25% based on contentLength
  static size_t otaWritten = 0;
  static size_t otaTotal   = 0;
  static int    otaNextPct = 25;

  if (len > 0) {
    if (Update.write(data, len) != len) {
      Update.printError(Serial);
      req->send(500, "text/plain", "OTA write failed");
      otaFailed = true;
      return;
    }
    otaWritten += len;
    yield();
  }

  if (final) {
    if (Update.end(true)) {
      Serial.println("[OTA] Upload complete, restarting...");
      req->send(200, "text/plain", "OK");
      delay(50);
      ESP.restart();
    } else {
      Update.printError(Serial);
      req->send(500, "text/plain", "OTA end failed");
      otaFailed = true;
    }
  }
}

void initWebServer() {
  // ─── Public endpoints (no auth required) ──────────────────────────────
  server.on("/login.html", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send_P(200, "text/html", LOGIN_HTML);
  });

  server.on("/docs", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send_P(200, "text/html", DOCS_HTML);
  });

  server.on("/api/login", HTTP_POST, apiLogin);
  server.on("/api/auth-status", HTTP_GET, apiAuthStatus);
  server.on("/api/logout", HTTP_POST, apiLogout);
  server.on("/api/change-password", HTTP_POST, [](AsyncWebServerRequest *req) {
    if (!requireAuth(req)) return;
    apiChangePassword(req);
  });

  // ─── Protected endpoints (auth required) ──────────────────────────────
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (!requireAuth(req)) return;
    req->send_P(200, "text/html", HTML);
  });

  server.on("/api/ota", HTTP_POST,
    [](AsyncWebServerRequest *req) {
      if (!requireAuth(req)) return;
      if (!checkHeap(req)) return;
    },
    handleOTAUpload);

  // External sensor trigger (for testing / API integration) — must be before generic ext sensors
  // SHOULD #7: defer the actual state mutation to webLoop() to avoid racing
  // sensorsLoop()/alarmLoop() on the async TCP task.
  server.on("/api/extsensors/trigger", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (!requireAuth(req)) return;
    int id = req->arg("id").toInt();
    String state = req->arg("state");
    if (id < 1 || id > MAX_EXT_SENSORS) {
      req->send(400, "application/json", "{\"error\":\"invalid id (1-" + String(MAX_EXT_SENSORS) + ")\"}");
      return;
    }
    bool active = (state == "active" || state == "1" || state == "on");
    pendingExtTrigger.pending = true;
    pendingExtTrigger.id      = (uint8_t)id;
    pendingExtTrigger.active  = active;
    req->send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/api/extsensors", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (!requireAuth(req)) return;
    apiExtSensorsConfig(req);
  });
  server.on("/api/extsensors", HTTP_POST, [](AsyncWebServerRequest *req) {
    if (!requireAuth(req)) return;
    apiExtSensorsConfig(req);
  });
  server.on("/api/alarmmodes", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (!requireAuth(req)) return;
    apiAlarmModesConfig(req);
  });
  server.on("/api/alarmmodes", HTTP_POST, [](AsyncWebServerRequest *req) {
    if (!requireAuth(req)) return;
    apiAlarmModesConfig(req);
  });
  server.on("/api/zones", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (!requireAuth(req)) return;
    apiZonesConfig(req);
  });
  server.on("/api/zones", HTTP_POST, [](AsyncWebServerRequest *req) {
    if (!requireAuth(req)) return;
    apiZonesConfig(req);
  });
  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (!requireAuth(req)) return;
    apiStatus(req);
  });

  // Lightweight status — relays, zones, alarm state only (~200 bytes vs ~3KB)
  server.on("/api/status-light", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (!requireAuth(req)) return;
    JsonDocument doc;
    doc["wifi"]     = wifiConnected ? "connected" : (apMode ? "ap" : "disconnected");
    doc["activeMode"]  = alarmModeToHaString(alarmCtx.activeMode);
    doc["globalState"] = alarmStateToHaString(alarmCtx.globalState);
    doc["mqttConnected"] = mqtt.connected();

    JsonArray relays = doc["relays"].to<JsonArray>();
    for (int i = 0; i < MAX_RELAYS; i++) {
      JsonObject r = relays.add<JsonObject>();
      r["id"] = i + 1;
      r["state"] = relayStates[i];
    }

    JsonArray zones = doc["zones"].to<JsonArray>();
    for (int i = 0; i < MAX_ZONES; i++) {
      JsonObject z = zones.add<JsonObject>();
      z["id"] = i + 1;
      z["name"] = config.zones[i].name;
      z["armed"] = zoneStates[i].armed;
      z["enabled"] = config.zones[i].enabled;
      z["state"] = zoneAlarmStateStr(zoneStates[i].alarmState);
      z["label"] = zoneAlarmStateLabel(zoneStates[i].alarmState);
    }

    String buf;
    buf.reserve(600);
    serializeJson(doc, buf);
    req->send(200, "application/json", buf);
  });
  server.on("/api/sensors", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (!requireAuth(req)) return;
    apiSensorsConfig(req);
  });
  server.on("/api/sensors", HTTP_POST, [](AsyncWebServerRequest *req) {
    if (!requireAuth(req)) return;
    apiSensorsConfig(req);
  });
  server.on("/api/network", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (!requireAuth(req)) return;
    apiNetworkConfig(req);
  });
  server.on("/api/network", HTTP_POST, [](AsyncWebServerRequest *req) {
    if (!requireAuth(req)) return;
    apiNetworkConfig(req);
  });
  server.on("/api/mode/set", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (!requireAuth(req)) return;
    apiModeSet(req);
  });
  server.on("/api/restart", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (!requireAuth(req)) return;
    apiRestart(req);
  });
  server.on("/api/reconnect", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (!requireAuth(req)) return;
    apiReconnect(req);
  });

  // ─── Backup & Restore (admin only) ───────────────────────────────────
  server.on("/api/backup", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (!requireAdmin(req)) return;
    String json = buildBackupJson();
    AsyncWebServerResponse *resp = req->beginResponse(200, "application/json", json);
    resp->addHeader("Content-Disposition", "attachment; filename=\"alarm-backup.json\"");
    req->send(resp);
  });

  server.on("/api/restore", HTTP_POST, [](AsyncWebServerRequest *req) {
    if (!requireAdmin(req)) return;
    if (!req->hasArg("data")) {
      req->send(400, "application/json", "{\"error\":\"missing_data\"}");
      return;
    }
    String error;
    bool ok = applyRestore(req->arg("data").c_str(), error);
    if (ok) {
      req->send(200, "application/json", "{\"ok\":true,\"msg\":\"Restore complete. Restart to apply.\"}");
    } else {
      String resp = "{\"error\":\"" + error + "\"}";
      req->send(400, "application/json", resp);
    }
  });

  server.on("/api/eventlog", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (!requireAuth(req)) return;
    if (!checkHeap(req)) return;  // getEventLogJson() can build ~3KB JSON
    req->send(200, "application/json", getEventLogJson());
  });

  server.on("/api/eventlog/clear", HTTP_POST, [](AsyncWebServerRequest *req) {
    if (!requireAuth(req)) return;
    clearEventLog();
    logSystem("Event log cleared");
    req->send(200, "application/json", "{\"ok\":true}");
  });

  // ─── User management (admin only) ─────────────────────────────────────
  server.on("/api/users", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (!requireAuth(req)) return;
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (int i = 0; i < MAX_USERS; i++) {
      if (!config.users[i].active) continue;
      JsonObject u = arr.add<JsonObject>();
      u["id"]       = i;
      u["username"] = config.users[i].username;
      u["role"]     = config.users[i].role;
      u["pin"]      = String(config.users[i].pin);
    }
    String buf;
    serializeJson(doc, buf);
    req->send(200, "application/json", buf);
  });

  server.on("/api/users/add", HTTP_POST, [](AsyncWebServerRequest *req) {
    if (!requireAdmin(req)) return;
    String username = req->arg("username");
    String password = req->arg("password");
    String pin      = req->arg("pin");
    String roleStr  = req->arg("role");
    if (username.length() < 2 || password.length() < 4 || pin.length() != 4) {
      req->send(400, "application/json", "{\"error\":\"invalid_input\"}");
      return;
    }
    // Check for duplicate username
    for (int i = 0; i < MAX_USERS; i++) {
      if (config.users[i].active && strcmp(config.users[i].username, username.c_str()) == 0) {
        req->send(400, "application/json", "{\"error\":\"duplicate_username\"}");
        return;
      }
    }
    // Check for duplicate PIN
    for (int i = 0; i < MAX_USERS; i++) {
      if (config.users[i].active && strcmp(config.users[i].pin, pin.c_str()) == 0) {
        req->send(400, "application/json", "{\"error\":\"duplicate_pin\"}");
        return;
      }
    }
    if (countActiveUsers() >= MAX_USERS) {
      req->send(400, "application/json", "{\"error\":\"max_users\"}");
      return;
    }
    // Find free slot
    int slot = -1;
    for (int i = 0; i < MAX_USERS; i++) {
      if (!config.users[i].active) { slot = i; break; }
    }
    if (slot < 0) {
      req->send(400, "application/json", "{\"error\":\"max_users\"}");
      return;
    }
    String hash = hashPassword(password.c_str());
    strlcpy(config.users[slot].username, username.c_str(), sizeof(config.users[slot].username));
    strlcpy(config.users[slot].passwordHash, hash.c_str(), sizeof(config.users[slot].passwordHash));
    strlcpy(config.users[slot].pin, pin.c_str(), sizeof(config.users[slot].pin));
    config.users[slot].role   = (uint8_t)roleStr.toInt();
    config.users[slot].active = true;
    config.userCount++;
    requestSaveConfig();
    char logBuf[80];
    snprintf(logBuf, sizeof(logBuf), "User '%s' added", username.c_str());
    logSystem(logBuf);
    req->send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/api/users/delete", HTTP_POST, [](AsyncWebServerRequest *req) {
    if (!requireAdmin(req)) return;
    int id = req->arg("id").toInt();
    if (id < 0 || id >= MAX_USERS || !config.users[id].active) {
      req->send(400, "application/json", "{\"error\":\"not_found\"}");
      return;
    }
    // Cannot delete the last admin
    if (config.users[id].role == USER_ROLE_ADMIN) {
      int adminCount = 0;
      for (int i = 0; i < MAX_USERS; i++) {
        if (config.users[i].active && config.users[i].role == USER_ROLE_ADMIN) adminCount++;
      }
      if (adminCount <= 1) {
        req->send(400, "application/json", "{\"error\":\"cannot_delete_last_admin\"}");
        return;
      }
    }
    char logBuf[80];
    snprintf(logBuf, sizeof(logBuf), "User '%s' deleted", config.users[id].username);
    logSystem(logBuf);
    config.users[id].active = false;
    config.userCount--;
    requestSaveConfig();
    req->send(200, "application/json", "{\"ok\":true}");
  });

  // Reset auth entirely (recovery endpoint — no auth required)
  server.on("/api/reset-auth", HTTP_POST, [](AsyncWebServerRequest *req) {
    String freshHash = hashPassword("admin");
    String apiHash = hashPassword("api_user");
    memset(config.users, 0, sizeof(config.users));
    strlcpy(config.users[0].username, "admin", sizeof(config.users[0].username));
    strlcpy(config.users[0].passwordHash, freshHash.c_str(), sizeof(config.users[0].passwordHash));
    strlcpy(config.users[0].pin, "0000", sizeof(config.users[0].pin));
    config.users[0].role   = USER_ROLE_ADMIN;
    config.users[0].active = true;
    strlcpy(config.users[1].username, "api_user", sizeof(config.users[1].username));
    strlcpy(config.users[1].passwordHash, apiHash.c_str(), sizeof(config.users[1].passwordHash));
    strlcpy(config.users[1].pin, "0001", sizeof(config.users[1].pin));
    config.users[1].role   = USER_ROLE_API;
    config.users[1].active = true;
    config.userCount = 2;
    config.authMigrated = EEPROM_AUTH_MIGRATED_FLAG;
    config.forcePasswordChange = true;
    requestSaveConfig();
    req->send(200, "application/json", "{\"ok\":true,\"msg\":\"Auth reset — admin/admin + api_user/api_user\"}");
  });

  // Temporary: force siren zoneId to 0
  server.on("/api/fix-siren", HTTP_POST, [](AsyncWebServerRequest *req) {
    if (!requireAuth(req)) return;
    config.relays[0].zoneId = 0;
    requestSaveConfig();
    req->send(200, "application/json", "{\"ok\":true,\"msg\":\"Siren zoneId set to 0\"}");
  });

  for (int z = 1; z <= MAX_ZONES; z++) {
    String base = "/api/zone/" + String(z);
    server.on((base + "/arm").c_str(),    HTTP_GET, [](AsyncWebServerRequest *req) {
      if (!requireAuth(req)) return;
      apiZoneCommand(req);
    });
    server.on((base + "/disarm").c_str(), HTTP_GET, [](AsyncWebServerRequest *req) {
      if (!requireAuth(req)) return;
      apiZoneCommand(req);
    });
    server.on((base + "/toggle").c_str(), HTTP_GET, [](AsyncWebServerRequest *req) {
      if (!requireAuth(req)) return;
      apiZoneCommand(req);
    });
  }

  for (int r = 1; r <= MAX_RELAYS; r++) {
    server.on(("/api/relay/" + String(r)).c_str(), HTTP_GET, [](AsyncWebServerRequest *req) {
      if (!requireAuth(req)) return;
      apiRelayCommand(req);
    });
  }

  server.on("/api/relays/config", HTTP_POST, [](AsyncWebServerRequest *req) {
    if (!requireAuth(req)) return;
    for (int i = 0; i < MAX_RELAYS; i++) {
      String prefix = "r" + String(i + 1);
      String keyName = prefix + "_name";
      String keyEnabled = prefix + "_enabled";
      if (req->hasArg(keyName.c_str())) {
        String v = req->arg(keyName.c_str());
        strlcpy(config.relays[i].name, v.c_str(), sizeof(config.relays[i].name));
      }
      if (req->hasArg(keyEnabled.c_str())) {
        config.relays[i].enabled = (req->arg(keyEnabled.c_str()) != "0");
        relayManualOverride[i] = false;
      }
    }
    requestSaveConfig();
    req->send(200, "application/json", "{\"ok\":true}");
  });

  server.begin();
}
