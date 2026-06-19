#include "hardware.h"
#include "event_log.h"

Adafruit_ADS1115 ads[ADS_COUNT];

static const uint8_t adcAddrs[ADS_COUNT] = { ADS_ADDR_1, ADS_ADDR_2, ADS_ADDR_3, ADS_ADDR_4 };
static const uint8_t relayPins[MAX_RELAYS] = { RELAY_SIREN, RELAY_PULSE, RELAY_TAMPER, RELAY_NOPOWER };

void initHardware() {
  // ─── I2C ───────────────────────────────────────────────────────────────
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000);

  // ─── ADS1115 ───────────────────────────────────────────────────────────
  for (int i = 0; i < ADS_COUNT; i++) {
    if (!ads[i].begin(adcAddrs[i], &Wire)) {
      // Log failure but continue
    }
    ads[i].setGain(GAIN_ONE);           // ±4.096V full-scale
    ads[i].setDataRate(RATE_ADS1115_128SPS);
  }

  // ─── Relay outputs (active LOW) ────────────────────────────────────────
  for (int i = 0; i < MAX_RELAYS; i++) {
    pinMode(relayPins[i], OUTPUT);
    digitalWrite(relayPins[i], HIGH);   // HIGH = OFF (active LOW)
    relayStates[i] = false;
  }

  // ─── Digital inputs with pull-up ───────────────────────────────────────
  pinMode(DIN_ARM_ZONE,   INPUT_PULLUP);
  pinMode(DIN_DISARM_ALL, INPUT_PULLUP);

  // ─── Prealarm output ───────────────────────────────────────────────────
  pinMode(DOUT_PREALARM, OUTPUT);
  digitalWrite(DOUT_PREALARM, LOW);

  // ─── Status LED ────────────────────────────────────────────────────────
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);
}

void readAllAdcChannels() {
  for (int chip = 0; chip < ADS_COUNT; chip++) {
    for (int ch = 0; ch < ADS_CHANNELS; ch++) {
      int idx = chip * ADS_CHANNELS + ch;
      int16_t raw = ads[chip].readADC_SingleEnded(ch);
      if (raw < 0) raw = 0;
      sensorStates[idx].rawValue = (uint16_t)raw;
    }
  }
}

void setRelay(uint8_t idx, bool on) {
  if (idx >= MAX_RELAYS) return;
  if (relayStates[idx] == on) return;        // avoid duplicate events
  relayStates[idx] = on;
  digitalWrite(relayPins[idx], on ? LOW : HIGH);  // active LOW

  char buf[64];
  const char* rname = config.relays[idx].name;
  if (strlen(rname) == 0) rname = "?";
  snprintf(buf, sizeof(buf), "Relay %d '%s' %s", idx + 1, rname, on ? "ON" : "OFF");
  logRelay(buf);
}

void applyRelayStates() {
  for (int i = 0; i < MAX_RELAYS; i++) {
    digitalWrite(relayPins[i], relayStates[i] ? LOW : HIGH);
  }
}

void readDigitalInputs() {
  static uint32_t dinDebounceStart[MAX_DINPUTS] = {0, 0};
  static bool     dinPending[MAX_DINPUTS] = {false, false};
  static bool     dinPendingVal[MAX_DINPUTS] = {false, false};

  uint32_t now = millis();

  for (int i = 0; i < MAX_DINPUTS; i++) {
    DigitalInputConfig &cfg = config.dinputs[i];
    if (cfg.action == INPUT_ACTION_NONE) continue;

    bool raw = digitalRead(cfg.pin);
    bool active = cfg.activeLow ? (raw == LOW) : (raw == HIGH);

    if (active != dinPendingVal[i]) {
      dinPendingVal[i] = active;
      dinDebounceStart[i] = now;
      dinPending[i] = true;
    }

    if (dinPending[i] && (now - dinDebounceStart[i] >= (uint32_t)cfg.debounceMs)) {
      dinPending[i] = false;
      dinputStates[i] = active;
    }
  }
}