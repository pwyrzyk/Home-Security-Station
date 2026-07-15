#include "keypad_comm.h"
#include "auth.h"
#include "alarm_mode.h"
#include "event_log.h"
#include "rs485_web.h"

// ═══════════════════════════════════════════════════════════════════════════════
// RS-485 Keypad Communication — Master Implementation
// ═══════════════════════════════════════════════════════════════════════════════
//
// This module is fully self-contained. It only interacts with the rest of the
// system through well-defined public APIs:
//   INPUT  (from system):  keypadCommNotifyState() — called on state change
//   OUTPUT (to system):    applyModeAndPublish() / disarmAndPublish()
//                          verifyUserByPin() — user identification
//                          logEvent() — event logging
//
// No existing logic is modified. This is a pure input/output peripheral module.
// ═══════════════════════════════════════════════════════════════════════════════

// ─── Internal state ────────────────────────────────────────────────────────
static KeypadSlaveStatus slaves[KEYPAD_MAX_SLAVES];
static char rxBuffer[KEYPAD_MAX_MSG_LEN];
static uint8_t rxPos = 0;
static AlarmState lastBroadcastState = AlarmState::DISARMED;

// ─── RS-485 direction control ──────────────────────────────────────────────
static inline void rs485SetTx() {
  if (RS485_DE_RE_PIN >= 0) {
    digitalWrite(RS485_DE_RE_PIN, HIGH);
    delayMicroseconds(50);  // transceiver settling time
  }
}

static inline void rs485SetRx() {
  if (RS485_DE_RE_PIN >= 0) {
    // Wait for UART TX to complete before switching direction
    Serial1.flush();
    delayMicroseconds(50);
    digitalWrite(RS485_DE_RE_PIN, LOW);
  }
}

// ─── Transmit a message to all slaves ──────────────────────────────────────
static void rs485Send(const char* msg) {
  rs485SetTx();
  Serial1.print(msg);
  Serial1.print('\n');
  Serial1.flush();
  rs485SetRx();

  // Notify web monitor of TX
  rs485NotifyTX(msg);
}

// ─── Map AlarmState to broadcast string ────────────────────────────────────
static const char* alarmStateToKeypadString(AlarmState s) {
  switch (s) {
    case AlarmState::DISARMED:            return "DISARMED";
    case AlarmState::ARMED_HOME:          return "ARMED_HOME";
    case AlarmState::ARMED_AWAY:          return "ARMED_AWAY";
    case AlarmState::ARMED_NIGHT:         return "ARMED_NIGHT";
    case AlarmState::ARMED_VACATION:      return "ARMED_VACATION";
    case AlarmState::ARMED_CUSTOM_BYPASS: return "ARMED_CUSTOM";
    case AlarmState::PENDING:             return "PENDING";
    case AlarmState::TRIGGERED:           return "ALARM_TRIGGERED";
    default:                              return "UNKNOWN";
  }
}

// ─── Map keypad mode letter to AlarmMode ───────────────────────────────────
static AlarmMode keypadLetterToMode(char letter) {
  switch (letter) {
    case KEYPAD_SUFFIX_ARM_HOME:     return AlarmMode::ARMED_HOME;
    case KEYPAD_SUFFIX_ARM_AWAY:     return AlarmMode::ARMED_AWAY;
    case KEYPAD_SUFFIX_ARM_NIGHT:    return AlarmMode::ARMED_NIGHT;
    case KEYPAD_SUFFIX_ARM_VACATION: return AlarmMode::ARMED_VACATION;
    default:                         return AlarmMode::DISARMED;  // invalid
  }
}

// ─── Process a heartbeat message: "HB:<slaveId>" ───────────────────────────
static void handleHeartbeat(const char* msg) {
  // Expected format: "HB:1" .. "HB:4"
  const char* idStr = msg + strlen(KEYPAD_HB_PREFIX);
  int slaveId = atoi(idStr);
  if (slaveId < 1 || slaveId > KEYPAD_MAX_SLAVES) {
    Serial.printf("[KEYPAD] Invalid heartbeat slave ID: %d\n", slaveId);
    return;
  }

  uint8_t idx = slaveId - 1;
  bool wasOffline = !slaves[idx].online;
  slaves[idx].online = true;
  slaves[idx].lastHeartbeatMs = millis();

  if (wasOffline) {
    Serial.printf("[KEYPAD] Slave %d came online\n", slaveId);
    char logBuf[48];
    snprintf(logBuf, sizeof(logBuf), "Keypad %d online", slaveId);
    logSystem(logBuf);

    // Send current state to newly connected slave
    AlarmState currentState = deriveGlobalAlarmState();
    char stateMsg[48];
    snprintf(stateMsg, sizeof(stateMsg), "%s%s", KEYPAD_STATE_PREFIX,
             alarmStateToKeypadString(currentState));
    rs485Send(stateMsg);
  }
}

