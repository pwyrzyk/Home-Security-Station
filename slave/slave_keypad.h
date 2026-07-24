#pragma once

#include <Arduino.h>

// ═══════════════════════════════════════════════════════════════════════════════
// RS-485 Keypad Slave Firmware — Hardware & Protocol Definitions
// ═══════════════════════════════════════════════════════════════════════════════
//
// This is the firmware for keypad + LCD slave devices on the RS-485 bus.
// It must be compiled SEPARATELY from the Master (separate PlatformIO env).
//
// Each physical slave device gets its own unique SLAVE_ID (1..KEYPAD_MAX_SLAVES).
// Change the #define below before flashing each device.
//
// Protocol (Master → Slave only, newline-terminated ASCII):
//   P:<id>              — Poll: query slave status
//   G:<id>              — Get payload: master wants the pending user input
//   A:<id>              — Ack: previous command was processed OK
//   E:<id>:<code>       — Error (WRONG_PIN, BAD_FMT)
//   S:<state>           — Broadcast alarm state (DISARMED, ARMED_HOME, etc.)
//
// Slave → Master (ONLY in response to P: or G:):
//   R:<id>:0            — Idle, no input pending
//   R:<id>:1            — User input ready (PIN + mode captured)
//   D:<id>:<payload>    — User command payload (e.g. "1234*A" or "5678")
//
// Hardware:
//   - ESP32-C3 (same as Master)
//   - RS-485 transceiver on UART1 (TX=GPIO4, RX=GPIO1)
//   - I2C LCD 16x2 (default addr 0x27)
//   - PCF8574 I2C port expander + 4x4 matrix keypad
//
// ═══════════════════════════════════════════════════════════════════════════════

// ─── Device Identity ───────────────────────────────────────────────────────
// CHANGE THIS for each physical slave device (1..4)
#ifndef SLAVE_ID
#define SLAVE_ID 1
#endif

#define FIRMWARE_VERSION "1.0.0-slave"

// ─── RS-485 hardware ───────────────────────────────────────────────────────
#define RS485_TX_PIN       4    // GPIO4 — UART1 TX
#define RS485_RX_PIN       1    // GPIO1 — UART1 RX
#define RS485_DE_RE_PIN   -1    // Direction control (-1 = auto-direction)
#define RS485_BAUD      9600    // Must match Master baud rate

// ─── Protocol constants ────────────────────────────────────────────────────
#define KEYPAD_MAX_SLAVES      4
#define KEYPAD_MAX_MSG_LEN     64

// Protocol prefixes
#define KEYPAD_PREFIX_POLL      'P'
#define KEYPAD_PREFIX_RESP      'R'
#define KEYPAD_PREFIX_GET       'G'
#define KEYPAD_PREFIX_DATA      'D'
#define KEYPAD_PREFIX_ACK       'A'
#define KEYPAD_PREFIX_ERR       'E'
#define KEYPAD_PREFIX_STATE     'S'

// Error codes from Master
#define KEYPAD_ERR_WRONG_PIN    "WRONG_PIN"
#define KEYPAD_ERR_BAD_FMT      "BAD_FMT"

// ─── I2C ──────────────────────────────────────────────────────────────────
#define I2C_SDA  0
#define I2C_SCL  3

// ─── I2C LCD ───────────────────────────────────────────────────────────────
#define LCD_ADDR      0x27
#define LCD_COLS      20
#define LCD_ROWS      4
#define LCD_BACKLIGHT_TIMEOUT_MS  30000   // auto-off backlight after 30s idle

// ─── PCF8574 I2C Keypad Expander ───────────────────────────────────────────
// 4x4 matrix keypad connected via PCF8574 I2C port expander.
// Default I2C address 0x20 (A0=A1=A2=GND). Change if address strap differs.
#define PCF8574_ADDR  0x20

// PCF8574 pin-to-keypad mapping:
//   P0-P3 = Rows 1-4 (outputs, driven LOW to scan)
//   P4-P7 = Columns 1-4 (inputs, read with internal pull-up via PCF8574)
#define PCF8574_ROW_MASK  0x0F   // P0-P3
#define PCF8574_COL_MASK  0xF0   // P4-P7

// Keypad debounce
#define KEYPAD_DEBOUNCE_MS   50

// ─── Keypad layout ─────────────────────────────────────────────────────────
static const char KEYPAD_MAP[4][4] = {
  {'1', '2', '3', 'A'},
  {'4', '5', '6', 'B'},
  {'7', '8', '9', 'C'},
  {'*', '0', '#', 'D'}
};

// ─── User input state ──────────────────────────────────────────────────────
#define PIN_MAX_LEN  4

// ─── Display messages ──────────────────────────────────────────────────────
#define LCD_MSG_OK           "OK"
#define LCD_MSG_WRONG_PIN    "WRONG PIN"
#define LCD_MSG_SENDING      "Sending..."
#define LCD_MSG_ENTER_PIN    "Enter PIN:"
#define LCD_MSG_PRESS_HASH   "Press # to DISARM"
#define LCD_MSG_MODES_ROW1   "A-Home   B-Away"
#define LCD_MSG_MODES_ROW2   "C-Night D-Vacation"
#define LCD_MSG_DONE_TIMEOUT 2000    // ms to show OK/Error before clearing

// ─── Function prototypes ───────────────────────────────────────────────────
void slaveSetup();
void slaveLoop();

// Inject a key press from external source (e.g., web UI virtual keypad).
// Same behavior as physical key press.
void slaveInjectKey(char key);

// Get the current LCD display content (mirrors the physical 20x4 LCD).
// Each row receives up to 21 bytes (20 chars + null).
void slaveGetLcdContent(char* row0, size_t r0len, char* row1, size_t r1len,
                        char* row2, size_t r2len, char* row3, size_t r3len);
