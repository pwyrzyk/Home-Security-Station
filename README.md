# Home Alarm System

DIY ESP32-based home alarm system with wired sensors, Home Assistant MQTT integration, RS-485 keypad support, and a built-in web dashboard.

## Features

- 16 wired analog sensor inputs (ADS1115 16-bit ADC ×4 via I2C)
- 16 external MQTT-triggered virtual sensors
- 8 configurable alarm zones with entry/exit delays
- 6 alarm modes: Home, Away, Night, Vacation, Custom Bypass, Disarmed
- 5 relay outputs (Siren, Alarm, Tamper, No-Power, Prealarm)
- 2 digital inputs (SOS Panic, Disarm All)
- Home Assistant auto-discovery via MQTT
- Web dashboard with real-time status, sensor configuration, zone management
- Event log persisted to LittleFS
- OTA firmware updates
- RS-485 keypad communication (master mode, up to 4 slaves)
- Backup & restore (JSON)

---

## 1. Hardware Description

### ESP32-C3-DevKitM-1

| Spec | Value |
|---|---|
| MCU | ESP32-C3 RISC-V |
| Clock | 160 MHz |
| RAM | 400 KB SRAM |
| Flash | 4 MB |
| WiFi | 2.4 GHz 802.11 b/g/n |
| Filesystem | LittleFS |

### GPIO Pin Mapping

| GPIO | Function | Notes |
|---|---|---|
| 0 | I2C SDA | ADS1115 bus |
| 1 | RS-485 RX | UART1 RX (keypad bus) |
| 2 | Status LED | Built-in (LOW=off, HIGH=on) |
| 3 | I2C SCL | ADS1115 bus |
| 4 | RS-485 TX | UART1 TX (keypad bus) |
| 5 | Relay Siren | Active LOW (LOW=ON) |
| 6 | Relay Alarm | Active LOW |
| 7 | Relay Tamper | Active LOW |
| 8 | Digital In SOS Panic | Active LOW, ext. pull-up 3.3V |
| 9 | Digital In DISARM ALL | Active LOW, ext. pull-up 3.3V |
| 10 | Relay No-Power | Active LOW |
| 20 | Prealarm Output | Digital OUT |

### ADS1115 16-bit ADC (×4, I2C)

| I2C Address | Channels | Range |
|---|---|---|
| 0x48 | T1 – T4 | ±4.096 V (1 mV/LSB) |
| 0x49 | T5 – T8 | ±4.096 V |
| 0x4A | T9 – T12 | ±4.096 V |
| 0x4B | T13 – T16 | ±4.096 V |

**16 analog sensor inputs total.** Each channel reads raw voltage in mV. Thresholds define three ranges — **idle** (standby), **active** (detection), and **fault** (wiring issue). Each sensor can be assigned to one or more zones.

---

## 2. Configuration Guide

### 2.1 First Boot — Access Point Mode

1. Power on the device. If no WiFi is configured, it starts in **AP mode**.
2. Connect to `Alarm-AP-XXXX` (password: `12345678`).
3. Open `http://192.168.4.1` in a browser.
4. Log in with default credentials: `admin` / `admin`.
5. You will be prompted to change the password on first login.

### 2.2 Network Setup

1. Go to **Config** tab.
2. Enter your WiFi SSID and password.
3. Enter MQTT broker address, port (default 1883), and credentials.
4. Click **Save Config**. The device reconnects to your WiFi.
5. Access the dashboard at the new LAN IP, or `http://alarm.local` (mDNS).

### 2.3 Sensor Configuration

For each wired sensor (T1–T16):
- **Type:** PIR, Contactron (reed switch), or Off
- **Idle range:** Voltage considered normal/standby (e.g. 0–2000 mV)
- **Active range:** Voltage triggering detection (e.g. 4000–6000 mV). Set Max to 65535 for no upper bound.
- **Fault range:** Voltage indicating wiring fault/tamper (e.g. 30000–33000 mV). Set both to 0 to disable.
- **Debounce:** Time (ms) voltage must be stable before state change
- **Active/Idle after:** On/off delay timers (ms)
- **Zones:** Assign to one or more alarm zones

### 2.4 Zone Configuration

