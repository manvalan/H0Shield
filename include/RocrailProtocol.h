#pragma once
#include <Arduino.h>
#include <map>
#include <vector>
#include "Config.h"
#include "ChannelObjects.h"
#include "TurnoutChannel.h"
#include "MQTTManager.h"
#include "WebConfig.h"

/**
 * Rocrail MQTT protocol adapter.
 *
 * Subscribes to:
 *   rocrail/service/info/sg      → signal aspect commands  (<sg .../>)
 *   rocrail/service/command      → turnout commands        (<sw .../>)
 *
 * Publishes feedback to:
 *   rocrail/service/client       → signal feedback
 *   rocrail/service/info         → turnout feedback
 *   railway/status/segnali (LWT) → signal board status
 *   railway/status/scambi  (LWT) → turnout board status
 *
 * XML format (identical to SignalBoard / TurnoutBoard):
 *   <sg id="sg1" cmd="aspect" aspect="green"/>
 *   <sw id="sw1" cmd="straight" addr1="1"/>
 */
class RocrailProtocol {
public:
    // ── Registration ─────────────────────────────────────────────────
    void registerSignal(const String& id, SignalType type, SignalRGBChannel* ch) {
        _signals[id] = { type, ch };
    }

    void registerTurnout(TurnoutChannel* t) {
        _turnouts[t->rocrailId] = t;
    }

    // Map a Rocrail block/sensor ID to a MUX channel index
    void registerSensorRb(const String& rocrailId, uint8_t muxCh) {
        _sensorsRb[muxCh] = rocrailId;
    }

    void setLiveStatus(LiveStatus* live) { _live = live; }

    // ── Call once after MQTT is connected ────────────────────────────
    void publishOnline(MQTTManager& mqtt, const String& boardName) {
        String sMsg = "{\"module\":\"" + boardName + "\",\"status\":\"online\"}";
        mqtt.publishRaw(ROCRAIL_TOPIC_SG_LWT,  sMsg, true);
        mqtt.publishRaw(ROCRAIL_TOPIC_SW_LWT,  sMsg, true);
    }

    // ── Main dispatcher – call from MQTTManager's command handler ────
    void handle(const String& topic, const String& payload,
                MQTTManager& mqtt, MuxDriver& mux) {

        if (topic == ROCRAIL_TOPIC_SG_CMD) {
            _handleSignal(payload, mqtt);
        } else if (topic == ROCRAIL_TOPIC_SW_CMD) {
            _handleTurnout(payload, mqtt, mux);
        }
    }

    // ── Publish occupancy feedback for a sensor channel ─────────────
    // Call from main.cpp whenever a sensor state changes.
    // Topic: rocrail/service/info  XML: <fb id="bk1" state="free|occupied"/>
    void publishSensorFeedback(uint8_t muxCh, bool occupied, MQTTManager& mqtt) {
        auto it = _sensorsRb.find(muxCh);
        if (it == _sensorsRb.end()) return;   // not mapped to Rocrail

        const String& id  = it->second;
        String state      = occupied ? "occupied" : "free";
        String xml        = "<fb id=\"" + id + "\" state=\"" + state + "\"/>";
        mqtt.publishRaw(ROCRAIL_TOPIC_SW_FB, xml, false);
        Serial.printf("[ROCR] Sensor %s (ch%u) → %s\n",
                      id.c_str(), muxCh, state.c_str());
    }

    // ── Turnout pulse watchdog – call every loop() ───────────────────
    void update(MuxDriver& mux) {
        for (auto& [id, t] : _turnouts) {
            t->update(mux);
        }
    }

    bool hasTopic(const String& topic) const {
        return topic == ROCRAIL_TOPIC_SG_CMD || topic == ROCRAIL_TOPIC_SW_CMD;
    }

private:
    struct SignalEntry {
        SignalType       type;
        SignalRGBChannel* ch;
    };

    std::map<String, SignalEntry>     _signals;
    std::map<String, TurnoutChannel*> _turnouts;
    std::map<uint8_t, String>         _sensorsRb;
    LiveStatus*                       _live = nullptr;

    // ── XML helpers ───────────────────────────────────────────────────
    static String _attr(const String& xml, const String& name) {
        String search = " " + name + "=\"";
        int start = xml.indexOf(search);
        if (start < 0) return "";
        start += search.length();
        int end = xml.indexOf('"', start);
        return (end < 0) ? "" : xml.substring(start, end);
    }

