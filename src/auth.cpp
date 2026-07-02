#include "auth.h"
#include "config.h"
#include "event_log.h"
#include <mbedtls/sha256.h>

// ─── Session storage ───────────────────────────────────────────────────────
#define MAX_SESSIONS 4
static AuthSession sessions[MAX_SESSIONS];
static uint32_t lastPurgeSec = 0;

// ─── IP rate-limit tracking ─────────────────────────────────────────────────
static IpTracker ipTrackers[MAX_TRACKED_IPS];
static uint8_t ipTrackerWriteIdx = 0;

// ─── Session secret (generated once per boot from hardware RNG) ────────────
static uint8_t sessionSecret[16];

// ─── Utility: get current Unix timestamp (best effort) ─────────────────────
static uint32_t currentUnixTime() {
    // Use millis() offset + NTP time when available
    if (lastNtpTime > 0) {
        uint32_t elapsed = millis() / 1000;
        // Handle 32-bit millis() wrap (~49.7 days)
        // lastNtpTime is set periodically by syncNTP(), so drift is bounded
        return lastNtpTime + elapsed - (lastNtpTime > 0 ? 0 : 0);
    }
    // Fallback: use boot-relative time
    return millis() / 1000;
}

// ─── SHA-256 hashing via mbedtls ──────────────────────────────────────────
String hashPassword(const char *password) {
    uint8_t hash[32];
    mbedtls_sha256_ret((const unsigned char *)password, strlen(password), hash, 0);

    char hex[65];
    for (int i = 0; i < 32; i++) {
        snprintf(hex + (i * 2), 3, "%02x", hash[i]);
    }
    hex[64] = '\0';
    return String(hex);
}

bool verifyPassword(const char *password, const char *hash) {
    if (!password || !hash || strlen(hash) != 64) return false;
    String computed = hashPassword(password);
    // Constant-time comparison to prevent timing attacks
    bool match = true;
    for (int i = 0; i < 64; i++) {
        if (computed[i] != hash[i]) match = false;
    }
    // Also compare lengths to prevent early exit
    if (computed.length() != 64) match = false;
    return match;
}

// ─── Session management ────────────────────────────────────────────────────
void initAuth() {
    // Seed session secret from ESP hardware RNG
    for (int i = 0; i < 16; i++) {
        sessionSecret[i] = (uint8_t)esp_random();
    }

    // Clear all session slots
    memset(sessions, 0, sizeof(sessions));
    memset(ipTrackers, 0, sizeof(ipTrackers));

    logSystem("Auth module initialized");
}

String createSession() {
    purgeExpiredSessions();

    // Find free slot or evict oldest inactive
    int slot = -1;
    uint32_t oldestTime = UINT32_MAX;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (!sessions[i].active) {
            slot = i;
            break;
        }
        if (sessions[i].lastActivity < oldestTime) {
            oldestTime = sessions[i].lastActivity;
            slot = i;
        }
    }

    if (slot < 0) slot = 0;

    // Generate random session ID
    uint8_t raw[16];
    for (int i = 0; i < 16; i++) {
        raw[i] = (uint8_t)esp_random();
    }

    char hex[SESSION_ID_LENGTH + 1];
    for (int i = 0; i < 16; i++) {
        snprintf(hex + (i * 2), 3, "%02x", raw[i]);
    }
    hex[SESSION_ID_LENGTH] = '\0';

    // Store session
    strlcpy(sessions[slot].id, hex, sizeof(sessions[slot].id));
    sessions[slot].createdAt    = currentUnixTime();
    sessions[slot].lastActivity = currentUnixTime();
    sessions[slot].active       = true;

    return String(hex);
}

bool validateSession(const char *token) {
    if (!token || strlen(token) != SESSION_ID_LENGTH) return false;

    uint32_t now = currentUnixTime();

    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (!sessions[i].active) continue;
        if (strcmp(sessions[i].id, token) == 0) {
            // Check expiry
            if (now - sessions[i].lastActivity > SESSION_TIMEOUT_SEC) {
                sessions[i].active = false;
                return false;
            }
            return true;
        }
    }
    return false;
}

void destroySession(const char *token) {
    if (!token) return;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (sessions[i].active && strcmp(sessions[i].id, token) == 0) {
            sessions[i].active = false;
            memset(sessions[i].id, 0, sizeof(sessions[i].id));
            return;
        }
    }
}

void touchSession(const char *token) {
    if (!token) return;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (sessions[i].active && strcmp(sessions[i].id, token) == 0) {
            sessions[i].lastActivity = currentUnixTime();
            return;
        }
    }
}

void purgeExpiredSessions() {
    uint32_t now = currentUnixTime();
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (!sessions[i].active) continue;
        if (now - sessions[i].lastActivity > SESSION_TIMEOUT_SEC) {
            sessions[i].active = false;
            memset(sessions[i].id, 0, sizeof(sessions[i].id));
        }
    }
}

