#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <map>
#include <memory>
#include <vector>
#include "ConfigManager.h"
#include "ChannelObjects.h"
#include "LiveStatus.h"
#include "MuxDriver.h"
#include "MQTTManager.h"

/**
 * Named relay accessories with behaviour profiles.
 * MQTT: { "accessory": "luci_sala", "action": "on" }
 */
class AccessoryManager {
public:
    void begin(ConfigManager& cfgMgr, MuxDriver& mux,
               std::map<uint8_t, RelayChannel*>& relays,
               std::vector<std::unique_ptr<IChannel>>& channels) {
        _cfg     = &cfgMgr;
        _mux     = &mux;
        _relays  = &relays;
        _channels = &channels;
        _count   = 0;

        for (uint8_t i = 0; i < cfgMgr.cfg.numAccessories && i < BoardConfig::MAX_ACCESSORIES; i++) {
            const AccessoryConfig& ac = cfgMgr.cfg.accessories[i];
            if (ac.id[0] == '\0') continue;

            RuntimeAccessory& rt = _rt[_count];
            rt.cfg = ac;
            rt.active = false;
            rt.barDown = false;
            rt.lightsOn = false;
            rt.blinkPhase = false;
            rt.pulsing = false;

            const bool hasPrimary = ac.muxCh < MUX_CHANNELS && ac.muxCh != MUX_CH_UNUSED;
            const bool hasBar = ac.profile == AccessoryProfile::LEVEL_XING &&
                                ac.muxChBar < MUX_CHANNELS && ac.muxChBar != MUX_CH_UNUSED;

            if (!hasPrimary && !hasBar) {
                Serial.printf("[ACC] '%s' salvato (nessun canale MUX)\n", ac.id);
                _count++;
                continue;
            }

            if (hasPrimary && !_ensureRelay(ac.muxCh)) {
                Serial.printf("[ACC] Skip '%s' – MUX ch%u unavailable\n", ac.id, ac.muxCh);
                continue;
            }
            if (hasBar && !_ensureRelay(ac.muxChBar)) {
                Serial.printf("[ACC] Skip PL '%s' – bar ch%u unavailable\n", ac.id, ac.muxChBar);
                continue;
            }

            Serial.printf("[ACC] '%s' profile=%s ch=%u%s pulse=%ums\n",
                          ac.id, ConfigManager::profileName(ac.profile), ac.muxCh,
                          ac.profile == AccessoryProfile::LEVEL_XING
                              ? (String(" bar=") + ac.muxChBar).c_str() : "",
                          ac.pulseMs);
            _count++;
        }
        Serial.printf("[ACC] %u accessory(ies) active\n", _count);
    }

    void loop() {
        if (_count == 0) return;
        unsigned long now = millis();

        for (uint8_t i = 0; i < _count; i++) {
            RuntimeAccessory& rt = _rt[i];

            if (rt.pulsing && now >= rt.pulseEndMs) {
                rt.pulsing = false;
                _setRelay(rt.cfg.muxCh, false);
                rt.active = false;
            }

            if (rt.cfg.profile == AccessoryProfile::LEVEL_XING && rt.active) {
                uint16_t half = rt.cfg.pulseMs ? rt.cfg.pulseMs : 300;
                if (now - rt.lastBlinkMs >= half) {
                    rt.lastBlinkMs = now;
                    rt.blinkPhase = !rt.blinkPhase;
                    _setRelay(rt.cfg.muxCh, rt.blinkPhase);
                    rt.lightsOn = rt.blinkPhase;
                }
            }
        }
    }

