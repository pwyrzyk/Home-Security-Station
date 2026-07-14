#include "network.h"
#include "event_log.h"

// ─── WiFi retry state ──────────────────────────────────────────────────────
static uint8_t  wifiRetryCount        = 0;
static uint32_t wifiRetryNextMs       = 0;
static uint32_t apScanNextMs          = 0;
static bool     wifiWasEverConnected  = false;  // track if we ever had WiFi

// ─── Async WiFi scan state ─────────────────────────────────────────────────
static bool     apScanAsyncActive     = false;  // true while async scan is running

// Non-blocking WiFi station connection state machine
enum class WifiConnectState : uint8_t {
  IDLE,
  STARTING,
  POLLING
};
static WifiConnectState wifiConnectState = WifiConnectState::IDLE;
static uint32_t         wifiConnectStartMs = 0;

// ─── Initiate a non-blocking WiFi STA connection ──────────────────────────
// Returns true if already connected; caller must poll checkWifiConnectDone()
// to know when connection attempt finishes.
bool connectWiFiStation() {
  if (strlen(config.wifiSsid) == 0) return false;

  // Already connected — nothing to do
  if (WiFi.status() == WL_CONNECTED) {
    wifiConnectState = WifiConnectState::IDLE;
    wifiConnected = true;
    return true;
  }

  // Already attempting — let polling continue
  if (wifiConnectState == WifiConnectState::POLLING) return false;

  // Start new attempt
  // Set mode BEFORE hostname — ESP32 requires mode set first for hostname to stick
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(OTA_HOSTNAME);
  WiFi.begin(config.wifiSsid, config.wifiPass);
  wifiConnectState  = WifiConnectState::POLLING;
  wifiConnectStartMs = millis();
  return false;
}

// ─── Poll the ongoing WiFi connection attempt ─────────────────────────────
// Called from wifiStationRetryLoop() each iteration. Non-blocking.
// Returns true when connected, false while still trying.
static bool checkWifiConnectDone() {
  if (wifiConnectState != WifiConnectState::POLLING) return false;

  wl_status_t status = WiFi.status();
  uint32_t now = millis();

  // Connected
  if (status == WL_CONNECTED) {
    wifiConnectState = WifiConnectState::IDLE;
    wifiConnected = true;
    apMode = false;
    wifiRetryCount = 0;
    wifiWasEverConnected = true;
    char buf[80];
    snprintf(buf, sizeof(buf), "WiFi connected: %s", config.wifiSsid);
    logSystem(buf);
    snprintf(buf, sizeof(buf), "LAN IP: %s", WiFi.localIP().toString().c_str());
    logSystem(buf);
    return true;
  }

  // Timed out
  if (now - wifiConnectStartMs >= WIFI_CONNECT_TIMEOUT_MS) {
    wifiConnectState = WifiConnectState::IDLE;
    // Connection failed — caller will handle retry scheduling
    return false;
  }

  // Still connecting — keep polling
  return false;
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
  // Before entering retry logic, re-sync wifiConnected with actual WiFi state.
  // Without this, a transient de-sync triggers the entire retry/AP-fallback
  // cascade, killing MQTT, API, and the alarm engine.
  if (!apMode && !wifiConnected && WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    wifiRetryCount = 0;
    wifiRetryNextMs = 0;
    wifiConnectState = WifiConnectState::IDLE;
    logSystem("WiFi re-synced (was marked down but stack is connected)");
    return;
  }

  if (!apMode && !wifiConnected) {
    // If a connection attempt is in progress, poll it non-blockingly
    if (wifiConnectState == WifiConnectState::POLLING) {
      if (checkWifiConnectDone()) return;  // connected!
      // If still polling — do nothing else this iteration
      return;
    }

    // No active attempt — wait for retry interval if one is scheduled
    if (wifiRetryNextMs > 0 && now < wifiRetryNextMs) return;

    // Start a new connection attempt
    if (connectWiFiStation()) {
      // Already connected (unlikely here)
      return;
    }

    // Connection attempt started (wifiConnectState == POLLING) —
    // schedule retries AFTER this attempt times out
    wifiRetryCount++;
    if (wifiRetryCount >= WIFI_RETRY_MAX_ATTEMPTS) {
      // Max retries exhausted — fall back to AP
      wifiConnectState = WifiConnectState::IDLE;
      logSystem("WiFi retries exhausted, switching to AP mode");
      startConfigAP();
      apScanNextMs = now + WIFI_AP_SCAN_INTERVAL_MS;
      return;
    }

    // Schedule next retry attempt (only fires if current one times out)
    wifiRetryNextMs = now + WIFI_CONNECT_TIMEOUT_MS + 1000;  // wait for timeout + 1s
    return;
  }

  // ─── Case 2: In AP mode — periodically scan for configured SSID ─────────
  if (apMode) {
    // If an async scan is already running, check if results are ready
    if (apScanAsyncActive) {
      int8_t n = WiFi.scanComplete();
      if (n == WIFI_SCAN_RUNNING) return;  // still scanning — come back later
      if (n == WIFI_SCAN_FAILED) {
        apScanAsyncActive = false;
        apScanNextMs = now + WIFI_AP_SCAN_INTERVAL_MS;
        return;
      }
      // Scan complete — check results
      apScanAsyncActive = false;
      bool found = false;
      for (int8_t i = 0; i < n; i++) {
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
        WiFi.setSleep(false);  // re-assert after mode switch from AP
        if (!connectWiFiStation()) {
          // Failed to connect despite seeing SSID — keep trying
          wifiRetryCount = 1;
          wifiRetryNextMs = now + WIFI_RETRY_INTERVAL_MS;
        }
      } else {
        apScanNextMs = now + WIFI_AP_SCAN_INTERVAL_MS;
      }
      return;
    }

    // No scan active — start one if interval elapsed
    if (now < apScanNextMs) return;
    // Start async scan (non-blocking — results checked in subsequent iterations)
    WiFi.scanNetworks(true, true);  // async=true, show_hidden=true
    apScanAsyncActive = true;
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
  // ─── mDNS — always start, regardless of OTA enabled/disabled ──────────
  // mDNS makes the device reachable as http://alarm.local and advertises
  // the HTTP service. This must NOT be skipped when OTA is disabled.
  if (MDNS.begin(OTA_HOSTNAME)) {
    MDNS.addService("http", "tcp", HTTP_PORT);
    char buf[60];
    snprintf(buf, sizeof(buf), "mDNS active: http://%s.local", OTA_HOSTNAME);
    logSystem(buf);
  } else {
    logSystem("mDNS init failed");
  }

  // ─── ArduinoOTA — only enable if a password is configured ─────────────
  ArduinoOTA.setPort(OTA_PORT);
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  if (strlen(OTA_PASSWORD) > 0) {
    ArduinoOTA.setPassword(OTA_PASSWORD);
    ArduinoOTA.onStart([]() {});
    ArduinoOTA.onEnd([]() {});
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {});
    ArduinoOTA.onError([](ota_error_t error) {});
    ArduinoOTA.begin();
    logSystem("OTA enabled");
  } else {
    logSystem("OTA disabled — no password configured");
  }
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

  bool wasSynced = ntpSynced();
  configTime(TZ_OFFSET_SEC, 0, NTP_SERVER);

  // If NTP just became available for the first time, update the auth
  // session clock so active sessions don't instantly expire due to the
  // jump from millis-based fake time to real epoch time.
  if (!wasSynced && ntpSynced()) {
    lastNtpTime = time(nullptr);
    extern void onNtpSynced(uint32_t realEpoch);
    onNtpSynced(lastNtpTime);
  }
}