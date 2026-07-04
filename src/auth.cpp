#include "auth.h"
#include "config.h"
#include "event_log.h"
#include <mbedtls/sha256.h>

// ─── Session storage ───────────────────────────────────────────────────────
#define MAX_SESSIONS 4
static AuthSession sessions[MAX_SESSIONS];
static uint32_t lastPurgeSec = 0;
static uint32_t fakeToRealOffset = 0;  // added to millis-based timestamps after NTP sync

// ─── IP rate-limit tracking ─────────────────────────────────────────────────
static IpTracker ipTrackers[MAX_TRACKED_IPS];
static uint8_t ipTrackerWriteIdx = 0;

// ─── Session secret (generated once per boot from hardware RNG) ────────────
static uint8_t sessionSecret[16];

// ─── Utility: get current Unix timestamp (best effort) ─────────────────────
static uint32_t currentUnixTime() {
    if (lastNtpTime > 0) {
        uint32_t elapsed = millis() / 1000;
        return lastNtpTime + elapsed;
    }
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
        if (sessions[i].lastActivity < oldestTime) {
            oldestTime = sessions[i].lastActivity;
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

    strlcpy(sessions[slot].id, hex, sizeof(sessions[slot].id));
    if (username) strlcpy(sessions[slot].username, username, sizeof(sessions[slot].username));
    sessions[slot].role         = role;
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
            if (now - sessions[i].lastActivity > SESSION_TIMEOUT_SEC) {
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
            if (ipTrackers[i].lockoutUntil > 0) {
                uint32_t now = currentUnixTime();
                if (now < ipTrackers[i].lockoutUntil) {
                    return ipTrackers[i].lockoutUntil - now;
                }
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
    if (req->hasHeader("X-Forwarded-For")) {
        String forwarded = req->getHeader("X-Forwarded-For")->value();
        int comma = forwarded.indexOf(',');
        return (comma > 0) ? forwarded.substring(0, comma) : forwarded;
    }
    return req->client()->remoteIP().toString();
}

// ─── NTP sync callback — adjusts session timestamps when real time arrives ─
// Called from syncNTP() when the system transitions from millis-based fake
// clock to real Unix epoch time. Prevents sessions from instantly expiring
// due to the timestamp discontinuity.
void onNtpSynced(uint32_t realEpoch) {
  // Nothing to do if no sessions or no fake clock in use
  if (lastNtpTime == 0) return;

  // Compute the fake timestamp at the moment of NTP sync
  uint32_t fakeAtSync = millis() / 1000;

  // Offset: what to add to a fake timestamp to get a real epoch
  // (realEpoch - fakeAtSync), clamped to avoid underflow if somehow
  // realEpoch < fakeAtSync (should never happen in practice)
  int64_t offset = (int64_t)realEpoch - (int64_t)fakeAtSync;
  if (offset <= 0) return;  // nothing to adjust

  unsigned adjusted = 0;
  for (int i = 0; i < MAX_SESSIONS; i++) {
    if (!sessions[i].active) continue;
    // Only adjust timestamps that were set before NTP sync.
    // A crude heuristic: timestamps < 100000 (2001 epoch) are definitely
    // from the fake clock. Real Unix timestamps are always > 1.7e9 in 2024+.
    if (sessions[i].createdAt < 100000) {
      sessions[i].createdAt    += (uint32_t)offset;
      if (sessions[i].lastActivity < 100000) {
        sessions[i].lastActivity += (uint32_t)offset;
      }
      adjusted++;
    }
  }
  if (adjusted > 0) {
    char buf[70];
    snprintf(buf, sizeof(buf), "NTP synced: adjusted %u session timestamps", adjusted);
    logSystem(buf);
  }
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