    /** Test da UI: usa accessorio salvato oppure mux_ch/profile dal form (senza riavvio). */
    bool testFromJson(JsonObject doc, String& err) {
        String id  = doc["id"] | "";
        String cmd = doc["cmd"] | doc["action"] | "";
        if (cmd.isEmpty()) {
            err = "Comando mancante";
            return false;
        }
        if (!muxCanDriveDigital()) {
            err = "Uscita MUX su GPIO34 (solo ingresso) — relè non pilotabili su questo ESP32";
            return false;
        }

        if (id.length() && handleCommand(id, cmd))
            return true;

        if (!doc["mux_ch"].is<int>() && !doc["mux_ch_lights"].is<int>()) {
            err = id.length()
                ? "Accessorio non attivo — salva e riavvia, oppure indica il canale MUX"
                : "Indica ID e canale MUX";
            return false;
        }

        AccessoryConfig tmp = {};
        const char* prof = doc["profile"] | "generic";
        tmp.profile = ConfigManager::parseProfile(prof);
        tmp.pulseMs = doc["pulse_ms"] | 300;
        if (tmp.profile == AccessoryProfile::LEVEL_XING) {
            tmp.muxCh    = doc["mux_ch_lights"] | doc["mux_ch"] | 0;
            tmp.muxChBar = doc["mux_ch_bar"] | 0;
        } else {
            tmp.muxCh = doc["mux_ch"] | 0;
        }

        if (tmp.muxCh >= MUX_CHANNELS) {
            err = "Canale MUX non valido";
            return false;
        }
        if (!_ensureRelay(tmp.muxCh)) {
            err = "Canale MUX occupato da altro ruolo";
            return false;
        }
        if (tmp.profile == AccessoryProfile::LEVEL_XING &&
            tmp.muxChBar < MUX_CHANNELS && !_ensureRelay(tmp.muxChBar)) {
            err = "Canale sbarra MUX non disponibile";
            return false;
        }

        RuntimeAccessory rt = {};
        rt.cfg = tmp;
        return _runProfileCommand(rt, cmd);
    }

    bool handleCommand(const String& id, const String& action) {
        int idx = _findById(id);
        if (idx < 0) return false;

        RuntimeAccessory& rt = _rt[idx];
        return _runProfileCommand(rt, action);
    }

    bool handleMqttJson(JsonDocument& doc) {
        if (!doc["accessory"].is<const char*>()) return false;
        String id     = doc["accessory"].as<String>();
        String action = doc["action"] | "toggle";
        return handleCommand(id, action);
    }

    void syncLiveStatus(LiveStatus& live) {
        for (uint8_t i = 0; i < _count; i++) {
            const RuntimeAccessory& rt = _rt[i];
            AccessoryLive& ls = live.accessories[String(rt.cfg.id)];
            ls.profile  = ConfigManager::profileName(rt.cfg.profile);
            ls.active   = rt.active || rt.pulsing;
            ls.lights   = rt.lightsOn;
            ls.bar      = rt.barDown;
            ls.muxCh    = rt.cfg.muxCh;
        }
    }

    void appendStatus(JsonObject parent) const {
        JsonArray arr = parent["accessories"].to<JsonArray>();
        for (uint8_t i = 0; i < _count; i++) {
            const RuntimeAccessory& rt = _rt[i];
            JsonObject o = arr.add<JsonObject>();
            o["id"]      = rt.cfg.id;
            o["profile"] = ConfigManager::profileName(rt.cfg.profile);
            o["active"]  = rt.active || rt.pulsing;
            o["lights"]  = rt.lightsOn;
            o["bar"]     = rt.barDown;
            o["mux_ch"]  = rt.cfg.muxCh;
            if (rt.cfg.profile == AccessoryProfile::LEVEL_XING)
                o["mux_ch_bar"] = rt.cfg.muxChBar;
        }
    }

    void publishState(MQTTManager& mqtt) const {
        if (_count == 0) return;
        JsonDocument doc;
        appendStatus(doc.as<JsonObject>());
        String payload;
        serializeJson(doc["accessories"], payload);
        mqtt.publish("accessories/state", payload, true);
    }

private:
    struct RuntimeAccessory {
        AccessoryConfig cfg;
        bool     active     = false;
        bool     barDown    = false;
        bool     lightsOn   = false;
        bool     blinkPhase = false;
        bool     pulsing    = false;
        unsigned long lastBlinkMs = 0;
        unsigned long pulseEndMs  = 0;
    };

