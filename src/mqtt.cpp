#include "mqtt.h"
#include "sensors.h"
#include "zones.h"
#include "alarm.h"
#include "hardware.h"
#include <ArduinoJson.h>

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

// ─── Extern from ha_discovery.cpp ──────────────────────────────────────────
extern void haPublishAllDiscoveries();

// ─── MQTT state ────────────────────────────────────────────────────────────
static uint32_t mqttRetryAt  = 0;
static uint32_t mqttBackoff  = 1000;
static bool     mqttPostConnect = false;
static bool     mqttLoopRan      = false;
static uint32_t lastStatusPubMs = 0;

void mqttApplyServerConfig() {
  mqtt.setServer(config.mqttServer, config.mqttPort);
}

void pub(const String& subtopic, const String& value) {
  if (!mqtt.connected()) return;
  String topic = mqttBase + "/" + subtopic;
  mqtt.publish(topic.c_str(), value.c_str(), true);
}

// ─── Connection with exponential backoff ───────────────────────────────────

void connectMQTT() {
  if (!wifiConnected || apMode) return;
  if (mqtt.connected()) { mqttBackoff = 1000; return; }

  uint32_t now = millis();
  if (now < mqttRetryAt) return;
  mqttRetryAt = now + mqttBackoff;
  if (mqttBackoff < 60000) mqttBackoff *= 2;

  String will = mqttBase + "/status/running";
  bool ok;
  if (strlen(config.mqttUser) > 0) {
    ok = mqtt.connect(deviceId.c_str(), config.mqttUser, config.mqttPass,
                      will.c_str(), 0, true, "false");
  } else {
    ok = mqtt.connect(deviceId.c_str(), nullptr, nullptr,
                      will.c_str(), 0, true, "false");
  }

  if (ok) {
    mqttBackoff      = 1000;
    mqttPostConnect  = true;
    mqttLoopRan      = false;
  }
}

void mqttFlushPostConnect() {
  if (!mqttPostConnect) return;
  if (!mqtt.connected()) return;
  if (!mqttLoopRan) { mqttLoopRan = true; return; }

  mqttPostConnect = false;

  // Subscribe to command topics
  mqtt.subscribe((mqttBase + "/cmd/zone/+/arm").c_str());
  mqtt.subscribe((mqttBase + "/cmd/zone/+/disarm").c_str());
  mqtt.subscribe((mqttBase + "/cmd/zone/+/toggle").c_str());
  for (int i = 1; i <= MAX_RELAYS; i++) {
    mqtt.subscribe((mqttBase + "/cmd/relay/" + String(i)).c_str());
  }
  // External sensor topics
  for (int i = 1; i <= MAX_EXT_SENSORS; i++) {
    mqtt.subscribe((mqttBase + "/ext_sensor/" + String(i)).c_str());
  }

  // Drain retained MQTT messages delivered after subscribe
  mqtt.loop();
  // Override any retained arm commands — zones must start disarmed
  disarmAllZones();

  pub("status/running", "true");
  haPublishAllDiscoveries();
  publishStatus();
}

// ─── MQTT callback ─────────────────────────────────────────────────────────

static void handleZoneCmd(const String& topic, const String& payload, uint8_t zoneId) {
  if (topic.endsWith("/arm"))    zoneArm(zoneId);
  if (topic.endsWith("/disarm")) zoneDisarm(zoneId);
  if (topic.endsWith("/toggle")) zoneToggle(zoneId);
}

void mqttCallback(char* topic, byte* payload, unsigned int len) {
  char buf[256];
  unsigned int n = len < 255 ? len : 255;
  memcpy(buf, payload, n);
  buf[n] = 0;
  String p(buf);
  String t(topic);

  // ─── Zone arm/disarm ─────────────────────────────────────────────────┐
  if (t.startsWith(mqttBase + "/cmd/zone/")) {
    int zoneId = 0;
    String rest = t.substring((mqttBase + "/cmd/zone/").length());
    int slash = rest.indexOf('/');
    if (slash > 0) {
      zoneId = rest.substring(0, slash).toInt();
      if (zoneId >= 1 && zoneId <= MAX_ZONES) {
        handleZoneCmd(rest.substring(slash), p, zoneId);
      }
    }
  }

  // ─── External sensor state ───────────────────────────────────────────┐
  if (t.startsWith(mqttBase + "/ext_sensor/")) {
    String num = t.substring((mqttBase + "/ext_sensor/").length());
    int s = num.toInt();
    if (s >= 1 && s <= MAX_EXT_SENSORS) {
      String pl = p;
      pl.toLowerCase();
      bool active = (pl == "active" || pl == "1" || pl == "on");
      extSensorStates[s - 1].active = active;
      extSensorStates[s - 1].lastChangeMs = millis();
    }
  }

  // ─── Relay control ────────────────────────────────────────────────────┐
  if (t.startsWith(mqttBase + "/cmd/relay/")) {
    String num = t.substring((mqttBase + "/cmd/relay/").length());
    int r = num.toInt();
    if (r >= 1 && r <= MAX_RELAYS) {
      bool on = (p == "ON" || p == "1" || p == "true");
      setRelay(r - 1, on);
      publishStatus();
    }
  }
}

// ─── Status publishing ─────────────────────────────────────────────────────

// Called on every zone state change
static void onZoneChangeHandler(uint8_t zoneId) {
  if (!mqtt.connected()) return;
  uint8_t idx = zoneId - 1;
  String sub = "status/zone/" + String(zoneId);
  pub(sub + "/armed", zoneStates[idx].armed ? "true" : "false");
  pub(sub + "/state",  zoneAlarmStateStr(zoneStates[idx].alarmState));
}

void publishStatus() {
  if (!mqtt.connected()) return;

  // Zones
  for (uint8_t z = 1; z <= MAX_ZONES; z++) {
    uint8_t idx = z - 1;
    String base = "status/zone/" + String(z);
    pub(base + "/armed", zoneStates[idx].armed ? "true" : "false");
    pub(base + "/state", zoneAlarmStateStr(zoneStates[idx].alarmState));
  }

  // Sensors
  for (int s = 0; s < TOTAL_SENSORS; s++) {
    if (config.sensors[s].type == SENSOR_DISABLED) continue;
    String base = "status/sensor/" + String(s + 1);
    pub(base + "/state", sensorStateStr(sensorStates[s].state));
    pub(base + "/raw",   String(sensorStates[s].rawValue));
  }

  // Relays
  for (int r = 0; r < MAX_RELAYS; r++) {
    pub("status/relay/" + String(r + 1), relayStates[r] ? "ON" : "OFF");
  }

  // Digital inputs
  for (int d = 0; d < MAX_DINPUTS; d++) {
    pub("status/din/" + String(d + 1), dinputStates[d] ? "ON" : "OFF");
  }

  // System
  pub("status/wifi", wifiConnected ? "connected" : (apMode ? "ap" : "disconnected"));
  pub("status/rssi", wifiConnected ? String(WiFi.RSSI()) : "0");
}

// ─── Periodic status (called from loop) ────────────────────────────────────

void mqttStatusLoop() {
  if (!mqtt.connected()) return;
  uint32_t now = millis();
  if (now - lastStatusPubMs < 10000) return;  // every 10s
  lastStatusPubMs = now;
  publishStatus();
}

// ─── Wire up zone change callback ──────────────────────────────────────────

void setupMQTT() {
  onZoneStateChanged = onZoneChangeHandler;
}