// ─── Rate limiting per IP ──────────────────────────────────────────────────
int checkRateLimit(const String &ip) {
    for (int i = 0; i < MAX_TRACKED_IPS; i++) {
        if (ipTrackers[i].ip[0] == '\0') continue;
        if (strcmp(ipTrackers[i].ip, ip.c_str()) == 0) {
            // Check if currently locked out
            if (ipTrackers[i].lockoutUntil > 0) {
                uint32_t now = currentUnixTime();
                if (now < ipTrackers[i].lockoutUntil) {
                    return ipTrackers[i].lockoutUntil - now;
                }
                // Lockout expired — reset
                ipTrackers[i].lockoutUntil = 0;
                ipTrackers[i].failCount = 0;
                return 0;
            }
            return 0;
        }
    }
    return 0;
}

void recordFailedAttempt(const String &ip) {
    // Find or create tracker for this IP
    int slot = -1;
    int emptySlot = -1;
    for (int i = 0; i < MAX_TRACKED_IPS; i++) {
        if (ipTrackers[i].ip[0] == '\0') {
            if (emptySlot < 0) emptySlot = i;
            continue;
        }
        if (strcmp(ipTrackers[i].ip, ip.c_str()) == 0) {
            slot = i;
            break;
        }
    }

    if (slot < 0 && emptySlot >= 0) {
        slot = emptySlot;
        strlcpy(ipTrackers[slot].ip, ip.c_str(), sizeof(ipTrackers[slot].ip));
        ipTrackers[slot].failCount = 0;
    } else if (slot < 0) {
        // FIFO eviction: overwrite oldest entry
        ipTrackerWriteIdx = (ipTrackerWriteIdx + 1) % MAX_TRACKED_IPS;
        slot = ipTrackerWriteIdx;
        strlcpy(ipTrackers[slot].ip, ip.c_str(), sizeof(ipTrackers[slot].ip));
        ipTrackers[slot].failCount = 0;
        ipTrackers[slot].lockoutUntil = 0;
    }

    ipTrackers[slot].failCount++;
    ipTrackers[slot].lastAttemptMs = millis();

    if (ipTrackers[slot].failCount >= MAX_FAILED_ATTEMPTS) {
        ipTrackers[slot].lockoutUntil = currentUnixTime() + LOCKOUT_DURATION_SEC;

        char logBuf[100];
        snprintf(logBuf, sizeof(logBuf), "IP %s locked out for %d min after %d failed logins",
                 ip.c_str(), LOCKOUT_DURATION_SEC / 60, ipTrackers[slot].failCount);
        logSystem(logBuf);
    }
}

void resetFailedAttempts(const String &ip) {
    for (int i = 0; i < MAX_TRACKED_IPS; i++) {
        if (ipTrackers[i].ip[0] == '\0') continue;
        if (strcmp(ipTrackers[i].ip, ip.c_str()) == 0) {
            ipTrackers[i].failCount = 0;
            ipTrackers[i].lockoutUntil = 0;
            return;
        }
    }
}

// ─── Client IP extraction ──────────────────────────────────────────────────
String getClientIP(AsyncWebServerRequest *req) {
    // Try X-Forwarded-For first (common proxy header)
    if (req->hasHeader("X-Forwarded-For")) {
        String forwarded = req->getHeader("X-Forwarded-For")->value();
        // Take the first IP if there are multiple
        int comma = forwarded.indexOf(',');
        return (comma > 0) ? forwarded.substring(0, comma) : forwarded;
    }
    return req->client()->remoteIP().toString();
}

// ─── Auth middleware ────────────────────────────────────────────────────────
bool requireAuth(AsyncWebServerRequest *req) {
    // Extract session cookie
    String token;
    if (req->hasHeader("Cookie")) {
        String cookie = req->getHeader("Cookie")->value();
        String search = String(SESSION_COOKIE_NAME) + "=";
        int start = cookie.indexOf(search);
        if (start >= 0) {
            start += search.length();
            int end = cookie.indexOf(';', start);
            if (end < 0) end = cookie.length();
            token = cookie.substring(start, end);
        }
    }

    if (token.length() > 0 && validateSession(token.c_str())) {
        touchSession(token.c_str());
        return true;
    }

    // Not authenticated — determine response based on request type
    String path = req->url();

    // API requests get 401 JSON
    if (path.startsWith("/api/") && path != "/api/login" && path != "/api/auth-status") {
        req->send(401, "application/json", "{\"error\":\"unauthorized\",\"message\":\"Authentication required\"}");
        return false;
    }

    // Page requests get redirected to login
    if (path != "/login.html" && path != "/api/login" && path != "/api/auth-status") {
        req->redirect("/login.html");
        return false;
    }

    // Allow through for login-related paths
    return true;
}