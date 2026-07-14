#pragma once

#include <Arduino.h>
#include "config.h"

// ═══════════════════════════════════════════════════════════════════════════════
// RS-485 Keypad Communication Module — Master Side
// ═══════════════════════════════════════════════════════════════════════════════
//
// This module handles RS-485 half-duplex communication between the alarm
// controller (Master) and up to 4 external keypad devices (Slave 1..4).
//
// Hardware:
//   - UART1: TX1 = GPIO4, RX1 = GPIO1 (directly on ESP32-S2/S3 Serial1)
//   - RS-485 transceiver (e.g. MAX485) connected to UART1
//   - DE/RE pin optional (if auto-direction transceiver is used, set to -1)
//
// Protocol (simple ASCII line-based, newline-terminated):
//   Slave → Master:
//     "HB:<slaveId>"                    — heartbeat (every 60s)
//     "CMD:<slaveId>:<pin>*<mode>"      — arm/disarm command
//                                          pin = user PIN (digits)
//                                          mode = A/B/C/D (arm) or # (disarm)
//
//   Master → Slaves (broadcast on state change only):
//     "STATE:<stateString>"             — alarm state notification
//                                          stateString: "ARMED_HOME", "ARMED_AWAY",
//                                          "ARMED_NIGHT", "ARMED_VACATION",
//                                          "DISARMED", "PENDING_EXIT", "PENDING_ENTRY",
//                                          "TRIGGERED"
//
// ═══════════════════════════════════════════════════════════════════════════════

// ─── Hardware pins ─────────────────────────────────────────────────────────
#define RS485_TX_PIN       4    // GPIO4 — UART1 TX
#define RS485_RX_PIN       1    // GPIO1 — UART1 RX
#define RS485_DE_RE_PIN   -1    // Direction control pin (-1 = auto-direction transceiver)
#define RS485_BAUD      9600    // Baud rate for RS-485 bus

// ─── Protocol constants ────────────────────────────────────────────────────
#define KEYPAD_MAX_SLAVES        4
#define KEYPAD_HEARTBEAT_TIMEOUT_MS  120000   // 2 min — slave considered offline
#define KEYPAD_MAX_MSG_LEN       64          // max incoming message length
#define KEYPAD_CMD_PREFIX        "CMD:"
#define KEYPAD_HB_PREFIX         "HB:"
#define KEYPAD_STATE_PREFIX      "STATE:"

// ─── Slave status ──────────────────────────────────────────────────────────
struct KeypadSlaveStatus {
  bool     online;
  uint32_t lastHeartbeatMs;
};

// ─── Public API ────────────────────────────────────────────────────────────

// Initialize RS-485 UART and internal state. Call once in setup().
void keypadCommInit();

// Process incoming RS-485 data. Call every loop() iteration.
void keypadCommLoop();

// Notify keypads of alarm state change. Called externally when state changes.
// This is the ONLY output from the main system into this module.
void keypadCommNotifyState(AlarmState state);

// Query slave online status (0-based index, 0..3)
bool keypadSlaveOnline(uint8_t slaveIdx);

// Get last heartbeat time for a slave (0-based)
uint32_t keypadSlaveLastHB(uint8_t slaveIdx);
