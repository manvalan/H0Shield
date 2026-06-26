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
#include "DisplayManager.h"
#include "I2cBus.h"

class WebConfig {
public:
    using TestHandler = std::function<bool(JsonObject doc, JsonObject result, String& error)>;

    void begin(ConfigManager& cfgMgr, LiveStatus* live = nullptr,
               TestHandler testHandler = nullptr,
               DisplayManager* displays = nullptr,
               I2cBus* i2c = nullptr) {
        _cfg         = &cfgMgr;
        _live        = live;
        _testHandler = testHandler;
        _displays    = displays;
        _i2c         = i2c;

        _server.on("/", HTTP_GET, [this]() { _serveIndex(); });
        _server.on("/api/cfg",    HTTP_GET,  [this]() { _handleGet(); });
        _server.on("/api/cfg",    HTTP_POST, [this]() { _handlePost(); });
        _server.on("/api/status", HTTP_GET,  [this]() { _handleStatus(); });
        _server.on("/api/board/discover", HTTP_GET, [this]() { _handleBoardDiscover(); });
        _server.on("/api/test",   HTTP_POST, [this]() { _handleTest(); });
        _server.on("/api/wifi/status", HTTP_GET,  [this]() { _handleWifiStatus(); });
        _server.on("/api/wifi/scan",   HTTP_GET,  [this]() { _handleWifiScan(); });
        _server.on("/api/wifi/connect", HTTP_POST, [this]() { _handleWifiConnect(); });
        _server.on("/api/wifi/test",   HTTP_POST, [this]() { _handleWifiTest(); });
        _server.on("/api/wifi/dismiss", HTTP_POST, [this]() { _handleWifiDismiss(); });
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
    DisplayManager* _displays   = nullptr;
    I2cBus*         _i2c        = nullptr;
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
        ConfigJson::write(_cfg->cfg, doc.to<JsonObject>(), true);

        if (_i2c) {
            JsonArray i2Arr = doc["i2c_slots"].as<JsonArray>();
            for (uint8_t s = 0; s < I2C_SLOTS && s < i2Arr.size(); s++) {
                const I2cSlotDiscovery& d = _i2c->slots[s];
                JsonObject sl = i2Arr[s];
                sl["detected"]      = I2cBus::detectedName(d.type);
                sl["detected_addr"] = d.addr;
                sl["present"]       = d.present;
            }
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

        ConfigJson::readApi(_cfg->cfg, doc.as<JsonObject>());

        if (doc["web_pass"].is<const char*>() && doc["web_pass"].as<String>().length() > 0) {
            String h = SecureStore::hashPassword(doc["web_pass"].as<String>());
            strlcpy(_cfg->cfg.webPassHash, h.c_str(), sizeof(_cfg->cfg.webPassHash));
        }
        if (doc["mqtt_pass"].is<const char*>() && doc["mqtt_pass"].as<String>().length() > 0)
            SecureStore::saveMqttPassword(doc["mqtt_pass"].as<String>());

        String conflict = _checkConflicts();
        if (conflict.length()) {
            _server.send(409, "application/json",
                         "{\"error\":\"conflict\",\"detail\":\"" + conflict + "\"}");
            return;
        }

        if (!_cfg->save()) {
            _server.send(500, "application/json",
                         "{\"error\":\"save_failed\",\"detail\":\"Scrittura config fallita\"}");
            return;
        }
        _server.send(200, "application/json", "{\"status\":\"ok\",\"restarting\":true}");
        _server.client().flush();
        delay(100);
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
            if (s.usePca) continue;
            for (uint8_t ch : {s.chR, s.chG, s.chV}) {
                String key = "ch" + String(ch);
                if (assigned.count(key)) {
                    return String("Segnale '") + s.id + "' ch" + ch
                         + " in conflitto con " + assigned[key];
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

        for (uint8_t i = 0; i < _cfg->cfg.numAccessories; i++) {
            const AccessoryConfig& a = _cfg->cfg.accessories[i];
            if (a.id[0] == '\0') continue;
            if (a.muxCh < MUX_CHANNELS && a.muxCh != MUX_CH_UNUSED) {
                String key = "ch" + String(a.muxCh);
                if (assigned.count(key)) {
                    return String("Accessorio '") + a.id + "' ch" + a.muxCh
                         + " in conflitto con " + assigned[key];
                }
                assigned[key] = String("accessory ") + a.id;
            }
            if (a.profile == AccessoryProfile::LEVEL_XING &&
                a.muxChBar < MUX_CHANNELS && a.muxChBar != MUX_CH_UNUSED) {
                String key = "ch" + String(a.muxChBar);
                if (assigned.count(key)) {
                    return String("Accessorio '") + a.id + "' ch" + a.muxChBar + " in conflitto";
                }
                assigned[key] = String("accessory ") + a.id + " (bar)";
            }
        }

        return "";
    }

    void _handleStatus() {
        JsonDocument doc;
        doc["uptime_s"]      = millis() / 1000;
        doc["ip"]            = wifiHasStaIp() ? WiFi.localIP().toString()
                                               : String(_cfg->cfg.lastStaIp);
        doc["sta_ip"]        = doc["ip"];
        doc["last_sta_ip"]   = _cfg->cfg.lastStaIp;
        doc["ap_ip"]         = WiFi.softAPIP().toString();
        if (doc["ip"].as<String>().length())
            doc["web_url"]   = String("http://") + doc["ip"].as<const char*>() + "/";
        doc["ap_url"]        = String("http://") + WiFi.softAPIP().toString() + "/";
        doc["rssi"]          = _live ? _live->rssi : WiFi.RSSI();
        doc["mqtt"]          = _live ? _live->mqttConnected : false;
        doc["hostname"]      = _cfg->cfg.hostname;
        doc["free_heap"]     = ESP.getFreeHeap();
        doc["display_driver"]= DisplayManager::driverEnabled();
        doc["wifi_ssid"]     = WiFi.status() == WL_CONNECTED ? WiFi.SSID() : _cfg->cfg.wifiSsid;
        doc["wifi_connected"]= wifiHasStaIp();
        doc["setup_mode"]    = wifiInSetupMode(*_cfg);
        doc["ap_mode"]       = wifiApRunning();
        {
            char host[32];
            wifiNormalizeHostname(_cfg->cfg.hostname, host, sizeof(host));
            doc["mdns_host"] = host;
            doc["mdns_url"]  = String("http://") + host + ".local/";
        }

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
            for (auto& [id, tw] : _live->turnouts) {
                JsonObject o = sw[id].to<JsonObject>();
                o["position"] = tw.position;
                o["busy"]     = tw.busy;
            }

            JsonObject acc = doc["accessories"].to<JsonObject>();
            for (auto& [id, ac] : _live->accessories) {
                JsonObject o = acc[id].to<JsonObject>();
                o["profile"] = ac.profile;
                o["active"]  = ac.active;
                o["lights"]  = ac.lights;
                o["bar"]     = ac.bar;
                o["mux_ch"]  = ac.muxCh;
            }

            JsonObject scn = doc["scenarios"].to<JsonObject>();
            for (auto& [id, sc] : _live->scenarios) {
                JsonObject o = scn[id].to<JsonObject>();
                o["enabled"]   = sc.enabled;
                o["active"]    = sc.active;
                o["triggered"] = sc.triggered;
                o["trigger"]   = sc.trigger;
            }
        }

        if (_displays) _displays->appendStatus(doc.as<JsonObject>());

        if (_i2c) {
            JsonObject bi = doc["board"].to<JsonObject>();
            bi["tca9548"]   = _i2c->hasMux;
            bi["tca_addr"]  = _i2c->muxAddr;
            JsonArray slots = bi["i2c_slots"].to<JsonArray>();
            for (uint8_t s = 0; s < I2C_SLOTS; s++) {
                JsonObject o = slots.add<JsonObject>();
                o["slot"]     = s;
                o["present"]  = _i2c->slots[s].present;
                o["detected"] = I2cBus::detectedName(_i2c->slots[s].type);
                o["addr"]     = _i2c->slots[s].addr;
                o["mode"]     = I2cBus::modeName(_cfg->cfg.i2cSlots[s].mode);
            }
        }

        String out;
        serializeJson(doc, out);
        _server.send(200, "application/json", out);
    }

    void _handleTest() {
        if (!_checkAuth()) { _send401(); return; }
        if (!_server.hasArg("plain") || !_testHandler) {
            _server.send(400, "application/json", "{\"ok\":false,\"error\":\"non disponibile\"}");
            return;
        }
        JsonDocument doc;
        if (deserializeJson(doc, _server.arg("plain"))) {
            _server.send(400, "application/json", "{\"ok\":false,\"error\":\"JSON non valido\"}");
            return;
        }
        String err;
        JsonDocument out;
        JsonObject res = out.to<JsonObject>();
        const bool ok = _testHandler(doc.as<JsonObject>(), res, err);
        res["ok"] = ok;
        if (!ok) res["error"] = err.length() ? err : "Comando fallito";
        String body;
        serializeJson(res, body);
        _server.send(ok ? 200 : 409, "application/json", body);
    }

    void _handleBoardDiscover() {
        JsonDocument doc;
        if (_i2c) {
            _i2c->discoverAll();
            doc["tca9548"]  = _i2c->hasMux;
            doc["tca_addr"] = _i2c->muxAddr;
            JsonArray arr = doc["slots"].to<JsonArray>();
            for (uint8_t s = 0; s < I2C_SLOTS; s++) {
                JsonObject o = arr.add<JsonObject>();
                o["slot"]     = s;
                o["connector"]= String("U") + String(s + 9);
                o["present"]  = _i2c->slots[s].present;
                o["detected"] = I2cBus::detectedName(_i2c->slots[s].type);
                o["addr"]     = _i2c->slots[s].addr;
                o["mode"]     = I2cBus::modeName(_cfg->cfg.i2cSlots[s].mode);
                if (_cfg->cfg.i2cSlots[s].elementId[0])
                    o["element_id"] = _cfg->cfg.i2cSlots[s].elementId;
            }
        } else {
            doc["error"] = "i2c_unavailable";
        }
        String out;
        serializeJson(doc, out);
        _server.send(200, "application/json", out);
    }

    void _fillWifiAccess(JsonObject doc) const {
        wifiFillAccessStatus(*_cfg, doc);
        char host[32];
        wifiNormalizeHostname(_cfg->cfg.hostname, host, sizeof(host));
        doc["mdns_host"] = host;
        doc["mdns_url"]  = String("http://") + host + ".local/";
    }

    void _handleWifiStatus() {
        JsonDocument doc;
        _fillWifiAccess(doc.as<JsonObject>());
        doc["uptime_s"] = millis() / 1000;
        wifiJob.fillJobStatus(doc.as<JsonObject>());
        String out;
        serializeJson(doc, out);
        _server.send(200, "application/json", out);
    }

    void _handleWifiScan() {
        JsonDocument doc;

        if (wifiJob.busy()) {
            doc["scanning"] = true;
            doc["busy"]     = true;
            doc["message"]  = "Connessione WiFi in corso — riprova tra poco";
            String out;
            serializeJson(doc, out);
            _server.send(409, "application/json", out);
            return;
        }

        const bool wantFresh = _server.hasArg("fresh");

        int n = WiFi.scanComplete();
        if ((n == -1 || wifiJob.phase() == WiFiJobManager::Phase::SCANNING)) {
            doc["scanning"]   = true;
            doc["message"]    = "Scansione in corso…";
            doc["elapsed_ms"] = wifiJob.phase() == WiFiJobManager::Phase::SCANNING
                ? static_cast<uint32_t>(millis()) : 0;
            String out;
            serializeJson(doc, out);
            _server.send(200, "application/json", out);
            return;
        }

        if (n >= 0 && !wantFresh) {
            wifiJob.fillScanResults(doc);
            String out;
            serializeJson(doc, out);
            _server.send(200, "application/json", out);
            return;
        }

        if (!wifiJob.startScan()) {
            doc["scanning"] = true;
            doc["busy"]     = true;
            doc["message"]  = "Operazione WiFi già in corso";
            String out;
            serializeJson(doc, out);
            _server.send(409, "application/json", out);
            return;
        }

        doc["scanning"] = true;
        doc["started"]  = true;
        doc["message"]  = "Scansione avviata…";
        String out;
        serializeJson(doc, out);
        _server.send(202, "application/json", out);
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
        if (wifiJob.busy()) {
            _server.send(409, "application/json",
                         "{\"ok\":false,\"busy\":true,\"message\":\"Operazione WiFi già in corso\"}");
            return;
        }
        if (!wifiJob.startConnect(ssid, pass, true)) {
            _server.send(500, "application/json",
                         "{\"ok\":false,\"message\":\"Impossibile avviare la connessione\"}");
            return;
        }
        _server.send(202, "application/json",
                     "{\"ok\":true,\"pending\":true,\"message\":\"Connessione avviata — attendi il risultato\"}");
    }

    void _handleWifiTest() {
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
            _server.send(400, "application/json",
                         "{\"ok\":false,\"message\":\"Seleziona una rete\"}");
            return;
        }
        if (wifiJob.busy()) {
            _server.send(409, "application/json",
                         "{\"ok\":false,\"busy\":true,\"message\":\"Operazione WiFi già in corso\"}");
            return;
        }
        if (!wifiJob.startConnect(ssid, pass, false)) {
            _server.send(500, "application/json",
                         "{\"ok\":false,\"message\":\"Impossibile avviare il test\"}");
            return;
        }
        _server.send(202, "application/json",
                     "{\"ok\":true,\"pending\":true,\"message\":\"Test connessione avviato\"}");
    }

    void _handleWifiDismiss() {
        wifiJob.dismiss();
        _server.send(200, "application/json", "{\"ok\":true}");
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
        resetWifiAndRestart(*_cfg);
    }
};
