#include "slave_keypad.h"
#include "slave_web.h"
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// ═══════════════════════════════════════════════════════════════════════════════
// RS-485 Keypad Slave Firmware — Main Logic
// ═══════════════════════════════════════════════════════════════════════════════
//
// This firmware runs on each keypad+LCD slave device.
// It NEVER initiates communication — it only responds to Master polls.
//
// ═══════════════════════════════════════════════════════════════════════════════

// ─── LCD ────────────────────────────────────────────────────────────────────
LiquidCrystal_I2C lcd(LCD_ADDR, LCD_COLS, LCD_ROWS);

// ─── RS-485 state ───────────────────────────────────────────────────────────
static char rxBuffer[KEYPAD_MAX_MSG_LEN];
static uint8_t rxPos = 0;

// ─── Keypad state (PCF8574 I2C) ─────────────────────────────────────────────
static char lastKey = 0;
static uint32_t lastKeyTime = 0;

// ─── User input state ───────────────────────────────────────────────────────
static char pinBuffer[PIN_MAX_LEN + 1] = {0};    // 4-digit PIN (null-terminated)
static uint8_t pinPos = 0;
static char modeLetter = 0;                        // A/B/C/D set by user
static bool userInputReady = false;                // true when # was pressed
static char payloadBuffer[16] = {0};               // e.g. "1234*A" or "1234"
static bool isArmed = false;                       // cached from S: broadcast
static char currentStateStr[20] = "DISARMED";      // cached state display string

// ─── LCD display state ──────────────────────────────────────────────────────
static bool showDoneMsg = false;
static char doneMsg[18] = {0};
static uint32_t doneMsgStartMs = 0;
static bool backlightOn = true;
static uint32_t lastActivityMs = 0;

// ─── RS-485 direction control ───────────────────────────────────────────────
static inline void rs485SetTx() {
  if (RS485_DE_RE_PIN >= 0) {
    digitalWrite(RS485_DE_RE_PIN, HIGH);
    delayMicroseconds(50);
  }
}

static inline void rs485SetRx() {
  if (RS485_DE_RE_PIN >= 0) {
    Serial1.flush();
    delayMicroseconds(50);
    digitalWrite(RS485_DE_RE_PIN, LOW);
  }
}

// ─── Send a response (only when addressed) ──────────────────────────────────
static void sendResponse(const char* msg) {
  rs485SetTx();
  Serial1.print(msg);
  Serial1.print('\n');
  Serial1.flush();
  rs485SetRx();
  lastActivityMs = millis();
  slaveWebNotifyTX(msg);  // log to web UI
  Serial.printf("[SLAVE %d] TX: %s\n", SLAVE_ID, msg);
}

// ─── LCD helpers ────────────────────────────────────────────────────────────
static void lcdClearRow(uint8_t row) {
  lcd.setCursor(0, row);
  for (uint8_t i = 0; i < LCD_COLS; i++) lcd.print(' ');
  lcd.setCursor(0, row);
}

static void lcdCenterPrint(uint8_t row, const char* msg) {
  uint8_t len = strlen(msg);
  uint8_t pad = (LCD_COLS > len) ? (LCD_COLS - len) / 2 : 0;
  lcd.setCursor(pad, row);
  lcd.print(msg);
}

static void ensureBacklight() {
  if (!backlightOn) {
    lcd.backlight();
    backlightOn = true;
  }
  lastActivityMs = millis();
}

// ─── Helper: replace _ with space for display ──────────────────────────────
static void formatState(char* out, size_t outLen, const char* raw) {
  size_t i = 0;
  while (raw[i] && i < outLen - 1) {
    out[i] = (raw[i] == '_') ? ' ' : raw[i];
    i++;
  }
  out[i] = '\0';
}