For each zone (Z1–Z8):
- **Name** — e.g. "Perimeter"
- **Exit Delay** (0–120s) — time to leave after arming
- **Entry Delay** (0–120s) — time to disarm after sensor triggers
- **Siren cycle** — ON/OFF times (0=continuous)
- **Alarm relay cycle** — ON/OFF times for pulse relay
- **Always Armed** — zone cannot be disarmed; fires immediately on sensor trigger; auto-clears when sensors go idle. For panic/24h zones.

### 2.5 SOS Panic / Always-Armed Zones

Zone 8 "Panic" is pre-configured with `alwaysArmed=true`. External sensor E16 triggers it:
- Dashboard 🆘 Panic button toggles E16
- Home Assistant SOS Panic switch entity
- GPIO8 digital input toggles E16
- MQTT: publish `active`/`idle` to `ext_sensor/16`
- REST API: `GET /api/extsensors/trigger?id=16&state=on|off`

### 2.6 Alarm Mode Profiles

Each mode (Armed Home/Away/Night/Vacation/Custom Bypass) defines which zones are active. If a mode has no zones assigned, arming is rejected.

### 2.7 User Management

Three roles:
- **Admin** — full access to settings and tabs
- **Operator** — arm/disarm only, no config access
- **API** — REST API sensor triggers only, no dashboard

Each user has username, password, and 4-digit PIN (for keypad arming). Default accounts: `admin/admin` (Admin), `api_user/api_user` (API).

### 2.8 Backup & Restore

Config tab → Backup & Restore. Exports/imports all configuration + event log as JSON. Device reboots after restore.

---

## 3. Home Assistant Integration

### 3.1 Auto-Discovery

Ensure MQTT is configured and connected. Discovery is enabled by default. The device publishes MQTT discovery messages on connect — check Settings → Devices & Services → MQTT in Home Assistant.

### 3.2 Auto-Discovered Entities

| Entity | Type | Description |
|---|---|---|
| `alarm_control_panel.home_alarm` | Alarm Panel | Arm/disarm: disarmed, armed_home, armed_away, armed_night, armed_vacation, armed_custom_bypass |
| `binary_sensor.*` | Binary Sensor (×16) | Wired sensor inputs T1–T16 |
| `binary_sensor.*` | Binary Sensor (×16) | External sensors E1–E16 |
| `switch.*` | Switch (×5) | Relay control + SOS Panic |
| `sensor.*` | Sensor | WiFi RSSI, uptime, heap free |

### 3.3 MQTT Topics

All topics prefixed with `homealarm/XXXXXX` (device ID suffix).

| Topic | Direction | Payload |
|---|---|---|
| `cmd/mode` | HA → Device | `DISARM`, `ARM_HOME`, `ARM_AWAY`, `ARM_NIGHT`, `ARM_VACATION`, `ARM_CUSTOM_BYPASS` |
| `state` | Device → HA | `disarmed`, `armed_home`, `armed_away`, `armed_night`, `armed_vacation`, `armed_custom_bypass`, `pending`, `triggered` |
| `cmd/relay/1..4` | HA → Device | `ON` / `OFF` |
| `ext_sensor/1..16` | HA → Device | `active` / `idle` / `on` / `off` / `1` / `0` |
| `zones/1..8/state` | Device → HA | `disarmed`, `armed_idle`, `arming`, `disarming`, `alarm` |
| `sensors/1..16/state` | Device → HA | `idle`, `active`, `fault` |
| `status/relay/1..4` | Device → HA | `ON` / `OFF` |
| `status/wifi` | Device → HA | `connected` / `ap` / `disconnected` |
| `status/rssi` | Device → HA | dBm value |
| `status/ext_sensor/16` | Device → HA | `active` / `idle` (panic) |

---

## 4. REST API Reference

All endpoints use Cookie-based session authentication (except `/api/login` and `/api/auth-status`). Session timeout: 30 minutes of inactivity.

### 4.1 Authentication

```bash
# Login (get session cookie)
curl -c cookies.txt -X POST \
  -d "user=admin&pass=admin" \
  http://DEVICE_IP/api/login

# Check auth status
curl -b cookies.txt http://DEVICE_IP/api/auth-status

# Change password
curl -b cookies.txt -X POST \
  -d "current=admin&new=MyNewPassword" \
  http://DEVICE_IP/api/change-password

# Logout
curl -b cookies.txt -X POST http://DEVICE_IP/api/logout
```

