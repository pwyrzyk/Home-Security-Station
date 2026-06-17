#include "network.h"

bool connectWiFiStation() {
  if (strlen(config.wifiSsid) == 0) return false;

  WiFi.mode(WIFI_STA);
  WiFi.begin(config.wifiSsid, config.wifiPass);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED &&
         (millis() - start) < WIFI_CONNECT_TIMEOUT_MS) {
    delay(500);
  }

  wifiConnected = (WiFi.status() == WL_CONNECTED);
  apMode = false;
  return wifiConnected;
}

void startConfigAP() {
  WiFi.mode(WIFI_AP);
  String apSsid = String(AP_SSID_PREFIX) + deviceSuffix;
  WiFi.softAP(apSsid.c_str(), AP_PASS);
  delay(200);
  apMode = true;
  wifiConnected = false;
}

void ensureWiFiMode() {
  if (!connectWiFiStation()) startConfigAP();
}

// ─── OTA ───────────────────────────────────────────────────────────────────

void initOTA() {
  ArduinoOTA.setPort(OTA_PORT);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.setHostname(OTA_HOSTNAME);

  ArduinoOTA.onStart([]() {});
  ArduinoOTA.onEnd([]() {});
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {});
  ArduinoOTA.onError([](ota_error_t error) {});
  ArduinoOTA.begin();

  // mDNS
  MDNS.begin(OTA_HOSTNAME);
  MDNS.addService("http", "tcp", HTTP_PORT);
}

// ─── NTP ───────────────────────────────────────────────────────────────────

static int getYear(time_t t) {
  struct tm *tm = localtime(&t);
  return tm ? tm->tm_year + 1900 : 0;
}

bool ntpSynced() {
  time_t now = time(nullptr);
  return (now > 100000 && getYear(now) >= 2024);
}

void syncNTP() {
  if (!wifiConnected || apMode) return;
  static uint32_t lastNtpAttempt = 0;
  uint32_t now = millis();
  if (now - lastNtpAttempt < 3600000UL) return; // every hour
  lastNtpAttempt = now;

  configTime(TZ_OFFSET_SEC, 0, NTP_SERVER);
}