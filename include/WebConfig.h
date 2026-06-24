#pragma once
#include <Arduino.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <map>
#include <functional>
#include "ConfigManager.h"

/**
 * Lightweight configuration web server (port 80).
 *   GET  /            → config UI  (index.html from LittleFS)
 *   GET  /api/cfg     → current config as JSON
 *   POST /api/cfg     → update config, saves & reboots
 *   GET  /api/status  → live status (uptime, IP, RSSI, MQTT, objects)
 *   POST /api/test    → manual test: signal aspect or turnout command
 */

// Live-state provider injected by main.cpp
struct LiveStatus {
    bool   mqttConnected = false;
    int8_t rssi          = 0;

    // Sensors: muxCh → occupied
    std::map<uint8_t, bool>   sensorStates;
    // Signals: rocrailId → aspect string
    std::map<String, String>  signalStates;
    // Turnouts: rocrailId → "straight"|"turnout"
    std::map<String, String>  turnoutStates;
};

class WebConfig {
public:
    using TestHandler = std::function<void(const String& type, const String& id,
                                           const String& cmd, const String& extra)>;

    void begin(ConfigManager& cfgMgr, LiveStatus* live = nullptr,
               TestHandler testHandler = nullptr) {
        _cfg         = &cfgMgr;
        _live        = live;
        _testHandler = testHandler;

        _server.on("/", HTTP_GET, [this]() { _serveIndex(); });
        _server.on("/api/cfg",    HTTP_GET,  [this]() { _handleGet(); });
        _server.on("/api/cfg",    HTTP_POST, [this]() { _handlePost(); });
        _server.on("/api/status", HTTP_GET,  [this]() { _handleStatus(); });
        _server.on("/api/test",   HTTP_POST, [this]() { _handleTest(); });
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
    ConfigManager* _cfg         = nullptr;
    LiveStatus*    _live        = nullptr;
    TestHandler    _testHandler;

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

        JsonArray pmArr = doc["pin_map"].to<JsonArray>();
        for (uint8_t i = 0; i < MUX_CHANNELS; i++) {
            pmArr.add(static_cast<uint8_t>(_cfg->cfg.pinMap[i]));
        }

        JsonArray sgArr = doc["signals"].to<JsonArray>();
        for (uint8_t i = 0; i < _cfg->cfg.numSignals; i++) {
            JsonObject o = sgArr.add<JsonObject>();
            o["id"]   = _cfg->cfg.signals[i].id;
            o["type"] = _cfg->cfg.signals[i].type;
            o["chR"]  = _cfg->cfg.signals[i].chR;
            o["chG"]  = _cfg->cfg.signals[i].chG;
            o["chV"]  = _cfg->cfg.signals[i].chV;
        }

        JsonArray swArr = doc["turnouts"].to<JsonArray>();
        for (uint8_t i = 0; i < _cfg->cfg.numTurnouts; i++) {
            JsonObject o = swArr.add<JsonObject>();
            o["id"]    = _cfg->cfg.turnouts[i].id;
            o["chS"]   = _cfg->cfg.turnouts[i].chS;
            o["chD"]   = _cfg->cfg.turnouts[i].chD;
            o["pulse"] = _cfg->cfg.turnouts[i].pulse;
        }

        JsonArray rbArr = doc["sensors_rb"].to<JsonArray>();
        for (uint8_t i = 0; i < _cfg->cfg.numSensorsRb; i++) {
            JsonObject o = rbArr.add<JsonObject>();
            o["rocrail_id"] = _cfg->cfg.sensorsRb[i].rocrailId;
            o["mux_ch"]     = _cfg->cfg.sensorsRb[i].muxCh;
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
            strlcpy(_cfg->cfg.hostname,   doc["hostname"],    sizeof(_cfg->cfg.hostname));
        if (doc["mqtt_broker"].is<const char*>())
            strlcpy(_cfg->cfg.mqttBroker, doc["mqtt_broker"], sizeof(_cfg->cfg.mqttBroker));
        if (doc["mqtt_port"].is<uint16_t>())
            _cfg->cfg.mqttPort = doc["mqtt_port"];
        if (doc["mqtt_user"].is<const char*>())
            strlcpy(_cfg->cfg.mqttUser, doc["mqtt_user"], sizeof(_cfg->cfg.mqttUser));
        if (doc["mqtt_pass"].is<const char*>())
            strlcpy(_cfg->cfg.mqttPass, doc["mqtt_pass"], sizeof(_cfg->cfg.mqttPass));

        JsonArray pmArr = doc["pin_map"].as<JsonArray>();
        for (uint8_t i = 0; i < MUX_CHANNELS && i < (uint8_t)pmArr.size(); i++) {
            _cfg->cfg.pinMap[i] = static_cast<ChannelRole>(pmArr[i].as<uint8_t>());
        }

        // Signals
        _cfg->cfg.numSignals = 0;
        for (JsonObject sg : doc["signals"].as<JsonArray>()) {
            if (_cfg->cfg.numSignals >= BoardConfig::MAX_SIGNALS) break;
            SignalConfig& s = _cfg->cfg.signals[_cfg->cfg.numSignals++];
            strlcpy(s.id, sg["id"] | "", sizeof(s.id));
            s.type = sg["type"] | 0;
            s.chR  = sg["chR"]  | 0;
            s.chG  = sg["chG"]  | 1;
            s.chV  = sg["chV"]  | 2;
        }

        // Turnouts
        _cfg->cfg.numTurnouts = 0;
        for (JsonObject sw : doc["turnouts"].as<JsonArray>()) {
            if (_cfg->cfg.numTurnouts >= BoardConfig::MAX_TURNOUTS) break;
            TurnoutConfig& t = _cfg->cfg.turnouts[_cfg->cfg.numTurnouts++];
            strlcpy(t.id, sw["id"] | "", sizeof(t.id));
            t.chS   = sw["chS"]   | 0;
            t.chD   = sw["chD"]   | 1;
            t.pulse = sw["pulse"] | 300;
        }

        // Sensors Rocrail
        _cfg->cfg.numSensorsRb = 0;
        for (JsonObject rb : doc["sensors_rb"].as<JsonArray>()) {
            if (_cfg->cfg.numSensorsRb >= BoardConfig::MAX_SENSORS_RB) break;
            SensorRbConfig& sr = _cfg->cfg.sensorsRb[_cfg->cfg.numSensorsRb++];
            strlcpy(sr.rocrailId, rb["rocrail_id"] | "", sizeof(sr.rocrailId));
            sr.muxCh = rb["mux_ch"] | 0;
        }

        // ── Channel conflict check ─────────────────────────────────────
        String conflict = _checkConflicts();
        if (conflict.length()) {
            _server.send(409, "application/json",
                         "{\"error\":\"conflict\",\"detail\":\"" + conflict + "\"}");
            return;
        }

        _cfg->save();
        _server.send(200, "application/json", "{\"status\":\"ok\",\"restarting\":true}");
        delay(500);
        ESP.restart();
    }

    // Returns a description string if conflicts exist, empty string if OK
    String _checkConflicts() {
        // Collect all MUX channel assignments  key="chN" → source description
        std::map<String, String> assigned;

        // pin_map roles
        for (uint8_t i = 0; i < MUX_CHANNELS; i++) {
            if (_cfg->cfg.pinMap[i] == ChannelRole::UNUSED) continue;
            uint8_t span = (_cfg->cfg.pinMap[i] == ChannelRole::SIGNAL_RGB) ? 3 : 1;
            for (uint8_t j = 0; j < span; j++) {
                String key = "ch" + String(i + j);
                if (assigned.count(key)) {
                    return "Canale MUX " + String(i+j) + " assegnato due volte";
                }
                assigned[key] = "pin_map[" + String(i) + "]";
            }
        }

        // Signals (3 channels each)
        for (uint8_t i = 0; i < _cfg->cfg.numSignals; i++) {
            const SignalConfig& s = _cfg->cfg.signals[i];
            for (uint8_t ch : {s.chR, s.chG, s.chV}) {
                String key = "ch" + String(ch);
                if (assigned.count(key)) {
                    return String("Segnale '") + s.id + "' ch" + ch + " in conflitto";
                }
                assigned[key] = String("signal ") + s.id;
            }
        }

        // Turnouts (2 channels each)
        for (uint8_t i = 0; i < _cfg->cfg.numTurnouts; i++) {
            const TurnoutConfig& t = _cfg->cfg.turnouts[i];
            for (uint8_t ch : {t.chS, t.chD}) {
                String key = "ch" + String(ch);
                if (assigned.count(key)) {
                    return String("Scambio '") + t.id + "' ch" + ch + " in conflitto";
                }
                assigned[key] = String("turnout ") + t.id;
            }
        }

        // Sensors
        for (uint8_t i = 0; i < _cfg->cfg.numSensorsRb; i++) {
            const SensorRbConfig& sr = _cfg->cfg.sensorsRb[i];
            String key = "ch" + String(sr.muxCh);
            if (assigned.count(key)) {
                return String("Sensore '") + sr.rocrailId + "' ch" + sr.muxCh + " in conflitto";
            }
            assigned[key] = String("sensor ") + sr.rocrailId;
        }

        return "";
    }

    void _handleStatus() {
        JsonDocument doc;
        doc["uptime_s"]      = millis() / 1000;
        doc["ip"]            = WiFi.localIP().toString();
        doc["rssi"]          = _live ? _live->rssi : WiFi.RSSI();
        doc["mqtt"]          = _live ? _live->mqttConnected : false;
        doc["hostname"]      = _cfg->cfg.hostname;
        doc["free_heap"]     = ESP.getFreeHeap();

        if (_live) {
            JsonObject sens = doc["sensors"].to<JsonObject>();
            for (auto& [ch, occ] : _live->sensorStates) {
                sens[String(ch)] = occ;
            }
            JsonObject sigs = doc["signals"].to<JsonObject>();
            for (auto& [id, asp] : _live->signalStates) {
                sigs[id] = asp;
            }
            JsonObject sw = doc["turnouts"].to<JsonObject>();
            for (auto& [id, st] : _live->turnoutStates) {
                sw[id] = st;
            }
        }

        String out;
        serializeJson(doc, out);
        _server.send(200, "application/json", out);
    }

    void _handleTest() {
        if (!_server.hasArg("plain") || !_testHandler) {
            _server.send(400, "text/plain", "Not available");
            return;
        }
        JsonDocument doc;
        if (deserializeJson(doc, _server.arg("plain"))) {
            _server.send(400, "text/plain", "Bad JSON");
            return;
        }
        String type  = doc["type"]  | "";   // "signal" | "turnout"
        String id    = doc["id"]    | "";
        String cmd   = doc["cmd"]   | "";   // aspect or "straight"/"turnout"
        String extra = doc["extra"] | "";
        _testHandler(type, id, cmd, extra);
        _server.send(200, "application/json", "{\"ok\":true}");
    }
};
