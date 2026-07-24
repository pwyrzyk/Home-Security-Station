#include "keypad_comm.h"
#include "auth.h"
#include "alarm_mode.h"
#include "event_log.h"
#include "rs485_web.h"

// ═══════════════════════════════════════════════════════════════════════════════
// RS-485 Keypad Communication — Master Implementation (Polling Protocol)
// ═══════════════════════════════════════════════════════════════════════════════
//
// This module implements a true Master-Slave polling protocol. Only the Master
// initiates communication — slaves never transmit unprompted.
//
// Round-robin polling: Master sends P:<id> to each slave in sequence.
// If a slave reports user_input_pending (R:<id>:1), the Master pauses the cycle,
// sends G:<id> to fetch the payload, processes the command, and resumes.
//
// Broadcasts (S:<state>) are queued and sent between poll cycles.
//
// PUBLIC API (unchanged):
//   keypadCommInit()      — called once in setup()
//   keypadCommLoop()      — called every loop iteration
//   keypadCommNotifyState() — called externally on state change
//   keypadSlaveOnline()   — query slave status
//   keypadSlaveLastHB()   — last response timestamp
//   rs485SendRaw()        — raw send for web monitor
//
// ═══════════════════════════════════════════════════════════════════════════════

// ─── Internal state ────────────────────────────────────────────────────────
static KeypadSlaveStatus slaves[KEYPAD_MAX_SLAVES];
static char rxBuffer[KEYPAD_MAX_MSG_LEN];
static uint8_t rxPos = 0;

// Polling state machine
static PollPhase pollPhase = PollPhase::PHASE_IDLE;
static uint8_t currentSlaveIdx = 0;       // 0..3, current round-robin position
static uint32_t pollActionStartMs = 0;    // when current action began (for timeout)
static uint32_t lastPollAdvanceMs = 0;    // when last poll was sent

// Broadcast queue (non-blocking — sent between polls)
static bool broadcastPending = false;
static char broadcastMsg[48];
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
    Serial1.flush();
    delayMicroseconds(50);
    digitalWrite(RS485_DE_RE_PIN, LOW);
  }
}

// ─── Transmit a message (low-level) ────────────────────────────────────────
static void rs485Send(const char* msg) {
  rs485SetTx();
  Serial1.print(msg);
  Serial1.print('\n');
  Serial1.flush();
  rs485SetRx();
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
    default:                         return AlarmMode::DISARMED;
  }
}

// ─── Mark a slave as having responded successfully ─────────────────────────
static void markSlaveResponse(uint8_t idx) {
  bool wasOffline = !slaves[idx].online;
  slaves[idx].online = true;
  slaves[idx].lastResponseMs = millis();
  slaves[idx].consecutiveTimeouts = 0;

  if (wasOffline) {
    uint8_t slaveId = idx + 1;
    Serial.printf("[KEYPAD] Slave %d came online\n", slaveId);
    char logBuf[48];
    snprintf(logBuf, sizeof(logBuf), "Keypad %d online", slaveId);
    logSystem(logBuf);

    // Send current state to newly connected slave
    AlarmState currentState = deriveGlobalAlarmState();
    char stateMsg[48];
    snprintf(stateMsg, sizeof(stateMsg), "%c:%s",
             KEYPAD_PREFIX_STATE, alarmStateToKeypadString(currentState));
    // Queue this broadcast — it will be sent when bus is free
    // Actually, send immediately since we just received from this slave and
    // the bus direction is still controlled
    delayMicroseconds(500);  // inter-frame gap
    rs485Send(stateMsg);
  }
}

// ─── Mark a slave timeout ──────────────────────────────────────────────────
static void markSlaveTimeout(uint8_t idx) {
  slaves[idx].consecutiveTimeouts++;
  if (slaves[idx].online && slaves[idx].consecutiveTimeouts >= KEYPAD_OFFLINE_THRESHOLD) {
    slaves[idx].online = false;
    uint8_t slaveId = idx + 1;
    Serial.printf("[KEYPAD] Slave %d went offline (no response)\n", slaveId);
    char logBuf[48];
    snprintf(logBuf, sizeof(logBuf), "Keypad %d offline", slaveId);
    logSystem(logBuf);
  }
}

