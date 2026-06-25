#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include "Config.h"

// ── Rocrail signal config entry ──────────────────────────────────────
struct SignalConfig {
    char     id[16]   = "";      // Rocrail object ID (e.g. "sg1")
    uint8_t  type     = 0;       // 0 = MAIN, 1 = SHUNT
    uint8_t  chR      = 0;       // MUX or PCA9685 channel for Red / A lamp
    uint8_t  chG      = 1;       // MUX or PCA9685 channel for Yellow / B lamp
    uint8_t  chV      = 2;       // MUX or PCA9685 channel for Green / C lamp
    bool     usePca    = false;   // true → chR/G/V are PCA9685 pins (USE_PCA9685 build)
};

// ── Rocrail sensor (block detector) config entry ─────────────────────
struct SensorRbConfig {
    char    rocrailId[16] = "";   // Rocrail block/sensor ID (e.g. "bk1")
    uint8_t muxCh         = 0;   // MUX channel of the absorption sensor
};

// ── Rocrail turnout config entry ─────────────────────────────────────
struct TurnoutConfig {
    char     id[16]   = "";      // Rocrail object ID (e.g. "sw1")
    uint8_t  chS      = 0;       // MUX channel for "straight" relay coil
    uint8_t  chD      = 1;       // MUX channel for "diverge" relay coil
    uint32_t pulse    = 300;     // coil pulse duration in ms
};

// ── Rocrail ToF block detector (VL6180X distance) ────────────────────
struct ToFBlockConfig {
    char    rocrailId[16] = "";
    uint8_t thresholdMm   = 0;    // 0 = use global tof_threshold_mm
};

// ── I2C OLED display (SH1106 128×64) ─────────────────────────────────
enum class DisplayType : uint8_t { PLATFORM = 0, TIMETABLE = 1 };

struct TimetableRowConfig {
    char timeStr[6]     = "";
    char destination[24]= "";
    char platformBin[4] = "";
    char status[12]     = "";
};

struct DisplayConfig {
    char     id[16]           = "";
    DisplayType type          = DisplayType::PLATFORM;
    uint8_t  i2cAddr          = 0x3C;
    uint8_t  platformNum      = 1;
    char     stationName[32]  = "Stazione H0";
    char     destination[32]  = "";
    char     departureTime[8] = "";
    char     status[16]       = "libero";
    static constexpr uint8_t MAX_ROWS = 6;
    TimetableRowConfig rows[MAX_ROWS];
    uint8_t  numRows          = 0;
};

// ── Relay accessories (luci, PL, campana, …) ─────────────────────────
enum class AccessoryProfile : uint8_t {
    GENERIC    = 0,
    LIGHT      = 1,
    LEVEL_XING = 2,
    BELL       = 3,
    GATE       = 4
};

struct AccessoryConfig {
    char     id[16]      = "";
    AccessoryProfile profile = AccessoryProfile::GENERIC;
    uint8_t  muxCh       = 0;     // primary relay (lights / generic / bell / gate)
    uint8_t  muxChBar    = 0;     // level_xing barrier relay
    uint16_t pulseMs     = 300;   // bell duration or PL blink half-period
};

// ── Automation scenarios (sensor → accessory + display) ──────────────
struct ScenarioConfig {
    char     id[16]           = "";
    bool     enabled          = true;
    uint8_t  muxCh            = 255;   // MUX sensor channel (255 = unused)
    bool     tofTrigger       = false;
    char     rocrailId[16]    = "";    // or trigger by Rocrail block id
    char     accessoryPl[16]  = "";    // level_xing id
    char     accessoryLight[16]= "";
    char     accessoryBell[16]= "";    // optional ring on occupied
    char     displayId[16]    = "";    // platform display id
    char     statusOcc[16]    = "in arrivo";
    char     statusFree[16]   = "libero";
    char     destOcc[32]      = "";
    char     destFree[32]     = "";
};

struct BoardConfig {
    char hostname[32]     = "ShieldH0";
    char wifiSsid[33]     = "";          // saved network name (password in NVS)
    char webUser[32]      = "admin";
    char webPassHash[65]  = "";          // SHA-256 hex; empty = no login required
    char mqttBroker[64]   = "";
    uint16_t mqttPort     = 1883;
    char mqttUser[32]     = "";
    char mqttPass[32]     = "";          // runtime only, loaded from SecureStore
    uint16_t sensorThreshold = 512;      // ADC threshold for occupancy
    bool     tofEnabled      = true;
    uint8_t  tofThresholdMm  = 35;       // dist < threshold → occupied
    char     bootAspectMain[12]  = "red";
    char     bootAspectShunt[12] = "stop";
    ChannelRole pinMap[MUX_CHANNELS] = {};

