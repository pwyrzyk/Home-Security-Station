#pragma once

#include <Arduino.h>

// ─── RS-485 Monitor Web Page ────────────────────────────────────────────────
// Served at /rs485 — full-duplex RS-485 bus monitor with WebSocket real-time log
// and send capability for testing purposes.

extern const char RS485_HTML[] PROGMEM;

// Initialize WebSocket endpoint (/ws-rs485) and page route (/rs485).
// Call once from initWebServer().
void initRS485Web();

// Periodic loop — broadcasts slave status to connected WebSocket clients.
// Call from main loop(). Must run regularly to update status panel.
void rs485WebLoop();

// ─── Traffic callback (called by keypad_comm for every TX/RX byte) ──────────
// Registered by rs485_web.cpp to capture RS-485 bus activity and broadcast
// it to connected WebSocket clients.

typedef enum { RS485_DIR_RX, RS485_DIR_TX } RS485Direction;

// Callback signature: (direction, message)
typedef void (*RS485TrafficCallback)(RS485Direction dir, const char* msg);

// Register a callback for RS-485 traffic monitoring. Only one callback is
// supported; calling again overwrites the previous one. Pass nullptr to clear.
void rs485RegisterTrafficCallback(RS485TrafficCallback cb);

// Called by keypad_comm when data is sent or received on the RS-485 bus.
// These are the hooks that keypad_comm.cpp calls in its TX/RX paths.
void rs485NotifyRX(const char* msg);
void rs485NotifyTX(const char* msg);