### 4.2 External Sensor Trigger

```bash
# Trigger sensor E1 as active
curl -b cookies.txt \
  "http://DEVICE_IP/api/extsensors/trigger?id=1&state=on"

# Trigger sensor E1 as idle
curl -b cookies.txt \
  "http://DEVICE_IP/api/extsensors/trigger?id=1&state=off"
```

### 4.3 Full Endpoint Table

| Method | Endpoint | Auth | Description |
|---|---|---|---|
| POST | `/api/login` | — | Authenticate |
| POST | `/api/logout` | Session | Destroy session |
| GET | `/api/auth-status` | — | Check auth state |
| GET | `/api/status` | Session | Full system status JSON |
| GET | `/api/status-light` | Session | Lightweight status (~200 bytes) |
| GET/POST | `/api/sensors` | Admin | Sensor configuration |
| GET/POST | `/api/zones` | Session | Zone configuration |
| GET/POST | `/api/extsensors` | Session | External sensor configuration |
| GET/POST | `/api/alarmmodes` | Admin | Alarm mode profiles |
| GET/POST | `/api/network` | Admin | WiFi/MQTT configuration |
| GET | `/api/mode/set?mode=MODE` | Session | Arm/disarm |
| GET | `/api/extsensors/trigger?id=N&state=S` | Session | Trigger external sensor |
| GET | `/api/relay/N?state=S` | Session | Manual relay control |
| GET/POST | `/api/relays/config` | Admin | Relay configuration |
| GET | `/api/eventlog` | Session | Event log (JSON) |
| POST | `/api/eventlog/clear` | Admin | Clear event log |
| GET | `/api/backup` | Admin | Download backup JSON |
| POST | `/api/restore` | Admin | Restore from backup |
| POST | `/api/ota` | Admin | Firmware upload (multipart) |
| GET | `/api/restart` | Admin | Soft restart |
| GET | `/api/reconnect` | Admin | Reconnect WiFi |
| GET | `/api/users` | Session | List users |
| POST | `/api/users/add` | Admin | Add user |
| POST | `/api/users/delete` | Admin | Delete user |
| POST | `/api/change-password` | Session | Change own password |

### 4.4 Alarm Mode Values

| Mode String | Description |
|---|---|
| `disarmed` | All zones disarmed |
| `armed_home` | Perimeter zones armed |
| `armed_away` | All zones armed |
| `armed_night` | Bedroom perimeter armed |
| `armed_vacation` | All zones armed (extended away) |
| `armed_custom_bypass` | Custom zone selection |
| `pending` | Arming/disarming in progress (state only) |
| `triggered` | Alarm active (state only) |

---

## 5. RS-485 Keypad Communication

### 5.1 Hardware

- **UART:** UART1 (TX=GPIO4, RX=GPIO1)
- **Transceiver:** MAX485 (or compatible)
- **DE/RE pin:** Optional; set `RS485_DE_RE_PIN` to `-1` for auto-direction transceivers
- **Baud rate:** 9600, 8N1
- **Topology:** Master (ESP32) ↔ up to 4 Slave keypads

### 5.2 Protocol

ASCII line-based, newline-terminated (`\n`). Maximum message length: 64 bytes.

### 5.3 Master → Slaves

The master broadcasts alarm state changes to all slaves. Messages are sent **on every state transition** (not polled).

```
STATE:<state>
```

| State String | Meaning |
|---|---|
| `DISARMED` | System disarmed |
| `PENDING` | Arming/disarming in progress (exit/entry delay active) |
| `ARMED_HOME` | Armed in Home mode |
| `ARMED_AWAY` | Armed in Away mode |
| `ARMED_NIGHT` | Armed in Night mode |
| `ARMED_VACATION` | Armed in Vacation mode |
| `ARMED_CUSTOM` | Armed in Custom Bypass mode |
| `TRIGGERED` | Alarm is active |

**State flow (full lifecycle):**
```
DISARMED → PENDING → ARMED_HOME/AWAY/NIGHT/VACATION/CUSTOM
                              ↓ (sensor tripped with entry delay)
                           PENDING → TRIGGERED
                              ↑ (disarm command)
                           DISARMED
```