    static constexpr uint8_t MAX_SIGNALS    = 8;
    static constexpr uint8_t MAX_TURNOUTS   = 8;
    static constexpr uint8_t MAX_SENSORS_RB = 8;
    static constexpr uint8_t MAX_TOF_BLOCKS = 4;
    static constexpr uint8_t MAX_DISPLAYS   = 4;
    static constexpr uint8_t MAX_ACCESSORIES = 8;
    static constexpr uint8_t MAX_SCENARIOS   = 6;
    SignalConfig   signals[MAX_SIGNALS];
    TurnoutConfig  turnouts[MAX_TURNOUTS];
    SensorRbConfig sensorsRb[MAX_SENSORS_RB];
    ToFBlockConfig tofBlocks[MAX_TOF_BLOCKS];
    DisplayConfig  displays[MAX_DISPLAYS];
    AccessoryConfig accessories[MAX_ACCESSORIES];
    ScenarioConfig  scenarios[MAX_SCENARIOS];
    uint8_t        numSignals    = 0;
    uint8_t        numTurnouts   = 0;
    uint8_t        numSensorsRb  = 0;
    uint8_t        numTofBlocks  = 0;
    uint8_t        numDisplays    = 0;
    uint8_t        numAccessories = 0;
    uint8_t        numScenarios   = 0;
};

class ConfigManager {
public:
    BoardConfig cfg;

    bool begin() {
        if (!LittleFS.begin(true)) {
            Serial.println("[CFG] LittleFS mount failed");
            return false;
        }
        return load();
    }

    bool load() {
        if (!LittleFS.exists(CONFIG_PATH)) {
            Serial.println("[CFG] No config found – using defaults");
            return save();   // persist defaults
        }
        File f = LittleFS.open(CONFIG_PATH, "r");
        if (!f) return false;

        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, f);
        f.close();
        if (err) {
            Serial.printf("[CFG] JSON parse error: %s\n", err.c_str());
            return false;
        }

        strlcpy(cfg.hostname,   doc["hostname"]   | cfg.hostname,   sizeof(cfg.hostname));
        strlcpy(cfg.wifiSsid,   doc["wifi_ssid"]  | cfg.wifiSsid,   sizeof(cfg.wifiSsid));
        strlcpy(cfg.webUser,    doc["web_user"]   | cfg.webUser,    sizeof(cfg.webUser));
        strlcpy(cfg.webPassHash,doc["web_pass_hash"] | cfg.webPassHash, sizeof(cfg.webPassHash));
        strlcpy(cfg.mqttBroker, doc["mqtt_broker"] | cfg.mqttBroker, sizeof(cfg.mqttBroker));
        cfg.mqttPort = doc["mqtt_port"] | cfg.mqttPort;
        cfg.sensorThreshold = doc["sensor_threshold"] | cfg.sensorThreshold;
        cfg.tofEnabled      = doc["tof_enabled"]      | cfg.tofEnabled;
        cfg.tofThresholdMm  = doc["tof_threshold_mm"] | cfg.tofThresholdMm;
        strlcpy(cfg.bootAspectMain,  doc["boot_aspect_main"]  | cfg.bootAspectMain,
                sizeof(cfg.bootAspectMain));
        strlcpy(cfg.bootAspectShunt, doc["boot_aspect_shunt"] | cfg.bootAspectShunt,
                sizeof(cfg.bootAspectShunt));
        strlcpy(cfg.mqttUser,   doc["mqtt_user"]  | cfg.mqttUser,   sizeof(cfg.mqttUser));
        cfg.mqttPass[0] = '\0';   // never from JSON – loaded via SecureStore

        JsonArray arr = doc["pin_map"].as<JsonArray>();
        for (uint8_t i = 0; i < MUX_CHANNELS && i < arr.size(); i++) {
            cfg.pinMap[i] = static_cast<ChannelRole>(arr[i].as<uint8_t>());
        }

        // ── Signals ────────────────────────────────────────────────
        cfg.numSignals = 0;
        for (JsonObject sg : doc["signals"].as<JsonArray>()) {
            if (cfg.numSignals >= BoardConfig::MAX_SIGNALS) break;
            SignalConfig& s = cfg.signals[cfg.numSignals++];
            strlcpy(s.id, sg["id"] | "", sizeof(s.id));
            s.type = sg["type"] | 0;
            s.chR  = sg["chR"]  | 0;
            s.chG  = sg["chG"]  | 1;
            s.chV  = sg["chV"]  | 2;
            s.usePca = sg["use_pca"] | false;
        }