    // ── Signal handler ────────────────────────────────────────────────
    void _handleSignal(const String& xml, MQTTManager& mqtt) {
        if (xml.indexOf("<sg") < 0) return;

        String id     = _attr(xml, "id");
        String cmd    = _attr(xml, "cmd");
        String aspect = _attr(xml, "aspect");

        if (id.isEmpty() || cmd != "aspect") return;

        auto it = _signals.find(id);
        if (it == _signals.end()) {
            Serial.printf("[ROCR] Unknown signal id: %s\n", id.c_str());
            return;
        }

        SignalEntry& entry = it->second;
        SignalAspect asp   = _parseAspect(aspect, entry.type);

        _applySignalAspect(entry, asp);

        // Feedback to Rocrail
        String fb = "<sg id=\"" + id + "\" cmd=\"aspect\" aspect=\"" + aspect + "\"/>";
        mqtt.publishRaw(ROCRAIL_TOPIC_SG_FB, fb, false);

        Serial.printf("[ROCR] Signal %s → %s\n", id.c_str(), aspect.c_str());

        if (_live) _live->signalStates[id] = aspect;
    }

    // ── Turnout handler ───────────────────────────────────────────────
    void _handleTurnout(const String& xml, MQTTManager& mqtt, MuxDriver& mux) {
        if (xml.indexOf("<sw") < 0) return;

        String id    = _attr(xml, "id");
        String cmd   = _attr(xml, "cmd");
        String addr1 = _attr(xml, "addr1");

        if (id.isEmpty()) return;

        auto it = _turnouts.find(id);
        if (it == _turnouts.end()) {
            Serial.printf("[ROCR] Unknown turnout id: %s\n", id.c_str());
            return;
        }

        TurnoutChannel* t = it->second;
        if (t->isBusy()) {
            Serial.printf("[ROCR] Turnout %s busy, command ignored\n", id.c_str());
            return;
        }

        if (cmd == "straight") {
            t->commandStraight(mux);
        } else if (cmd == "turnout") {
            t->commandDiverge(mux);
        } else {
            return;
        }

        // Feedback
        String fb = "<fb id=\"" + id + "\" addr1=\"" + addr1 + "\" cmd=\"" + cmd + "\"/>";
        mqtt.publishRaw(ROCRAIL_TOPIC_SW_FB, fb, false);

        Serial.printf("[ROCR] Turnout %s → %s\n", id.c_str(), cmd.c_str());
    }

    // ── Aspect parsing ────────────────────────────────────────────────
    static SignalAspect _parseAspect(const String& s, SignalType type) {
        if (type == SignalType::MAIN) {
            if (s == "green")  return SignalAspect::GREEN;
            if (s == "yellow") return SignalAspect::YELLOW;
            return SignalAspect::RED;
        } else {
            if (s == "go"  || s == "green")   return SignalAspect::GO;
            if (s == "oblique")               return SignalAspect::OBLIQUE;
            return SignalAspect::STOP;
        }
    }

    // ── Drive MUX-based RGB signal (3 independent channels: chR, chG, chV) ─
    static void _applySignalAspect(SignalEntry& e, SignalAspect asp) {
        bool r = false, g = false, b = false;

        if (e.type == SignalType::MAIN) {
            switch (asp) {
                case SignalAspect::RED:    r = true;             break;
                case SignalAspect::GREEN:             b = true;  break;  // green LED on "blue" pin
                case SignalAspect::YELLOW: r = true; g = true;   break;
                default: r = true; break;
            }
        } else {  // SHUNT: pins map to: R=A, G=B(middle), V=C
            switch (asp) {
                case SignalAspect::STOP:    g = true; b = true;  break;  // B+C horizontal
                case SignalAspect::GO:      r = true; g = true;  break;  // A+B vertical
                case SignalAspect::OBLIQUE: r = true; b = true;  break;  // A+C diagonal
                default: g = true; b = true; break;
            }
        }

        // MuxDriver is not accessible here; we store the target and let
        // SignalRGBChannel::update() apply it in the next scan cycle.
        e.ch->r = r;
        e.ch->g = g;
        e.ch->b = b;
    }
};
