#pragma once
#include <Arduino.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include "ConfigManager.h"

/**
 * Thin wrapper around PubSubClient.
 * Callers register a command handler via onCommand().
 * The loop() method must be called every iteration of the Arduino loop.
 */
class MQTTManager {
public:
    using CommandHandler    = std::function<void(const String& topic, const String& payload)>;
    using OnConnectHandler  = std::function<void()>;   // called each time MQTT connects

    void begin(ConfigManager& cfg, CommandHandler handler,
               OnConnectHandler onConnect = nullptr) {
        _cfg       = &cfg;
        _handler   = handler;
        _onConnect = onConnect;
        _enabled   = (cfg.cfg.mqttBroker[0] != '\0');

        _client.setClient(_wifiClient);
        _client.setCallback([this](char* t, uint8_t* p, unsigned int l) {
            _handler(String(t), String((char*)p, l));
        });
        _client.setKeepAlive(60);
        _client.setSocketTimeout(5);

        if (!_enabled) {
            Serial.println("[MQTT] Broker not configured – MQTT disabled until set via web");
        }
    }

    void loop() {
        if (!_enabled) return;

        if (!_client.connected()) {
            if (millis() >= _nextReconnectMs)
                _reconnect();
        } else {
            _client.loop();
            if (millis() - _lastHeartbeat > MQTT_HEARTBEAT_MS) {
                _lastHeartbeat = millis();
                _client.publish(_cfg->topic("status/heartbeat").c_str(), "online", true);
            }
        }
    }

    bool isEnabled() const { return _enabled; }

    // Publish on a board-namespaced topic: railway/<name>/<suffix>
    bool publish(const char* suffix, const String& payload, bool retain = false) {
        if (!_enabled || !_client.connected()) return false;
        return _client.publish(_cfg->topic(suffix).c_str(),
                               payload.c_str(), retain);
    }

    bool publishRaw(const char* topic, const String& payload, bool retain = false) {
        if (!_enabled || !_client.connected()) return false;
        return _client.publish(topic, payload.c_str(), retain);
    }

    bool subscribe(const char* topic) {
        if (!_enabled || !_client.connected()) return false;
        return _client.subscribe(topic);
    }

    bool connected() { return _client.connected(); }

private:
    WiFiClient     _wifiClient;
    PubSubClient   _client;
    ConfigManager* _cfg            = nullptr;
    CommandHandler   _handler;
    OnConnectHandler _onConnect;
    unsigned long    _lastHeartbeat = 0;
    unsigned long    _nextReconnectMs = 0;
    unsigned long    _lastResolveMs  = 0;
    uint8_t          _retries       = 0;
    bool             _enabled       = false;
    bool             _brokerResolved = false;
    IPAddress        _resolvedBroker;

    // Resolve broker address (supports IP strings and *.local mDNS hostnames)
    bool _resolveBroker() {
        const char* broker = _cfg->cfg.mqttBroker;
        if (broker[0] == '\0') return false;

        if (_brokerResolved) return true;

        if (millis() - _lastResolveMs < 15000) return false;
        _lastResolveMs = millis();

        if (_resolvedBroker.fromString(broker)) {
            _brokerResolved = true;
            return true;
        }

        Serial.printf("[MQTT] Resolving mDNS: %s\n", broker);
        _resolvedBroker = MDNS.queryHost(broker, 1500);
        if (_resolvedBroker == IPAddress(0, 0, 0, 0)) {
            Serial.println("[MQTT] mDNS resolution failed");
            return false;
        }
        Serial.printf("[MQTT] Resolved %s → %s\n", broker,
                      _resolvedBroker.toString().c_str());
        _brokerResolved = true;
        return true;
    }

    void _reconnect() {
        _nextReconnectMs = millis() + 8000;

        if (_retries >= 5) {
            _nextReconnectMs = millis() + 60000;
            _retries = 0;
            Serial.println("[MQTT] Backoff 60s – broker non raggiungibile");
            return;
        }

        if (!_resolveBroker()) {
            _retries++;
            return;
        }

        _client.setServer(_resolvedBroker, _cfg->cfg.mqttPort);
        Serial.printf("[MQTT] Connecting to %s (%s):%u ...\n",
                      _cfg->cfg.mqttBroker,
                      _resolvedBroker.toString().c_str(),
                      _cfg->cfg.mqttPort);

        const bool ok = _client.connect(
            _cfg->cfg.hostname,
            _cfg->cfg.mqttUser[0] ? _cfg->cfg.mqttUser : nullptr,
            _cfg->cfg.mqttPass[0] ? _cfg->cfg.mqttPass : nullptr,
            _cfg->topic("status/heartbeat").c_str(),
            1, true, "offline");

        if (ok) {
            _retries = 0;
            _nextReconnectMs = 0;
            Serial.println("[MQTT] Connected");
            _client.subscribe(_cfg->topic("command/set").c_str());
            Serial.printf("[MQTT] Subscribed to %s\n",
                          _cfg->topic("command/set").c_str());
            if (_onConnect) _onConnect();
        } else {
            _retries++;
            Serial.printf("[MQTT] Failed (rc=%d) – retry %u/5\n",
                          _client.state(), _retries);
        }
    }
};