// ─── Process a command message: "CMD:<slaveId>:<pin>*<mode>" ────────────────
static void handleCommand(const char* msg) {
  // Expected format: "CMD:1:1234*A"
  const char* payload = msg + strlen(KEYPAD_CMD_PREFIX);

  // Parse slave ID
  char* colonPos = strchr(payload, ':');
  if (!colonPos) {
    Serial.println("[KEYPAD] Malformed CMD: missing slave separator");
    return;
  }

  int slaveId = atoi(payload);
  if (slaveId < 1 || slaveId > KEYPAD_MAX_SLAVES) {
    Serial.printf("[KEYPAD] Invalid CMD slave ID: %d\n", slaveId);
    return;
  }

  // Parse PIN and mode: "1234*A"
  const char* pinStart = colonPos + 1;
  char* starPos = strchr(pinStart, '*');
  if (!starPos || starPos == pinStart) {
    Serial.println("[KEYPAD] Malformed CMD: missing * separator or empty PIN");
    return;
  }

  // Extract PIN (up to 4 digits)
  size_t pinLen = starPos - pinStart;
  if (pinLen == 0 || pinLen > 4) {
    Serial.println("[KEYPAD] Invalid PIN length");
    return;
  }

  char pin[5] = {0};
  memcpy(pin, pinStart, pinLen);
  pin[pinLen] = '\0';

  // Extract mode letter (single char after *)
  char modeLetter = *(starPos + 1);
  if (modeLetter == '\0') {
    Serial.println("[KEYPAD] Missing mode letter after *");
    return;
  }

  // ─── Authenticate user by PIN ─────────────────────────────────────────
  UserEntry* user = verifyUserByPin(pin);
  if (!user) {
    Serial.printf("[KEYPAD] Auth failed from slave %d: unknown PIN\n", slaveId);
    char logBuf[64];
    snprintf(logBuf, sizeof(logBuf), "Keypad %d: auth failed (bad PIN)", slaveId);
    logSystem(logBuf);
    return;
  }

  Serial.printf("[KEYPAD] User '%s' authenticated from slave %d\n",
                user->username, slaveId);

  // ─── Determine action based on current alarm state ────────────────────
  AlarmState currentState = deriveGlobalAlarmState();

  if (currentState == AlarmState::DISARMED) {
    // System is disarmed → arm in the requested mode
    AlarmMode targetMode = keypadLetterToMode(modeLetter);
    if (targetMode == AlarmMode::DISARMED) {
      Serial.printf("[KEYPAD] Invalid mode letter '%c' from slave %d\n",
                    modeLetter, slaveId);
      return;
    }

    char source[32];
    snprintf(source, sizeof(source), "keypad_%d/%s", slaveId, user->username);

    if (applyModeAndPublish(targetMode, source)) {
      Serial.printf("[KEYPAD] Armed %s by %s via keypad %d\n",
                    alarmStateToKeypadString(static_cast<AlarmState>(
                      static_cast<uint8_t>(targetMode))),
                    user->username, slaveId);
      char logBuf[80];
      snprintf(logBuf, sizeof(logBuf), "Keypad %d: %s armed %s",
               slaveId, user->username,
               alarmStateToKeypadString(static_cast<AlarmState>(
                 static_cast<uint8_t>(targetMode))));
      logSystem(logBuf);
    } else {
      Serial.printf("[KEYPAD] Arm failed (no mode profile?) from slave %d\n", slaveId);
    }
  } else {
    // System is armed (any mode) → disarm with valid PIN
    char source[32];
    snprintf(source, sizeof(source), "keypad_%d/%s", slaveId, user->username);

    disarmAndPublish(source);
    Serial.printf("[KEYPAD] Disarmed by %s via keypad %d\n",
                  user->username, slaveId);
    char logBuf[64];
    snprintf(logBuf, sizeof(logBuf), "Keypad %d: %s disarmed", slaveId, user->username);
    logSystem(logBuf);
  }
}

