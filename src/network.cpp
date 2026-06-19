#include "network.h"
#include "event_log.h"

// ─── WiFi retry state ──────────────────────────────────────────────────────
static uint8_t  wifiRetryCount     = 0;
static uint32_t wifiRetryNextMs    = 0;
static uint32_t apScanNextMs       = 0;
static bool     wifiWasEverConnected = false;  // track if we ever had WiFi

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
  if (wifiConnected) {
    apMode = false;
    wifiRetryCount = 0;
    wifiWasEverConnected = true;
    char buf[80];
    snprintf(buf, sizeof(buf), "WiFi connected: %s", config.wifiSsid);
    logSystem(buf);
    snprintf(buf, sizeof(buf), "LAN IP: %s", WiFi.localIP().toString().c_str());
    logSystem(buf);
  }
  return wifiConnected;
}

void startConfigAP() {
  WiFi.mode(WIFI_AP);
  String apSsid = String(AP_SSID_PREFIX) + deviceSuffix;
  WiFi.softAP(apSsid.c_str(), AP_PASS);
  delay(200);
  apMode = true;
  wifiConnected = false;
  logSystem("AP mode active");
}

// ─── Smart WiFi connect with retry backoff ─────────────────────────────────
// Called once in setup() — tries multiple times before giving up and starting AP

void ensureWiFiMode() {
  // If no SSID configured, go directly to AP
  if (strlen(config.wifiSsid) == 0) {
    startConfigAP();
    return;
  }

  // Try immediate connection
  if (connectWiFiStation()) return;

  // Failed — schedule retries
  wifiRetryCount = 1;
  wifiRetryNextMs = millis() + WIFI_RETRY_INTERVAL_MS;
  wifiConnected = false;
  apMode = false;   // not in AP yet — still trying station
}

// ─── Runtime WiFi watchdog ─────────────────────────────────────────────────
// Called from loop() each iteration

void wifiStationRetryLoop() {
  uint32_t now = millis();

  // ─── Case 1: Trying to connect (not AP, not connected) ─────────────────
  if (!apMode && !wifiConnected) {
    if (wifiRetryNextMs > 0 && now < wifiRetryNextMs) return;

    if (connectWiFiStation()) {
      // Success — back to normal
      return;
    }

    // Still failing
    wifiRetryCount++;
    if (wifiRetryCount >= WIFI_RETRY_MAX_ATTEMPTS) {
      // Max retries exhausted — fall back to AP
      logSystem("WiFi retries exhausted, switching to AP mode");
      startConfigAP();
      apScanNextMs = now + WIFI_AP_SCAN_INTERVAL_MS;
      return;
    }

    // Schedule next retry
    wifiRetryNextMs = now + WIFI_RETRY_INTERVAL_MS;
    return;
  }

  // ─── Case 2: In AP mode — periodically scan for configured SSID ─────────
  if (apMode) {
    if (now < apScanNextMs) return;
    apScanNextMs = now + WIFI_AP_SCAN_INTERVAL_MS;

    // Quick scan for the configured SSID
    int n = WiFi.scanNetworks(false, true);  // async=false, show_hidden=true
    bool found = false;
    for (int i = 0; i < n; i++) {
      if (WiFi.SSID(i) == String(config.wifiSsid)) {
        found = true;
        break;
      }
    }
    WiFi.scanDelete();

    if (found) {
      // Switch back to station mode
      logSystem("Configured SSID found, switching back to station");
      WiFi.softAPdisconnect(true);
      apMode = false;
      wifiRetryCount = 0;
      wifiRetryNextMs = 0;
      if (!connectWiFiStation()) {
        // Failed to connect despite seeing SSID — keep trying
        wifiRetryCount = 1;
        wifiRetryNextMs = now + WIFI_RETRY_INTERVAL_MS;
      }
    }
    return;
  }

  // ─── Case 3: Connected — check for disconnection ────────────────────────
  if (wifiConnected && WiFi.status() != WL_CONNECTED) {
    wifiConnected = false;
    wifiRetryCount = 1;
    wifiRetryNextMs = now + WIFI_RETRY_INTERVAL_MS;
    logSystem("WiFi disconnected, retrying...");
  }
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