#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include "Config.h"

struct BoardConfig {
    char hostname[32]     = "ShieldH0";
    char mqttBroker[64]   = "";
    uint16_t mqttPort     = 1883;
    char mqttUser[32]     = "";
    char mqttPass[32]     = "";
    ChannelRole pinMap[MUX_CHANNELS] = {};   // all UNUSED by default
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

        Serial.printf("[CFG] Loaded – hostname: %s, broker: %s\n",
                      cfg.hostname, cfg.mqttBroker);
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
