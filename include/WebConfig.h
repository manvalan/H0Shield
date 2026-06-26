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
    using TestHandler = std::function<void(const String& type, const String& id,
                                           const String& cmd, const String& extra)>;

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
        doc["hostname"]    = _cfg->cfg.hostname;
        doc["wifi_ssid"]   = _cfg->cfg.wifiSsid;
        if (_cfg->cfg.lastStaIp[0]) doc["last_sta_ip"] = _cfg->cfg.lastStaIp;
        doc["web_user"]    = _cfg->cfg.webUser;
        doc["has_web_pass"]= _authRequired();
        doc["sensor_threshold"] = _cfg->cfg.sensorThreshold;
        doc["tof_enabled"]      = _cfg->cfg.tofEnabled;
        doc["tof_threshold_mm"] = _cfg->cfg.tofThresholdMm;
        doc["boot_aspect_main"]  = _cfg->cfg.bootAspectMain;
        doc["boot_aspect_shunt"] = _cfg->cfg.bootAspectShunt;
        doc["tca9548_addr"]      = _cfg->cfg.tca9548Addr;
        doc["tof_i2c_slot"]      = _cfg->cfg.tofI2cSlot;

        JsonArray i2Arr = doc["i2c_slots"].to<JsonArray>();
        for (uint8_t s = 0; s < I2C_SLOTS; s++) {
            JsonObject sl = i2Arr.add<JsonObject>();
            sl["slot"] = s;
            sl["mode"] = I2cBus::modeName(_cfg->cfg.i2cSlots[s].mode);
            if (_cfg->cfg.i2cSlots[s].elementId[0])
                sl["element_id"] = _cfg->cfg.i2cSlots[s].elementId;
            if (_i2c) {
                const I2cSlotDiscovery& d = _i2c->slots[s];
                sl["detected"]    = I2cBus::detectedName(d.type);
                sl["detected_addr"] = d.addr;
                sl["present"]     = d.present;
            }
        }

        doc["mqtt_broker"] = _cfg->cfg.mqttBroker;
        doc["mqtt_port"]   = _cfg->cfg.mqttPort;
        doc["mqtt_user"]   = _cfg->cfg.mqttUser;
        doc["info_mqtt_topic"]   = _cfg->cfg.infoMqttTopic;
        doc["info_mqtt_enabled"] = _cfg->cfg.infoMqttEnabled;

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
            if (_cfg->cfg.signals[i].usePca) o["use_pca"] = true;
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

        JsonArray dpArr = doc["displays"].to<JsonArray>();
        for (uint8_t i = 0; i < _cfg->cfg.numDisplays; i++) {
            const DisplayConfig& d = _cfg->cfg.displays[i];
            JsonObject o = dpArr.add<JsonObject>();
            o["id"]            = d.id;
            o["type"]          = (d.type == DisplayType::TIMETABLE) ? "timetable" : "platform";
            if (d.i2cSlot < I2C_SLOTS) o["i2c_slot"] = d.i2cSlot;
            o["i2c_addr"]      = d.i2cAddr;
            o["platform_num"]  = d.platformNum;
            if (d.stationId[0]) o["station_id"] = d.stationId;
            o["station_name"]  = d.stationName;
            o["destination"]   = d.destination;
            o["departure_time"]= d.departureTime;
            o["status"]        = d.status;
            JsonArray rows = o["rows"].to<JsonArray>();
            for (uint8_t r = 0; r < d.numRows; r++) {
                JsonObject ro = rows.add<JsonObject>();
                ro["time"]        = d.rows[r].timeStr;
                ro["destination"] = d.rows[r].destination;
                ro["platform"]    = d.rows[r].platformBin;
                ro["status"]      = d.rows[r].status;
            }
        }

        JsonArray acArr = doc["accessories"].to<JsonArray>();
        for (uint8_t i = 0; i < _cfg->cfg.numAccessories; i++) {
            const AccessoryConfig& a = _cfg->cfg.accessories[i];
            JsonObject o = acArr.add<JsonObject>();
            o["id"]       = a.id;
            o["profile"]  = ConfigManager::profileName(a.profile);
            if (a.profile == AccessoryProfile::LEVEL_XING) {
                o["mux_ch_lights"] = a.muxCh;
                o["mux_ch_bar"]    = a.muxChBar;
            } else {
                o["mux_ch"] = a.muxCh;
            }
            o["pulse_ms"] = a.pulseMs;
        }

        JsonArray scArr = doc["scenarios"].to<JsonArray>();
        for (uint8_t i = 0; i < _cfg->cfg.numScenarios; i++) {
            const ScenarioConfig& s = _cfg->cfg.scenarios[i];
            JsonObject o = scArr.add<JsonObject>();
            o["id"] = s.id;
            o["enabled"] = s.enabled;
            if (s.muxCh != 255) o["mux_ch"] = s.muxCh;
            o["tof_trigger"] = s.tofTrigger;
            if (s.rocrailId[0]) o["rocrail_id"] = s.rocrailId;
            if (s.accessoryPl[0])    o["accessory_pl"] = s.accessoryPl;
            if (s.accessoryLight[0]) o["accessory_light"] = s.accessoryLight;
            if (s.accessoryBell[0])  o["accessory_bell"] = s.accessoryBell;
            if (s.displayId[0])      o["display_id"] = s.displayId;
            o["status_occupied"] = s.statusOcc;
            o["status_free"] = s.statusFree;
            o["dest_occupied"] = s.destOcc;
            o["dest_free"] = s.destFree;
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
        if (doc["boot_aspect_main"].is<const char*>())
            strlcpy(_cfg->cfg.bootAspectMain, doc["boot_aspect_main"], sizeof(_cfg->cfg.bootAspectMain));
        if (doc["boot_aspect_shunt"].is<const char*>())
            strlcpy(_cfg->cfg.bootAspectShunt, doc["boot_aspect_shunt"], sizeof(_cfg->cfg.bootAspectShunt));
        if (doc["tca9548_addr"].is<uint8_t>() || doc["tca9548_addr"].is<uint16_t>())
            _cfg->cfg.tca9548Addr = doc["tca9548_addr"].as<uint8_t>();
        if (doc["tof_i2c_slot"].is<uint8_t>())
            _cfg->cfg.tofI2cSlot = doc["tof_i2c_slot"];

        if (doc["i2c_slots"].is<JsonArray>()) {
            uint8_t si = 0;
            for (JsonObject sl : doc["i2c_slots"].as<JsonArray>()) {
                if (si >= I2C_SLOTS) break;
                _cfg->cfg.i2cSlots[si].mode =
                    I2cBus::parseMode(sl["mode"] | "auto");
                strlcpy(_cfg->cfg.i2cSlots[si].elementId,
                        sl["element_id"] | "", sizeof(_cfg->cfg.i2cSlots[si].elementId));
                si++;
            }
        }

        if (doc["mqtt_broker"].is<const char*>())
            strlcpy(_cfg->cfg.mqttBroker, doc["mqtt_broker"], sizeof(_cfg->cfg.mqttBroker));
        if (doc["mqtt_port"].is<uint16_t>())
            _cfg->cfg.mqttPort = doc["mqtt_port"];
        if (doc["mqtt_user"].is<const char*>())
            strlcpy(_cfg->cfg.mqttUser, doc["mqtt_user"], sizeof(_cfg->cfg.mqttUser));
        if (doc["mqtt_pass"].is<const char*>() && doc["mqtt_pass"].as<String>().length() > 0)
            SecureStore::saveMqttPassword(doc["mqtt_pass"].as<String>());
        if (doc["info_mqtt_topic"].is<const char*>())
            strlcpy(_cfg->cfg.infoMqttTopic, doc["info_mqtt_topic"],
                    sizeof(_cfg->cfg.infoMqttTopic));
        if (doc["info_mqtt_enabled"].is<bool>())
            _cfg->cfg.infoMqttEnabled = doc["info_mqtt_enabled"];

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
            s.usePca = sg["use_pca"] | false;
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

        _cfg->cfg.numDisplays = 0;
        for (JsonObject dp : doc["displays"].as<JsonArray>()) {
            if (_cfg->cfg.numDisplays >= BoardConfig::MAX_DISPLAYS) break;
            DisplayConfig& d = _cfg->cfg.displays[_cfg->cfg.numDisplays++];
            strlcpy(d.id, dp["id"] | "", sizeof(d.id));
            const char* typeStr = dp["type"] | "platform";
            d.type = (strcmp(typeStr, "timetable") == 0)
                ? DisplayType::TIMETABLE : DisplayType::PLATFORM;
            d.i2cSlot = dp["i2c_slot"].isNull()
                ? 255 : static_cast<uint8_t>(dp["i2c_slot"].as<uint8_t>());
            d.i2cAddr = static_cast<uint8_t>(dp["i2c_addr"] | 0x3C);
            d.platformNum = dp["platform_num"] | 1;
            strlcpy(d.stationId, dp["station_id"] | "", sizeof(d.stationId));
            strlcpy(d.stationName, dp["station_name"] | "Stazione H0", sizeof(d.stationName));
            strlcpy(d.destination, dp["destination"] | "", sizeof(d.destination));
            strlcpy(d.departureTime, dp["departure_time"] | "", sizeof(d.departureTime));
            strlcpy(d.status, dp["status"] | "libero", sizeof(d.status));
            d.numRows = 0;
            for (JsonObject row : dp["rows"].as<JsonArray>()) {
                if (d.numRows >= DisplayConfig::MAX_ROWS) break;
                TimetableRowConfig& r = d.rows[d.numRows++];
                strlcpy(r.timeStr, row["time"] | "", sizeof(r.timeStr));
                const char* dest = row["destination"] | row["dest"] | "";
                strlcpy(r.destination, dest, sizeof(r.destination));
                const char* bin = row["platform"] | row["bin"] | "";
                strlcpy(r.platformBin, bin, sizeof(r.platformBin));
                strlcpy(r.status, row["status"] | "", sizeof(r.status));
            }
        }

        _cfg->cfg.numAccessories = 0;
        for (JsonObject ac : doc["accessories"].as<JsonArray>()) {
            if (_cfg->cfg.numAccessories >= BoardConfig::MAX_ACCESSORIES) break;
            AccessoryConfig& a = _cfg->cfg.accessories[_cfg->cfg.numAccessories++];
            strlcpy(a.id, ac["id"] | "", sizeof(a.id));
            a.profile = ConfigManager::parseProfile(ac["profile"] | "generic");
            a.muxCh = ac["mux_ch"] | ac["mux_ch_lights"] | 0;
            a.muxChBar = ac["mux_ch_bar"] | 0;
            a.pulseMs = ac["pulse_ms"] | 300;
        }

        _cfg->cfg.numScenarios = 0;
        for (JsonObject sc : doc["scenarios"].as<JsonArray>()) {
            if (_cfg->cfg.numScenarios >= BoardConfig::MAX_SCENARIOS) break;
            ScenarioConfig& s = _cfg->cfg.scenarios[_cfg->cfg.numScenarios++];
            strlcpy(s.id, sc["id"] | "", sizeof(s.id));
            s.enabled = sc["enabled"] | true;
            s.muxCh = sc["mux_ch"].isNull() ? 255 : static_cast<uint8_t>(sc["mux_ch"].as<uint8_t>());
            s.tofTrigger = sc["tof_trigger"] | false;
            strlcpy(s.rocrailId, sc["rocrail_id"] | "", sizeof(s.rocrailId));
            strlcpy(s.accessoryPl, sc["accessory_pl"] | "", sizeof(s.accessoryPl));
            strlcpy(s.accessoryLight, sc["accessory_light"] | "", sizeof(s.accessoryLight));
            strlcpy(s.accessoryBell, sc["accessory_bell"] | "", sizeof(s.accessoryBell));
            strlcpy(s.displayId, sc["display_id"] | "", sizeof(s.displayId));
            strlcpy(s.statusOcc, sc["status_occupied"] | "in arrivo", sizeof(s.statusOcc));
            strlcpy(s.statusFree, sc["status_free"] | "libero", sizeof(s.statusFree));
            strlcpy(s.destOcc, sc["dest_occupied"] | "", sizeof(s.destOcc));
            strlcpy(s.destFree, sc["dest_free"] | "", sizeof(s.destFree));
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

        for (uint8_t i = 0; i < _cfg->cfg.numAccessories; i++) {
            const AccessoryConfig& a = _cfg->cfg.accessories[i];
            if (a.id[0] == '\0') continue;
            for (uint8_t ch : {a.muxCh, a.muxChBar}) {
                if (ch >= MUX_CHANNELS) continue;
                String key = "ch" + String(ch);
                if (assigned.count(key)) {
                    return String("Accessorio '") + a.id + "' ch" + ch + " in conflitto";
                }
                assigned[key] = String("accessory ") + a.id;
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
        const bool sta = wifiHasStaIp();
        String staIp = sta ? WiFi.localIP().toString() : String(_cfg->cfg.lastStaIp);
        doc["connected"]   = sta;
        doc["ssid"]        = sta ? WiFi.SSID() : _cfg->cfg.wifiSsid;
        doc["sta_ip"]      = staIp;
        doc["last_sta_ip"] = _cfg->cfg.lastStaIp;
        doc["saved_ssid"]  = _cfg->cfg.wifiSsid;
        if (staIp.length())
            doc["web_url"] = String("http://") + staIp + "/";
        doc["ap_ip"]  = WiFi.softAPIP().toString();
        doc["ap_url"] = String("http://") + WiFi.softAPIP().toString() + "/";
        char host[32];
        wifiNormalizeHostname(_cfg->cfg.hostname, host, sizeof(host));
        doc["mdns_host"] = host;
        doc["mdns_url"]  = String("http://") + host + ".local/";
        doc["rssi"]      = WiFi.RSSI();
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

        if (wifiJob.busy() || wifiJob.restartPending()) {
            doc["scanning"] = true;
            doc["busy"]     = true;
            doc["message"]  = wifiJob.restartPending()
                ? "Riavvio scheda in corso…"
                : "Connessione WiFi in corso — riprova tra poco";
            String out;
            serializeJson(doc, out);
            _server.send(409, "application/json", out);
            return;
        }

        const bool wantFresh = _server.hasArg("fresh");

        int n = WiFi.scanComplete();
        if ((n == -1 || wifiJob.phase() == WiFiJobManager::Phase::SCANNING) &&
            !wifiJob.restartPending()) {
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
