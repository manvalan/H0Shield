#pragma once
#include <Arduino.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include "ConfigManager.h"

/**
 * Lightweight configuration web server (port 80).
 * Serves:
 *   GET  /         → config UI (HTML/JS stored in LittleFS at /index.html)
 *   GET  /api/cfg  → current config as JSON
 *   POST /api/cfg  → update config (JSON body), saves & reboots
 */
class WebConfig {
public:
    void begin(ConfigManager& cfgMgr) {
        _cfg = &cfgMgr;

        _server.on("/", HTTP_GET, [this]() { _serveIndex(); });
        _server.on("/api/cfg", HTTP_GET,  [this]() { _handleGet(); });
        _server.on("/api/cfg", HTTP_POST, [this]() { _handlePost(); });
        _server.onNotFound([this]() {
            _server.send(404, "text/plain", "Not found");
        });

        _server.begin();
        Serial.printf("[WEB] Config server started – http://%s.local/\n",
                      _cfg->cfg.hostname);
    }

    void loop() { _server.handleClient(); }

private:
    WebServer      _server{80};
    ConfigManager* _cfg = nullptr;

    void _serveIndex() {
        File f = LittleFS.open("/index.html", "r");
        if (!f) {
            _server.send(500, "text/plain", "index.html not found in LittleFS");
            return;
        }
        _server.streamFile(f, "text/html");
        f.close();
    }

    void _handleGet() {
        JsonDocument doc;
        doc["hostname"]    = _cfg->cfg.hostname;
        doc["mqtt_broker"] = _cfg->cfg.mqttBroker;
        doc["mqtt_port"]   = _cfg->cfg.mqttPort;
        doc["mqtt_user"]   = _cfg->cfg.mqttUser;

        JsonArray arr = doc["pin_map"].to<JsonArray>();
        for (uint8_t i = 0; i < MUX_CHANNELS; i++) {
            arr.add(static_cast<uint8_t>(_cfg->cfg.pinMap[i]));
        }

        String out;
        serializeJson(doc, out);
        _server.send(200, "application/json", out);
    }

    void _handlePost() {
        if (!_server.hasArg("plain")) {
            _server.send(400, "text/plain", "No body");
            return;
        }

        JsonDocument doc;
        if (deserializeJson(doc, _server.arg("plain"))) {
            _server.send(400, "text/plain", "Bad JSON");
            return;
        }

        if (doc["hostname"].is<const char*>())
            strlcpy(_cfg->cfg.hostname, doc["hostname"], sizeof(_cfg->cfg.hostname));
        if (doc["mqtt_broker"].is<const char*>())
            strlcpy(_cfg->cfg.mqttBroker, doc["mqtt_broker"], sizeof(_cfg->cfg.mqttBroker));
        if (doc["mqtt_port"].is<uint16_t>())
            _cfg->cfg.mqttPort = doc["mqtt_port"];
        if (doc["mqtt_user"].is<const char*>())
            strlcpy(_cfg->cfg.mqttUser, doc["mqtt_user"], sizeof(_cfg->cfg.mqttUser));
        if (doc["mqtt_pass"].is<const char*>())
            strlcpy(_cfg->cfg.mqttPass, doc["mqtt_pass"], sizeof(_cfg->cfg.mqttPass));

        JsonArray arr = doc["pin_map"].as<JsonArray>();
        for (uint8_t i = 0; i < MUX_CHANNELS && i < arr.size(); i++) {
            _cfg->cfg.pinMap[i] = static_cast<ChannelRole>(arr[i].as<uint8_t>());
        }

        _cfg->save();
        _server.send(200, "application/json", "{\"status\":\"ok\",\"restarting\":true}");
        delay(500);
        ESP.restart();
    }
};
