#include "ha_discovery.h"
#include "mqtt.h"
#include "sensors.h"
#include "zones.h"
#include "hardware.h"
#include "event_log.h"
#include <ArduinoJson.h>

// ─── Helpers ───────────────────────────────────────────────────────────────

static JsonObject addDeviceInfo(JsonDocument& doc) {
  JsonObject dev = doc["dev"].to<JsonObject>();
  dev["name"] = "Home Alarm System";
  dev["ids"].add(deviceId);
  dev["mf"]   = "ESP32-C3";
  dev["mdl"]  = "Home Alarm System";
  dev["sw"]   = FIRMWARE_VERSION;
  return dev;
}

static bool publishDiscoveryPayload(const String& cfgTopic, JsonDocument& doc) {
  String payload;
  size_t len = serializeJson(doc, payload);
  if (len == 0) {
    char buf[100];
    snprintf(buf, sizeof(buf), "HA discovery: empty payload for %s", cfgTopic.c_str());
    logSystem(buf);
    return false;
  }
  bool ok = mqtt.publish(cfgTopic.c_str(), payload.c_str(), true);
  if (!ok) {
    char buf[100];
    snprintf(buf, sizeof(buf), "HA discovery: publish failed for %s", cfgTopic.c_str());
    logSystem(buf);
  }
  return ok;
}

// ─── Per-sensor binary_sensor ──────────────────────────────────────────────

static void publishBinarySensor(uint8_t sensorIdx, const SensorConfig &sc) {
  JsonDocument doc;
  String uid = deviceId + "-sensor-" + String(sensorIdx + 1);

  doc["~"]            = mqttBase;
  doc["uniq_id"]      = uid;
  doc["name"]         = sc.name;
  doc["stat_t"]       = "~/sensors/" + String(sensorIdx + 1) + "/state";
  doc["avty_t"]       = "~/status/running";
  doc["pl_avail"]     = "true";
  doc["pl_not_avail"] = "false";
  doc["pl_on"]        = "active";
  doc["pl_off"]       = "idle";
  doc["dev_cla"]      = (sc.type == SENSOR_PIR) ? "motion" : "door";
  addDeviceInfo(doc);

  String cfgTopic = String(config.haDiscoveryPrefix) + "/binary_sensor/" + uid + "/config";
  publishDiscoveryPayload(cfgTopic, doc);
}

// ─── Main alarm_control_panel (single entity) ──────────────────────────────

static void publishMainAlarmPanel() {
  JsonDocument doc;
  String uid = deviceId + "-alarm";

  doc["~"]                     = mqttBase;
  doc["uniq_id"]               = uid;
  doc["name"]                  = "Home Alarm";
  doc["stat_t"]                = "~/state";
  doc["cmd_t"]                 = "~/cmd/mode";
  doc["avty_t"]                = "~/status/running";
  doc["pl_avail"]              = "true";
  doc["pl_not_avail"]          = "false";

  // HA command payloads (what HA sends to cmd_t)
  doc["pl_disarm"]             = "DISARM";
  doc["pl_arm_home"]           = "ARM_HOME";
  doc["pl_arm_away"]           = "ARM_AWAY";
  doc["pl_arm_night"]          = "ARM_NIGHT";
  doc["pl_arm_vacation"]       = "ARM_VACATION";
  doc["pl_arm_custom_bypass"]  = "ARM_CUSTOM_BYPASS";

  // Override the generic arm payload to ARM_AWAY (so default arm works)
  doc["pl_arm"]                = "ARM_AWAY";

  // State payloads
  doc["pl_triggered"]          = "triggered";

  doc["code_arm_required"]     = false;
  doc["code_disarm_required"]  = false;
  doc["ret"]                   = true;  // state topic is retained

  addDeviceInfo(doc);

  String cfgTopic = String(config.haDiscoveryPrefix) + "/alarm_control_panel/" + uid + "/config";
  publishDiscoveryPayload(cfgTopic, doc);
}

// ─── Per-zone state sensor (text) ──────────────────────────────────────────

static void publishZoneStateSensor(uint8_t zoneIdx) {
  uint8_t zid = zoneIdx + 1;
  JsonDocument doc;
  String uid = deviceId + "-zone-" + String(zid) + "-state";

  doc["~"]            = mqttBase;
  doc["uniq_id"]      = uid;
  doc["name"]         = String(config.zones[zoneIdx].name) + " State";
  doc["stat_t"]       = "~/zones/" + String(zid) + "/state";
  doc["avty_t"]       = "~/status/running";
  doc["pl_avail"]     = "true";
  doc["pl_not_avail"] = "false";
  addDeviceInfo(doc);

  String cfgTopic = String(config.haDiscoveryPrefix) + "/sensor/" + uid + "/config";
  publishDiscoveryPayload(cfgTopic, doc);
}

// ─── Active profile sensor ─────────────────────────────────────────────────

static void publishActiveProfileSensor() {
  JsonDocument doc;
  String uid = deviceId + "-active-profile";

  doc["~"]            = mqttBase;
  doc["uniq_id"]      = uid;
  doc["name"]         = "Active Alarm Profile";
  doc["stat_t"]       = "~/meta/active_profile";
  doc["avty_t"]       = "~/status/running";
  doc["pl_avail"]     = "true";
  doc["pl_not_avail"] = "false";
  doc["ic"]           = "mdi:shield-account";
  addDeviceInfo(doc);

  String cfgTopic = String(config.haDiscoveryPrefix) + "/sensor/" + uid + "/config";
  publishDiscoveryPayload(cfgTopic, doc);
}