// ─── Advance to the next slave in round-robin ──────────────────────────────
static void advanceToNextSlave() {
  currentSlaveIdx = (currentSlaveIdx + 1) % KEYPAD_MAX_SLAVES;
  pollPhase = PollPhase::PHASE_IDLE;
  lastPollAdvanceMs = millis();
}

// ─── Send a poll message P:<id> ────────────────────────────────────────────
static void sendPoll() {
  uint8_t slaveId = currentSlaveIdx + 1;
  char msg[8];
  snprintf(msg, sizeof(msg), "%c:%d", KEYPAD_PREFIX_POLL, slaveId);
  rs485Send(msg);

  pollPhase = PollPhase::PHASE_WAIT_POLL;
  pollActionStartMs = millis();
}

// ─── Process a status response R:<slaveId>:<flag> ──────────────────────────
static void handleResponse(const char* msg) {
  // Expected format: "R:1:0" or "R:1:1"
  const char* colon1 = strchr(msg, ':');
  if (!colon1) return;
  const char* colon2 = strchr(colon1 + 1, ':');
  if (!colon2) return;

  int slaveId = atoi(colon1 + 1);
  if (slaveId < 1 || slaveId > KEYPAD_MAX_SLAVES) return;

  uint8_t idx = slaveId - 1;
  if (idx != currentSlaveIdx) {
    // Response from wrong slave — ignore
    return;
  }

  int flag = atoi(colon2 + 1);
  markSlaveResponse(idx);

  if (flag == 1) {
    // Slave has user input pending — request payload
    slaves[idx].userInputPending = true;
    char getMsg[8];
    snprintf(getMsg, sizeof(getMsg), "%c:%d", KEYPAD_PREFIX_GET, slaveId);
    rs485Send(getMsg);
    pollPhase = PollPhase::PHASE_WAIT_DATA;
    pollActionStartMs = millis();
  } else {
    // No pending input — advance to next slave
    advanceToNextSlave();
  }
}

// ─── Helper: send error response to a slave ────────────────────────────────
static void sendError(uint8_t slaveId, const char* code) {
  char errMsg[24];
  snprintf(errMsg, sizeof(errMsg), "%c:%d:%s", KEYPAD_PREFIX_ERR, slaveId, code);
  rs485Send(errMsg);
}

