#include "auth.h"
#include "config.h"
#include "event_log.h"
#include <mbedtls/sha256.h>

// ─── Session storage ───────────────────────────────────────────────────────
#define MAX_SESSIONS 4
static AuthSession sessions[MAX_SESSIONS];

// Session timeout in milliseconds (derived from SESSION_TIMEOUT_SEC)
static const uint32_t SESSION_TIMEOUT_MS = (uint32_t)SESSION_TIMEOUT_SEC * 1000UL;
// Lockout duration in milliseconds (derived from LOCKOUT_DURATION_SEC)
static const uint32_t LOCKOUT_DURATION_MS = (uint32_t)LOCKOUT_DURATION_SEC * 1000UL;

// ─── IP rate-limit tracking ─────────────────────────────────────────────────
static IpTracker ipTrackers[MAX_TRACKED_IPS];
static uint8_t ipTrackerWriteIdx = 0;

// ─── Session secret (generated once per boot from hardware RNG) ────────────
static uint8_t sessionSecret[16];

// ─── Monotonic time helper ─────────────────────────────────────────────────
// All session/IP-tracking timestamps use millis() directly. This avoids the
// discontinuity that occurs when NTP syncs and time(nullptr) jumps from a
// small fake value to the real Unix epoch. millis() is monotonic and wraps
// only after ~49.7 days, which is handled correctly by unsigned subtraction.
static inline uint32_t nowMs() {
    return millis();
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
    bool match = true;
    for (int i = 0; i < 64; i++) {
        if (computed[i] != hash[i]) match = false;
    }
    if (computed.length() != 64) match = false;
    return match;
}

// ─── Multi-user authentication ────────────────────────────────────────────
UserEntry* verifyCredentials(const char *username, const char *password) {
    if (!username || !password) return nullptr;
    for (int i = 0; i < MAX_USERS; i++) {
        if (!config.users[i].active) continue;
        if (strcmp(config.users[i].username, username) == 0) {
            if (verifyPassword(password, config.users[i].passwordHash)) {
                return &config.users[i];
            }
            return nullptr;  // found user but wrong password
        }
    }
    return nullptr;  // user not found
}

UserEntry* verifyUserByPin(const char *pin) {
    if (!pin || strlen(pin) != 4) return nullptr;
    for (int i = 0; i < MAX_USERS; i++) {
        if (!config.users[i].active) continue;
        if (strcmp(config.users[i].pin, pin) == 0) {
            return &config.users[i];
        }
    }
    return nullptr;
}

int findUserSlot(const char *username) {
    if (!username) return -1;
    for (int i = 0; i < MAX_USERS; i++) {
        if (config.users[i].active && strcmp(config.users[i].username, username) == 0) {
            return i;
        }
    }
    return -1;
}

int countActiveUsers() {
    int count = 0;
    for (int i = 0; i < MAX_USERS; i++) {
        if (config.users[i].active) count++;
    }
    return count;
}

bool hasActiveAdmin() {
    for (int i = 0; i < MAX_USERS; i++) {
        if (config.users[i].active && config.users[i].role == USER_ROLE_ADMIN) {
            return true;
        }
    }
    return false;
}

// ─── Session management ────────────────────────────────────────────────────
void initAuth() {
    for (int i = 0; i < 16; i++) {
        sessionSecret[i] = (uint8_t)esp_random();
    }
    memset(sessions, 0, sizeof(sessions));
    memset(ipTrackers, 0, sizeof(ipTrackers));
    logSystem("Auth module initialized");
}

String createSession(const char *username, uint8_t role) {
    purgeExpiredSessions();

    int slot = -1;
    uint32_t oldestTime = UINT32_MAX;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (!sessions[i].active) {
            slot = i;
            break;
        }
        if (sessions[i].lastActivityMs < oldestTime) {
            oldestTime = sessions[i].lastActivityMs;
            slot = i;
        }
    }
    if (slot < 0) slot = 0;

    uint8_t raw[16];
    for (int i = 0; i < 16; i++) {
        raw[i] = (uint8_t)esp_random();
    }

    char hex[SESSION_ID_LENGTH + 1];
    for (int i = 0; i < 16; i++) {
        snprintf(hex + (i * 2), 3, "%02x", raw[i]);
    }
    hex[SESSION_ID_LENGTH] = '\0';

    uint32_t now = nowMs();
    strlcpy(sessions[slot].id, hex, sizeof(sessions[slot].id));
    if (username) strlcpy(sessions[slot].username, username, sizeof(sessions[slot].username));
    sessions[slot].role            = role;
    sessions[slot].createdAtMs     = now;
    sessions[slot].lastActivityMs  = now;
    sessions[slot].active          = true;

    return String(hex);
}

bool validateSession(const char *token) {
    if (!token || strlen(token) != SESSION_ID_LENGTH) return false;
    uint32_t now = nowMs();
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (!sessions[i].active) continue;
        if (strcmp(sessions[i].id, token) == 0) {
            // Unsigned subtraction handles millis() wrap-around correctly
            if (now - sessions[i].lastActivityMs > SESSION_TIMEOUT_MS) {
                sessions[i].active = false;
                return false;
            }
            return true;
        }
    }
    return false;
}

uint8_t getSessionRole(const char *token) {
    if (!token || strlen(token) != SESSION_ID_LENGTH) return USER_ROLE_OPERATOR;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (!sessions[i].active) continue;
        if (strcmp(sessions[i].id, token) == 0) {
            return sessions[i].role;
        }
    }
    return USER_ROLE_OPERATOR;
}