// ─── Process a complete received line ──────────────────────────────────────
static void processMessage(const char* msg) {
  // Notify web monitor of all RX traffic
  rs485NotifyRX(msg);

  if (strncmp(msg, KEYPAD_HB_PREFIX, strlen(KEYPAD_HB_PREFIX)) == 0) {
    handleHeartbeat(msg);
  } else if (strncmp(msg, KEYPAD_CMD_PREFIX, strlen(KEYPAD_CMD_PREFIX)) == 0) {
    handleCommand(msg);
  } else {
    Serial.printf("[KEYPAD] Unknown message: %.32s\n", msg);
  }
}

// ─── Check for slave timeouts ──────────────────────────────────────────────
static void checkSlaveTimeouts() {
  uint32_t now = millis();
  for (uint8_t i = 0; i < KEYPAD_MAX_SLAVES; i++) {
    if (slaves[i].online &&
        (now - slaves[i].lastHeartbeatMs) > KEYPAD_HEARTBEAT_TIMEOUT_MS) {
      slaves[i].online = false;
      Serial.printf("[KEYPAD] Slave %d went offline (heartbeat timeout)\n", i + 1);
      char logBuf[48];
      snprintf(logBuf, sizeof(logBuf), "Keypad %d offline", i + 1);
      logSystem(logBuf);
    }
  }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Public API
// ═══════════════════════════════════════════════════════════════════════════════

void keypadCommInit() {
  // Initialize direction control pin if used
  if (RS485_DE_RE_PIN >= 0) {
    pinMode(RS485_DE_RE_PIN, OUTPUT);
    digitalWrite(RS485_DE_RE_PIN, LOW);  // start in RX mode
  }

  // Initialize UART1 for RS-485
  Serial1.begin(RS485_BAUD, SERIAL_8N1, RS485_RX_PIN, RS485_TX_PIN);

  // Clear slave status
  for (uint8_t i = 0; i < KEYPAD_MAX_SLAVES; i++) {
    slaves[i].online = false;
    slaves[i].lastHeartbeatMs = 0;
  }

  rxPos = 0;
  lastBroadcastState = deriveGlobalAlarmState();

  Serial.println("[KEYPAD] RS-485 Master initialized (TX=GPIO4, RX=GPIO1)");
}

void keypadCommLoop() {
  // ─── Read incoming data from RS-485 bus ─────────────────────────────────
  while (Serial1.available()) {
    char c = Serial1.read();

    if (c == '\n' || c == '\r') {
      if (rxPos > 0) {
        rxBuffer[rxPos] = '\0';
        processMessage(rxBuffer);
        rxPos = 0;
      }
    } else {
      if (rxPos < KEYPAD_MAX_MSG_LEN - 1) {
        rxBuffer[rxPos++] = c;
      } else {
        // Buffer overflow — discard
        rxPos = 0;
        Serial.println("[KEYPAD] RX buffer overflow, discarding");
      }
    }
  }

  // ─── Periodic slave timeout check (every 10s) ──────────────────────────
  static uint32_t lastTimeoutCheck = 0;
  uint32_t now = millis();
  if (now - lastTimeoutCheck >= 10000) {
    lastTimeoutCheck = now;
    checkSlaveTimeouts();
  }
}

void keypadCommNotifyState(AlarmState state) {
  // Only broadcast on actual state change
  if (state == lastBroadcastState) return;
  lastBroadcastState = state;

  char msg[48];
  snprintf(msg, sizeof(msg), "%s%s", KEYPAD_STATE_PREFIX,
           alarmStateToKeypadString(state));
  rs485Send(msg);

  Serial.printf("[KEYPAD] Broadcast state: %s\n", alarmStateToKeypadString(state));
}

bool keypadSlaveOnline(uint8_t slaveIdx) {
  if (slaveIdx >= KEYPAD_MAX_SLAVES) return false;
  return slaves[slaveIdx].online;
}

uint32_t keypadSlaveLastHB(uint8_t slaveIdx) {
  if (slaveIdx >= KEYPAD_MAX_SLAVES) return 0;
  return slaves[slaveIdx].lastHeartbeatMs;
}

// ─── Raw send from external sources (e.g. RS-485 web monitor) ──────────
void rs485SendRaw(const char* msg) {
  rs485Send(msg);
}