        // ── Turnouts ───────────────────────────────────────────────
        cfg.numTurnouts = 0;
        for (JsonObject sw : doc["turnouts"].as<JsonArray>()) {
            if (cfg.numTurnouts >= BoardConfig::MAX_TURNOUTS) break;
            TurnoutConfig& t = cfg.turnouts[cfg.numTurnouts++];
            strlcpy(t.id, sw["id"] | "", sizeof(t.id));
            t.chS   = sw["chS"]   | 0;
            t.chD   = sw["chD"]   | 1;
            t.pulse = sw["pulse"] | 300;
        }

        // ── Sensors Rocrail ────────────────────────────────────────
        cfg.numSensorsRb = 0;
        for (JsonObject rb : doc["sensors_rb"].as<JsonArray>()) {
            if (cfg.numSensorsRb >= BoardConfig::MAX_SENSORS_RB) break;
            SensorRbConfig& sr = cfg.sensorsRb[cfg.numSensorsRb++];
            strlcpy(sr.rocrailId, rb["rocrail_id"] | "", sizeof(sr.rocrailId));
            sr.muxCh = rb["mux_ch"] | 0;
        }

        cfg.numTofBlocks = 0;
        for (JsonObject tb : doc["tof_blocks"].as<JsonArray>()) {
            if (cfg.numTofBlocks >= BoardConfig::MAX_TOF_BLOCKS) break;
            ToFBlockConfig& t = cfg.tofBlocks[cfg.numTofBlocks++];
            strlcpy(t.rocrailId, tb["rocrail_id"] | "", sizeof(t.rocrailId));
            t.thresholdMm = tb["threshold_mm"] | 0;
        }

        cfg.numDisplays = 0;
        for (JsonObject dp : doc["displays"].as<JsonArray>()) {
            if (cfg.numDisplays >= BoardConfig::MAX_DISPLAYS) break;
            DisplayConfig& d = cfg.displays[cfg.numDisplays++];
            strlcpy(d.id, dp["id"] | "", sizeof(d.id));
            const char* typeStr = dp["type"] | "platform";
            d.type = (strcmp(typeStr, "timetable") == 0)
                ? DisplayType::TIMETABLE : DisplayType::PLATFORM;
            d.i2cAddr = static_cast<uint8_t>(dp["i2c_addr"] | 0x3C);
            d.platformNum = dp["platform_num"] | 1;
            strlcpy(d.stationName, dp["station_name"] | "Stazione H0", sizeof(d.stationName));
            strlcpy(d.destination, dp["destination"] | "", sizeof(d.destination));
            strlcpy(d.departureTime, dp["departure_time"] | "", sizeof(d.departureTime));
            strlcpy(d.status, dp["status"] | "libero", sizeof(d.status));
            d.numRows = 0;
            for (JsonObject row : dp["rows"].as<JsonArray>()) {
                if (d.numRows >= DisplayConfig::MAX_ROWS) break;
                TimetableRowConfig& r = d.rows[d.numRows++];
                strlcpy(r.timeStr, row["time"] | "", sizeof(r.timeStr));
                const char* dest = row["destination"] | row["dest"] | "";
                strlcpy(r.destination, dest, sizeof(r.destination));
                const char* bin = row["platform"] | row["bin"] | "";
                strlcpy(r.platformBin, bin, sizeof(r.platformBin));
                strlcpy(r.status, row["status"] | "", sizeof(r.status));
            }
        }

        cfg.numAccessories = 0;
        for (JsonObject ac : doc["accessories"].as<JsonArray>()) {
            if (cfg.numAccessories >= BoardConfig::MAX_ACCESSORIES) break;
            AccessoryConfig& a = cfg.accessories[cfg.numAccessories++];
            strlcpy(a.id, ac["id"] | "", sizeof(a.id));
            a.profile = _parseProfile(ac["profile"] | "generic");
            a.muxCh = ac["mux_ch"] | ac["mux_ch_lights"] | 0;
            a.muxChBar = ac["mux_ch_bar"] | 0;
            a.pulseMs = ac["pulse_ms"] | 300;
        }