// ─── Last trigger zone sensor ──────────────────────────────────────────────

static void publishLastTriggerZoneSensor() {
  JsonDocument doc;
  String uid = deviceId + "-last-trigger-zone";

  doc["~"]            = mqttBase;
  doc["uniq_id"]      = uid;
  doc["name"]         = "Last Triggered Zone";
  doc["stat_t"]       = "~/meta/last_trigger/zone";
  doc["avty_t"]       = "~/status/running";
  doc["pl_avail"]     = "true";
  doc["pl_not_avail"] = "false";
  doc["ic"]           = "mdi:map-marker-alert";
  addDeviceInfo(doc);

  String cfgTopic = String(config.haDiscoveryPrefix) + "/sensor/" + uid + "/config";
  publishDiscoveryPayload(cfgTopic, doc);
}

// ─── Last trigger sensor sensor ────────────────────────────────────────────

static void publishLastTriggerSensorSensor() {
  JsonDocument doc;
  String uid = deviceId + "-last-trigger-sensor";

  doc["~"]            = mqttBase;
  doc["uniq_id"]      = uid;
  doc["name"]         = "Last Triggered Sensor";
  doc["stat_t"]       = "~/meta/last_trigger/sensor";
  doc["avty_t"]       = "~/status/running";
  doc["pl_avail"]     = "true";
  doc["pl_not_avail"] = "false";
  doc["ic"]           = "mdi:motion-sensor";
  addDeviceInfo(doc);

  String cfgTopic = String(config.haDiscoveryPrefix) + "/sensor/" + uid + "/config";
  publishDiscoveryPayload(cfgTopic, doc);
}

// ─── Relay switch ──────────────────────────────────────────────────────────

static void publishSwitchRelay(uint8_t relayIdx) {
  RelayConfig &rc = config.relays[relayIdx];
  JsonDocument doc;
  String uid = deviceId + "-relay-" + String(relayIdx + 1);

  doc["~"]            = mqttBase;
  doc["uniq_id"]      = uid;
  doc["name"]         = rc.name;
  doc["stat_t"]       = "~/status/relay/" + String(relayIdx + 1);
  doc["cmd_t"]        = "~/cmd/relay/"   + String(relayIdx + 1);
  doc["avty_t"]       = "~/status/running";
  doc["pl_avail"]     = "true";
  doc["pl_not_avail"] = "false";
  doc["pl_on"]        = "ON";
  doc["pl_off"]       = "OFF";
  addDeviceInfo(doc);

  String cfgTopic = String(config.haDiscoveryPrefix) + "/switch/" + uid + "/config";
  publishDiscoveryPayload(cfgTopic, doc);
}

// ─── Cleanup: delete stale old-style per-zone alarm panels ─────────────────

static void cleanupStaleDiscoveries() {
  // Delete old per-zone alarm_control_panel configs (replaced by single main panel)
  for (uint8_t z = 0; z < MAX_ZONES; z++) {
    String oldUid = deviceId + "-zone-" + String(z + 1);
    String oldTopic = String(config.haDiscoveryPrefix) + "/alarm_control_panel/" + oldUid + "/config";
    mqtt.publish(oldTopic.c_str(), "", true);  // empty retained = delete
    delay(2);
  }

  // Delete stale "Zone N Armed" binary_sensor configs (removed — redundant)
  for (uint8_t z = 0; z < MAX_ZONES; z++) {
    String armedUid = deviceId + "-zone-" + String(z + 1) + "-armed";
    String armedTopic = String(config.haDiscoveryPrefix) + "/binary_sensor/" + armedUid + "/config";
    mqtt.publish(armedTopic.c_str(), "", true);  // empty retained = delete
    delay(2);
  }
}

// ─── Publish all discoveries ───────────────────────────────────────────────

void haPublishAllDiscoveries() {
  if (!mqtt.connected()) return;
  if (!config.haDiscoveryEnabled) return;

  logSystem("HA autodiscovery: publishing...");

  // Clean up stale old-style per-zone alarm panels + armed binary_sensors
  cleanupStaleDiscoveries();

  // Main alarm panel (single entity)
  publishMainAlarmPanel();
  delay(10);

  // Per-sensor binary_sensors (T1..T16)
  for (int i = 0; i < TOTAL_SENSORS; i++) {
    if (config.sensors[i].type == SENSOR_DISABLED) continue;
    publishBinarySensor(i, config.sensors[i]);
    delay(10);
  }

  // Per-zone state entities
  for (uint8_t z = 0; z < MAX_ZONES; z++) {
    publishZoneStateSensor(z);
    delay(5);
  }

  // Meta sensors
  publishActiveProfileSensor();
  delay(10);
  publishLastTriggerZoneSensor();
  delay(10);
  publishLastTriggerSensorSensor();
  delay(10);

  // Relay switches
  for (uint8_t r = 0; r < MAX_RELAYS; r++) {
    publishSwitchRelay(r);
    delay(10);
  }

  logSystem("HA autodiscovery: complete");
}