const char* getSessionUsername(const char *token) {
    if (!token || strlen(token) != SESSION_ID_LENGTH) return "";
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (!sessions[i].active) continue;
        if (strcmp(sessions[i].id, token) == 0) {
            return sessions[i].username;
        }
    }
    return "";
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
    uint32_t now = nowMs();
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (sessions[i].active && strcmp(sessions[i].id, token) == 0) {
            sessions[i].lastActivityMs = now;
            return;
        }
    }
}

void purgeExpiredSessions() {
    uint32_t now = nowMs();
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (!sessions[i].active) continue;
        if (now - sessions[i].lastActivityMs > SESSION_TIMEOUT_MS) {
            sessions[i].active = false;
            memset(sessions[i].id, 0, sizeof(sessions[i].id));
        }
    }
}

// ─── Rate limiting per IP ──────────────────────────────────────────────────
// Returns retry_after_sec (0 = allowed). Uses monotonic millis() for lockout.
// Wrap-safe: lockoutUntilMs is set as (now + duration); the elapsed time is
// computed with unsigned subtraction so a 49.7-day millis() rollover is fine.
int checkRateLimit(const String &ip) {
    uint32_t now = nowMs();
    for (int i = 0; i < MAX_TRACKED_IPS; i++) {
        if (ipTrackers[i].ip[0] == '\0') continue;
        if (strcmp(ipTrackers[i].ip, ip.c_str()) == 0) {
            if (ipTrackers[i].lockoutUntilMs == 0) return 0;  // not locked
            // Elapsed since lockout start; compare against LOCKOUT_DURATION_MS.
            // We store the absolute expiry (lockoutUntilMs = now + duration),
            // so "still locked" means (lockoutUntilMs - now) is in the future.
            uint32_t elapsed = now - ipTrackers[i].lastAttemptMs;
            if (elapsed >= LOCKOUT_DURATION_MS) {
                // Lockout expired
                ipTrackers[i].lockoutUntilMs = 0;
                ipTrackers[i].failCount = 0;
                return 0;
            }
            uint32_t remainingMs = LOCKOUT_DURATION_MS - elapsed;
            return (int)((remainingMs + 999) / 1000);  // round up to seconds
        }
    }
    return 0;
}

void recordFailedAttempt(const String &ip) {
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
        ipTrackerWriteIdx = (ipTrackerWriteIdx + 1) % MAX_TRACKED_IPS;
        slot = ipTrackerWriteIdx;
        strlcpy(ipTrackers[slot].ip, ip.c_str(), sizeof(ipTrackers[slot].ip));
        ipTrackers[slot].failCount = 0;
        ipTrackers[slot].lockoutUntilMs = 0;
    }

    ipTrackers[slot].failCount++;
    ipTrackers[slot].lastAttemptMs = nowMs();

    if (ipTrackers[slot].failCount >= MAX_FAILED_ATTEMPTS) {
        ipTrackers[slot].lockoutUntilMs = nowMs() + LOCKOUT_DURATION_MS;
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
            ipTrackers[i].lockoutUntilMs = 0;
            return;
        }
    }
}

// ─── Client IP extraction ──────────────────────────────────────────────────
String getClientIP(AsyncWebServerRequest *req) {
    if (req->hasHeader("X-Forwarded-For")) {
        String forwarded = req->getHeader("X-Forwarded-For")->value();
        int comma = forwarded.indexOf(',');
        return (comma > 0) ? forwarded.substring(0, comma) : forwarded;
    }
    return req->client()->remoteIP().toString();
}

// ─── NTP sync callback ─────────────────────────────────────────────────────
// With monotonic millis()-based session clocks, NTP sync no longer causes a
// timestamp discontinuity. This callback is now a no-op but kept for API
// compatibility with network.cpp.
void onNtpSynced(uint32_t realEpoch) {
    (void)realEpoch;  // no adjustment needed — sessions use millis()
}

// ─── Auth middleware ────────────────────────────────────────────────────────
static String extractSessionToken(AsyncWebServerRequest *req) {
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
    return token;
}

bool requireAuth(AsyncWebServerRequest *req) {
    String token = extractSessionToken(req);

    if (token.length() > 0 && validateSession(token.c_str())) {
        touchSession(token.c_str());
        return true;
    }

    String path = req->url();
    if (path.startsWith("/api/") && path != "/api/login" && path != "/api/auth-status" && path != "/api/reset-auth") {
        req->send(401, "application/json", "{\"error\":\"unauthorized\",\"message\":\"Authentication required\"}");
        return false;
    }
    if (path != "/login.html" && path != "/api/login" && path != "/api/auth-status" && path != "/api/reset-auth") {
        req->redirect("/login.html");
        return false;
    }
    return true;
}

bool requireAdmin(AsyncWebServerRequest *req) {
    String token = extractSessionToken(req);

    if (token.length() > 0 && validateSession(token.c_str())) {
        touchSession(token.c_str());
        if (getSessionRole(token.c_str()) == USER_ROLE_ADMIN) {
            return true;
        }
        req->send(403, "application/json", "{\"error\":\"forbidden\",\"message\":\"Admin access required\"}");
        return false;
    }

    req->send(401, "application/json", "{\"error\":\"unauthorized\",\"message\":\"Authentication required\"}");
    return false;
}