        cfg.numScenarios = 0;
        for (JsonObject sc : doc["scenarios"].as<JsonArray>()) {
            if (cfg.numScenarios >= BoardConfig::MAX_SCENARIOS) break;
            ScenarioConfig& s = cfg.scenarios[cfg.numScenarios++];
            strlcpy(s.id, sc["id"] | "", sizeof(s.id));
            s.enabled = sc["enabled"] | true;
            s.muxCh = sc["mux_ch"].isNull() ? 255 : static_cast<uint8_t>(sc["mux_ch"].as<uint8_t>());
            s.tofTrigger = sc["tof_trigger"] | false;
            strlcpy(s.rocrailId, sc["rocrail_id"] | "", sizeof(s.rocrailId));
            strlcpy(s.accessoryPl, sc["accessory_pl"] | "", sizeof(s.accessoryPl));
            strlcpy(s.accessoryLight, sc["accessory_light"] | "", sizeof(s.accessoryLight));
            strlcpy(s.accessoryBell, sc["accessory_bell"] | "", sizeof(s.accessoryBell));
            strlcpy(s.displayId, sc["display_id"] | "", sizeof(s.displayId));
            strlcpy(s.statusOcc, sc["status_occupied"] | "in arrivo", sizeof(s.statusOcc));
            strlcpy(s.statusFree, sc["status_free"] | "libero", sizeof(s.statusFree));
            strlcpy(s.destOcc, sc["dest_occupied"] | "", sizeof(s.destOcc));
            strlcpy(s.destFree, sc["dest_free"] | "", sizeof(s.destFree));
        }

