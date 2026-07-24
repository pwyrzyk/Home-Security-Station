// ═══════════════════════════════════════════════════════════════════════════════
// RS-485 Keypad Slave — Application Entry Point
// ═══════════════════════════════════════════════════════════════════════════════
//
// Compile with PlatformIO environment "esp32-c3-slave" (see platformio.ini).
//
// Before flashing each physical device, change SLAVE_ID in slave_keypad.h
// to match the device's address (1..4).
//
// ═══════════════════════════════════════════════════════════════════════════════

#include "slave_keypad.h"

void setup() {
  slaveSetup();
}

void loop() {
  slaveLoop();
}