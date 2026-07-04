#include "sensors.h"
#include "event_log.h"

static const char* sensorStateLabel(SensorState s) {
  switch (s) {
    case SENSOR_ACTIVE: return "ACTIVE";
    case SENSOR_FAULT:  return "FAULT";
    default:            return "IDLE";
  }
}


void sensorsLoop() {
  uint32_t now = millis();

  for (int i = 0; i < TOTAL_SENSORS; i++) {
    SensorConfig   &cfg = config.sensors[i];
    SensorStateData &st  = sensorStates[i];

    if (cfg.type == SENSOR_DISABLED) {
      st.state = SENSOR_IDLE;
      continue;
    }

    uint16_t raw = st.rawValue;
    bool rawActive = false;
    bool rawFault  = false;

    // Range-based evaluation with hysteresis support
    // Each range is [min, max] inclusive.
    // 65535 on detectMax/faultMax means "no upper bound".
    // faultMin==0 && faultMax==0 means fault range is disabled.

    bool faultDisabled = (cfg.faultMin == 0 && cfg.faultMax == 0);
    bool inFault   = !faultDisabled && raw >= cfg.faultMin &&
                     (cfg.faultMax == 65535 || raw <= cfg.faultMax);
    bool inDetect  = !inFault && raw >= cfg.detectMin &&
                     (cfg.detectMax == 65535 || raw <= cfg.detectMax);
    bool inStandby = !inFault && !inDetect &&
                     raw >= cfg.standbyMin && raw <= cfg.standbyMax;

    if (inFault) {
      rawFault = true;
    } else if (inDetect) {
      rawActive = true;
    } else if (inStandby) {
      // idle — both flags false
    } else {
      // hysteresis zone: between ranges — keep last state
      rawActive = (st.state == SENSOR_ACTIVE);
      rawFault  = (st.state == SENSOR_FAULT);
    }

    if (cfg.invert) {
      rawActive = !rawActive && !rawFault;
    }

    // ─── Debounce ──────────────────────────────────────────────────────
    // Require state to be stable for debounceMs before accepting change.
    // debounceTarget tracks the 3-way target: 0=idle, 1=active, 2=fault
    uint8_t target = rawFault ? 2 : (rawActive ? 1 : 0);
    if (st.debounceTarget != target) {
      st.debouncePending  = true;
      st.debounceTarget   = target;
      st.debounceStartMs  = now;
    }

    bool stable = !st.debouncePending || (now - st.debounceStartMs >= cfg.debounceMs);
    bool finalActive = stable ? (target == 1) : (st.state == SENSOR_ACTIVE);
    bool finalFault  = stable ? (target == 2) : (st.state == SENSOR_FAULT);

    // ─── On/Off delay timers ────────────────────────────────────────────
    if (finalFault && st.state != SENSOR_FAULT) {
      SensorState prevState = st.state;
      st.state        = SENSOR_FAULT;
      st.lastChangeMs = now;
      char buf[64];
      snprintf(buf, sizeof(buf), "T%d '%s' %s -> FAULT raw=%u mV", i+1, cfg.name, sensorStateLabel(prevState), raw);
      logSensor(buf);
    } else if (finalActive && st.state == SENSOR_IDLE) {
      if (st.activeSinceMs == 0) st.activeSinceMs = now;
      if (now - st.activeSinceMs >= cfg.onDelayMs) {
        st.state        = SENSOR_ACTIVE;
        st.lastChangeMs = now;
        st.idleSinceMs  = 0;
        char buf[64];
        snprintf(buf, sizeof(buf), "T%d '%s' IDLE -> ACTIVE raw=%u mV", i+1, cfg.name, raw);
        logSensor(buf);
      }
    } else if (!finalActive && !finalFault && st.state == SENSOR_ACTIVE) {
      if (st.idleSinceMs == 0) st.idleSinceMs = now;
      if (now - st.idleSinceMs >= cfg.offDelayMs) {
        st.state        = SENSOR_IDLE;
        st.lastChangeMs = now;
        st.activeSinceMs = 0;
        char buf[64];
        snprintf(buf, sizeof(buf), "T%d '%s' ACTIVE -> IDLE raw=%u mV", i+1, cfg.name, raw);
        logSensor(buf);
      }
    } else {
      if (finalActive) st.idleSinceMs   = 0;
      else             st.activeSinceMs = 0;
    }
  }
}

bool isSensorActive(uint8_t idx) {
  if (idx >= TOTAL_SENSORS) return false;
  return sensorStates[idx].state == SENSOR_ACTIVE;
}

const char* sensorTypeStr(SensorType t) {
  switch (t) {
    case SENSOR_PIR:        return "pir";
    case SENSOR_CONTACTRON: return "contactron";
    default:                return "disabled";
  }
}

const char* sensorStateStr(SensorState s) {
  switch (s) {
    case SENSOR_ACTIVE: return "active";
    case SENSOR_FAULT:  return "fault";
    default:            return "idle";
  }
}