// ─── Display the appropriate screen ─────────────────────────────────────────
static void updateDisplay() {
  // Row 0: alarm state with spaces instead of underscores
  char displayState[21];
  formatState(displayState, sizeof(displayState), currentStateStr);
  lcdClearRow(0);
  lcdCenterPrint(0, displayState);

  if (showDoneMsg) {
    // OK/Error overlay: show on row 3, clear other rows
    lcdClearRow(1);
    lcdClearRow(2);
    lcdClearRow(3);
    lcdCenterPrint(3, doneMsg);
    return;
  }

  if (pinPos == 0 && !userInputReady) {
    // Idle — show placeholder so "Enter PIN:" stays in same position as typing
    lcdClearRow(1);
    lcdCenterPrint(1, "Enter PIN: ____");
    lcdClearRow(2);
    lcdClearRow(3);
  } else if (pinPos > 0 && pinPos < PIN_MAX_LEN) {
    // Typing PIN — build centered "Enter PIN: **__" string
    char pinLine[16];
    snprintf(pinLine, sizeof(pinLine), "Enter PIN: ");
    for (uint8_t i = 0; i < pinPos; i++) pinLine[11 + i] = '*';
    for (uint8_t i = pinPos; i < PIN_MAX_LEN; i++) pinLine[11 + i] = '_';
    pinLine[15] = '\0';
    lcdClearRow(1);
    lcdCenterPrint(1, pinLine);
    lcdClearRow(2);
    lcdClearRow(3);
  } else if (pinPos >= PIN_MAX_LEN && !userInputReady && !isArmed) {
    // PIN complete, disarmed — show mode choices on rows 2 and 3
    lcdClearRow(1);
    lcdCenterPrint(1, "Enter PIN: ****");
    lcdClearRow(2);
    lcdCenterPrint(2, LCD_MSG_MODES_ROW1);   // A-Home   B-Away
    lcdClearRow(3);
    lcdCenterPrint(3, LCD_MSG_MODES_ROW2);   // C-Night D-Vacation
  } else if (pinPos >= PIN_MAX_LEN && !userInputReady && isArmed) {
    // PIN complete, armed — show disarm prompt on row 2
    lcdClearRow(1);
    lcdCenterPrint(1, "Enter PIN: ****");
    lcdClearRow(2);
    lcdCenterPrint(2, LCD_MSG_PRESS_HASH);
    lcdClearRow(3);
  } else if (userInputReady) {
    lcdClearRow(1);
    lcdCenterPrint(1, "Enter PIN: ****");
    lcdClearRow(2);
    lcdClearRow(3);
    lcdCenterPrint(3, LCD_MSG_SENDING);
  }
}

// ─── Parse keypad input character ──────────────────────────────────────────
static bool isDigit(char c) { return c >= '0' && c <= '9'; }
static bool isModeKey(char c) { return c == 'A' || c == 'B' || c == 'C' || c == 'D'; }

static void handleKeyInput(char key) {
  ensureBacklight();

  if (key == '*') {
    memset(pinBuffer, 0, sizeof(pinBuffer));
    pinPos = 0;
    modeLetter = 0;
    userInputReady = false;
    showDoneMsg = false;
    memset(payloadBuffer, 0, sizeof(payloadBuffer));
    Serial.printf("[SLAVE %d] Input cleared\n", SLAVE_ID);
    return;
  }

  if (showDoneMsg) {
    showDoneMsg = false;
    memset(pinBuffer, 0, sizeof(pinBuffer));
    pinPos = 0;
    modeLetter = 0;
    userInputReady = false;
    memset(payloadBuffer, 0, sizeof(payloadBuffer));
  }

  if (userInputReady) {
    return;
  }

  if (isDigit(key)) {
    if (pinPos < PIN_MAX_LEN) {
      pinBuffer[pinPos++] = key;
      pinBuffer[pinPos] = '\0';
      Serial.printf("[SLAVE %d] PIN digit %d/%d\n", SLAVE_ID, pinPos, PIN_MAX_LEN);
    }
    return;
  }

  if (isModeKey(key)) {
    if (pinPos == PIN_MAX_LEN) {
      modeLetter = key;
      snprintf(payloadBuffer, sizeof(payloadBuffer), "%s*%c", pinBuffer, modeLetter);
      userInputReady = true;
      Serial.printf("[SLAVE %d] Input finalized: %s\n", SLAVE_ID, payloadBuffer);
    }
    return;
  }

  if (key == '#') {
    if (pinPos == PIN_MAX_LEN) {
      if (modeLetter != 0) {
        snprintf(payloadBuffer, sizeof(payloadBuffer), "%s*%c", pinBuffer, modeLetter);
      } else if (isArmed) {
        snprintf(payloadBuffer, sizeof(payloadBuffer), "%s", pinBuffer);
      } else {
        return;
      }
      userInputReady = true;
      Serial.printf("[SLAVE %d] Input finalized (#): %s\n", SLAVE_ID, payloadBuffer);
    }
    return;
  }
}

