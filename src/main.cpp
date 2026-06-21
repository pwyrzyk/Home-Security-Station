#include <Arduino.h>

#include "config.h"
#include "hardware.h"
#include "sensors.h"
#include "zones.h"
#include "alarm.h"
#include "alarm_mode.h"
#include "network.h"
#include "mqtt.h"
#include "ha_discovery.h"
#include "web.h"
#include "event_log.h"
#include <LittleFS.h>

// ─── Sensor poll interval ──────────────────────────────────────────────────
static uint32_t lastSensorReadMs = 0;
#define SENSOR_READ_MS 100

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n\n[BOT] Alarm ESP booting...");
  Serial.printf("[BOT] Firmware: %s\n", FIRMWARE_VERSION);

  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);   // LED on during boot

  Serial.println("[BOT] Loading config...");
  loadConfig();
  computeDeviceIdentifiers();
  Serial.printf("[BOT] Device ID: %s\n", deviceId.c_str());

  Serial.println("[BOT] Init hardware...");
  initHardware();

  // Disarm all zones on boot
  Serial.println("[BOT] Disarming zones...");
  disarmAllZones();

  // ─── LittleFS (must be before WiFi — so WiFi events can be logged) ──
  Serial.println("[BOT] Mounting LittleFS...");
  if (!LittleFS.begin(true)) {
    Serial.println("[BOT] LittleFS mount failed!");
  } else {
    Serial.println("[BOT] LittleFS mounted");
  }

  Serial.println("[BOT] Init event log...");
  eventLogInit();

  logSystem("System booted");

  // Network
  Serial.println("[BOT] Starting WiFi...");
  ensureWiFiMode();
  Serial.printf("[BOT] WiFi: %s (AP=%s)\n", wifiConnected ? "connected" : "failed", apMode ? "yes" : "no");
  if (wifiConnected && !apMode) {
    configTime(TZ_OFFSET_SEC, 0, NTP_SERVER);
  }

  Serial.println("[BOT] Init OTA...");
  initOTA();
  mqttApplyServerConfig();
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(2048);

  Serial.println("[BOT] Init web server...");
  initWebServer();

  if (wifiConnected && !apMode) {
    connectMQTT();
  }

  setupMQTT();

  digitalWrite(LED_BUILTIN, LOW);     // boot complete
}

void loop() {
  // ─── WiFi watchdog ─────────────────────────────────────────────────────
  wifiStationRetryLoop();

  // ─── OTA + mDNS ────────────────────────────────────────────────────────
  ArduinoOTA.handle();

  // ─── Sensor read + state machine ───────────────────────────────────────
  uint32_t now = millis();
  if (now - lastSensorReadMs >= SENSOR_READ_MS) {
    lastSensorReadMs = now;
    readAllAdcChannels();
    readDigitalInputs();
    sensorsLoop();
  }

  // ─── Alarm engine ──────────────────────────────────────────────────────
  alarmLoop();

  // ─── NTP ────────────────────────────────────────────────────────────
  syncNTP();

  // ─── MQTT ───────────────────────────────────────────────────────────
  if (wifiConnected && !mqtt.connected()) {
    connectMQTT();
  }
  mqtt.loop();
  mqttFlushPostConnect();
  mqttStatusLoop();

  // ─── Status LED: fast blink during disarming/prealarm, slow blink during alarm ───
  static uint32_t lastLedBlink = 0;
  bool anyWarning = false, anyAlarm = false;
  for (int z = 0; z < MAX_ZONES; z++) {
    if (zoneStates[z].alarmState == ZONE_DISARMING) anyWarning = true;
    if (zoneStates[z].alarmState == ZONE_ALARM)     anyAlarm  = true;
  }
  uint32_t blinkRate = anyAlarm ? 500 : (anyWarning ? 200 : 0);
  if (blinkRate > 0) {
    if (now - lastLedBlink >= blinkRate) {
      lastLedBlink = now;
      digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
    }
  } else {
    digitalWrite(LED_BUILTIN, LOW);
  }

  // ─── Heap warning ──────────────────────────────────────────────────────
  static uint32_t lastHeapLogMs = 0;
  if (now - lastHeapLogMs >= 60000) {
    lastHeapLogMs = now;
    uint32_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < 30000) {
      char buf[80];
      snprintf(buf, sizeof(buf), "Low heap: %u bytes free", freeHeap);
      logSystem(buf);
    }
  }

  delay(2);
}
