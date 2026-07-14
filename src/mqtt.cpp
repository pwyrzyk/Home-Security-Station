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

// ─── Deferred publish flags (set in callback, flushed in mqttStatusLoop) ───
// Publishing from inside mqttCallback can collide with PubSubClient buffers.
// Instead, handlers set these flags and the actual publish happens in loop().
static bool pendingRelayStatusPub = false;

// ─── Incremental status publishing (SHOULD #10) ────────────────────────────
// Instead of publishing ~40 topics every 10s in a burst, we track the
// last-published value for each topic and only republish on change.
// A full publish is still done on connect (mqttPostConnect) and when a
// deferred relay-status publish is requested.

struct CachedString {
  char value[24];
  bool valid;
};

// Cache for string-valued topics
static CachedString cacheGlobalState   = {"", false};
static CachedString cacheActiveProfile = {"", false};
static CachedString cacheWifiStatus    = {"", false};
static char         cacheRssi[8]       = {0};

// Per-zone cache
static CachedString cacheZoneArmed[MAX_ZONES]   = {};
static CachedString cacheZoneState[MAX_ZONES]   = {};

// Per-sensor cache
static CachedString cacheSensorState[TOTAL_SENSORS] = {};

// Per-relay cache
static CachedString cacheRelay[MAX_RELAYS] = {};

// Per-digital-input cache
static CachedString cacheDin[MAX_DINPUTS] = {};

static bool cacheEq(const CachedString &c, const char *v) {
  return c.valid && strcmp(c.value, v) == 0;
}

static void cacheSet(CachedString &c, const char *v) {
  strlcpy(c.value, v, sizeof(c.value));
  c.valid = true;
}

static void invalidateAllCache() {
  cacheGlobalState.valid   = false;
  cacheActiveProfile.valid = false;
  cacheWifiStatus.valid    = false;
  cacheRssi[0] = 0;
  for (int i = 0; i < MAX_ZONES; i++) {
    cacheZoneArmed[i].valid = false;
    cacheZoneState[i].valid = false;
  }
  for (int i = 0; i < TOTAL_SENSORS; i++) cacheSensorState[i].valid = false;
  for (int i = 0; i < MAX_RELAYS; i++) cacheRelay[i].valid = false;
  for (int i = 0; i < MAX_DINPUTS; i++) cacheDin[i].valid = false;
}

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
    invalidateAllCache();  // force full publish on reconnect
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

  // Clear any stale retained command on /cmd/mode so future reconnects
  // don't re-deliver it
  mqtt.publish((mqttBase + "/cmd/mode").c_str(), "", true);

  pub("status/running", "true");
  haPublishAllDiscoveries();
  logSystem("HA autodiscovery published");
  publishStatus();  // full publish (cache was invalidated)
}

// ─── Global state publishing ───────────────────────────────────────────────

void publishGlobalAlarmState() {
  if (!mqtt.connected()) return;
  const char *s = alarmStateToHaString(alarmCtx.globalState);
  if (!cacheEq(cacheGlobalState, s)) {
    pub("state", s);
    cacheSet(cacheGlobalState, s);
  }
}

void publishActiveProfile() {
  if (!mqtt.connected()) return;
  const char *s = alarmModeToHaString(alarmCtx.activeMode);
  if (!cacheEq(cacheActiveProfile, s)) {
    pub("meta/active_profile", s);
    cacheSet(cacheActiveProfile, s);
  }
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
    String armed = zoneStates[idx].armed ? "true" : "false";
    String state = zoneAlarmStateStr(zoneStates[idx].alarmState);
    if (!cacheEq(cacheZoneArmed[idx], armed.c_str())) {
      pub(base + "/armed", armed);
      cacheSet(cacheZoneArmed[idx], armed.c_str());
    }
    if (!cacheEq(cacheZoneState[idx], state.c_str())) {
      pub(base + "/state", state);
      cacheSet(cacheZoneState[idx], state.c_str());
    }
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

    applyModeAndPublish(haCommandToMode(cmd.c_str()), "MQTT");
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
        updateZoneSensorCache();  // keep zone tripped cache in sync
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
      // Defer the status publish — publishing ~40 topics from inside the
      // callback can collide with PubSubClient's send buffer. The flag is
      // serviced in mqttStatusLoop().
      pendingRelayStatusPub = true;
    }
    return;
  }
}