// ─── PCF8574 I2C helper — write a byte to the expander ──────────────────────
static bool pcf8574Write(uint8_t data) {
  Wire.beginTransmission(PCF8574_ADDR);
  Wire.write(data);
  return Wire.endTransmission() == 0;
}

// ─── PCF8574 I2C helper — read a byte from the expander ────────────────────
static bool pcf8574Read(uint8_t* out) {
  Wire.requestFrom((uint8_t)PCF8574_ADDR, (uint8_t)1);
  if (Wire.available()) {
    *out = Wire.read();
    return true;
  }
  return false;
}

// ─── Keypad scanning via PCF8574 I2C (non-blocking, debounced) ──────────────
static void scanKeypad() {
  uint32_t now = millis();
  if (now - lastKeyTime < KEYPAD_DEBOUNCE_MS) return;

  char pressed = 0;

  for (uint8_t r = 0; r < 4; r++) {
    uint8_t outVal = 0xFF;  // all pins HIGH (inputs float / pull-ups active)
    outVal &= ~(1U << r);    // drive row r LOW (active row scan)

    if (!pcf8574Write(outVal)) {
      return;
    }

    delayMicroseconds(10);

    uint8_t portVal = 0;
    if (!pcf8574Read(&portVal)) {
      return;
    }

    uint8_t colBits = (portVal & PCF8574_COL_MASK) >> 4;
    for (uint8_t c = 0; c < 4; c++) {
      if (!(colBits & (1U << c))) {
        pressed = KEYPAD_MAP[r][c];
        break;
      }
    }

    if (pressed) break;
  }

  pcf8574Write(0xFF);

  if (pressed && pressed != lastKey) {
    lastKey = pressed;
    lastKeyTime = now;
    handleKeyInput(pressed);
  } else if (!pressed) {
    lastKey = 0;
  }
}

// ─── Handle incoming message from Master ────────────────────────────────────
static void handleMasterMessage(const char* msg) {
  ensureBacklight();
  slaveWebNotifyRX(msg);  // log to web UI
  Serial.printf("[SLAVE %d] RX: %s\n", SLAVE_ID, msg);

  if (strlen(msg) < 3) return;

  char prefix = msg[0];
  if (msg[1] != ':') return;

  // Broadcast messages: "S:<state>" — all slaves process
  if (prefix == KEYPAD_PREFIX_STATE) {
    const char* state = msg + 2;
    strlcpy(currentStateStr, state, sizeof(currentStateStr));
    isArmed = (strcmp(state, "DISARMED") != 0);
    updateDisplay();
    return;
  }

  // Point-to-point messages: parse slave ID
  const char* idStart = msg + 2;
  int msgSlaveId = atoi(idStart);

  // Use runtime ID (web-configurable) or compile-time SLAVE_ID
  uint8_t effectiveId = slaveWebGetRuntimeId();
  if (msgSlaveId != effectiveId) return;

  switch (prefix) {
    case KEYPAD_PREFIX_POLL: {
      char resp[10];
      snprintf(resp, sizeof(resp), "%c:%d:%d",
               KEYPAD_PREFIX_RESP, effectiveId, userInputReady ? 1 : 0);
      sendResponse(resp);
      break;
    }

    case KEYPAD_PREFIX_GET: {
      if (userInputReady) {
        char resp[KEYPAD_MAX_MSG_LEN];
        snprintf(resp, sizeof(resp), "%c:%d:%s",
                 KEYPAD_PREFIX_DATA, effectiveId, payloadBuffer);
        sendResponse(resp);
      }
      break;
    }

    case KEYPAD_PREFIX_ACK: {
      userInputReady = false;
      memset(pinBuffer, 0, sizeof(pinBuffer));
      pinPos = 0;
      modeLetter = 0;
      memset(payloadBuffer, 0, sizeof(payloadBuffer));

      strlcpy(doneMsg, LCD_MSG_OK, sizeof(doneMsg));
      showDoneMsg = true;
      doneMsgStartMs = millis();
      updateDisplay();
      break;
    }

    case KEYPAD_PREFIX_ERR: {
      const char* errCode = strchr(idStart, ':');
      if (errCode) errCode++;

      userInputReady = false;
      memset(pinBuffer, 0, sizeof(pinBuffer));
      pinPos = 0;
      modeLetter = 0;
      memset(payloadBuffer, 0, sizeof(payloadBuffer));

      if (errCode && strncmp(errCode, KEYPAD_ERR_WRONG_PIN, strlen(KEYPAD_ERR_WRONG_PIN)) == 0) {
        strlcpy(doneMsg, LCD_MSG_WRONG_PIN, sizeof(doneMsg));
      } else {
        snprintf(doneMsg, sizeof(doneMsg), "ERROR");
      }
      showDoneMsg = true;
      doneMsgStartMs = millis();
      updateDisplay();
      break;
    }

    default:
      break;
  }
}