// ─── Process data payload D:<slaveId>:<pin>*<mode> ─────────────────────────
// RESTRUCTURED: every exit path sends a response (ACK or error) so the slave
// never gets stuck waiting forever.
static void handleDataPayload(const char* msg) {
  // Expected format: "D:<slaveId>:<pin>" or "D:<slaveId>:<pin>*<mode>"
  const char* payload = msg + 2;  // skip "D:"

  // ─── Parse slave ID ──────────────────────────────────────────────────
  char* colonPos = strchr(payload, ':');
  if (!colonPos) {
    Serial.printf("[KEYPAD] Data malformed: no colon in '%s'\n", payload);
    return;  // nothing to do, no slave to respond to
  }

  int slaveId = atoi(payload);
  if (slaveId < 1 || slaveId > KEYPAD_MAX_SLAVES) {
    Serial.printf("[KEYPAD] Data with invalid slave ID: %d\n", slaveId);
    return;
  }

  uint8_t idx = slaveId - 1;

  // Accept data from any slave we heard from, not just the one we last polled.
  // (currentSlaveIdx may have advanced if a timeout fired between handleResponse
  //  and handleDataPayload — a rare but real race condition.)
  markSlaveResponse(idx);

  // ─── Parse PIN and optional mode letter ──────────────────────────────
  const char* pinStart = colonPos + 1;
  char* starPos = strchr(pinStart, '*');

  char pin[5] = {0};
  char modeLetter = '\0';

  if (starPos && starPos != pinStart) {
    size_t pinLen = starPos - pinStart;
    if (pinLen == 0 || pinLen > 4) {
      Serial.printf("[KEYPAD] Invalid PIN length from slave %d\n", slaveId);
      sendError(slaveId, KEYPAD_ERR_BAD_FMT);
      advanceToNextSlave();
      return;
    }
    memcpy(pin, pinStart, pinLen);
    pin[pinLen] = '\0';
    modeLetter = *(starPos + 1);
    if (modeLetter == '\0') {
      Serial.printf("[KEYPAD] Missing mode letter after * from slave %d\n", slaveId);
      sendError(slaveId, KEYPAD_ERR_BAD_FMT);
      advanceToNextSlave();
      return;
    }
  } else {
    size_t pinLen = strlen(pinStart);
    if (pinLen == 0 || pinLen > 4) {
      Serial.printf("[KEYPAD] Invalid PIN length (no star) from slave %d\n", slaveId);
      sendError(slaveId, KEYPAD_ERR_BAD_FMT);
      advanceToNextSlave();
      return;
    }
    memcpy(pin, pinStart, pinLen);
    pin[pinLen] = '\0';
  }

  // ─── Authenticate user by PIN ─────────────────────────────────────────
  UserEntry* user = verifyUserByPin(pin);
  if (!user) {
    Serial.printf("[KEYPAD] Auth failed from slave %d: unknown PIN '%s'\n", slaveId, pin);
    char logBuf[64];
    snprintf(logBuf, sizeof(logBuf), "Keypad %d: auth failed (bad PIN)", slaveId);
    logSystem(logBuf);
    sendError(slaveId, KEYPAD_ERR_WRONG_PIN);
    advanceToNextSlave();
    return;
  }

  Serial.printf("[KEYPAD] User '%s' authenticated from slave %d\n",
                user->username, slaveId);

  // ─── Determine action based on current alarm state ────────────────────
  AlarmState currentState = deriveGlobalAlarmState();

  if (currentState == AlarmState::DISARMED) {
    if (modeLetter == '\0') {
      Serial.printf("[KEYPAD] PIN-only from slave %d refused (disarmed, no mode)\n", slaveId);
      // No error — just ignore. But still ACK so slave clears its buffer.
      char ackMsg[8];
      snprintf(ackMsg, sizeof(ackMsg), "%c:%d", KEYPAD_PREFIX_ACK, slaveId);
      rs485Send(ackMsg);
      slaves[idx].userInputPending = false;
      advanceToNextSlave();
      return;
    }

    AlarmMode targetMode = keypadLetterToMode(modeLetter);
    if (targetMode == AlarmMode::DISARMED) {
      Serial.printf("[KEYPAD] Invalid mode letter '%c' from slave %d\n", modeLetter, slaveId);
      sendError(slaveId, KEYPAD_ERR_BAD_FMT);
      advanceToNextSlave();
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
      // Send error even if profile missing — slave needs to know
      sendError(slaveId, KEYPAD_ERR_BAD_FMT);
      advanceToNextSlave();
      return;
    }
  } else {
    // System armed → disarm with valid PIN
    char source[32];
    snprintf(source, sizeof(source), "keypad_%d/%s", slaveId, user->username);

    disarmAndPublish(source);
    Serial.printf("[KEYPAD] Disarmed by %s via keypad %d\n",
                  user->username, slaveId);
    char logBuf[64];
    snprintf(logBuf, sizeof(logBuf), "Keypad %d: %s disarmed", slaveId, user->username);
    logSystem(logBuf);
  }

  // ─── Send ACK (common success path) ──────────────────────────────────
  char ackMsg[8];
  snprintf(ackMsg, sizeof(ackMsg), "%c:%d", KEYPAD_PREFIX_ACK, slaveId);
  rs485Send(ackMsg);

  slaves[idx].userInputPending = false;
  advanceToNextSlave();
}

// ─── Process a complete received line ──────────────────────────────────────
static void processMessage(const char* msg) {
  rs485NotifyRX(msg);

  if (msg[0] == KEYPAD_PREFIX_RESP && msg[1] == ':') {
    handleResponse(msg);
  } else if (msg[0] == KEYPAD_PREFIX_DATA && msg[1] == ':') {
    handleDataPayload(msg);
  } else {
    Serial.printf("[KEYPAD] Unknown message: %.32s\n", msg);
  }
  // Note: P:, G:, A:, E:, S: are Master→Slave — Master ignores them if echoed
}