### 5.4 Slaves → Master

#### Heartbeat (keep-alive)
```
HB:<slaveId>
```
Every ~60 seconds. `slaveId` = 1–4. Slaves that miss heartbeats for >2 minutes are marked offline.

#### Arm Command (system disarmed)
```
CMD:<slaveId>:<pin>*<mode>
```

| Field | Description |
|---|---|
| `<slaveId>` | Keypad ID: 1, 2, 3, or 4 |
| `<pin>` | User PIN: 1–4 digits |
| `<mode>` | Arm mode letter (A/B/C/D) |

**Mode letters:**

| Letter | Action |
|---|---|
| `A` | Arm in HOME mode |
| `B` | Arm in AWAY mode |
| `C` | Arm in NIGHT mode |
| `D` | Arm in VACATION mode |

**Example:**
```
CMD:1:1234*A     # Keypad 1, PIN 1234, arm HOME
```

**Behavior:**
- PIN-only (no `*` or mode letter) is **rejected** when disarmed — a mode must be specified
- Unknown PINs are rejected with `ERR:WRONG_PIN`
- PINs are validated against registered users in the system

#### Disarm Command (system armed)
```
CMD:<slaveId>:<pin>
```

When the system is armed, the keypad sends **only the PIN** (no `*` or mode letter). The `#` character is reserved on the keypad as an end-of-input confirmation marker.

| Field | Description |
|---|---|
| `<slaveId>` | Keypad ID: 1, 2, 3, or 4 |
| `<pin>` | User PIN: 1–4 digits |

**Example:**
```
CMD:1:1234       # Keypad 1, PIN 1234, disarm
```

**Behavior:**
- Any valid PIN disarms (the `*` separator and mode letter are optional when armed)
- The legacy `CMD:<slaveId>:<pin>*<any>` format also disarms for backwards compatibility
- Unknown PINs are rejected with `ERR:WRONG_PIN`

### 5.5 WebSocket Bus Monitor

Accessible at `/rs485` on the device. Provides:
- Real-time RS-485 traffic log (RX/TX with timestamps)
- Keypad slave status panel (online/offline/last heartbeat)
- Raw message send capability for testing
- Auto-reconnecting WebSocket at `/ws-rs485`

### 5.6 Error Responses (Master → Slave)

The master sends error codes back to the slave when a command fails:

```
ERR:<code>
```

| Error Code | When |
|---|---|
| `ERR:WRONG_PIN` | PIN authentication failed — PIN does not match any registered user |
| `ERR:UNKNOWN_CMD` | Message received that does not match any known format (`HB:`, `CMD:`) |

### 5.7 Recent Protocol Fixes

- **Full state transition broadcast:** The slave now receives every state change, not just the initial arm/disarm command. Exit delay expiry, entry delay expiry, and alarm triggers all generate `STATE:` broadcasts automatically.
- **PIN-only disarm:** When the system is armed, the keypad can send `CMD:<slaveId>:<pin>` without `*` or mode letter. The `#` character is reserved as an end-of-input marker on the keypad side.
- **Error feedback:** Unknown PINs now reply `ERR:WRONG_PIN` and unknown messages reply `ERR:UNKNOWN_CMD` so the slave can display error feedback.
- **Periodic slave status:** The web monitor's slave status panel updates every 3 seconds (previously was stuck on "Loading...").

---

## 6. MQTT External Sensor Trigger

Publish to the device's MQTT topic to activate an external sensor. No authentication required beyond MQTT broker credentials.

```bash
# Activate E1
mosquitto_pub -h BROKER_IP -t "homealarm/XXXXXX/ext_sensor/1" -m "active"

# Deactivate E1
mosquitto_pub -h BROKER_IP -t "homealarm/XXXXXX/ext_sensor/1" -m "idle"
```

Accepted payloads: `active`, `idle`, `on`, `off`, `1`, `0`.

---

## 7. Building & Flashing

This project uses [PlatformIO](https://platformio.org/).

```bash
# Build
pio run

# Build and upload
pio run --target upload

# Monitor serial
pio device monitor
```

Target board: `esp32-c3-devkitm-1`