#pragma once

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include "config.h"

// ─── Constants ──────────────────────────────────────────────────────────────
#define MAX_FAILED_ATTEMPTS      5
#define LOCKOUT_DURATION_SEC     300      // 5 minutes
#define MIN_ATTEMPT_INTERVAL_MS  1000     // 1 second between attempts
#define SESSION_TIMEOUT_SEC      1800     // 30 minutes
#define MAX_TRACKED_IPS          16       // max IPs to track for rate limiting
#define SESSION_COOKIE_NAME      "alarm_session"
#define AUTH_SESSION_MAGIC       0xB7
#define SESSION_ID_LENGTH        32

// ─── Session structure ──────────────────────────────────────────────────────
struct AuthSession {
    char     id[SESSION_ID_LENGTH + 1];   // hex token
    char     username[16];                // logged-in username
    uint8_t  role;                        // 0=admin, 1=operator
    uint32_t createdAt;                   // unix timestamp when created
    uint32_t lastActivity;                // unix timestamp of last request
    bool     active;
};

// ─── IP rate-limit tracking ─────────────────────────────────────────────────
struct IpTracker {
    char     ip[16];
    uint8_t  failCount;
    uint32_t lockoutUntil;               // unix timestamp, 0 = not locked
    uint32_t lastAttemptMs;              // millis() of last attempt
};

// ─── API ────────────────────────────────────────────────────────────────────
void     initAuth();
bool     requireAuth(AsyncWebServerRequest *req);
bool     requireAdmin(AsyncWebServerRequest *req);

String   hashPassword(const char *password);
bool     verifyPassword(const char *password, const char *hash);

// Multi-user auth
UserEntry* verifyCredentials(const char *username, const char *password);
UserEntry* verifyUserByPin(const char *pin);
int       findUserSlot(const char *username);
int       countActiveUsers();
bool      hasActiveAdmin();

String   createSession(const char *username, uint8_t role);
bool     validateSession(const char *token);
uint8_t  getSessionRole(const char *token);
void     destroySession(const char *token);
void     touchSession(const char *token);
void     purgeExpiredSessions();

int      checkRateLimit(const String &ip);      // returns retry_after_sec, 0 = allowed
void     recordFailedAttempt(const String &ip);
void     resetFailedAttempts(const String &ip);

String   getClientIP(AsyncWebServerRequest *req);
