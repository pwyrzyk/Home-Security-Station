#include "ha_discovery.h"
#include "mqtt.h"
#include "sensors.h"
#include "zones.h"
#include "hardware.h"
#include <ArduinoJson.h>

static void publishBinarySensor(uint8_t sensorIdx, const SensorConfig &sc) {
  JsonDocument doc;
  String uid = deviceId + "-sensor-" + String(sensorIdx + 1);
  String stateTopic = mqttBase + "/status/sensor/" + String(sensorIdx + 1) + "/state";
  String availTopic = mqttBase + "/status/running";

  doc["~"]       = mqttBase;
  doc["uniq_id"] = uid;
  doc["name"]    = sc.name;
  doc["stat_t"]  = stateTopic;
  doc["avty_t"]  = availTopic;
  doc["pl_on"]   = "active";
  doc["pl_off"]  = "idle";
  doc["dev_cla"] = (sc.type == SENSOR_PIR) ? "motion" : "door";
  doc["dev"]     = JsonObject();
  doc["dev"]["name"] = deviceId;
  doc["dev"]["ids"]  = deviceId;
  doc["dev"]["mf"]   = "ESP32-C3";
  doc["dev"]["mdl"]  = "Home Alarm System";

  String cfgTopic = "homeassistant/binary_sensor/" + uid + "/config";
  String payload;
  serializeJson(doc, payload);
  mqtt.publish(cfgTopic.c_str(), payload.c_str(), true);
}

static void publishAlarmControlPanel(uint8_t zoneIdx) {
  ZoneConfig &zc = config.zones[zoneIdx];
  JsonDocument doc;
  String uid = deviceId + "-zone-" + String(zoneIdx + 1);
  String stateTopic = mqttBase + "/status/zone/" + String(zoneIdx + 1) + "/state";
  String cmdTopic   = mqttBase + "/cmd/zone/" + String(zoneIdx + 1);
  String availTopic = mqttBase + "/status/running";

  doc["~"]         = mqttBase;
  doc["uniq_id"]   = uid;
  doc["name"]      = zc.name;
  doc["stat_t"]    = stateTopic;
  doc["cmd_t"]     = cmdTopic + "/arm";
  doc["pl_arm"]    = "ARM";
  doc["pl_disarm"] = "DISARM";
  doc["avty_t"]    = availTopic;
  doc["dev"]       = JsonObject();
  doc["dev"]["name"] = deviceId;
  doc["dev"]["ids"]  = deviceId;
  doc["dev"]["mf"]   = "ESP32-C3";
  doc["dev"]["mdl"]  = "Home Alarm System";

  String cfgTopic = "homeassistant/alarm_control_panel/" + uid + "/config";
  String payload;
  serializeJson(doc, payload);
  mqtt.publish(cfgTopic.c_str(), payload.c_str(), true);
}

static void publishSwitchRelay(uint8_t relayIdx) {
  RelayConfig &rc = config.relays[relayIdx];
  JsonDocument doc;
  String uid = deviceId + "-relay-" + String(relayIdx + 1);
  String stateTopic = mqttBase + "/status/relay/" + String(relayIdx + 1);
  String cmdTopic   = mqttBase + "/cmd/relay/"   + String(relayIdx + 1);
  String availTopic = mqttBase + "/status/running";

  doc["~"]         = mqttBase;
  doc["uniq_id"]   = uid;
  doc["name"]      = rc.name;
  doc["stat_t"]    = stateTopic;
  doc["cmd_t"]     = cmdTopic;
  doc["pl_on"]     = "ON";
  doc["pl_off"]    = "OFF";
  doc["avty_t"]    = availTopic;
  doc["dev"]       = JsonObject();
  doc["dev"]["name"] = deviceId;
  doc["dev"]["ids"]  = deviceId;
  doc["dev"]["mf"]   = "ESP32-C3";
  doc["dev"]["mdl"]  = "Home Alarm System";

  String cfgTopic = "homeassistant/switch/" + uid + "/config";
  String payload;
  serializeJson(doc, payload);
  mqtt.publish(cfgTopic.c_str(), payload.c_str(), true);
}

void haPublishAllDiscoveries() {
  if (!mqtt.connected()) return;

  // Binary sensors (T1..T16)
  for (int i = 0; i < TOTAL_SENSORS; i++) {
    if (config.sensors[i].type == SENSOR_DISABLED) continue;
    publishBinarySensor(i, config.sensors[i]);
    delay(10); // rate-limit
  }

  // Alarm control panels (Zones 1..8)
  for (uint8_t z = 0; z < MAX_ZONES; z++) {
    publishAlarmControlPanel(z);
    delay(10);
  }

  // Switches (Relays 1..4)
  for (uint8_t r = 0; r < MAX_RELAYS; r++) {
    publishSwitchRelay(r);
    delay(10);
  }
}