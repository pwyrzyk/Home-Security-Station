#include "backup.h"
#include "config.h"
#include "event_log.h"
#include "auth.h"
#include <ArduinoJson.h>
#include <LittleFS.h>

String buildBackupJson() {
    JsonDocument doc;

    doc["version"]   = FIRMWARE_VERSION;
    doc["timestamp"] = lastNtpTime > 0 ? lastNtpTime : (uint32_t)(millis() / 1000);
    doc["device"]    = deviceId;

    // ─── Serialize full Config struct ──────────────────────────────────
    JsonObject cfg = doc["config"].to<JsonObject>();

    cfg["wifiSsid"]    = config.wifiSsid;
    cfg["wifiPass"]    = config.wifiPass;
    cfg["mqttServer"]  = config.mqttServer;
    cfg["mqttPort"]    = config.mqttPort;
    cfg["mqttUser"]    = config.mqttUser;
    cfg["mqttPass"]    = config.mqttPass;
    cfg["haDiscoveryEnabled"] = config.haDiscoveryEnabled;
    cfg["haDiscoveryPrefix"]  = config.haDiscoveryPrefix;

    // Users
    JsonArray users = cfg["users"].to<JsonArray>();
    for (int i = 0; i < MAX_USERS; i++) {
        if (!config.users[i].active) continue;
        JsonObject u = users.add<JsonObject>();
        u["username"]     = config.users[i].username;
        u["passwordHash"] = config.users[i].passwordHash;
        u["pin"]          = String(config.users[i].pin);
        u["role"]         = config.users[i].role;
    }

    // Sensors
    JsonArray sensors = cfg["sensors"].to<JsonArray>();
    for (int i = 0; i < TOTAL_SENSORS; i++) {
        JsonObject s = sensors.add<JsonObject>();
        s["name"]       = config.sensors[i].name;
        s["type"]       = (int)config.sensors[i].type;
        s["standbyMin"] = config.sensors[i].standbyMin;
        s["standbyMax"] = config.sensors[i].standbyMax;
        s["detectMin"]  = config.sensors[i].detectMin;
        s["detectMax"]  = config.sensors[i].detectMax;
        s["faultMin"]   = config.sensors[i].faultMin;
        s["faultMax"]   = config.sensors[i].faultMax;
        s["invert"]     = config.sensors[i].invert;
        s["debounceMs"] = config.sensors[i].debounceMs;
        s["onDelayMs"]  = config.sensors[i].onDelayMs;
        s["offDelayMs"] = config.sensors[i].offDelayMs;
        s["zoneMask"]   = config.sensors[i].zoneMask;
    }

    // Zones
    JsonArray zones = cfg["zones"].to<JsonArray>();
    for (int i = 0; i < MAX_ZONES; i++) {
        JsonObject z = zones.add<JsonObject>();
        z["name"]              = config.zones[i].name;
        z["entryDelayS"]       = config.zones[i].entryDelayS;
        z["exitDelayS"]        = config.zones[i].exitDelayS;
        z["sirenOnS"]          = config.zones[i].sirenOnS;
        z["sirenOffS"]         = config.zones[i].sirenOffS;
        z["relayMask"]         = config.zones[i].relayMask;
        z["enabled"]           = config.zones[i].enabled;
        z["sirenEnabled"]      = config.zones[i].sirenEnabled;
        z["alarmRelayEnabled"] = config.zones[i].alarmRelayEnabled;
        z["alarmRelayOnS"]     = config.zones[i].alarmRelayOnS;
        z["alarmRelayOffS"]    = config.zones[i].alarmRelayOffS;
    }

    // Relays
    JsonArray relays = cfg["relays"].to<JsonArray>();
    for (int i = 0; i < MAX_RELAYS; i++) {
        JsonObject r = relays.add<JsonObject>();
        r["name"]    = config.relays[i].name;
        r["mode"]    = (int)config.relays[i].mode;
        r["zoneId"]  = config.relays[i].zoneId;
        r["enabled"] = config.relays[i].enabled;
    }

    // External sensors
    JsonArray extSensors = cfg["extSensors"].to<JsonArray>();
    for (int i = 0; i < MAX_EXT_SENSORS; i++) {
        JsonObject e = extSensors.add<JsonObject>();
        e["name"]     = config.extSensors[i].name;
        e["enabled"]  = config.extSensors[i].enabled;
        e["zoneMask"] = config.extSensors[i].zoneMask;
    }

    // Digital inputs
    JsonArray dinputs = cfg["dinputs"].to<JsonArray>();
    for (int i = 0; i < MAX_DINPUTS; i++) {
        JsonObject d = dinputs.add<JsonObject>();
        d["action"]     = (int)config.dinputs[i].action;
        d["zoneId"]     = config.dinputs[i].zoneId;
        d["pin"]        = config.dinputs[i].pin;
        d["activeLow"]  = config.dinputs[i].activeLow;
        d["debounceMs"] = config.dinputs[i].debounceMs;
    }

    // Alarm mode profiles
    JsonArray modes = cfg["modeProfiles"].to<JsonArray>();
    for (int m = 0; m < 6; m++) {
        JsonObject mp = modes.add<JsonObject>();
        mp["zoneMask"] = config.modeProfiles[m].zoneMask;
        mp["defined"]  = config.modeProfiles[m].defined;
    }

    // ─── Event log ─────────────────────────────────────────────────────
    JsonArray events = doc["eventlog"].to<JsonArray>();
    if (LittleFS.exists(EVENT_LOG_FILE)) {
        File f = LittleFS.open(EVENT_LOG_FILE, "r");
        if (f) {
            size_t size = f.size();
            uint8_t *buf = (uint8_t*)malloc(size);
            if (buf) {
                f.read(buf, size);
                int count = size / sizeof(EventLogEntry);
                EventLogEntry *entries = (EventLogEntry*)buf;
                for (int i = 0; i < count; i++) {
                    if (entries[i].timestamp == 0) continue;
                    JsonObject ev = events.add<JsonObject>();
                    ev["type"]        = entries[i].type;
                    ev["timestamp"]   = entries[i].timestamp;
                    ev["description"] = entries[i].description;
                }
                free(buf);
            }
            f.close();
        }
    }

    String out;
    serializeJson(doc, out);
    return out;
}