// ─── Per-zone state change handler ─────────────────────────────────────────

static void onZoneChangeHandler(uint8_t zoneId) {
  if (!mqtt.connected()) return;
  uint8_t idx = zoneId - 1;
  String base = "zones/" + String(zoneId);
  String armed = zoneStates[idx].armed ? "true" : "false";
  String state = zoneAlarmStateStr(zoneStates[idx].alarmState);
  if (!cacheEq(cacheZoneArmed[idx], armed.c_str())) {
    pub(base + "/armed", armed);
    cacheSet(cacheZoneArmed[idx], armed.c_str());
  }
  if (!cacheEq(cacheZoneState[idx], state.c_str())) {
    pub(base + "/state", state);
    cacheSet(cacheZoneState[idx], state.c_str());
  }
}

// ─── Global state change handler ───────────────────────────────────────────

static void onGlobalChangeHandler(AlarmState newState) {
  const char *s = alarmStateToHaString(newState);
  if (!cacheEq(cacheGlobalState, s)) {
    pub("state", s);
    cacheSet(cacheGlobalState, s);
  }

  if (newState == AlarmState::TRIGGERED) {
    publishLastTriggerMeta();
  }
}

// ─── Full status publishing (periodic 10s + on-connect) ────────────────────
// Incremental: only topics whose value changed since last publish are sent.
// A full publish happens on reconnect (cache invalidated in connectMQTT).

void publishStatus() {
  if (!mqtt.connected()) return;

  // Global alarm state
  publishGlobalAlarmState();
  publishActiveProfile();

  // Per-zone topics
  publishZoneTopics();

  // Sensors
  for (int s = 0; s < TOTAL_SENSORS; s++) {
    if (config.sensors[s].type == SENSOR_DISABLED) continue;
    String base = "sensors/" + String(s + 1);
    String state = sensorStateStr(sensorStates[s].state);
    if (!cacheEq(cacheSensorState[s], state.c_str())) {
      pub(base + "/state", state);
      cacheSet(cacheSensorState[s], state.c_str());
    }
  }

  // Relays
  for (int r = 0; r < MAX_RELAYS; r++) {
    String state = relayStates[r] ? "ON" : "OFF";
    if (!cacheEq(cacheRelay[r], state.c_str())) {
      pub("status/relay/" + String(r + 1), state);
      cacheSet(cacheRelay[r], state.c_str());
    }
  }

  // Digital inputs
  for (int d = 0; d < MAX_DINPUTS; d++) {
    String state = dinputStates[d] ? "ON" : "OFF";
    if (!cacheEq(cacheDin[d], state.c_str())) {
      pub("status/din/" + String(d + 1), state);
      cacheSet(cacheDin[d], state.c_str());
    }
  }

  // System
  String wifi = wifiConnected ? "connected" : (apMode ? "ap" : "disconnected");
  if (!cacheEq(cacheWifiStatus, wifi.c_str())) {
    pub("status/wifi", wifi);
    cacheSet(cacheWifiStatus, wifi.c_str());
  }
  String rssi = wifiConnected ? String(WiFi.RSSI()) : "0";
  if (strcmp(cacheRssi, rssi.c_str()) != 0) {
    pub("status/rssi", rssi);
    strlcpy(cacheRssi, rssi.c_str(), sizeof(cacheRssi));
  }

  // Meta
  publishLastTriggerMeta();
}

// ─── Periodic status (called from loop) ────────────────────────────────────

void mqttStatusLoop() {
  if (!mqtt.connected()) return;

  // Service deferred publish requests from mqttCallback (SHOULD #9)
  if (pendingRelayStatusPub) {
    pendingRelayStatusPub = false;
    publishStatus();
    lastStatusPubMs = millis();  // reset periodic timer after a burst
    return;
  }

  uint32_t now = millis();
  if (now - lastStatusPubMs < 10000) return;  // every 10s
  lastStatusPubMs = now;
  publishStatus();  // incremental — only changed topics
}

// ─── Wire up callbacks ─────────────────────────────────────────────────────

void setupMQTT() {
  onZoneStateChanged   = onZoneChangeHandler;
  onGlobalStateChanged = onGlobalChangeHandler;
}