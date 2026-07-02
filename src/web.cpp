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
#include <ArduinoJson.h>
#include <Update.h>

AsyncWebServer server(HTTP_PORT);

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

  if (modeArg == "disarmed") {
    disarmMode("web user");
    alarmCtx.globalState = deriveGlobalAlarmState();
    publishGlobalAlarmState();
    publishActiveProfile();
    publishZoneTopics();
  } else if (modeArg == "armed_home") {
    armMode(AlarmMode::ARMED_HOME, "web user");
    alarmCtx.globalState = deriveGlobalAlarmState();
    publishGlobalAlarmState();
    publishActiveProfile();
    publishZoneTopics();
  } else if (modeArg == "armed_away") {
    armMode(AlarmMode::ARMED_AWAY, "web user");
    alarmCtx.globalState = deriveGlobalAlarmState();
    publishGlobalAlarmState();
    publishActiveProfile();
    publishZoneTopics();
  } else if (modeArg == "armed_night") {
    armMode(AlarmMode::ARMED_NIGHT, "web user");
    alarmCtx.globalState = deriveGlobalAlarmState();
    publishGlobalAlarmState();
    publishActiveProfile();
    publishZoneTopics();
  } else if (modeArg == "armed_vacation") {
    armMode(AlarmMode::ARMED_VACATION, "web user");
    alarmCtx.globalState = deriveGlobalAlarmState();
    publishGlobalAlarmState();
    publishActiveProfile();
    publishZoneTopics();
  } else if (modeArg == "armed_custom_bypass") {
    armMode(AlarmMode::ARMED_CUSTOM_BYPASS, "web user");
    alarmCtx.globalState = deriveGlobalAlarmState();
    publishGlobalAlarmState();
    publishActiveProfile();
    publishZoneTopics();
  } else {
    req->send(400, "application/json", "{\"error\":\"unknown mode\"}");
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
    uint8_t idx = relayId - 1;
    relayManualOverride[idx] = true;
    relayManualState[idx] = on;
    setRelay(idx, on);
  }
  req->send(200, "application/json", "{\"ok\":true}");
}

// ─── Dashboard HTML ────────────────────────────────────────────────────────

