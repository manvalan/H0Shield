#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include "Config.h"
#include "I2cBus.h"
#include "SecureStore.h"

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

struct I2cSlotConfig {
    I2cSlotMode mode = I2cSlotMode::MODE_AUTO;
    char        elementId[16] = "";   // display id when mode=DISPLAY
};

struct DisplayConfig {
    char     id[16]           = "";
    DisplayType type          = DisplayType::PLATFORM;
    uint8_t  i2cSlot          = 255;  // 255 = main bus (legacy); 0–7 = U9–U16
    uint8_t  i2cAddr          = 0x3C;
    uint8_t  platformNum      = 1;
    char     stationId[24]    = "";   // slug SIP, es. "castellina"
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
    uint8_t  muxCh       = MUX_CH_UNUSED;  // primary relay (lights / generic / bell / gate)
    uint8_t  muxChBar    = MUX_CH_UNUSED;  // level_xing barrier relay
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
    char wifiSsid[33]     = "";          // saved network (LittleFS + NVS)
    char lastStaIp[16]    = "";          // ultimo IP LAN (per accesso senza mDNS)
    char webUser[32]      = "admin";
    char webPassHash[65]  = "";          // SHA-256 hex; empty = no login required
    char mqttBroker[64]   = "";
    uint16_t mqttPort     = 1883;
    char mqttUser[32]     = "";
    char mqttPass[32]     = "";          // runtime only, loaded from SecureStore
    char infoMqttTopic[64]  = INFO_MQTT_TOPIC_DEFAULT;
    bool infoMqttEnabled      = true;
    uint16_t sensorThreshold = 512;      // ADC threshold for occupancy
    bool     tofEnabled      = true;
    uint8_t  tofThresholdMm  = 35;       // dist < threshold → occupied
    char     bootAspectMain[12]  = "red";
    char     bootAspectShunt[12] = "stop";
    uint8_t  tca9548Addr         = TCA9548A_ADDR_DEFAULT;
    uint8_t  tofI2cSlot          = 0;   // slot with VL6180X (when used)
    I2cSlotConfig i2cSlots[I2C_SLOTS] = {};
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
        if (!LittleFS.begin(false)) {
            Serial.println("[CFG] LittleFS mount failed — retry with format");
            if (!LittleFS.begin(true)) {
                Serial.println("[CFG] LittleFS unavailable");
                return false;
            }
        }
        return load();
    }

    bool _loadFromJsonString(const String& json);
    bool _logLoaded();

    bool load();
    bool save();

    // Convenience: build MQTT topic strings
    String topic(const char* suffix) const {
        return String("railway/") + cfg.hostname + "/" + suffix;
    }

    /** NVS è la fonte di verità per WiFi (sopravvive a uploadfs). */
    static void _syncWifiFromNvs(BoardConfig& c) {
        const String ssid = SecureStore::loadWifiSsid();
        if (ssid.length())
            strlcpy(c.wifiSsid, ssid.c_str(), sizeof(c.wifiSsid));
        const String lip = SecureStore::loadLastStaIp();
        if (lip.length())
            strlcpy(c.lastStaIp, lip.c_str(), sizeof(c.lastStaIp));
    }

    static AccessoryProfile parseProfile(const char* s);
    static const char* profileName(AccessoryProfile p);
};

#include "ConfigJson.h"

inline AccessoryProfile ConfigManager::parseProfile(const char* s) {
    return ConfigJson::parseProfile(s);
}

inline const char* ConfigManager::profileName(AccessoryProfile p) {
    return ConfigJson::profileName(p);
}

inline bool ConfigManager::_loadFromJsonString(const String& json) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        Serial.printf("[CFG] JSON parse error: %s\n", err.c_str());
        return false;
    }
    ConfigJson::readFile(cfg, doc.as<JsonObject>());
    _syncWifiFromNvs(cfg);
    return true;
}

inline bool ConfigManager::_logLoaded() {
    Serial.printf("[CFG] Loaded – hostname: %s, broker: %s, signals: %u, turnouts: %u, sensors_rb: %u, tof_blocks: %u, displays: %u, accessories: %u, scenarios: %u\n",
                  cfg.hostname, cfg.mqttBroker,
                  cfg.numSignals, cfg.numTurnouts, cfg.numSensorsRb, cfg.numTofBlocks,
                  cfg.numDisplays, cfg.numAccessories, cfg.numScenarios);
    return true;
}

inline bool ConfigManager::load() {
    if (LittleFS.exists(CONFIG_PATH)) {
        File f = LittleFS.open(CONFIG_PATH, "r");
        if (f) {
            String json = f.readString();
            f.close();
            if (json.length() > 0 && _loadFromJsonString(json))
                return _logLoaded();
            Serial.println("[CFG] config.json illeggibile — provo NVS");
        }
    } else {
        Serial.println("[CFG] Nessun config.json su LittleFS");
    }

    const String nvs = SecureStore::loadConfigJson();
    if (nvs.length() > 0 && _loadFromJsonString(nvs)) {
        Serial.println("[CFG] Ripristinato da NVS");
        save();   // riscrive LittleFS
        return _logLoaded();
    }

    if (!LittleFS.exists(CONFIG_PATH)) {
        Serial.println("[CFG] Prima configurazione — defaults");
        return save();
    }
    return false;
}

inline bool ConfigManager::save() {
    _syncWifiFromNvs(cfg);
    JsonDocument doc;
    ConfigJson::write(cfg, doc.to<JsonObject>(), false);

    String json;
    serializeJson(doc, json);
    if (json.length() < 8) return false;

    File f = LittleFS.open(CONFIG_PATH, "w");
    if (!f) {
        Serial.println("[CFG] LittleFS write failed");
        return false;
    }
    const size_t n = serializeJson(doc, f);
    f.flush();
    f.close();
    if (n != json.length()) {
        Serial.printf("[CFG] LittleFS write incomplete (%u/%u)\n", n, json.length());
        return false;
    }

    if (!SecureStore::saveConfigJson(json)) {
        Serial.println("[CFG] WARN: backup NVS fallito");
    }
    Serial.printf("[CFG] Config saved (%u bytes)\n", n);
    return true;
}
