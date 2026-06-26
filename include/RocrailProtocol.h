#pragma once
#include <Arduino.h>
#include <map>
#include <memory>
#include <vector>
#include "Config.h"
#include "ChannelObjects.h"
#include "TurnoutChannel.h"
#include "MQTTManager.h"
#include "LiveStatus.h"

#ifdef USE_PCA9685
#include "PCA9685Signal.h"
#endif

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
        SignalEntry& e = _signals[id];
        e.type   = type;
        e.ch     = ch;
#ifdef USE_PCA9685
        e.pca    = nullptr;
#endif
        e.aspect = (type == SignalType::MAIN) ? "red" : "stop";
        _initLiveSignal(id, type, e.aspect);
    }

#ifdef USE_PCA9685
    void registerPcaSignal(const String& id, SignalType type, PCA9685Signal* pca) {
        SignalEntry& e = _signals[id];
        e.type = type;
        e.ch   = nullptr;
        e.pca  = pca;
        e.aspect = (type == SignalType::MAIN) ? "red" : "stop";
        _initLiveSignal(id, type, e.aspect);
    }
#endif

    /** Apply configured boot aspects to all registered signals. */
    void applyBootAspects(const char* bootMain, const char* bootShunt) {
        for (auto& [id, e] : _signals) {
            const char* aspStr = (e.type == SignalType::MAIN) ? bootMain : bootShunt;
            SignalAspect asp = _parseAspect(String(aspStr), e.type);
            _applySignalAspect(e, asp);
            e.aspect = aspStr;
            _syncLiveLamps(id, e);
        }
    }

    void registerTurnout(TurnoutChannel* t) {
        _turnouts[t->rocrailId] = t;
        if (_live) {
            TurnoutLive& tl = _live->turnouts[t->rocrailId];
            tl.position = "straight";
            tl.busy     = false;
        }
    }

    // Map a Rocrail block/sensor ID to a MUX channel index
    void registerSensorRb(const String& rocrailId, uint8_t muxCh) {
        _sensorsRb[muxCh] = rocrailId;
    }

    void registerToFBlock(const String& rocrailId) {
        if (rocrailId.isEmpty()) return;
        for (const auto& id : _tofBlocks) {
            if (id == rocrailId) return;
        }
        _tofBlocks.push_back(rocrailId);
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
        if (it == _sensorsRb.end()) return;
        publishBlockFeedback(it->second, occupied, mqtt);
    }

    void publishToFFeedback(bool occupied, MQTTManager& mqtt) {
        for (const auto& id : _tofBlocks) {
            publishBlockFeedback(id, occupied, mqtt);
        }
    }

    void publishBlockFeedback(const String& rocrailId, bool occupied, MQTTManager& mqtt) {
        if (rocrailId.isEmpty()) return;
        String state = occupied ? "occupied" : "free";
        String xml   = "<fb id=\"" + rocrailId + "\" state=\"" + state + "\"/>";
        mqtt.publishRaw(ROCRAIL_TOPIC_SW_FB, xml, false);
        Serial.printf("[ROCR] Block %s → %s\n", rocrailId.c_str(), state.c_str());
    }

    // ── Turnout pulse watchdog – call every loop() ───────────────────
    void update(MuxDriver& mux) {
        if (_testTurnout) {
            _testTurnout->update(mux);
            if (!_testTurnout->isBusy()) _testTurnout.reset();
        }
        for (auto& [id, t] : _turnouts) {
            t->update(mux);
        }
    }

    void syncLiveStatus() {
        if (!_live) return;
        for (auto& [id, e] : _signals) {
            SignalLive& sl = _live->signals[id];
            sl.type   = static_cast<uint8_t>(e.type);
            sl.aspect = e.aspect;
            if (e.ch) {
                sl.lamps.r = e.ch->r;
                sl.lamps.g = e.ch->g;
                sl.lamps.v = e.ch->b;
#ifdef USE_PCA9685
            } else if (e.pca) {
                sl.lamps.r = e.pca->lampR;
                sl.lamps.g = e.pca->lampG;
                sl.lamps.v = e.pca->lampV;
#endif
            }
        }
        for (auto& [id, t] : _turnouts) {
            TurnoutLive& tl = _live->turnouts[id];
            tl.position = t->stateStr();
            tl.busy     = t->isBusy();
        }
    }

    bool hasTopic(const String& topic) const {
        return topic == ROCRAIL_TOPIC_SG_CMD || topic == ROCRAIL_TOPIC_SW_CMD;
    }

#ifdef USE_PCA9685
    void setPcaDriver(Adafruit_PWMServoDriver* pca) { _pca = pca; }