static const char HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Home Alarm System</title><style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,BlinkMacSystemFont,"SF Pro Display","Helvetica Neue",sans-serif;background:#f2f2f7;color:#1d1d1f;min-height:100vh;-webkit-font-smoothing:antialiased;-moz-osx-font-smoothing:grayscale}
nav{display:flex;background:rgba(255,255,255,0.72);backdrop-filter:blur(20px);-webkit-backdrop-filter:blur(20px);padding:0;position:sticky;top:0;z-index:100;border-bottom:1px solid rgba(0,0,0,0.06)}
nav button{padding:14px 22px;background:none;border:none;color:#86868b;cursor:pointer;font-size:14px;font-weight:500;letter-spacing:-0.01em;border-bottom:2px solid transparent;transition:color 0.2s,border-color 0.2s}
nav button:hover{color:#1d1d1f}nav button.active{color:#0071e3;border-bottom-color:#0071e3}
.page{display:none;padding:32px 24px;max-width:1100px;margin:0 auto}
.page.active{display:block}h1{font-size:32px;font-weight:700;color:#1d1d1f;margin-bottom:4px;letter-spacing:-0.02em}h2{font-size:17px;font-weight:600;color:#1d1d1f;margin-bottom:14px;letter-spacing:-0.01em}
.card{background:#fff;border-radius:16px;padding:20px;margin:16px 0;box-shadow:0 1px 3px rgba(0,0,0,0.04),0 1px 2px rgba(0,0,0,0.06)}
.zone-grid{display:flex;gap:10px;justify-content:space-between;flex-wrap:wrap}
.zone-box{flex:1;min-width:100px;max-width:130px;border-radius:16px;padding:16px 8px 12px;text-align:center;display:flex;flex-direction:column;align-items:center;gap:8px;transition:all 0.25s ease;flex-shrink:0;background:#fff;box-shadow:0 1px 3px rgba(0,0,0,0.04)}
.zone-box.disarmed{border:1.5px solid #e5e5ea;background:#fff}
.zone-box.armed_idle{border:1.5px solid #34c759;background:#f0fff4}
.zone-box.arming{border:1.5px solid #0071e3;background:#f0f7ff}
.zone-box.prealarm{border:1.5px solid #ff9500;background:#fff8f0}
.zone-box.disarming{border:1.5px solid #ff9500;background:#fff8f0}
.zone-box.alarm{border:1.5px solid #ff3b30;background:#fff0f0}
.zone-dot{width:12px;height:12px;border-radius:50%;box-shadow:0 0 0 2px rgba(0,0,0,0.04)}
.zone-dot.disarmed{background:#aeaeb2}
.zone-dot.armed_idle{background:#34c759}
.zone-dot.arming{background:#0071e3}
.zone-dot.prealarm{background:#ff9500}
.zone-dot.disarming{background:#ff9500}
.zone-dot.alarm{background:#ff3b30}
.zone-name{font-size:12px;font-weight:600;line-height:1.2;color:#1d1d1f;letter-spacing:-0.01em}
.zone-label{font-size:11px;color:#86868b;font-weight:500}
.zone-label.has-state{color:#34c759}
.zone-label.arming-state{color:#0071e3}
.zone-label.prealarm-state{color:#ff9500}
.zone-label.disarming-state{color:#ff9500}
.zone-label.alarm-state{color:#ff3b30}
.toggle{position:relative;display:inline-block;width:51px;height:31px}
.toggle input{opacity:0;width:0;height:0}
.toggle-slider{position:absolute;cursor:pointer;top:0;left:0;right:0;bottom:0;background:#e5e5ea;border-radius:31px;transition:0.25s}
.toggle-slider:before{position:absolute;content:"";height:27px;width:27px;left:2px;bottom:2px;background:#fff;border-radius:50%;transition:0.25s;box-shadow:0 1px 3px rgba(0,0,0,0.15),0 1px 1px rgba(0,0,0,0.06)}
.toggle input:checked+.toggle-slider{background:#34c759}
.toggle input:checked+.toggle-slider:before{transform:translateX(20px)}
.toggle.disarm-slider input+.toggle-slider{background:#aeaeb2}
.toggle.alarm-slider input+.toggle-slider{background:#ff3b30}
.btn{padding:8px 18px;margin:2px;border:none;border-radius:10px;cursor:pointer;font-size:13px;font-weight:500;color:#fff;letter-spacing:-0.01em;transition:opacity 0.2s,transform 0.1s}
.btn:hover{opacity:0.88}.btn:active{transform:scale(0.97)}
.btn-arm{background:#34c759}.btn-disarm{background:#ff3b30}
.btn-save{background:#0071e3}.btn-danger{background:#ff3b30}
.sensor-grid{display:flex;flex-direction:column;gap:10px}
.sensor-row{display:flex;gap:10px;justify-content:space-between;flex-wrap:wrap}
.sensor-box{flex:1;min-width:100px;max-width:130px;border-radius:16px;padding:14px 8px;text-align:center;display:flex;flex-direction:column;align-items:center;gap:6px;transition:all 0.25s ease;flex-shrink:0;background:#fff;box-shadow:0 1px 3px rgba(0,0,0,0.04)}
.sensor-box.idle{border:1.5px solid #e5e5ea;background:#fff}
.sensor-box.active{border:1.5px solid #ff3b30;background:#fff0f0}
.sensor-box.fault{border:1.5px solid #ff9500;background:#fff8f0}
.sensor-box.disabled{border:1.5px solid #e5e5ea;background:#f9f9fb;opacity:0.55}
.sensor-box .sdot{width:12px;height:12px;border-radius:50%;box-shadow:0 0 0 2px rgba(0,0,0,0.04)}
.sensor-box .sdot.idle{background:#34c759}
.sensor-box .sdot.active{background:#ff3b30}
.sensor-box .sdot.fault{background:#ff9500}
.sensor-box .sdot.disabled{background:#aeaeb2}
.sensor-box .slabel{font-size:12px;font-weight:600;line-height:1.2;color:#1d1d1f;letter-spacing:-0.01em}
.sensor-box .sraw{font-size:13px;font-weight:600}
.sensor-box .sraw.idle{color:#34c759}
.sensor-box .sraw.active{color:#ff3b30}
.sensor-box .sraw.fault{color:#ff9500}
.sensor-box .sraw.disabled{color:#aeaeb2}
.sensor-box .sstate{font-size:10px;color:#86868b;font-weight:500;text-transform:uppercase;letter-spacing:0.02em}
.sensor-box .sstate.active-state{color:#ff3b30}
.sensor-box .sstate.fault-state{color:#ff9500}
.sensor{display:inline-block;padding:4px 10px;margin:2px;border-radius:8px;font-size:12px;font-weight:500}
.sensor.active{background:#ff3b30;color:#fff}.sensor.fault{background:#ff9500;color:#fff}.sensor.idle{background:#e5e5ea;color:#1d1d1f}
.relay-grid{display:flex;gap:10px;justify-content:center;flex-wrap:wrap}
.relay-box{flex:1;min-width:100px;max-width:140px;border-radius:16px;padding:14px 8px;text-align:center;display:flex;flex-direction:column;align-items:center;gap:6px;transition:all 0.25s ease;flex-shrink:0;background:#fff;box-shadow:0 1px 3px rgba(0,0,0,0.04)}
.relay-box.off{border:1.5px solid #e5e5ea;background:#fff}
.relay-box.on{border:1.5px solid #0071e3;background:#f0f7ff}
.relay-box .rdot{width:12px;height:12px;border-radius:50%;box-shadow:0 0 0 2px rgba(0,0,0,0.04)}
.relay-box .rdot.off{background:#aeaeb2}
.relay-box .rdot.on{background:#0071e3}
.relay-box .rlabel{font-size:12px;font-weight:600;line-height:1.2;color:#1d1d1f;letter-spacing:-0.01em}
.relay-box .rstate{font-size:13px;font-weight:600}
.relay-box .rstate.off{color:#aeaeb2}
.relay-box .rstate.on{color:#0071e3}
label{display:block;margin-top:8px;font-size:12px;color:#86868b;font-weight:500;letter-spacing:-0.01em}
input:not([type=checkbox]):not([type=file]),select{width:100%;padding:10px 12px;margin-top:4px;border:1.5px solid #e5e5ea;border-radius:10px;background:#f9f9fb;color:#1d1d1f;font-size:14px;font-family:inherit;transition:border-color 0.2s,box-shadow 0.2s}
input:focus,select:focus{outline:none;border-color:#0071e3;box-shadow:0 0 0 3px rgba(0,113,227,0.15);background:#fff}
input[type=number]{width:70px;display:inline;margin:0 2px;-moz-appearance:textfield}
input[type=number]::-webkit-outer-spin-button,input[type=number]::-webkit-inner-spin-button{-webkit-appearance:none;margin:0}
input[type=checkbox]{width:18px;height:18px;margin:0 2px;accent-color:#0071e3;-webkit-appearance:checkbox}
.ztoggle{display:inline-flex;align-items:center;margin-left:auto;flex-shrink:0}
.ztoggle input{width:40px;height:22px;margin:0;padding:0;cursor:pointer;accent-color:#34c759}
.form-row{display:grid;grid-template-columns:1fr 1fr;gap:16px}
@media(max-width:600px){.form-row{grid-template-columns:1fr}}
small{color:#86868b;font-size:12px}
#sensorCards,#zoneCards,#extCards{display:grid;grid-template-columns:1fr 1fr;gap:12px}
@media(max-width:750px){#sensorCards,#zoneCards,#extCards{grid-template-columns:1fr}}
.sens-card{border-radius:14px;padding:14px;font-size:12px;border:1.5px solid #e5e5ea;background:#fff;box-shadow:0 1px 3px rgba(0,0,0,0.03);transition:border-color 0.25s}
.sens-card.idle{border-color:#e5e5ea;background:#fff}
.sens-card.active{border-color:#ff3b30;background:#fff5f5}
.sens-card.fault{border-color:#ff9500;background:#fffaf5}
.sens-card.disabled{border-color:#e5e5ea;background:#f9f9fb;opacity:0.55}
.sens-top{display:flex;align-items:center;gap:8px;margin-bottom:6px;flex-wrap:wrap}
.sens-top .sid{color:#0071e3;font-weight:700;font-size:14px;min-width:24px;letter-spacing:-0.01em}
.sens-top input[type=text]{width:90px;padding:6px 8px;font-size:12px;margin-top:0}
.sens-top select{width:80px;padding:6px;font-size:12px;margin-top:0}
.sens-live{display:flex;align-items:center;gap:6px;font-size:12px;margin-left:auto;font-weight:500}
.sens-live .sdot{width:10px;height:10px;border-radius:50%;flex-shrink:0}
.sdot.idle{background:#34c759}.sdot.active{background:#ff3b30}.sdot.fault{background:#ff9500}.sdot.none{background:#aeaeb2}
.srange-row{display:flex;align-items:center;gap:4px;margin:2px 0;font-size:11px}
.srange-bar{width:8px;height:20px;border-radius:4px;flex-shrink:0}
.srange-bar.standby{background:#34c759}
.srange-bar.detected{background:#ff3b30}
.srange-bar.faultbar{background:#ff9500}
.srange-row input[type=number]{width:50px;padding:4px 6px;font-size:11px;margin-top:0}
.srange-label{width:60px;color:#86868b;font-size:10px;text-align:right;font-weight:500}
.stiming-row{display:flex;align-items:center;gap:4px;margin:3px 0;font-size:11px;flex-wrap:wrap}
.stiming-row span{color:#86868b;font-size:10px;font-weight:500}
.stiming-row input[type=number]{width:46px;padding:4px 6px;font-size:11px;margin-top:0}
.szone-row{display:flex;align-items:center;gap:3px;margin:3px 0;font-size:11px;flex-wrap:wrap}
.szone-row span{color:#86868b;font-size:10px;margin-right:3px;font-weight:500}
.szone-row label{display:inline-flex!important;align-items:center;font-size:10px!important;color:#86868b!important;margin-top:0!important;margin-right:2px;font-weight:500}
.szone-row input[type=checkbox]{width:14px;height:14px;margin:0 1px}
.ext-box{flex:1;min-width:100px;max-width:130px;border-radius:16px;padding:14px 8px;text-align:center;display:flex;flex-direction:column;align-items:center;gap:6px;transition:all 0.25s ease;flex-shrink:0;background:#fff;box-shadow:0 1px 3px rgba(0,0,0,0.04)}
.ext-box.idle{border:1.5px solid #e5e5ea;background:#fff}
.ext-box.active{border:1.5px solid #ff3b30;background:#fff0f0}
.ext-box .edot{width:12px;height:12px;border-radius:50%;box-shadow:0 0 0 2px rgba(0,0,0,0.04)}
.ext-box .edot.idle{background:#34c759}
.ext-box .edot.active{background:#ff3b30}
.ext-box .elabel{font-size:12px;font-weight:600;line-height:1.2;color:#1d1d1f;letter-spacing:-0.01em}
.ext-box .estate{font-size:13px;font-weight:600}
.ext-box .estate.idle{color:#34c759}
.ext-box .estate.active{color:#ff3b30}
.sens-foot{display:flex;justify-content:flex-end;margin-top:8px}
.sens-upd{background:#0071e3;padding:6px 16px;border:none;border-radius:10px;cursor:pointer;font-size:12px;color:#fff;font-weight:500;letter-spacing:-0.01em;transition:opacity 0.2s,transform 0.1s}
.sens-upd:hover{opacity:0.88}.sens-upd:active{transform:scale(0.97)}
.dot.idle{background:#34c759}.dot.active{background:#ff3b30}.dot.fault{background:#ff9500}.dot.none{background:#aeaeb2}
.range-row{display:flex;align-items:center;gap:6px;margin:4px 0;font-size:12px}
.range-bar{width:8px;height:24px;border-radius:4px;flex-shrink:0}
.range-bar.standby{background:#34c759}
.range-bar.detected{background:#ff3b30}
.range-bar.faultbar{background:#ff9500}
.range-row input[type=number]{width:62px;padding:4px 8px;font-size:12px}
.range-label{width:70px;color:#86868b;font-size:12px;text-align:right;font-weight:500}
.timing-row{display:flex;align-items:center;gap:8px;margin:4px 0;font-size:12px;flex-wrap:wrap}
.timing-row span{color:#86868b;font-weight:500}
.timing-row input[type=number]{width:58px;padding:4px 8px;font-size:12px}
.zone-row{display:flex;align-items:center;gap:4px;margin:4px 0;font-size:12px}
.zone-row span{color:#86868b;margin-right:4px;font-weight:500}
.sens-params-row{display:grid;grid-template-columns:1fr 1fr;gap:10px}
.zone-params{display:grid;grid-template-columns:1fr 1fr;gap:10px 20px;background:#f9f9fb;border-radius:10px;padding:12px 14px;margin:0}
.zone-params-stack{grid-template-columns:1fr}
.zone-params-field{display:flex;align-items:center;gap:6px}
.zone-params-field span{font-size:11px;color:#86868b;font-weight:500;white-space:nowrap}
.zone-params-field input[type=number]{width:56px;padding:5px 8px;font-size:12px;margin-top:0}
.zone-params-title{grid-column:1/-1;font-size:10px;font-weight:600;color:#1d1d1f;text-transform:uppercase;letter-spacing:0.04em;margin-bottom:-4px}
.log-row{display:flex;align-items:flex-start;gap:12px;padding:12px 16px;margin:0;border-left:4px solid #e5e5ea;transition:border-color 0.3s,background 0.3s}
.log-row:hover{background:#f9f9fb}
.log-row.alarm{border-left-color:#ff3b30}
.log-row.system{border-left-color:#0071e3}
.log-row.relay{border-left-color:#ff9500}
.log-row.sensor{border-left-color:#34c759}
.log-badge{display:inline-flex;align-items:center;padding:3px 10px;border-radius:8px;font-size:11px;font-weight:600;color:#fff;white-space:nowrap;min-width:64px;justify-content:center;letter-spacing:0.02em}
.log-badge.alarm{background:#ff3b30}
.log-badge.system{background:#0071e3}
.log-badge.relay{background:#ff9500}
.log-badge.sensor{background:#34c759}
.log-time{font-size:12px;color:#86868b;white-space:nowrap;min-width:140px;font-variant-numeric:tabular-nums}
.log-desc{font-size:13px;color:#1d1d1f;line-height:1.4;word-break:break-word}
.log-empty{text-align:center;padding:60px 20px;color:#aeaeb2;font-size:15px}
.log-empty div{font-size:40px;margin-bottom:12px}
.log-header{display:flex;align-items:center;justify-content:space-between;flex-wrap:wrap;gap:10px}
.log-header h1{margin-bottom:0}
@keyframes fadeIn{from{opacity:0;transform:translateY(-4px)}to{opacity:1;transform:translateY(0)}}
.log-footer{display:flex;align-items:center;justify-content:space-between;padding:8px 0 0;font-size:11px;color:#aeaeb2}
.filter-btn{padding:6px 14px;border:1.5px solid #e5e5ea;border-radius:10px;background:#fff;color:#86868b;cursor:pointer;font-size:12px;font-weight:500;transition:all 0.2s}
.filter-btn:hover{border-color:#0071e3;color:#0071e3}
.filter-btn.active{border-color:#0071e3;background:#f0f7ff;color:#0071e3;font-weight:600}
.mode-grid-row{display:flex;gap:8px;flex-wrap:wrap}
.mode-btn{border:2px solid #e5e5ea;border-radius:12px;padding:10px 14px;background:#fff;cursor:pointer;font-size:13px;font-weight:500;color:#1d1d1f;transition:all 0.2s;white-space:nowrap}
.mode-btn:hover{border-color:#0071e3;background:#f0f7ff}
.mode-btn.active{border-color:#34c759;background:#f0fff4;font-weight:600;color:#34c759;box-shadow:0 0 0 2px rgba(52,199,89,0.2)}
.mode-btn.triggered{border-color:#ff3b30;background:#fff0f0;color:#ff3b30}
.mode-btn.pending{border-color:#ff9500;background:#fff8f0;color:#ff9500}
.modal-overlay{display:none;position:fixed;top:0;left:0;right:0;bottom:0;background:rgba(0,0,0,0.45);backdrop-filter:blur(6px);-webkit-backdrop-filter:blur(6px);z-index:200;align-items:center;justify-content:center}
.modal-overlay.show{display:flex}
.modal-card{background:#fff;border-radius:20px;padding:36px 32px;width:100%;max-width:380px;box-shadow:0 8px 40px rgba(0,0,0,0.15);animation:fadeIn 0.3s ease}
.modal-card h2{font-size:22px;font-weight:700;margin-bottom:4px;letter-spacing:-0.02em}
.modal-card .sub{font-size:13px;color:#86868b;margin-bottom:20px}
.modal-card label{display:block;margin-top:10px;font-size:12px;color:#86868b;font-weight:500}
.modal-card input{width:100%;padding:10px 14px;margin-top:4px;border:1.5px solid #e5e5ea;border-radius:10px;background:#f9f9fb;color:#1d1d1f;font-size:14px;font-family:inherit;transition:border-color 0.2s}
.modal-card input:focus{outline:none;border-color:#0071e3;box-shadow:0 0 0 3px rgba(0,113,227,0.15);background:#fff}
.modal-card .btn{width:100%;margin-top:16px;padding:12px}
.modal-card .msg{margin-top:10px;font-size:13px;text-align:center;font-weight:500}
.logout-btn{padding:8px 16px;margin-left:auto;background:none;border:1.5px solid #ff3b30;border-radius:8px;color:#ff3b30;cursor:pointer;font-size:12px;font-weight:500;transition:all 0.2s}
.logout-btn:hover{background:#ff3b30;color:#fff}
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
</nav>
<div id="page-dashboard" class="page active"><h1>Home Alarm System</h1>
<div id="sysInfo" style="font-size:13px;color:#86868b;margin-bottom:12px">Loading...</div>
<div class="card"><h2>Alarm Mode</h2><div id="modeGrid">Loading...</div></div>
<div class="card"><h2>Zones</h2><div id="zones">Loading...</div></div>
<div class="card"><h2>Sensors</h2><div id="sensors">Loading...</div></div>
<div class="card"><h2>Relays</h2><div id="relays">Loading...</div></div>
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
<div class="form-row"><div><label>Role</label><select id="uRole"><option value="0">Admin (full access)</option><option value="1">Operator (arm/disarm only)</option></select></div>
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
    // Show Users tab for admins only
    document.getElementById('tab-users').style.display = (_authRole===0)?'':'none';
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
      h+='<td style="padding:10px 12px;font-size:13px;color:#86868b">'+(u.role==0?'Admin':'Operator')+'</td>';
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
  document.getElementById('sysInfo').textContent=
    data.device+' | '+data.firmware+' | http://alarm.local | WiFi: '+data.wifi+' | RSSI: '+data.rssi+' dBm';
  renderModeGrid();
  renderZones(data.zones);
  renderSensors(data.sensors);
  renderRelays(data.relays);
  renderExtSensors(data.ext_sensors);
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
  let h = '<div class="mode-grid-row">';
  modes.forEach(m => {
    let cls = 'mode-btn';
    if (m.id === active) cls += ' active';
    if (gs === 'triggered' && m.id === active) cls += ' triggered';
    if (gs === 'pending' && m.id === active) cls += ' pending';
    h += '<button class="' + cls + '" onclick="setMode(\'' + m.id + '\')">' + m.icon + ' ' + m.label + '</button>';
  });
  h += '</div>';
  document.getElementById('modeGrid').innerHTML = h;
}

async function setMode(mode) {
  await fetch('/api/mode/set?mode=' + encodeURIComponent(mode));
  load();
}

function renderExtSensors(a){
  let h='';
  for(let row=0;row<2;row++){
    h+='<div class="sensor-row">';
    for(let i=row*8;i<Math.min(row*8+8,a.length);i++){
      let e=a[i];
      let st=e.active?'active':'idle';
      h+=`<div class="ext-box ${st}">
<span class="edot ${st}"></span>
<span class="elabel">E${e.id}</span>
<span class="estate ${st}">${e.active?'Active':'Idle'}</span>
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
    let en=r.enabled!==false;
    h+=`<div class="relay-box ${on?'on':'off'}" style="${en?'':'opacity:0.5'}">
<span class="rdot ${on?'on':'off'}"></span>
<span class="rlabel">${r.name}</span>
<label class="toggle">
<input type="checkbox" ${on?'checked':''} onchange="fetch('/api/relay/${r.id}?state='+(this.checked?'ON':'OFF')).then(()=>load())">
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

// ─── Login page HTML ───────────────────────────────────────────────────────

static const char LOGIN_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Login - Home Alarm</title><style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,BlinkMacSystemFont,"SF Pro Display","Helvetica Neue",sans-serif;background:#f2f2f7;color:#1d1d1f;display:flex;align-items:center;justify-content:center;min-height:100vh;-webkit-font-smoothing:antialiased}
.login-card{background:#fff;border-radius:20px;padding:40px 36px;width:100%;max-width:380px;box-shadow:0 4px 20px rgba(0,0,0,0.08),0 1px 3px rgba(0,0,0,0.04)}
.login-card h1{font-size:28px;font-weight:700;text-align:center;margin-bottom:6px;letter-spacing:-0.02em}
.login-card .sub{font-size:13px;color:#86868b;text-align:center;margin-bottom:24px}
.login-card label{display:block;margin-top:12px;font-size:12px;color:#86868b;font-weight:500;letter-spacing:-0.01em}
.login-card input{width:100%;padding:12px 14px;margin-top:4px;border:1.5px solid #e5e5ea;border-radius:10px;background:#f9f9fb;color:#1d1d1f;font-size:15px;font-family:inherit;transition:border-color 0.2s,box-shadow 0.2s}
.login-card input:focus{outline:none;border-color:#0071e3;box-shadow:0 0 0 3px rgba(0,113,227,0.15);background:#fff}
.login-card button{width:100%;padding:12px;margin-top:20px;border:none;border-radius:12px;background:#0071e3;color:#fff;font-size:15px;font-weight:600;cursor:pointer;letter-spacing:-0.01em;transition:opacity 0.2s,transform 0.1s}
.login-card button:hover{opacity:0.88}
.login-card button:active{transform:scale(0.97)}
.login-card button:disabled{opacity:0.5;cursor:not-allowed}
.msg{margin-top:12px;font-size:13px;text-align:center;color:#ff3b30;font-weight:500}
.msg.success{color:#34c759}
.msg.warn{color:#ff9500}
.pw-toggle{position:relative}
.pw-toggle input{padding-right:44px}
.pw-toggle .eye{position:absolute;right:12px;top:50%;transform:translateY(0%);cursor:pointer;font-size:18px;user-select:none;opacity:0.5;transition:opacity 0.2s}
.pw-toggle .eye:hover{opacity:1}
.subtext{text-align:center;margin-top:20px;font-size:11px;color:#aeaeb2}
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
      saveConfig();
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
    saveConfig();
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
  // ─── Public endpoints (no auth required) ──────────────────────────────
  server.on("/login.html", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send_P(200, "text/html", LOGIN_HTML);
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
    },
    handleOTAUpload);

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

  server.on("/api/eventlog", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (!requireAuth(req)) return;
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
    saveConfig();
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
    saveConfig();
    req->send(200, "application/json", "{\"ok\":true}");
  });

  // Reset auth entirely (recovery endpoint — no auth required)
  server.on("/api/reset-auth", HTTP_POST, [](AsyncWebServerRequest *req) {
    String freshHash = hashPassword("admin");
    memset(config.users, 0, sizeof(config.users));
    strlcpy(config.users[0].username, "admin", sizeof(config.users[0].username));
    strlcpy(config.users[0].passwordHash, freshHash.c_str(), sizeof(config.users[0].passwordHash));
    strlcpy(config.users[0].pin, "0000", sizeof(config.users[0].pin));
    config.users[0].role   = USER_ROLE_ADMIN;
    config.users[0].active = true;
    config.userCount = 1;
    config.authMigrated = EEPROM_AUTH_MIGRATED_FLAG;
    config.forcePasswordChange = true;
    saveConfig();
    req->send(200, "application/json", "{\"ok\":true,\"msg\":\"Auth reset — password is now admin/admin\"}");
  });

  // Temporary: force siren zoneId to 0
  server.on("/api/fix-siren", HTTP_POST, [](AsyncWebServerRequest *req) {
    if (!requireAuth(req)) return;
    config.relays[0].zoneId = 0;
    saveConfig();
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
    saveConfig();
    req->send(200, "application/json", "{\"ok\":true}");
  });

  server.begin();
}
