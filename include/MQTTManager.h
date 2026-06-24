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

        _client.setClient(_wifiClient);
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

    // Publish on a board-namespaced topic: railway/<name>/<suffix>
    bool publish(const char* suffix, const String& payload, bool retain = false) {
        return _client.publish(_cfg->topic(suffix).c_str(),
                               payload.c_str(), retain);
    }

    // Publish on any arbitrary topic (used for Rocrail feedback)
    bool publishRaw(const char* topic, const String& payload, bool retain = false) {
        return _client.publish(topic, payload.c_str(), retain);
    }

    // Subscribe to an arbitrary topic (used for Rocrail command topics)
    bool subscribe(const char* topic) {
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
    uint8_t          _retries       = 0;
    IPAddress        _resolvedBroker;

    // Resolve broker address (supports IP strings and *.local mDNS hostnames)
    bool _resolveBroker() {
        const char* broker = _cfg->cfg.mqttBroker;
        if (broker[0] == '\0') return false;

        // Try direct IP parse first
        if (_resolvedBroker.fromString(broker)) return true;

        // mDNS resolution (works for "plastico.local" etc.)
        Serial.printf("[MQTT] Resolving mDNS: %s\n", broker);
        _resolvedBroker = MDNS.queryHost(broker, 2000);
        if (_resolvedBroker == IPAddress(0, 0, 0, 0)) {
            Serial.println("[MQTT] mDNS resolution failed");
            return false;
        }
        Serial.printf("[MQTT] Resolved %s → %s\n", broker,
                      _resolvedBroker.toString().c_str());
        return true;
    }

    void _reconnect() {
        if (_retries > 5) {
            static unsigned long _backoffUntil = 0;
            if (millis() < _backoffUntil) return;
            _backoffUntil = millis() + 30000;
            _retries = 0;
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
            _cfg->topic("status/heartbeat").c_str(),  // LWT topic
            1, true, "offline"                         // LWT
        );

        if (ok) {
            _retries = 0;
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