// ═══════════════════════════════════════════════════════════════════════════════
// Public API
// ═══════════════════════════════════════════════════════════════════════════════

void keypadCommInit() {
  if (RS485_DE_RE_PIN >= 0) {
    pinMode(RS485_DE_RE_PIN, OUTPUT);
    digitalWrite(RS485_DE_RE_PIN, LOW);  // start in RX mode
  }

  Serial1.begin(RS485_BAUD, SERIAL_8N1, RS485_RX_PIN, RS485_TX_PIN);

  for (uint8_t i = 0; i < KEYPAD_MAX_SLAVES; i++) {
    slaves[i].online = false;
    slaves[i].lastResponseMs = 0;
    slaves[i].consecutiveTimeouts = 0;
    slaves[i].userInputPending = false;
  }

  rxPos = 0;
  pollPhase = PollPhase::PHASE_IDLE;
  currentSlaveIdx = 0;
  lastPollAdvanceMs = 0;
  broadcastPending = false;
  lastBroadcastState = deriveGlobalAlarmState();

  Serial.println("[KEYPAD] RS-485 Master initialized (polling mode, TX=GPIO4, RX=GPIO1)");
}

void keypadCommLoop() {
  uint32_t now = millis();

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
        rxPos = 0;
        Serial.println("[KEYPAD] RX buffer overflow, discarding");
      }
    }
  }

  // ─── Handle pending broadcast (between poll cycles only) ────────────────
  if (broadcastPending && pollPhase == PollPhase::PHASE_IDLE) {
    rs485Send(broadcastMsg);
    broadcastPending = false;
    lastPollAdvanceMs = now;  // reset timer so poll doesn't fire immediately after broadcast
  }

  // ─── Polling state machine ──────────────────────────────────────────────
  switch (pollPhase) {
    case PollPhase::PHASE_IDLE: {
      // Wait for poll interval before sending next poll
      if (now - lastPollAdvanceMs >= KEYPAD_POLL_INTERVAL_MS) {
        sendPoll();
      }
      break;
    }

    case PollPhase::PHASE_WAIT_POLL: {
      // Check for timeout
      if (now - pollActionStartMs >= KEYPAD_RESPONSE_TIMEOUT_MS) {
        markSlaveTimeout(currentSlaveIdx);
        advanceToNextSlave();
      }
      break;
    }

    case PollPhase::PHASE_WAIT_DATA: {
      // Check for timeout waiting for data payload
      if (now - pollActionStartMs >= KEYPAD_RESPONSE_TIMEOUT_MS) {
        markSlaveTimeout(currentSlaveIdx);
        advanceToNextSlave();
      }
      break;
    }

    case PollPhase::PHASE_DONE:
      // Should not persist — advance immediately
      advanceToNextSlave();
      break;
  }
}

void keypadCommNotifyState(AlarmState state) {
  if (state == lastBroadcastState) return;
  lastBroadcastState = state;

  snprintf(broadcastMsg, sizeof(broadcastMsg), "%c:%s",
           KEYPAD_PREFIX_STATE, alarmStateToKeypadString(state));
  broadcastPending = true;

  Serial.printf("[KEYPAD] Queued broadcast: %s\n", alarmStateToKeypadString(state));
}

bool keypadSlaveOnline(uint8_t slaveIdx) {
  if (slaveIdx >= KEYPAD_MAX_SLAVES) return false;
  return slaves[slaveIdx].online;
}

uint32_t keypadSlaveLastHB(uint8_t slaveIdx) {
  if (slaveIdx >= KEYPAD_MAX_SLAVES) return 0;
  return slaves[slaveIdx].lastResponseMs;
}

// ─── Raw send from external sources (e.g. RS-485 web monitor) ──────────
void rs485SendRaw(const char* msg) {
  rs485Send(msg);
}