// ─── Read incoming data from RS-485 bus ─────────────────────────────────────
static void readRS485() {
  while (Serial1.available()) {
    char c = Serial1.read();

    if (c == '\n' || c == '\r') {
      if (rxPos > 0) {
        rxBuffer[rxPos] = '\0';
        handleMasterMessage(rxBuffer);
        rxPos = 0;
      }
    } else {
      if (rxPos < KEYPAD_MAX_MSG_LEN - 1) {
        rxBuffer[rxPos++] = c;
      } else {
        rxPos = 0;
      }
    }
  }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Public API
// ═══════════════════════════════════════════════════════════════════════════════

void slaveSetup() {
  Serial.begin(115200);
  delay(100);

  Serial.printf("\n[SLAVE %d] Booting... (v%s)\n", SLAVE_ID, FIRMWARE_VERSION);

  // ─── Init RS-485 direction pin ──────────────────────────────────────────
  if (RS485_DE_RE_PIN >= 0) {
    pinMode(RS485_DE_RE_PIN, OUTPUT);
    digitalWrite(RS485_DE_RE_PIN, LOW);
  }

  // ─── Init RS-485 UART ───────────────────────────────────────────────────
  Serial1.begin(RS485_BAUD, SERIAL_8N1, RS485_RX_PIN, RS485_TX_PIN);

  // ─── Init I2C bus ───────────────────────────────────────────────────────
  Wire.begin(I2C_SDA, I2C_SCL);

  // ─── Init PCF8574 I2C keypad expander ───────────────────────────────────
  pcf8574Write(0xFF);
  Serial.printf("[SLAVE %d] PCF8574 keypad expander at 0x%02X\n", SLAVE_ID, PCF8574_ADDR);

  // ─── Init I2C LCD ───────────────────────────────────────────────────────
  lcd.init();
  lcd.backlight();
  backlightOn = true;
  lcd.clear();

  // Boot screen
  char bootLine[17];
  snprintf(bootLine, sizeof(bootLine), "Alarm Keypad #%d", SLAVE_ID);
  lcdCenterPrint(0, bootLine);
  lcdCenterPrint(1, "Booting...");
  delay(1500);

  // Init web server (WiFi AP + WebSocket + HTTP)
  slaveWebInit(SLAVE_ID);

  // Show ready state
  lcdClearRow(0);
  lcdCenterPrint(0, currentStateStr);
  lcdClearRow(1);
  lcdCenterPrint(1, LCD_MSG_ENTER_PIN);

  lastActivityMs = millis();
  Serial.printf("[SLAVE %d] Ready. Waiting for Master polls...\n", SLAVE_ID);
}

void slaveLoop() {
  uint32_t now = millis();

  // ─── Read RS-485 messages from Master ───────────────────────────────────
  readRS485();

  // ─── Scan keypad (debounced, non-blocking) ──────────────────────────────
  scanKeypad();

  // ─── Web server & WebSocket events ──────────────────────────────────────
  slaveWebLoop();

  // ─── LCD done message timeout ────────────────────────────────────────────
  if (showDoneMsg && (now - doneMsgStartMs >= LCD_MSG_DONE_TIMEOUT)) {
    showDoneMsg = false;
    updateDisplay();
  }

  // ─── LCD backlight auto-off ─────────────────────────────────────────────
  if (backlightOn && (now - lastActivityMs >= LCD_BACKLIGHT_TIMEOUT_MS)) {
    lcd.noBacklight();
    backlightOn = false;
  }

  // ─── Periodic display refresh ───────────────────────────────────────────
  static uint32_t lastDisplayRefresh = 0;
  if (now - lastDisplayRefresh >= 500) {
    lastDisplayRefresh = now;
    updateDisplay();
  }

  yield();
}

// ─── Public key injection (called from web UI via slave_web.cpp) ────────────
void slaveInjectKey(char key) {
  handleKeyInput(key);
}

// ─── Helper: write a centered string into a 20-char row buffer ──────────────
static void fillRow(char* row, size_t rowLen, const char* msg) {
  memset(row, ' ', LCD_COLS); row[LCD_COLS] = '\0';
  size_t len = strlen(msg);
  size_t pad = (LCD_COLS > len) ? (LCD_COLS - len) / 2 : 0;
  memcpy(row + pad, msg, min((size_t)LCD_COLS - pad, len));
  row[min(rowLen - 1, (size_t)LCD_COLS)] = '\0';
}

// ─── Get LCD content for web UI virtual display (20x4) ──────────────────────
void slaveGetLcdContent(char* row0, size_t r0len, char* row1, size_t r1len,
                        char* row2, size_t r2len, char* row3, size_t r3len) {
  // Row 0: alarm state with spaces instead of underscores
  char displayState[21];
  formatState(displayState, sizeof(displayState), currentStateStr);
  fillRow(row0, r0len, displayState);

  if (showDoneMsg) {
    // OK/Error — clear rows 1-2, show message on row 3
    memset(row1, ' ', LCD_COLS); row1[LCD_COLS] = '\0'; row1[min(r1len-1,(size_t)LCD_COLS)] = '\0';
    memset(row2, ' ', LCD_COLS); row2[LCD_COLS] = '\0'; row2[min(r2len-1,(size_t)LCD_COLS)] = '\0';
    fillRow(row3, r3len, doneMsg);
    return;
  }

  if (pinPos == 0 && !userInputReady) {
    fillRow(row1, r1len, "Enter PIN: ____");
    memset(row2, ' ', LCD_COLS); row2[LCD_COLS] = '\0'; row2[min(r2len-1,(size_t)LCD_COLS)] = '\0';
    memset(row3, ' ', LCD_COLS); row3[LCD_COLS] = '\0'; row3[min(r3len-1,(size_t)LCD_COLS)] = '\0';
  } else if (pinPos > 0 && pinPos < PIN_MAX_LEN) {
    // Typing PIN — build centered "Enter PIN: **__" string
    char pinLine[16];
    snprintf(pinLine, sizeof(pinLine), "Enter PIN: ");
    for (uint8_t i = 0; i < pinPos; i++) pinLine[11 + i] = '*';
    for (uint8_t i = pinPos; i < PIN_MAX_LEN; i++) pinLine[11 + i] = '_';
    pinLine[15] = '\0';
    fillRow(row1, r1len, pinLine);
    memset(row2, ' ', LCD_COLS); row2[LCD_COLS] = '\0'; row2[min(r2len-1,(size_t)LCD_COLS)] = '\0';
    memset(row3, ' ', LCD_COLS); row3[LCD_COLS] = '\0'; row3[min(r3len-1,(size_t)LCD_COLS)] = '\0';
  } else if (pinPos >= PIN_MAX_LEN && !userInputReady && !isArmed) {
    // PIN complete, disarmed — mode choices on rows 2 & 3
    fillRow(row1, r1len, "Enter PIN: ****");
    fillRow(row2, r2len, LCD_MSG_MODES_ROW1);   // A-Home   B-Away
    fillRow(row3, r3len, LCD_MSG_MODES_ROW2);   // C-Night D-Vacation
  } else if (pinPos >= PIN_MAX_LEN && !userInputReady && isArmed) {
    // PIN complete, armed — disarm prompt
    fillRow(row1, r1len, "Enter PIN: ****");
    fillRow(row2, r2len, LCD_MSG_PRESS_HASH);
    memset(row3, ' ', LCD_COLS); row3[LCD_COLS] = '\0'; row3[min(r3len-1,(size_t)LCD_COLS)] = '\0';
  } else if (userInputReady) {
    fillRow(row1, r1len, "Enter PIN: ****");
    memset(row2, ' ', LCD_COLS); row2[LCD_COLS] = '\0'; row2[min(r2len-1,(size_t)LCD_COLS)] = '\0';
    fillRow(row3, r3len, LCD_MSG_SENDING);
  }
}