bool applyRestore(const char *json, String &error) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        error = String("JSON parse error: ") + err.c_str();
        return false;
    }

    JsonObject cfg = doc["config"].as<JsonObject>();
    if (cfg.isNull()) {
        error = "Missing 'config' section in backup";
        return false;
    }

    // ─── Apply config fields ───────────────────────────────────────────
    if (cfg.containsKey("wifiSsid"))   strlcpy(config.wifiSsid,   cfg["wifiSsid"].as<const char*>(), sizeof(config.wifiSsid));
    if (cfg.containsKey("wifiPass"))   strlcpy(config.wifiPass,   cfg["wifiPass"].as<const char*>(), sizeof(config.wifiPass));
    if (cfg.containsKey("mqttServer")) strlcpy(config.mqttServer, cfg["mqttServer"].as<const char*>(), sizeof(config.mqttServer));
    if (cfg.containsKey("mqttPort"))   config.mqttPort = cfg["mqttPort"].as<uint16_t>();
    if (cfg.containsKey("mqttUser"))   strlcpy(config.mqttUser, cfg["mqttUser"].as<const char*>(), sizeof(config.mqttUser));
    if (cfg.containsKey("mqttPass"))   strlcpy(config.mqttPass, cfg["mqttPass"].as<const char*>(), sizeof(config.mqttPass));
    if (cfg.containsKey("haDiscoveryEnabled")) config.haDiscoveryEnabled = cfg["haDiscoveryEnabled"].as<bool>();
    if (cfg.containsKey("haDiscoveryPrefix"))  strlcpy(config.haDiscoveryPrefix, cfg["haDiscoveryPrefix"].as<const char*>(), sizeof(config.haDiscoveryPrefix));

    // Users
    if (cfg.containsKey("users")) {
        memset(config.users, 0, sizeof(config.users));
        config.userCount = 0;
        JsonArray users = cfg["users"].as<JsonArray>();
        for (JsonObject u : users) {
            if (config.userCount >= MAX_USERS) break;
            int idx = config.userCount;
            if (u.containsKey("username"))     strlcpy(config.users[idx].username, u["username"].as<const char*>(), sizeof(config.users[idx].username));
            if (u.containsKey("passwordHash")) strlcpy(config.users[idx].passwordHash, u["passwordHash"].as<const char*>(), sizeof(config.users[idx].passwordHash));
            if (u.containsKey("pin"))          strlcpy(config.users[idx].pin, u["pin"].as<const char*>(), sizeof(config.users[idx].pin));
            if (u.containsKey("role"))         config.users[idx].role = u["role"].as<uint8_t>();
            config.users[idx].active = true;
            config.userCount++;
        }
        config.authMigrated = EEPROM_AUTH_MIGRATED_FLAG;
    }

    // Sensors
    if (cfg.containsKey("sensors")) {
        JsonArray sensors = cfg["sensors"].as<JsonArray>();
        int i = 0;
        for (JsonObject s : sensors) {
            if (i >= TOTAL_SENSORS) break;
            if (s.containsKey("name"))       strlcpy(config.sensors[i].name, s["name"].as<const char*>(), sizeof(config.sensors[i].name));
            if (s.containsKey("type"))       config.sensors[i].type = (SensorType)s["type"].as<int>();
            if (s.containsKey("standbyMin")) config.sensors[i].standbyMin = s["standbyMin"].as<uint16_t>();
            if (s.containsKey("standbyMax")) config.sensors[i].standbyMax = s["standbyMax"].as<uint16_t>();
            if (s.containsKey("detectMin"))  config.sensors[i].detectMin  = s["detectMin"].as<uint16_t>();
            if (s.containsKey("detectMax"))  config.sensors[i].detectMax  = s["detectMax"].as<uint16_t>();
            if (s.containsKey("faultMin"))   config.sensors[i].faultMin   = s["faultMin"].as<uint16_t>();
            if (s.containsKey("faultMax"))   config.sensors[i].faultMax   = s["faultMax"].as<uint16_t>();
            if (s.containsKey("invert"))     config.sensors[i].invert     = s["invert"].as<bool>();
            if (s.containsKey("debounceMs")) config.sensors[i].debounceMs = s["debounceMs"].as<uint16_t>();
            if (s.containsKey("onDelayMs"))  config.sensors[i].onDelayMs  = s["onDelayMs"].as<uint16_t>();
            if (s.containsKey("offDelayMs")) config.sensors[i].offDelayMs = s["offDelayMs"].as<uint16_t>();
            if (s.containsKey("zoneMask"))   config.sensors[i].zoneMask   = s["zoneMask"].as<uint16_t>();
            i++;
        }
    }

    // Zones
    if (cfg.containsKey("zones")) {
        JsonArray zones = cfg["zones"].as<JsonArray>();
        int i = 0;
        for (JsonObject z : zones) {
            if (i >= MAX_ZONES) break;
            if (z.containsKey("name"))              strlcpy(config.zones[i].name, z["name"].as<const char*>(), sizeof(config.zones[i].name));
            if (z.containsKey("entryDelayS"))       config.zones[i].entryDelayS = z["entryDelayS"].as<uint8_t>();
            if (z.containsKey("exitDelayS"))        config.zones[i].exitDelayS  = z["exitDelayS"].as<uint8_t>();
            if (z.containsKey("sirenOnS"))          config.zones[i].sirenOnS    = z["sirenOnS"].as<uint8_t>();
            if (z.containsKey("sirenOffS"))         config.zones[i].sirenOffS   = z["sirenOffS"].as<uint8_t>();
            if (z.containsKey("relayMask"))         config.zones[i].relayMask   = z["relayMask"].as<uint8_t>();
            if (z.containsKey("enabled"))           config.zones[i].enabled     = z["enabled"].as<bool>();
            if (z.containsKey("sirenEnabled"))      config.zones[i].sirenEnabled      = z["sirenEnabled"].as<bool>();
            if (z.containsKey("alarmRelayEnabled")) config.zones[i].alarmRelayEnabled = z["alarmRelayEnabled"].as<bool>();
            if (z.containsKey("alarmRelayOnS"))     config.zones[i].alarmRelayOnS     = z["alarmRelayOnS"].as<uint8_t>();
            if (z.containsKey("alarmRelayOffS"))    config.zones[i].alarmRelayOffS    = z["alarmRelayOffS"].as<uint8_t>();
            i++;
        }
    }

    // Relays
    if (cfg.containsKey("relays")) {
        JsonArray relays = cfg["relays"].as<JsonArray>();
        int i = 0;
        for (JsonObject r : relays) {
            if (i >= MAX_RELAYS) break;
            if (r.containsKey("name"))    strlcpy(config.relays[i].name, r["name"].as<const char*>(), sizeof(config.relays[i].name));
            if (r.containsKey("mode"))    config.relays[i].mode    = (RelayMode)r["mode"].as<int>();
            if (r.containsKey("zoneId"))  config.relays[i].zoneId  = r["zoneId"].as<uint8_t>();
            if (r.containsKey("enabled")) config.relays[i].enabled = r["enabled"].as<bool>();
            i++;
        }
    }

    // External sensors
    if (cfg.containsKey("extSensors")) {
        JsonArray extSensors = cfg["extSensors"].as<JsonArray>();
        int i = 0;
        for (JsonObject e : extSensors) {
            if (i >= MAX_EXT_SENSORS) break;
            if (e.containsKey("name"))     strlcpy(config.extSensors[i].name, e["name"].as<const char*>(), sizeof(config.extSensors[i].name));
            if (e.containsKey("enabled"))  config.extSensors[i].enabled  = e["enabled"].as<bool>();
            if (e.containsKey("zoneMask")) config.extSensors[i].zoneMask = e["zoneMask"].as<uint16_t>();
            i++;
        }
    }

    // Digital inputs
    if (cfg.containsKey("dinputs")) {
        JsonArray dinputs = cfg["dinputs"].as<JsonArray>();
        int i = 0;
        for (JsonObject d : dinputs) {
            if (i >= MAX_DINPUTS) break;
            if (d.containsKey("action"))     config.dinputs[i].action     = (InputAction)d["action"].as<int>();
            if (d.containsKey("zoneId"))     config.dinputs[i].zoneId     = d["zoneId"].as<uint8_t>();
            if (d.containsKey("pin"))        config.dinputs[i].pin        = d["pin"].as<uint8_t>();
            if (d.containsKey("activeLow"))  config.dinputs[i].activeLow  = d["activeLow"].as<bool>();
            if (d.containsKey("debounceMs")) config.dinputs[i].debounceMs = d["debounceMs"].as<uint16_t>();
            i++;
        }
    }

    // Alarm mode profiles
    if (cfg.containsKey("modeProfiles")) {
        JsonArray modes = cfg["modeProfiles"].as<JsonArray>();
        int i = 0;
        for (JsonObject mp : modes) {
            if (i >= 6) break;
            if (mp.containsKey("zoneMask")) config.modeProfiles[i].zoneMask = mp["zoneMask"].as<uint8_t>();
            if (mp.containsKey("defined"))  config.modeProfiles[i].defined  = mp["defined"].as<bool>();
            i++;
        }
    }

    // Save to EEPROM
    config.magic = EEPROM_MAGIC;
    config.forcePasswordChange = false;
    saveConfig();

    // ─── Restore event log ──────────────────────────────────────────────
    if (doc.containsKey("eventlog")) {
        JsonArray events = doc["eventlog"].as<JsonArray>();
        // Re-initialize the log file
        clearEventLog();
        for (JsonObject ev : events) {
            if (!ev.containsKey("timestamp")) continue;
            EventLogEntry entry;
            entry.type        = ev.containsKey("type")        ? (EventType)(uint8_t)ev["type"].as<int>() : (EventType)0;
            entry.timestamp   = ev["timestamp"].as<uint32_t>();
            const char* desc  = ev.containsKey("description") ? ev["description"].as<const char*>() : "";
            strlcpy(entry.description, desc, sizeof(entry.description));
            // Append to log file
            if (LittleFS.exists(EVENT_LOG_FILE)) {
                File f = LittleFS.open(EVENT_LOG_FILE, "a");
                if (f) {
                    f.write((uint8_t*)&entry, sizeof(EventLogEntry));
                    f.close();
                }
            }
        }
    }

    return true;
}