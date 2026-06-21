#include "mqtt.h"
#include "sensors.h"
#include "zones.h"
#include "alarm.h"
#include "alarm_mode.h"
#include "hardware.h"
#include "event_log.h"
#include <ArduinoJson.h>
#include <sys/time.h>  // for time_t / localtime_r in last-trigger timestamp

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
    char buf[80];
    snprintf(buf, sizeof(buf), "MQTT connected to %s", config.mqttServer);
    logSystem(buf);
  }
}

void mqttFlushPostConnect() {
  if (!mqttPostConnect) return;
  if (!mqtt.connected()) return;
  if (!mqttLoopRan) { mqttLoopRan = true; return; }

  mqttPostConnect = false;

  // Subscribe to command topics
  mqtt.subscribe((mqttBase + "/cmd/mode").c_str());
  for (int i = 1; i <= MAX_RELAYS; i++) {
    mqtt.subscribe((mqttBase + "/cmd/relay/" + String(i)).c_str());
  }
  // External sensor topics
  for (int i = 1; i <= MAX_EXT_SENSORS; i++) {
    mqtt.subscribe((mqttBase + "/ext_sensor/" + String(i)).c_str());
  }

  // Drain retained MQTT messages delivered after subscribe
  mqtt.loop();

  // Only disarm on the very first connect after boot (not on every reconnect)
  static bool bootDisarmDone = false;
  if (!bootDisarmDone) {
    bootDisarmDone = true;
    disarmAllZones();
    disarmMode("system boot");  // ensure alarmCtx is in sync
  }

  // Clear any stale retained command on /cmd/mode so future reconnects
  // don't re-deliver it
  mqtt.publish((mqttBase + "/cmd/mode").c_str(), "", true);

  pub("status/running", "true");
  haPublishAllDiscoveries();
  logSystem("HA autodiscovery published");
  publishStatus();
}

// ─── Global state publishing ───────────────────────────────────────────────

void publishGlobalAlarmState() {
  if (!mqtt.connected()) return;
  pub("state", alarmStateToHaString(alarmCtx.globalState));
}

void publishActiveProfile() {
  if (!mqtt.connected()) return;
  pub("meta/active_profile", alarmModeToHaString(alarmCtx.activeMode));
}

void publishLastTriggerMeta() {
  if (!mqtt.connected()) return;

  pub("meta/last_trigger/zone", alarmCtx.lastTriggerZoneName);

  char sensorLabel[40];
  if (alarmCtx.lastTriggerSensorId > 0) {
    snprintf(sensorLabel, sizeof(sensorLabel), "%s (#%u)",
             alarmCtx.lastTriggerSensorName, alarmCtx.lastTriggerSensorId);
  } else {
    strlcpy(sensorLabel, "", sizeof(sensorLabel));
  }
  pub("meta/last_trigger/sensor", sensorLabel);

  // Publish time: use NTP time if synced, otherwise empty
  time_t now = time(nullptr);
  if (now > 1700000000) { // year > 2024 = NTP synced
    struct tm tinfo;
    localtime_r(&now, &tinfo);
    char timeStr[32];
    strftime(timeStr, sizeof(timeStr), "%Y-%m-%dT%H:%M:%S", &tinfo);
    pub("meta/last_trigger/time", timeStr);
  } else {
    pub("meta/last_trigger/time", "");
  }
}

// ─── Per-zone topic publishing ─────────────────────────────────────────────

void publishZoneTopics() {
  if (!mqtt.connected()) return;
  for (uint8_t z = 1; z <= MAX_ZONES; z++) {
    uint8_t idx = z - 1;
    String base = "zones/" + String(z);
    pub(base + "/armed", zoneStates[idx].armed ? "true" : "false");
    pub(base + "/state", zoneAlarmStateStr(zoneStates[idx].alarmState));
  }
}

// ─── MQTT callback ─────────────────────────────────────────────────────────

