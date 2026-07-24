#pragma once

#include <Arduino.h>
#include "config.h"

// ═══════════════════════════════════════════════════════════════════════════════
// RS-485 Keypad Communication Module — Master Side
// ═══════════════════════════════════════════════════════════════════════════════
//
// Master-Slave polling protocol (Modbus RTU style).
// Only the Master initiates communication — slaves NEVER transmit unprompted.
// This prevents RS-485 bus collisions that occurred with the old fire-and-forget
// heartbeat + command protocol.
//
// Protocol messages are ASCII line-based, newline-terminated:
//
//   Master → Slave:
//     "P:<id>"                    — Poll: query slave status
//     "G:<id>"                    — Get payload: request pending user input
//     "A:<id>"                    — Ack: command processed successfully
//     "E:<id>:<code>"             — Error (WRONG_PIN, BAD_FMT, etc.)
//     "S:<state>"                 — Broadcast: alarm state change (all slaves)
//
//   Slave → Master (only in response to P: or G:):
//     "R:<id>:0"                  — Response to poll: no pending input
//     "R:<id>:1"                  — Response to poll: user input ready
//     "D:<id>:<pin>*<mode>"       — Response to G: — full user command
//
// Poll cycle: 200ms per slave. Response timeout: 80ms.
// Slave marked offline after 3 consecutive timeouts.
//
// Hardware:
//   - UART1: TX1 = GPIO4, RX1 = GPIO1 (ESP32-S2/S3 Serial1)
//   - RS-485 transceiver (e.g. MAX485) connected to UART1
//   - DE/RE pin optional (if auto-direction transceiver is used, set to -1)
//
// ═══════════════════════════════════════════════════════════════════════════════

// ─── Hardware pins ─────────────────────────────────────────────────────────
#define RS485_TX_PIN       4    // GPIO4 — UART1 TX
#define RS485_RX_PIN       1    // GPIO1 — UART1 RX
#define RS485_DE_RE_PIN   -1    // Direction control pin (-1 = auto-direction transceiver)
#define RS485_BAUD      9600    // Baud rate for RS-485 bus

// ─── Protocol constants ────────────────────────────────────────────────────
#define KEYPAD_MAX_SLAVES          4
#define KEYPAD_MAX_MSG_LEN         64
#define KEYPAD_OFFLINE_THRESHOLD   3      // consecutive timeouts before offline
#define KEYPAD_POLL_INTERVAL_MS    200    // ms between polling consecutive slaves
#define KEYPAD_RESPONSE_TIMEOUT_MS 80     // ms to wait for a slave response

// ─── Protocol prefixes (single chars for minimal traffic) ──────────────────
#define KEYPAD_PREFIX_POLL      'P'
#define KEYPAD_PREFIX_RESP      'R'
#define KEYPAD_PREFIX_GET       'G'
#define KEYPAD_PREFIX_DATA      'D'
#define KEYPAD_PREFIX_ACK       'A'
#define KEYPAD_PREFIX_ERR       'E'
#define KEYPAD_PREFIX_STATE     'S'

// ─── Error codes (E:<id>:<code>) ───────────────────────────────────────────
#define KEYPAD_ERR_WRONG_PIN    "WRONG_PIN"
#define KEYPAD_ERR_BAD_FMT      "BAD_FMT"

// ─── Master polling state machine ───────────────────────────────────────────
enum class PollPhase : uint8_t {
  PHASE_IDLE,       // ready to send next poll
  PHASE_WAIT_POLL,  // waiting for R: response after P:
  PHASE_WAIT_DATA,  // waiting for D: payload after G:
  PHASE_DONE        // transaction complete, will advance to next slave
};

// ─── Per-slave master state ────────────────────────────────────────────────
struct KeypadSlaveStatus {
  bool     online;
  uint32_t lastResponseMs;        // last successful response timestamp
  uint8_t  consecutiveTimeouts;   // counter; >= OFFLINE_THRESHOLD = offline
  bool     userInputPending;      // true when slave reported flag=1
};

// ─── Public API ────────────────────────────────────────────────────────────

// Initialize RS-485 UART and internal state. Call once in setup().
void keypadCommInit();

// Process the Master polling state machine. Call every loop() iteration.
// Handles: sending polls, waiting for responses, requesting payloads,
// processing commands, and advancing the round-robin slave index.
void keypadCommLoop();

// Notify keypads of alarm state change. The broadcast is queued and sent
// between poll cycles (never interrupts an active transaction).
void keypadCommNotifyState(AlarmState state);

// Query slave online status (0-based index, 0..3)
bool keypadSlaveOnline(uint8_t slaveIdx);

// Get last response time for a slave (0-based index)
uint32_t keypadSlaveLastHB(uint8_t slaveIdx);

// Send raw message to RS-485 bus from external sources (e.g., web monitor).
// Used by the RS-485 test page for debugging / protocol testing.
void rs485SendRaw(const char* msg);