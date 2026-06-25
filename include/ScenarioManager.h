#pragma once
#include <Arduino.h>
#include "ConfigManager.h"
#include "LiveStatus.h"
#include "AccessoryManager.h"
#include "DisplayManager.h"
#include "MQTTManager.h"

/**
 * Links occupancy triggers (MUX sensor, ToF, Rocrail block) to accessories
 * and platform displays.
 *
 * Example: train on block → PL activate + lights on + binario "in arrivo".
 */
class ScenarioManager {
public:
    void begin(ConfigManager& cfgMgr) {
        _cfg = &cfgMgr;
        _count = 0;

        for (uint8_t i = 0; i < cfgMgr.cfg.numScenarios && i < BoardConfig::MAX_SCENARIOS; i++) {
            const ScenarioConfig& sc = cfgMgr.cfg.scenarios[i];
            if (sc.id[0] == '\0' || !sc.enabled) continue;

            RuntimeScenario& rt = _rt[_count];
            rt.cfg = sc;
            rt.wasOccupied = false;
            rt.active = false;

            Serial.printf("[SCN] '%s' trigger=", sc.id);
            if (sc.muxCh != 255) Serial.printf("MUX%u ", sc.muxCh);
            if (sc.tofTrigger) Serial.print("ToF ");
            if (sc.rocrailId[0]) Serial.printf("rb:%s ", sc.rocrailId);
            if (sc.accessoryPl[0]) Serial.printf("→PL:%s ", sc.accessoryPl);
            if (sc.displayId[0]) Serial.printf("→disp:%s", sc.displayId);
            Serial.println();
            _count++;
        }
        _primed = false;
        Serial.printf("[SCN] %u scenario(s) active\n", _count);
    }

    void evaluate(const LiveStatus& live, AccessoryManager& acc, DisplayManager& disp,
                  MQTTManager& mqtt) {
        if (_count == 0) return;

        if (!_primed) {
            for (uint8_t i = 0; i < _count; i++)
                _rt[i].wasOccupied = _readTrigger(_rt[i].cfg, live);
            _primed = true;
            return;
        }

        for (uint8_t i = 0; i < _count; i++) {
            RuntimeScenario& rt = _rt[i];
            if (!rt.cfg.enabled) continue;

            const bool occ = _readTrigger(rt.cfg, live);
            if (occ == rt.wasOccupied) continue;

            rt.wasOccupied = occ;
            rt.active = occ;

            Serial.printf("[SCN] '%s' → %s\n", rt.cfg.id, occ ? "OCCUPIED" : "FREE");
            if (occ) _onOccupied(rt.cfg, acc, disp, mqtt);
            else     _onFree(rt.cfg, acc, disp, mqtt);
        }
    }

    void syncLiveStatus(LiveStatus& live) {
        for (uint8_t i = 0; i < _count; i++) {
            const RuntimeScenario& rt = _rt[i];
            ScenarioLive& ls = live.scenarios[String(rt.cfg.id)];
            ls.enabled   = rt.cfg.enabled;
            ls.active    = rt.active;
            ls.triggered = rt.wasOccupied;
            ls.trigger   = _triggerLabel(rt.cfg);
        }
    }

private:
    struct RuntimeScenario {
        ScenarioConfig cfg;
        bool wasOccupied = false;
        bool active      = false;
    };

    ConfigManager*   _cfg   = nullptr;
    RuntimeScenario _rt[BoardConfig::MAX_SCENARIOS];
    uint8_t         _count  = 0;
    bool            _primed = false;

    static String _triggerLabel(const ScenarioConfig& sc) {
        if (sc.muxCh != 255) return String("MUX ") + sc.muxCh;
        if (sc.tofTrigger) return "ToF";
        if (sc.rocrailId[0]) return String("rb:") + sc.rocrailId;
        return "?";
    }

    static bool _readTrigger(const ScenarioConfig& sc, const LiveStatus& live) {
        if (sc.muxCh != 255) {
            auto it = live.sensors.find(sc.muxCh);
            if (it != live.sensors.end()) return it->second.occupied;
            return false;
        }
        if (sc.tofTrigger) {
            return live.tof.present && live.tof.enabled &&
                   live.tof.valid && live.tof.occupied;
        }
        if (sc.rocrailId[0]) {
            for (auto& [ch, s] : live.sensors) {
                if (s.rocrailId.equalsIgnoreCase(sc.rocrailId))
                    return s.occupied;
            }
        }
        return false;
    }

    static void _onOccupied(const ScenarioConfig& sc, AccessoryManager& acc,
                            DisplayManager& disp, MQTTManager& mqtt) {
        if (sc.accessoryPl[0])    acc.handleCommand(sc.accessoryPl, "activate");
        if (sc.accessoryLight[0]) acc.handleCommand(sc.accessoryLight, "on");
        if (sc.accessoryBell[0])  acc.handleCommand(sc.accessoryBell, "ring");
        if (sc.displayId[0]) {
            disp.updatePlatform(sc.displayId, sc.statusOcc,
                                sc.destOcc[0] ? sc.destOcc : nullptr);
            _publishDisplay(mqtt, sc.displayId, sc.statusOcc, sc.destOcc);
        }
        acc.publishState(mqtt);
    }

    static void _onFree(const ScenarioConfig& sc, AccessoryManager& acc,
                        DisplayManager& disp, MQTTManager& mqtt) {
        if (sc.accessoryPl[0])    acc.handleCommand(sc.accessoryPl, "deactivate");
        if (sc.accessoryLight[0]) acc.handleCommand(sc.accessoryLight, "off");
        if (sc.displayId[0]) {
            disp.updatePlatform(sc.displayId, sc.statusFree,
                                sc.destFree);  // may be empty → clears text
            _publishDisplay(mqtt, sc.displayId, sc.statusFree, sc.destFree);
        }
        acc.publishState(mqtt);
    }

    static void _publishDisplay(MQTTManager& mqtt, const char* id,
                                const char* status, const char* dest) {
        if (!mqtt.isEnabled()) return;
        String base = String("display/") + id + "/platform/";
        mqtt.publish((base + "status").c_str(), status, true);
        if (dest && dest[0])
            mqtt.publish((base + "destination").c_str(), dest, true);
    }
};
