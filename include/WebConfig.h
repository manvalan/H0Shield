#pragma once
#include <Arduino.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <map>
#include <functional>
#include "ConfigManager.h"
#include "WifiSetup.h"
#include "SecureStore.h"
#include "LiveStatus.h"

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
        _server.on("/api/wifi/status", HTTP_GET,  [this]() { _handleWifiStatus(); });
        _server.on("/api/wifi/scan",   HTTP_GET,  [this]() { _handleWifiScan(); });
        _server.on("/api/wifi/connect", HTTP_POST, [this]() { _handleWifiConnect(); });
        _server.on("/api/wifi/reset",  HTTP_POST, [this]() { _handleWifiReset(); });
        _server.on("/api/auth/login",  HTTP_POST, [this]() { _handleAuthLogin(); });
        _server.on("/api/auth/check",  HTTP_GET,  [this]() { _handleAuthCheck(); });
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
    String         _authToken;

    bool _authRequired() const {
        return _cfg->cfg.webPassHash[0] != '\0';
    }

    bool _checkAuth() {
        if (!_authRequired()) return true;
        return _server.header("X-Auth-Token") == _authToken;
    }

    void _send401() {
        _server.send(401, "application/json", "{\"error\":\"auth_required\"}");
    }

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
        doc["wifi_ssid"]   = _cfg->cfg.wifiSsid;
        doc["web_user"]    = _cfg->cfg.webUser;
        doc["has_web_pass"]= _authRequired();
        doc["sensor_threshold"] = _cfg->cfg.sensorThreshold;
        doc["tof_enabled"]      = _cfg->cfg.tofEnabled;
        doc["tof_threshold_mm"] = _cfg->cfg.tofThresholdMm;
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

        JsonArray tbArr = doc["tof_blocks"].to<JsonArray>();
        for (uint8_t i = 0; i < _cfg->cfg.numTofBlocks; i++) {
            JsonObject o = tbArr.add<JsonObject>();
            o["rocrail_id"]   = _cfg->cfg.tofBlocks[i].rocrailId;
            o["threshold_mm"] = _cfg->cfg.tofBlocks[i].thresholdMm;
        }

        String out;
        serializeJson(doc, out);
        _server.send(200, "application/json", out);
    }

    void _handlePost() {
        if (!_checkAuth()) { _send401(); return; }
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
        if (doc["web_user"].is<const char*>())
            strlcpy(_cfg->cfg.webUser, doc["web_user"], sizeof(_cfg->cfg.webUser));
        if (doc["web_pass"].is<const char*>() && doc["web_pass"].as<String>().length() > 0) {
            String h = SecureStore::hashPassword(doc["web_pass"].as<String>());
            strlcpy(_cfg->cfg.webPassHash, h.c_str(), sizeof(_cfg->cfg.webPassHash));
        }
        if (doc["sensor_threshold"].is<uint16_t>())
            _cfg->cfg.sensorThreshold = doc["sensor_threshold"];
        if (doc["tof_enabled"].is<bool>())
            _cfg->cfg.tofEnabled = doc["tof_enabled"];
        if (doc["tof_threshold_mm"].is<uint8_t>() || doc["tof_threshold_mm"].is<uint16_t>())
            _cfg->cfg.tofThresholdMm = doc["tof_threshold_mm"].as<uint8_t>();
        if (doc["mqtt_broker"].is<const char*>())
            strlcpy(_cfg->cfg.mqttBroker, doc["mqtt_broker"], sizeof(_cfg->cfg.mqttBroker));
        if (doc["mqtt_port"].is<uint16_t>())
            _cfg->cfg.mqttPort = doc["mqtt_port"];
        if (doc["mqtt_user"].is<const char*>())
            strlcpy(_cfg->cfg.mqttUser, doc["mqtt_user"], sizeof(_cfg->cfg.mqttUser));
        if (doc["mqtt_pass"].is<const char*>() && doc["mqtt_pass"].as<String>().length() > 0)
            SecureStore::saveMqttPassword(doc["mqtt_pass"].as<String>());

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

        _cfg->cfg.numTofBlocks = 0;
        for (JsonObject tb : doc["tof_blocks"].as<JsonArray>()) {
            if (_cfg->cfg.numTofBlocks >= BoardConfig::MAX_TOF_BLOCKS) break;
            ToFBlockConfig& t = _cfg->cfg.tofBlocks[_cfg->cfg.numTofBlocks++];
            strlcpy(t.rocrailId, tb["rocrail_id"] | "", sizeof(t.rocrailId));
            t.thresholdMm = tb["threshold_mm"] | 0;
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
        doc["wifi_ssid"]     = WiFi.status() == WL_CONNECTED ? WiFi.SSID() : _cfg->cfg.wifiSsid;
        doc["wifi_connected"]= WiFi.status() == WL_CONNECTED;
        doc["ap_mode"]       = WiFi.getMode() & WIFI_AP;

        if (_live) {
            JsonObject sens = doc["sensors"].to<JsonObject>();
            for (auto& [ch, s] : _live->sensors) {
                JsonObject o = sens[String(ch)].to<JsonObject>();
                o["type"]      = "absorption";
                o["occupied"]  = s.occupied;
                o["raw"]       = s.raw;
                o["threshold"] = s.threshold;
                if (s.rocrailId.length()) o["id"] = s.rocrailId;
            }

            JsonObject tof = doc["tof"].to<JsonObject>();
            tof["present"]      = _live->tof.present;
            tof["enabled"]      = _live->tof.enabled;
            tof["distance_mm"]  = _live->tof.distanceMm;
            tof["threshold_mm"] = _live->tof.thresholdMm;
            tof["occupied"]     = _live->tof.occupied;
            tof["valid"]        = _live->tof.valid;
            tof["status"]       = _live->tof.status;

            JsonObject sigs = doc["signals"].to<JsonObject>();
            for (auto& [id, sg] : _live->signals) {
                JsonObject o = sigs[id].to<JsonObject>();
                o["type"]   = sg.type;
                o["aspect"] = sg.aspect;
                JsonObject lamps = o["lamps"].to<JsonObject>();
                lamps["r"] = sg.lamps.r;
                lamps["g"] = sg.lamps.g;
                lamps["v"] = sg.lamps.v;
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
        if (!_checkAuth()) { _send401(); return; }
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

    void _handleWifiStatus() {
        JsonDocument doc;
        doc["connected"]  = WiFi.status() == WL_CONNECTED;
        doc["ssid"]       = WiFi.status() == WL_CONNECTED ? WiFi.SSID() : _cfg->cfg.wifiSsid;
        doc["ip"]           = WiFi.localIP().toString();
        doc["ap_ip"]        = WiFi.softAPIP().toString();
        doc["rssi"]         = WiFi.RSSI();
        doc["saved_ssid"]   = _cfg->cfg.wifiSsid;
        String out;
        serializeJson(doc, out);
        _server.send(200, "application/json", out);
    }

    void _handleWifiScan() {
        int n = WiFi.scanNetworks(false, true);
        JsonDocument doc;
        JsonArray arr = doc["networks"].to<JsonArray>();
        for (int i = 0; i < n; i++) {
            JsonObject o = arr.add<JsonObject>();
            o["ssid"]  = WiFi.SSID(i);
            o["rssi"]  = WiFi.RSSI(i);
            o["secure"]= WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
            o["ch"]    = WiFi.channel(i);
        }
        doc["count"] = n;
        String out;
        serializeJson(doc, out);
        _server.send(200, "application/json", out);
    }

    void _handleWifiConnect() {
        if (!_server.hasArg("plain")) {
            _server.send(400, "text/plain", "No body");
            return;
        }
        JsonDocument doc;
        if (deserializeJson(doc, _server.arg("plain"))) {
            _server.send(400, "text/plain", "Bad JSON");
            return;
        }
        String ssid = doc["ssid"] | "";
        String pass = doc["password"] | "";
        if (ssid.isEmpty()) {
            _server.send(400, "application/json", "{\"error\":\"ssid_required\"}");
            return;
        }
        const bool ok = connectToNetwork(*_cfg, ssid, pass);
        if (ok) {
            _server.send(200, "application/json",
                "{\"ok\":true,\"ip\":\"" + WiFi.localIP().toString() + "\"}");
        } else {
            _server.send(502, "application/json", "{\"error\":\"connection_failed\"}");
        }
    }

    void _handleAuthCheck() {
        if (!_authRequired()) {
            _server.send(200, "application/json", "{\"ok\":true}");
            return;
        }
        if (_checkAuth())
            _server.send(200, "application/json", "{\"ok\":true}");
        else
            _server.send(401, "application/json", "{\"ok\":false}");
    }

    void _handleAuthLogin() {
        if (!_server.hasArg("plain")) {
            _server.send(400, "text/plain", "No body");
            return;
        }
        JsonDocument doc;
        deserializeJson(doc, _server.arg("plain"));
        String user = doc["user"] | "";
        String pass = doc["pass"] | "";

        if (user != _cfg->cfg.webUser ||
            !SecureStore::verifyPassword(pass, _cfg->cfg.webPassHash)) {
            _server.send(401, "application/json", "{\"error\":\"invalid_credentials\"}");
            return;
        }
        _authToken = SecureStore::hashPassword(user + ":" + pass + ":session");
        _server.send(200, "application/json",
                      "{\"ok\":true,\"token\":\"" + _authToken + "\"}");
    }

    void _handleWifiReset() {
        if (_authRequired() && !_checkAuth()) { _send401(); return; }
        _server.send(200, "application/json",
                     "{\"ok\":true,\"message\":\"Riavvio in modalità setup WiFi\"}");
        delay(300);
        resetWifiAndRestart();
    }
};
