#pragma once

#include "config.h"
#include <Adafruit_ADS1X15.h>

// ─── ADS1115 instances ─────────────────────────────────────────────────────
extern Adafruit_ADS1115 ads[ADS_COUNT];

// ─── Functions ─────────────────────────────────────────────────────────────
void initHardware();
void readAllAdcChannels();
void setRelay(uint8_t idx, bool on);    // idx 0..3; LOW=ON
void applyRelayStates();
void readDigitalInputs();