#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include "Config.h"

// ── Rocrail signal config entry ──────────────────────────────────────
struct SignalConfig {
    char     id[16]   = "";      // Rocrail object ID (e.g. "sg1")
    uint8_t  type     = 0;       // 0 = MAIN, 1 = SHUNT
    uint8_t  chR      = 0;       // MUX channel for Red / A lamp
    uint8_t  chG      = 1;       // MUX channel for Yellow / B lamp
    uint8_t  chV      = 2;       // MUX channel for Green / C lamp
};

// ── Rocrail turnout config entry ─────────────────────────────────────
struct TurnoutConfig {
    char     id[16]   = "";      // Rocrail object ID (e.g. "sw1")
    uint8_t  chS      = 0;       // MUX channel for "straight" relay coil
    uint8_t  chD      = 1;       // MUX channel for "diverge" relay coil
    uint32_t pulse    = 300;     // coil pulse duration in ms
};

struct BoardConfig {
    char hostname[32]     = "ShieldH0";
    char mqttBroker[64]   = "";
    uint16_t mqttPort     = 1883;
    char mqttUser[32]     = "";
    char mqttPass[32]     = "";
    ChannelRole pinMap[MUX_CHANNELS] = {};   // all UNUSED by default

    // Rocrail-mapped objects (up to 8 signals and 8 turnouts per board)
    static constexpr uint8_t MAX_SIGNALS  = 8;
    static constexpr uint8_t MAX_TURNOUTS = 8;
    SignalConfig  signals[MAX_SIGNALS];
    TurnoutConfig turnouts[MAX_TURNOUTS];
    uint8_t       numSignals  = 0;
    uint8_t       numTurnouts = 0;
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
        strlcpy(cfg.mqttBroker, doc["mqtt_broker"] | cfg.mqttBroker, sizeof(cfg.mqttBroker));
        cfg.mqttPort = doc["mqtt_port"] | cfg.mqttPort;
        strlcpy(cfg.mqttUser,   doc["mqtt_user"]  | cfg.mqttUser,   sizeof(cfg.mqttUser));
        strlcpy(cfg.mqttPass,   doc["mqtt_pass"]  | cfg.mqttPass,   sizeof(cfg.mqttPass));

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

        Serial.printf("[CFG] Loaded – hostname: %s, broker: %s, signals: %u, turnouts: %u\n",
                      cfg.hostname, cfg.mqttBroker, cfg.numSignals, cfg.numTurnouts);
        return true;
    }

    bool save() {
        JsonDocument doc;
        doc["hostname"]    = cfg.hostname;
        doc["mqtt_broker"] = cfg.mqttBroker;
        doc["mqtt_port"]   = cfg.mqttPort;
        doc["mqtt_user"]   = cfg.mqttUser;
        doc["mqtt_pass"]   = cfg.mqttPass;

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
        }

        JsonArray swArr = doc["turnouts"].to<JsonArray>();
        for (uint8_t i = 0; i < cfg.numTurnouts; i++) {
            JsonObject o = swArr.add<JsonObject>();
            o["id"]    = cfg.turnouts[i].id;
            o["chS"]   = cfg.turnouts[i].chS;
            o["chD"]   = cfg.turnouts[i].chD;
            o["pulse"] = cfg.turnouts[i].pulse;
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
};
