#pragma once
#include <Arduino.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include "ConfigManager.h"

/**
 * Thin wrapper around PubSubClient.
 * Callers register a command handler via onCommand().
 * The loop() method must be called every iteration of the Arduino loop.
 */
class MQTTManager {
public:
    using CommandHandler = std::function<void(const String& topic, const String& payload)>;

    void begin(ConfigManager& cfg, CommandHandler handler) {
        _cfg     = &cfg;
        _handler = handler;

        _client.setClient(_wifiClient);
        _client.setServer(cfg.cfg.mqttBroker, cfg.cfg.mqttPort);
        _client.setCallback([this](char* t, uint8_t* p, unsigned int l) {
            _handler(String(t), String((char*)p, l));
        });
        _client.setKeepAlive(60);
        _client.setSocketTimeout(5);
    }

    void loop() {
        if (!_client.connected()) _reconnect();
        _client.loop();

        if (millis() - _lastHeartbeat > MQTT_HEARTBEAT_MS) {
            _lastHeartbeat = millis();
            _client.publish(_cfg->topic("status/heartbeat").c_str(), "online", true);
        }
    }

    bool publish(const char* suffix, const String& payload, bool retain = false) {
        return _client.publish(_cfg->topic(suffix).c_str(),
                               payload.c_str(), retain);
    }

    bool connected() const { return _client.connected(); }

private:
    WiFiClient     _wifiClient;
    PubSubClient   _client;
    ConfigManager* _cfg           = nullptr;
    CommandHandler _handler;
    unsigned long  _lastHeartbeat = 0;
    uint8_t        _retries       = 0;

    void _reconnect() {
        if (_retries > 5) {
            // Back-off: wait 30 s before trying again
            static unsigned long _backoffUntil = 0;
            if (millis() < _backoffUntil) return;
            _backoffUntil = millis() + 30000;
            _retries = 0;
        }

        Serial.printf("[MQTT] Connecting to %s:%u ...\n",
                      _cfg->cfg.mqttBroker, _cfg->cfg.mqttPort);

        const bool ok = _client.connect(
            _cfg->cfg.hostname,
            _cfg->cfg.mqttUser[0] ? _cfg->cfg.mqttUser : nullptr,
            _cfg->cfg.mqttPass[0] ? _cfg->cfg.mqttPass : nullptr,
            _cfg->topic("status/heartbeat").c_str(),  // LWT topic
            1, true, "offline"                         // LWT
        );

        if (ok) {
            _retries = 0;
            Serial.println("[MQTT] Connected");
            _client.subscribe(_cfg->topic("command/set").c_str());
            Serial.printf("[MQTT] Subscribed to %s\n",
                          _cfg->topic("command/set").c_str());
        } else {
            _retries++;
            Serial.printf("[MQTT] Failed (rc=%d) – retry %u/5\n",
                          _client.state(), _retries);
        }
    }
};