        Serial.printf("[CFG] Loaded – hostname: %s, broker: %s, signals: %u, turnouts: %u, sensors_rb: %u, tof_blocks: %u, displays: %u, accessories: %u, scenarios: %u\n",
                      cfg.hostname, cfg.mqttBroker,
                      cfg.numSignals, cfg.numTurnouts, cfg.numSensorsRb, cfg.numTofBlocks,
                      cfg.numDisplays, cfg.numAccessories, cfg.numScenarios);
        return true;
    }

    bool save() {
        JsonDocument doc;
        doc["hostname"]    = cfg.hostname;
        doc["wifi_ssid"]   = cfg.wifiSsid;
        doc["web_user"]    = cfg.webUser;
        doc["web_pass_hash"]= cfg.webPassHash;
        doc["mqtt_broker"] = cfg.mqttBroker;
        doc["mqtt_port"]   = cfg.mqttPort;
        doc["mqtt_user"]   = cfg.mqttUser;
        doc["sensor_threshold"] = cfg.sensorThreshold;
        doc["tof_enabled"]      = cfg.tofEnabled;
        doc["tof_threshold_mm"] = cfg.tofThresholdMm;
        doc["boot_aspect_main"]  = cfg.bootAspectMain;
        doc["boot_aspect_shunt"] = cfg.bootAspectShunt;

        JsonArray arr = doc["pin_map"].to<JsonArray>();
        for (uint8_t i = 0; i < MUX_CHANNELS; i++) {
            arr.add(static_cast<uint8_t>(cfg.pinMap[i]));
        }

        JsonArray sgArr = doc["signals"].to<JsonArray>();
        for (uint8_t i = 0; i < cfg.numSignals; i++) {
            JsonObject o = sgArr.add<JsonObject>();
            o["id"]   = cfg.signals[i].id;
            o["type"] = cfg.signals[i].type;
            o["chR"]  = cfg.signals[i].chR;
            o["chG"]  = cfg.signals[i].chG;
            o["chV"]  = cfg.signals[i].chV;
            if (cfg.signals[i].usePca) o["use_pca"] = true;
        }

        JsonArray swArr = doc["turnouts"].to<JsonArray>();
        for (uint8_t i = 0; i < cfg.numTurnouts; i++) {
            JsonObject o = swArr.add<JsonObject>();
            o["id"]    = cfg.turnouts[i].id;
            o["chS"]   = cfg.turnouts[i].chS;
            o["chD"]   = cfg.turnouts[i].chD;
            o["pulse"] = cfg.turnouts[i].pulse;
        }

        JsonArray rbArr = doc["sensors_rb"].to<JsonArray>();
        for (uint8_t i = 0; i < cfg.numSensorsRb; i++) {
            JsonObject o = rbArr.add<JsonObject>();
            o["rocrail_id"] = cfg.sensorsRb[i].rocrailId;
            o["mux_ch"]     = cfg.sensorsRb[i].muxCh;
        }

        JsonArray tbArr = doc["tof_blocks"].to<JsonArray>();
        for (uint8_t i = 0; i < cfg.numTofBlocks; i++) {
            JsonObject o = tbArr.add<JsonObject>();
            o["rocrail_id"]   = cfg.tofBlocks[i].rocrailId;
            o["threshold_mm"] = cfg.tofBlocks[i].thresholdMm;
        }

        JsonArray dpArr = doc["displays"].to<JsonArray>();
        for (uint8_t i = 0; i < cfg.numDisplays; i++) {
            const DisplayConfig& d = cfg.displays[i];
            JsonObject o = dpArr.add<JsonObject>();
            o["id"]            = d.id;
            o["type"]          = (d.type == DisplayType::TIMETABLE) ? "timetable" : "platform";
            o["i2c_addr"]      = d.i2cAddr;
            o["platform_num"]  = d.platformNum;
            o["station_name"]  = d.stationName;
            o["destination"]   = d.destination;
            o["departure_time"]= d.departureTime;
            o["status"]        = d.status;
            JsonArray rows = o["rows"].to<JsonArray>();
            for (uint8_t r = 0; r < d.numRows; r++) {
                JsonObject ro = rows.add<JsonObject>();
                ro["time"]        = d.rows[r].timeStr;
                ro["destination"] = d.rows[r].destination;
                ro["platform"]    = d.rows[r].platformBin;
                ro["status"]      = d.rows[r].status;
            }
        }

        JsonArray acArr = doc["accessories"].to<JsonArray>();
        for (uint8_t i = 0; i < cfg.numAccessories; i++) {
            const AccessoryConfig& a = cfg.accessories[i];
            JsonObject o = acArr.add<JsonObject>();
            o["id"]       = a.id;
            o["profile"]  = _profileName(a.profile);
            if (a.profile == AccessoryProfile::LEVEL_XING) {
                o["mux_ch_lights"] = a.muxCh;
                o["mux_ch_bar"]    = a.muxChBar;
            } else {
                o["mux_ch"] = a.muxCh;
            }
            o["pulse_ms"] = a.pulseMs;
        }

        JsonArray scArr = doc["scenarios"].to<JsonArray>();
        for (uint8_t i = 0; i < cfg.numScenarios; i++) {
            const ScenarioConfig& s = cfg.scenarios[i];
            JsonObject o = scArr.add<JsonObject>();
            o["id"]              = s.id;
            o["enabled"]         = s.enabled;
            if (s.muxCh != 255) o["mux_ch"] = s.muxCh;
            o["tof_trigger"]     = s.tofTrigger;
            if (s.rocrailId[0]) o["rocrail_id"] = s.rocrailId;
            if (s.accessoryPl[0])    o["accessory_pl"]    = s.accessoryPl;
            if (s.accessoryLight[0]) o["accessory_light"] = s.accessoryLight;
            if (s.accessoryBell[0])  o["accessory_bell"]  = s.accessoryBell;
            if (s.displayId[0])      o["display_id"]      = s.displayId;
            o["status_occupied"] = s.statusOcc;
            o["status_free"]     = s.statusFree;
            if (s.destOcc[0])  o["dest_occupied"] = s.destOcc;
            if (s.destFree[0]) o["dest_free"]     = s.destFree;
        }

        File f = LittleFS.open(CONFIG_PATH, "w");
        if (!f) return false;
        serializeJson(doc, f);
        f.close();
        Serial.println("[CFG] Config saved");
        return true;
    }

    // Convenience: build MQTT topic strings
    String topic(const char* suffix) const {
        return String("railway/") + cfg.hostname + "/" + suffix;
    }

    static AccessoryProfile parseProfile(const char* s) {
        if (strcmp(s, "light") == 0)       return AccessoryProfile::LIGHT;
        if (strcmp(s, "level_xing") == 0)  return AccessoryProfile::LEVEL_XING;
        if (strcmp(s, "bell") == 0)        return AccessoryProfile::BELL;
        if (strcmp(s, "gate") == 0)        return AccessoryProfile::GATE;
        return AccessoryProfile::GENERIC;
    }

    static const char* profileName(AccessoryProfile p) {
        switch (p) {
            case AccessoryProfile::LIGHT:      return "light";
            case AccessoryProfile::LEVEL_XING: return "level_xing";
            case AccessoryProfile::BELL:       return "bell";
            case AccessoryProfile::GATE:       return "gate";
            default:                           return "generic";
        }
    }

private:
    static AccessoryProfile _parseProfile(const char* s) { return parseProfile(s); }
    static const char* _profileName(AccessoryProfile p) { return profileName(p); }
};
