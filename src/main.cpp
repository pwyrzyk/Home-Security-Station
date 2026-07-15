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
#include "rs485_web.h"
#include "event_log.h"
#include "auth.h"
#include "keypad_comm.h"
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

  // Restore armed state after power failure
  Serial.println("[BOT] Restoring alarm state...");
  restoreArmedState();

  // Clear any spurious digital input triggers from boot
  for (int i = 0; i < MAX_DINPUTS; i++) {
    dinputStates[i] = false;
  }

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
  // Disable WiFi modem sleep BEFORE connecting — must be set unconditionally
  // because ensureWiFiMode() returns asynchronously and wifiConnected is still
  // false at this point. Without this, idle TCP connections (MQTT, HTTP) drop
  // after ~60s when the modem enters power-save.
  WiFi.setSleep(false);

  Serial.println("[BOT] Starting WiFi...");
  ensureWiFiMode();
  Serial.printf("[BOT] WiFi: %s (AP=%s)\n", wifiConnected ? "connected" : "failed", apMode ? "yes" : "no");
  if (wifiConnected && !apMode) {
    configTime(TZ_OFFSET_SEC, 0, NTP_SERVER);
  }

  Serial.println("[BOT] Init OTA...");
  initOTA();
  mqttApplyServerConfig();
  mqtt.setKeepAlive(60);   // 60s keepalive — matches broker defaults
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(2048);

  Serial.println("[BOT] Init auth...");
  initAuth();

  Serial.println("[BOT] Init web server...");
  initWebServer();

  if (wifiConnected && !apMode) {
    connectMQTT();
  }

  setupMQTT();

  // ─── RS-485 Keypad communication (Master) ──────────────────────────────
  Serial.println("[BOT] Init keypad RS-485...");
  keypadCommInit();

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

  // ─── Deferred web actions (ext sensor triggers, relay commands, restart) ▐
  // MUST run before MQTT reconnect/status — an ext-sensor API call should
  // take effect within one loop iteration (<200ms), not be blocked for
  // seconds while MQTT reconnects and publishes HA discovery.
  webLoop();

  // ─── NTP ────────────────────────────────────────────────────────────
  syncNTP();

  // ─── MQTT ───────────────────────────────────────────────────────────
  // Always run mqtt.loop() — PubSubClient needs it to process the TCP
  // handshake during connect(). Skipping it when disconnected makes
  // reconnection take longer.
  mqtt.loop();
  if (wifiConnected && !mqtt.connected()) {
    connectMQTT();
  }
  mqttFlushPostConnect();
  mqttStatusLoop();

  // ─── Deferred config save (dirty-flag flush) ───────────────────────────
  configSaveLoop();

  // ─── Event log batch flush ────────────────────────────────────────────
  eventLogFlushIfNeeded();

  // ─── RS-485 Keypad communication ──────────────────────────────────────
  keypadCommLoop();

  // ─── RS-485 WebSocket monitor (slave status broadcast) ─────────────────
  rs485WebLoop();

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

  yield();
}