#endif

    /** Test segnale da UI: id salvato oppure chR/chG/chV dal form. */
    bool testSignal(JsonObject doc, MuxDriver& mux, String& err) {
        String aspect = doc["cmd"] | doc["aspect"] | "";
        if (aspect.isEmpty()) {
            err = "Aspect mancante (es. red, green, yellow, stop, go)";
            return false;
        }

        String id = doc["id"] | "";
        auto it = id.length() ? _signals.find(id) : _signals.end();
        if (it != _signals.end()) {
            SignalEntry& e = it->second;
            SignalAspect asp = _parseAspect(aspect, e.type);
            _applySignalAspect(e, asp);
            _flushSignalOutput(e, mux);
            e.aspect = aspect;
            _syncLiveLamps(id, e);
            return true;
        }

        const bool usePca = doc["use_pca"] | false;
        const uint8_t typeVal = doc["type"] | 0;
        const SignalType sigType = (typeVal == 1) ? SignalType::SHUNT : SignalType::MAIN;
        const uint8_t chR = doc["chR"] | 0;
        const uint8_t chG = doc["chG"] | 1;
        const uint8_t chV = doc["chV"] | 2;
        const SignalAspect asp = _parseAspect(aspect, sigType);

#ifdef USE_PCA9685
        if (usePca) {
            if (!_pca) {
                err = "PCA9685 non disponibile su questo hardware";
                return false;
            }
            PCA9685Signal ps("_test", sigType, _pca, chR, chG, chV);
            ps.setAspect(asp);
            if (id.length() && _live) {
                SignalLive& sl = _live->signals[id];
                sl.type   = static_cast<uint8_t>(sigType);
                sl.aspect = aspect;
                sl.lamps.r = ps.lampR;
                sl.lamps.g = ps.lampG;
                sl.lamps.v = ps.lampV;
            }
            return true;
        }
#endif

        if (!muxCanDriveDigital()) {
            err = "Uscita MUX su GPIO34 (solo ingresso) — segnali non pilotabili su questo ESP32";
            return false;
        }
        if (!doc["chR"].is<int>() && id.length()) {
            err = "Segnale non attivo — salva e riavvia, oppure indica chR/chG/chV";
            return false;
        }

        SignalRGBChannel ch(chR, chG, chV);
        SignalEntry tmp{};
        tmp.type = sigType;
        tmp.ch   = &ch;
        _applySignalAspect(tmp, asp);
        ch.update(mux);
        if (id.length() && _live) {
            SignalLive& sl = _live->signals[id];
            sl.type   = static_cast<uint8_t>(sigType);
            sl.aspect = aspect;
            sl.lamps.r = ch.r;
            sl.lamps.g = ch.g;
            sl.lamps.v = ch.b;
        }
        return true;
    }

    /** Test scambio da UI: id salvato oppure chS/chD dal form. */
    bool testTurnout(JsonObject doc, MuxDriver& mux, String& err) {
        String cmd = doc["cmd"] | "";
        if (cmd.isEmpty()) {
            err = "Comando mancante (straight o turnout)";
            return false;
        }
        if (cmd != "straight" && cmd != "turnout") {
            err = "Comando scambio non valido";
            return false;
        }
        if (!muxCanDriveDigital()) {
            err = "Uscita MUX su GPIO34 (solo ingresso) — scambi non pilotabili su questo ESP32";
            return false;
        }

        String id = doc["id"] | "";
        auto it = id.length() ? _turnouts.find(id) : _turnouts.end();
        if (it != _turnouts.end()) {
            TurnoutChannel* t = it->second;
            if (t->isBusy()) {
                err = "Scambio in movimento — attendi fine impulso";
                return false;
            }
            if (cmd == "straight") t->commandStraight(mux);
            else t->commandDiverge(mux);
            return true;
        }

        if (!doc["chS"].is<int>() && id.length()) {
            err = "Scambio non attivo — salva e riavvia, oppure indica chS/chD";
            return false;
        }

        const uint8_t chS = doc["chS"] | 0;
        const uint8_t chD = doc["chD"] | 1;
        const uint32_t pulse = doc["pulse"] | 300;

        _testTurnout = std::make_unique<TurnoutChannel>(
            id.length() ? id : "_test", chS, chD, pulse);
        _testTurnout->begin(mux);
        if (cmd == "straight") _testTurnout->commandStraight(mux);
        else _testTurnout->commandDiverge(mux);
        return true;
    }

private:
    struct SignalEntry {
        SignalType        type;
        SignalRGBChannel* ch;
#ifdef USE_PCA9685
        PCA9685Signal*    pca = nullptr;
#endif
        String            aspect;
    };

    std::map<String, SignalEntry>     _signals;
    std::map<String, TurnoutChannel*> _turnouts;
    std::map<uint8_t, String>         _sensorsRb;
    std::vector<String>               _tofBlocks;
    LiveStatus*                       _live = nullptr;
#ifdef USE_PCA9685
    Adafruit_PWMServoDriver*          _pca  = nullptr;
#endif
    std::unique_ptr<TurnoutChannel>   _testTurnout;

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

        entry.aspect = aspect;
        _syncLiveLamps(id, entry);
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
#ifdef USE_PCA9685
        if (e.pca) {
            e.pca->setAspect(asp);
            return;
        }
#endif
        if (!e.ch) return;
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

    void _flushSignalOutput(SignalEntry& e, MuxDriver& mux) {
#ifdef USE_PCA9685
        if (e.pca) return;
#endif
        if (e.ch) e.ch->update(mux);
    }

    void _syncLiveLamps(const String& id, SignalEntry& e) {
        if (!_live) return;
        SignalLive& sl = _live->signals[id];
        sl.type   = static_cast<uint8_t>(e.type);
        sl.aspect = e.aspect;
        if (e.ch) {
            sl.lamps.r = e.ch->r;
            sl.lamps.g = e.ch->g;
            sl.lamps.v = e.ch->b;
#ifdef USE_PCA9685
        } else if (e.pca) {
            sl.lamps.r = e.pca->lampR;
            sl.lamps.g = e.pca->lampG;
            sl.lamps.v = e.pca->lampV;
#endif
        }
    }

    void _initLiveSignal(const String& id, SignalType type, const String& aspect) {
        if (!_live) return;
        SignalLive& sl = _live->signals[id];
        sl.type   = static_cast<uint8_t>(type);
        sl.aspect = aspect;
        sl.lamps  = {};
    }
};