void mqttCallback(char* topic, byte* payload, unsigned int len) {
  char buf[256];
  unsigned int n = len < 255 ? len : 255;
  memcpy(buf, payload, n);
  buf[n] = 0;
  String p(buf);
  String t(topic);

  // ─── Alarm mode command ──────────────────────────────────────────────┐
  if (t == mqttBase + "/cmd/mode") {
    String cmd = p;
    cmd.trim();
    cmd.toUpperCase();

    if (cmd == "DISARM") {
      disarmMode("MQTT");
      alarmCtx.globalState = deriveGlobalAlarmState();
      publishGlobalAlarmState();
      publishActiveProfile();
      publishZoneTopics();
    } else if (cmd == "ARM_HOME") {
      armMode(AlarmMode::ARMED_HOME, "MQTT");
      alarmCtx.globalState = deriveGlobalAlarmState();
      publishGlobalAlarmState();
      publishActiveProfile();
      publishZoneTopics();
    } else if (cmd == "ARM_AWAY") {
      armMode(AlarmMode::ARMED_AWAY, "MQTT");
      alarmCtx.globalState = deriveGlobalAlarmState();
      publishGlobalAlarmState();
      publishActiveProfile();
      publishZoneTopics();
    } else if (cmd == "ARM_NIGHT") {
      armMode(AlarmMode::ARMED_NIGHT, "MQTT");
      alarmCtx.globalState = deriveGlobalAlarmState();
      publishGlobalAlarmState();
      publishActiveProfile();
      publishZoneTopics();
    } else if (cmd == "ARM_VACATION") {
      armMode(AlarmMode::ARMED_VACATION, "MQTT");
      alarmCtx.globalState = deriveGlobalAlarmState();
      publishGlobalAlarmState();
      publishActiveProfile();
      publishZoneTopics();
    } else if (cmd == "ARM_CUSTOM_BYPASS") {
      armMode(AlarmMode::ARMED_CUSTOM_BYPASS, "MQTT");
      alarmCtx.globalState = deriveGlobalAlarmState();
      publishGlobalAlarmState();
      publishActiveProfile();
      publishZoneTopics();
    }
    return;
  }

  // ─── External sensor state ───────────────────────────────────────────┐
  if (t.startsWith(mqttBase + "/ext_sensor/")) {
    String num = t.substring((mqttBase + "/ext_sensor/").length());
    int s = num.toInt();
    if (s >= 1 && s <= MAX_EXT_SENSORS) {
      String pl = p;
      pl.toLowerCase();
      bool active = (pl == "active" || pl == "1" || pl == "on");
      bool wasActive = extSensorStates[s - 1].active;
      extSensorStates[s - 1].active = active;
      extSensorStates[s - 1].lastChangeMs = millis();
      if (active != wasActive) {
        char buf[80];
        snprintf(buf, sizeof(buf), "E%d '%s' -> %s", s, config.extSensors[s-1].name, active ? "ACTIVE" : "IDLE");
        logSensor(buf);
      }
    }
    return;
  }

  // ─── Relay control ────────────────────────────────────────────────────┐
  if (t.startsWith(mqttBase + "/cmd/relay/")) {
    String num = t.substring((mqttBase + "/cmd/relay/").length());
    int r = num.toInt();
    if (r >= 1 && r <= MAX_RELAYS) {
      bool on = (p == "ON" || p == "1" || p == "true");
      uint8_t idx = r - 1;
      relayManualOverride[idx] = true;
      relayManualState[idx] = on;
      setRelay(idx, on);
      publishStatus();
    }
    return;
  }
}

// ─── Per-zone state change handler ─────────────────────────────────────────

static void onZoneChangeHandler(uint8_t zoneId) {
  if (!mqtt.connected()) return;
  uint8_t idx = zoneId - 1;
  String base = "zones/" + String(zoneId);
  pub(base + "/armed", zoneStates[idx].armed ? "true" : "false");
  pub(base + "/state", zoneAlarmStateStr(zoneStates[idx].alarmState));
}

// ─── Global state change handler ───────────────────────────────────────────

static void onGlobalChangeHandler(AlarmState newState) {
  pub("state", alarmStateToHaString(newState));

  if (newState == AlarmState::TRIGGERED) {
    publishLastTriggerMeta();
  }
}

// ─── Full status publishing (periodic 10s + on-connect) ────────────────────

void publishStatus() {
  if (!mqtt.connected()) return;

  // Global alarm state
  pub("state", alarmStateToHaString(alarmCtx.globalState));
  pub("meta/active_profile", alarmModeToHaString(alarmCtx.activeMode));

  // Per-zone topics
  publishZoneTopics();

  // Sensors
  for (int s = 0; s < TOTAL_SENSORS; s++) {
    if (config.sensors[s].type == SENSOR_DISABLED) continue;
    String base = "sensors/" + String(s + 1);
    pub(base + "/state", sensorStateStr(sensorStates[s].state));
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

  // Meta
  publishLastTriggerMeta();
}

// ─── Periodic status (called from loop) ────────────────────────────────────

void mqttStatusLoop() {
  if (!mqtt.connected()) return;
  uint32_t now = millis();
  if (now - lastStatusPubMs < 10000) return;  // every 10s
  lastStatusPubMs = now;
  publishStatus();
}

// ─── Wire up callbacks ─────────────────────────────────────────────────────

void setupMQTT() {
  onZoneStateChanged   = onZoneChangeHandler;
  onGlobalStateChanged = onGlobalChangeHandler;
}