    ConfigManager* _cfg      = nullptr;
    MuxDriver*     _mux      = nullptr;
    std::map<uint8_t, RelayChannel*>* _relays = nullptr;
    std::vector<std::unique_ptr<IChannel>>* _channels = nullptr;
    RuntimeAccessory _rt[BoardConfig::MAX_ACCESSORIES];
    uint8_t          _count = 0;
    bool             _muxReady = false;

    bool _runProfileCommand(RuntimeAccessory& rt, String action) {
        String act = action;
        act.toLowerCase();
        const String id = String(rt.cfg.id);

        if (act == "toggle") {
            if (id.length())
                return handleCommand(id, rt.active ? "off" : "on");
            return _runProfileCommand(rt, rt.active ? "off" : "on");
        }

        switch (rt.cfg.profile) {
            case AccessoryProfile::BELL:
                if (act == "ring" || act == "on" || act == "trigger") {
                    _pulseRelay(rt, rt.cfg.pulseMs ? rt.cfg.pulseMs : 400);
                    return true;
                }
                if (act == "off") {
                    rt.pulsing = false;
                    _setRelay(rt.cfg.muxCh, false);
                    rt.active = false;
                    return true;
                }
                break;

            case AccessoryProfile::LEVEL_XING:
                if (act == "on" || act == "activate" || act == "close") {
                    rt.active = true;
                    rt.barDown = true;
                    rt.lastBlinkMs = millis();
                    rt.blinkPhase = true;
                    _setRelay(rt.cfg.muxCh, true);
                    rt.lightsOn = true;
                    if (rt.cfg.muxChBar < MUX_CHANNELS)
                        _setRelay(rt.cfg.muxChBar, true);
                    return true;
                }
                if (act == "off" || act == "deactivate" || act == "open") {
                    rt.active = false;
                    rt.barDown = false;
                    rt.lightsOn = false;
                    _setRelay(rt.cfg.muxCh, false);
                    if (rt.cfg.muxChBar < MUX_CHANNELS)
                        _setRelay(rt.cfg.muxChBar, false);
                    return true;
                }
                break;

            default:
                if (act == "on") {
                    _setRelay(rt.cfg.muxCh, true);
                    rt.active = true;
                    return true;
                }
                if (act == "off") {
                    _setRelay(rt.cfg.muxCh, false);
                    rt.active = false;
                    return true;
                }
                break;
        }
        return false;
    }

    void _ensureMuxReady() {
        if (_muxReady || !_mux) return;
        _mux->begin();
        _muxReady = true;
    }

    int _findById(const String& id) const {
        for (uint8_t i = 0; i < _count; i++)
            if (id.equalsIgnoreCase(_rt[i].cfg.id)) return i;
        return -1;
    }

    bool _ensureRelay(uint8_t ch) {
        if (ch >= MUX_CHANNELS) return false;
        if (_relays->count(ch)) return true;

        ChannelRole role = _cfg->cfg.pinMap[ch];
        if (role != ChannelRole::UNUSED && role != ChannelRole::RELAY) {
            Serial.printf("[ACC] MUX ch%u busy (role %u)\n", ch, static_cast<uint8_t>(role));
            return false;
        }

        auto* r = new RelayChannel(ch);
        (*_relays)[ch] = r;
        _channels->emplace_back(r);
        _cfg->cfg.pinMap[ch] = ChannelRole::RELAY;
        Serial.printf("[ACC] Auto relay on MUX ch%u\n", ch);
        return true;
    }

    void _setRelay(uint8_t ch, bool on) {
        _ensureMuxReady();
        auto it = _relays->find(ch);
        if (it != _relays->end())
            it->second->setState(on, *_mux);
    }

    void _pulseRelay(RuntimeAccessory& rt, uint16_t ms) {
        _setRelay(rt.cfg.muxCh, true);
        rt.active = true;
        rt.pulsing = true;
        rt.pulseEndMs = millis() + ms;
    }
};
