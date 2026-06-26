#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <Wire.h>
#include <VL6180X.h>

#include "Config.h"
#include "ConfigManager.h"
#include "MuxDriver.h"
#include "ChannelObjects.h"
#include "MQTTManager.h"
#include "WebConfig.h"
#include "OTAManager.h"
#include "ToFManager.h"
#include "TurnoutChannel.h"
#include "RocrailProtocol.h"
#include "HardwareProbe.h"
#include "I2cBus.h"
#include "WifiSetup.h"
#include "LiveStatus.h"
#include "DisplayManager.h"
#include "AccessoryManager.h"
#include "ScenarioManager.h"

#ifdef USE_PCA9685
#include "PCA9685Signal.h"
#include <Adafruit_PWMServoDriver.h>
#endif

// ── Globals ──────────────────────────────────────────────────────────
ConfigManager cfgMgr;
MuxDriver     mux;
MQTTManager   mqtt;
WebConfig     webConfig;
OTAManager    ota;
VL6180X       tof;
ToFManager    tofMgr;
DisplayManager displayMgr;
AccessoryManager accessoryMgr;
ScenarioManager  scenarioMgr;
I2cBus           i2cBus;
bool          tofPresent = false;

#ifdef USE_PCA9685
Adafruit_PWMServoDriver pca9685(0x40);
std::vector<std::unique_ptr<PCA9685Signal>> pcaSignals;
bool          pcaPresent = false;
#endif

// Channel pools
std::vector<std::unique_ptr<IChannel>> channels;
std::map<uint8_t, RelayChannel*>       relays;
std::map<uint8_t, SensorChannel*>      sensors;
std::map<uint8_t, SerialRGBChannel*>   strips;
std::map<uint8_t, SignalRGBChannel*>   signalChannels;
std::map<uint8_t, MarmottaChannel*>    marmottas;

// Rocrail objects (signals + turnouts)
RocrailProtocol rocrail;
std::vector<std::unique_ptr<TurnoutChannel>> turnouts;

// Live status shared with the web server
LiveStatus liveStatus;

// Scan timing
static unsigned long lastScanMs   = 0;
// Track previous sensor states to publish only on change
static bool prevSensorState[MUX_CHANNELS] = {};

// ── Forward declarations ─────────────────────────────────────────────
void buildChannelObjects();
void buildRocrailObjects();
void onMqttCommand(const String& topic, const String& payload);
void publishSensorStates();
void checkResetButton();
void ensureMdns(ConfigManager& cfgMgr);
SensorChannel* ensureSensorChannel(uint8_t ch);
uint8_t countConfiguredMuxChannels();

// ─────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    Serial.println("\n\n==== ShieldH0 booting ====");

    // ── Reset button ──────────────────────────────────────────────────
    pinMode(WIFI_RESET_PIN, INPUT_PULLUP);

    // ── Phase 1: Config + Filesystem ─────────────────────────────────
    if (!cfgMgr.begin()) {
        Serial.println("[BOOT] Config load failed – using defaults");
    }
    {
        String mp = SecureStore::loadMqttPassword();
        if (mp.length()) strlcpy(cfgMgr.cfg.mqttPass, mp.c_str(), sizeof(cfgMgr.cfg.mqttPass));
    }

    // ── Phase 1: WiFi ─────────────────────────────────────────────────
    if (digitalRead(WIFI_RESET_PIN) == LOW) {
        Serial.println("[WIFI] Reset button – cancello credenziali");
        SecureStore::clearWifi();
        cfgMgr.cfg.wifiSsid[0] = '\0';
        cfgMgr.cfg.lastStaIp[0] = '\0';
        cfgMgr.save();
    }
    setupWiFi(cfgMgr);

    // Web subito — accessibile via AP 192.168.4.1 mentre il resto inizializza
    webConfig.begin(cfgMgr, &liveStatus, [](const String& type, const String& id,
                                             const String& cmd, const String& extra) {
        if (type == "signal") {
            String xml = "<sg id=\"" + id + "\" cmd=\"aspect\" aspect=\"" + cmd + "\"/>";
            rocrail.handle(ROCRAIL_TOPIC_SG_CMD, xml, mqtt, mux);
        } else if (type == "turnout") {
            String xml = "<sw id=\"" + id + "\" cmd=\"" + cmd + "\" addr1=\"0\"/>";
            rocrail.handle(ROCRAIL_TOPIC_SW_CMD, xml, mqtt, mux);
        } else if (type == "accessory") {
            accessoryMgr.handleCommand(id, cmd);
            accessoryMgr.publishState(mqtt);
        }
    }, &displayMgr, &i2cBus);

    ensureMdns(cfgMgr);

    // ── Phase 2: Hardware init (all optional except WiFi/web) ─────────
    HardwareProbe hw;
    i2cBus.begin(cfgMgr.cfg.tca9548Addr);
    hw.scanI2C(i2cBus);

    uint8_t tofSlot = cfgMgr.cfg.tofI2cSlot;
    if (tofSlot >= I2C_SLOTS) tofSlot = 0;
    bool tofOnSlot = i2cBus.deviceAt(VL6180X_ADDR, tofSlot);
    if (!tofOnSlot && hw.vl6180x) tofOnSlot = true;

    if (tofOnSlot && cfgMgr.cfg.tofEnabled) {
        i2cBus.selectSlot(tofSlot);
        tof.init();
        tof.configureDefault();
        tof.setTimeout(500);
        tofPresent = true;
        Serial.printf("[HW] VL6180X on slot U%u\n", tofSlot + 9);
    }
    if (hw.pca9685) {
#ifdef USE_PCA9685
        pca9685.begin();
        pca9685.setPWMFreq(200);
        pcaPresent = true;
        Serial.println("[HW] PCA9685 initialized @ 0x40");
#else
        Serial.println("[HW] PCA9685 detected (build with -e wemos_d1_mini32_pca to use)");
#endif
    }
    if (hw.oled3c || hw.oled3d) {
        Serial.printf("[HW] OLED detected @ %s%s\n",
                      hw.oled3c ? "0x3C " : "",
                      hw.oled3d ? "0x3D" : "");
    }

    displayMgr.begin(cfgMgr, hw, i2cBus);

    const uint8_t muxUsed = countConfiguredMuxChannels();
    if (muxUsed > 0) {
        mux.begin();
        Serial.printf("[HW] MUX active – %u channel(s) configured\n", muxUsed);
    } else {
        Serial.println("[HW] MUX idle – no channels configured yet");
    }

    // ── Phase 3: Channel objects ──────────────────────────────────────
    buildChannelObjects();

    // ── Phase 3b: Rocrail signal/turnout objects ──────────────────────
    buildRocrailObjects();
    rocrail.setLiveStatus(&liveStatus);

    accessoryMgr.begin(cfgMgr, mux, relays, channels);
    scenarioMgr.begin(cfgMgr);

    // ── Phase 4: MQTT ────────────────────────────────────────────────
    mqtt.begin(cfgMgr, onMqttCommand, [&]() {
        // Subscribe to Rocrail topics every time MQTT (re)connects
        mqtt.subscribe(ROCRAIL_TOPIC_SG_CMD);
        mqtt.subscribe(ROCRAIL_TOPIC_SW_CMD);
        displayMgr.subscribeAll(mqtt);
        Serial.printf("[MQTT] Subscribed to Rocrail topics\n");
        rocrail.publishOnline(mqtt, cfgMgr.cfg.hostname);
    });

    // ToF manager (only if sensor present)
    if (tofPresent) {
        tofMgr.begin(tof, mqtt, cfgMgr, &rocrail, &liveStatus, true);
    } else {
        liveStatus.tof.present = false;
    }

    // ── OTA ───────────────────────────────────────────────────────────
    ota.begin(cfgMgr);

    Serial.println("==== Boot complete ====");
    if (wifiHasStaIp())
        Serial.printf("==== Rete di casa: http://%s/ ====\n", WiFi.localIP().toString().c_str());
    else if (wifiApRunning())
        Serial.println("==== Setup AP: http://192.168.4.1/ ====");
    Serial.printf("[SUM] MUX:%u ch | I2C:%s%s | MQTT:%s | Web:OK\n",
                  muxUsed,
                  tofPresent ? "VL6180X" : "",
#ifdef USE_PCA9685
                  pcaPresent ? "+PCA9685" : "",
#else
                  "",
#endif
                  mqtt.isEnabled() ? "on" : "off (configura broker)");
    Serial.println();
}

// ─────────────────────────────────────────────────────────────────────
void loop() {
    checkResetButton();
    wifiBootLoop(cfgMgr);
    wifiJob.loop();
    ensureMdns(cfgMgr);
    if (wifiHasStaIp()) wifiSaveLastIp(cfgMgr);
    ota.loop();
    webConfig.loop();
    mqtt.loop();
    if (tofPresent) tofMgr.loop();
    displayMgr.loop();

    if (millis() - lastScanMs >= SENSOR_POLL_MS) {
        lastScanMs = millis();
        if (!channels.empty()) {
            for (auto& ch : channels) {
                ch->update(mux);
            }
            publishSensorStates();
        }
    }

    // Turnout pulse watchdog (cut coil power after pulseDurationMs)
    rocrail.update(mux);
    rocrail.syncLiveStatus();
    accessoryMgr.loop();
    accessoryMgr.syncLiveStatus(liveStatus);

    liveStatus.mqttConnected = mqtt.connected();
    liveStatus.rssi          = (int8_t)WiFi.RSSI();
    for (auto& [ch, s] : sensors) {
        SensorLive& ls = liveStatus.sensors[ch];
        ls.occupied  = s->occupied;
        ls.raw       = s->rawValue;
        ls.threshold = s->threshold;
        ls.rocrailId = "";
        for (uint8_t i = 0; i < cfgMgr.cfg.numSensorsRb; i++) {
            if (cfgMgr.cfg.sensorsRb[i].muxCh == ch) {
                ls.rocrailId = cfgMgr.cfg.sensorsRb[i].rocrailId;
                break;
            }
        }
    }

    scenarioMgr.evaluate(liveStatus, accessoryMgr, displayMgr, mqtt);
    scenarioMgr.syncLiveStatus(liveStatus);
}

// ── Phase 3b: build Rocrail signal and turnout objects ───────────────
void buildRocrailObjects() {
    turnouts.clear();
#ifdef USE_PCA9685
    pcaSignals.clear();
#endif

    for (uint8_t i = 0; i < cfgMgr.cfg.numSignals; i++) {
        const SignalConfig& sc = cfgMgr.cfg.signals[i];
        if (sc.id[0] == '\0') continue;

        const SignalType sigType = static_cast<SignalType>(sc.type);

#ifdef USE_PCA9685
        if (sc.usePca) {
            if (!pcaPresent) {
                Serial.printf("[ROCR] Signal '%s': use_pca set but PCA9685 missing\n", sc.id);
                continue;
            }
            auto* ps = new PCA9685Signal(sc.id, sigType, &pca9685, sc.chR, sc.chG, sc.chV);
            pcaSignals.emplace_back(ps);
            rocrail.registerPcaSignal(sc.id, sigType, ps);
            Serial.printf("[ROCR] Signal '%s' type=%u → PCA9685 ch %u/%u/%u\n",
                          sc.id, sc.type, sc.chR, sc.chG, sc.chV);
            continue;
        }
#endif

        SignalRGBChannel* found = nullptr;
        auto sigIt = signalChannels.find(sc.chR);
        if (sigIt != signalChannels.end()) {
            found = sigIt->second;
        } else {
            auto* sig = new SignalRGBChannel(sc.chR, sc.chG, sc.chV);
            signalChannels[sc.chR] = sig;
            channels.emplace_back(sig);
            found = sig;
            Serial.printf("[ROCR] Auto-created SignalRGBChannel for %s (ch %u/%u/%u)\n",
                          sc.id, sc.chR, sc.chG, sc.chV);
        }

        rocrail.registerSignal(sc.id, sigType, found);
        Serial.printf("[ROCR] Signal '%s' type=%u → MUX ch %u/%u/%u\n",
                      sc.id, sc.type, sc.chR, sc.chG, sc.chV);
    }

    // Turnouts
    for (uint8_t i = 0; i < cfgMgr.cfg.numTurnouts; i++) {
        const TurnoutConfig& tc = cfgMgr.cfg.turnouts[i];
        if (tc.id[0] == '\0') continue;

        auto* t = new TurnoutChannel(tc.id, tc.chS, tc.chD, tc.pulse);
        t->begin(mux);
        rocrail.registerTurnout(t);
        turnouts.emplace_back(t);
        Serial.printf("[ROCR] Turnout '%s' → MUX chS=%u chD=%u pulse=%ums\n",
                      tc.id, tc.chS, tc.chD, tc.pulse);
    }

    // Sensor → Rocrail block mapping
    for (uint8_t i = 0; i < cfgMgr.cfg.numSensorsRb; i++) {
        const SensorRbConfig& sr = cfgMgr.cfg.sensorsRb[i];
        if (sr.rocrailId[0] == '\0') continue;
        rocrail.registerSensorRb(sr.rocrailId, sr.muxCh);
        ensureSensorChannel(sr.muxCh);
        Serial.printf("[ROCR] Sensor rb '%s' → MUX ch%u\n",
                      sr.rocrailId, sr.muxCh);
    }

    for (uint8_t i = 0; i < cfgMgr.cfg.numTofBlocks; i++) {
        const ToFBlockConfig& tb = cfgMgr.cfg.tofBlocks[i];
        if (tb.rocrailId[0] == '\0') continue;
        rocrail.registerToFBlock(tb.rocrailId);
        Serial.printf("[ROCR] ToF block '%s' (threshold %u mm)\n",
                      tb.rocrailId, tb.thresholdMm ? tb.thresholdMm : cfgMgr.cfg.tofThresholdMm);
    }

    Serial.printf("[ROCR] %u signals, %u turnouts, %u sensor-rb, %u tof-blocks registered\n",
                  cfgMgr.cfg.numSignals, cfgMgr.cfg.numTurnouts,
                  cfgMgr.cfg.numSensorsRb, cfgMgr.cfg.numTofBlocks);

    rocrail.applyBootAspects(cfgMgr.cfg.bootAspectMain, cfgMgr.cfg.bootAspectShunt);
    Serial.printf("[ROCR] Boot aspects: MAIN=%s SHUNT=%s\n",
                  cfgMgr.cfg.bootAspectMain, cfgMgr.cfg.bootAspectShunt);
}

// ── Phase 3: populate channel objects from config ────────────────────
void buildChannelObjects() {
    channels.clear();
    relays.clear();
    sensors.clear();
    strips.clear();
    signalChannels.clear();
    marmottas.clear();

    for (uint8_t i = 0; i < MUX_CHANNELS; i++) {
        switch (cfgMgr.cfg.pinMap[i]) {

            case ChannelRole::SENSOR: {
                auto* s = new SensorChannel(i);
                s->threshold = cfgMgr.cfg.sensorThreshold;
                sensors[i] = s;
                channels.emplace_back(s);
                Serial.printf("[MAP] Ch%02u → Sensor\n", i);
                break;
            }

            case ChannelRole::RELAY: {
                auto* r = new RelayChannel(i);
                relays[i] = r;
                channels.emplace_back(r);
                Serial.printf("[MAP] Ch%02u → Relay\n", i);
                break;
            }

            case ChannelRole::SIGNAL_RGB: {
                uint8_t gCh = (i + 1 < MUX_CHANNELS) ? i + 1 : i;
                uint8_t vCh = (i + 2 < MUX_CHANNELS) ? i + 2 : i;
                auto* sig = new SignalRGBChannel(i, gCh, vCh);
                signalChannels[i] = sig;
                channels.emplace_back(sig);
                Serial.printf("[MAP] Ch%02u → Signal RGB (ch %u/%u/%u)\n", i, i, gCh, vCh);
                i += 2;  // pin_map marks three consecutive MUX channels
                break;
            }

            case ChannelRole::MARMOTTA: {
                auto* m = new MarmottaChannel(i);
                marmottas[i] = m;
                channels.emplace_back(m);
                Serial.printf("[MAP] Ch%02u → Marmotta\n", i);
                break;
            }

            case ChannelRole::SERIAL_RGB: {
                auto* strip = new SerialRGBChannel(i);
                if (!strip->enabled) {
                    delete strip;
                    Serial.printf("[MAP] Ch%02u → WS2812 skipped (no wiring)\n", i);
                    break;
                }
                strip->begin();
                strips[i] = strip;
                channels.emplace_back(strip);
                Serial.printf("[MAP] Ch%02u → WS2812 strip (GPIO %u, %u LEDs)\n",
                              i, strip->gpio, strip->numLeds);
                break;
            }

            default:
                break;
        }
    }
    Serial.printf("[MAP] Total channel objects: %u\n", channels.size());
}

// ── Publish sensor states (only changed channels) ────────────────────
void publishSensorStates() {
    if (sensors.empty()) return;

    bool anyChange = false;
    JsonDocument doc;
    JsonObject states = doc.to<JsonObject>();

    for (auto& [ch, s] : sensors) {
        states[String(ch)] = s->occupied;
        if (s->occupied != prevSensorState[ch]) {
            prevSensorState[ch] = s->occupied;
            anyChange = true;
            // Rocrail block-detector feedback only on actual change
            rocrail.publishSensorFeedback(ch, s->occupied, mqtt);
        }
    }

    if (anyChange) {
        String payload;
        serializeJson(doc, payload);
        mqtt.publish("sensors/state", payload, true);  // retained
    }
}

// ── MQTT command handler ─────────────────────────────────────────────
// Rocrail topics are delegated directly to RocrailProtocol.
// Board-specific topic (railway/<name>/command/set) accepts JSON:
//   Relay:  {"channel":N, "action":"on"|"off"|"toggle"}
//   Strip:  {"channel":N, "action":"color", "r":255,"g":0,"b":0}
//           {"channel":N, "action":"off"|"pixel"}
void onMqttCommand(const String& topic, const String& payload) {
    Serial.printf("[MQTT] ← %s : %s\n", topic.c_str(), payload.c_str());

    if (displayMgr.handleInfoMqtt(topic, payload)) return;
    if (displayMgr.handleMqtt(topic, payload)) return;

    // Delegate Rocrail XML topics
    if (rocrail.hasTopic(topic)) {
        rocrail.handle(topic, payload, mqtt, mux);
        return;
    }

    JsonDocument doc;
    if (deserializeJson(doc, payload)) {
        Serial.println("[MQTT] Bad JSON");
        return;
    }

    if (accessoryMgr.handleMqttJson(doc)) {
        accessoryMgr.publishState(mqtt);
        return;
    }

    if (!doc["channel"].is<uint8_t>()) return;
    uint8_t ch     = doc["channel"];
    String  action = doc["action"] | "toggle";

    // ── Relay command ─────────────────────────────────────────────────
    auto relayIt = relays.find(ch);
    if (relayIt != relays.end()) {
        RelayChannel* r = relayIt->second;
        if      (action == "on")     r->setState(true,   mux);
        else if (action == "off")    r->setState(false,  mux);
        else if (action == "toggle") r->setState(!r->state, mux);

        String ack = "{\"channel\":" + String(ch) +
                     ",\"state\":"   + (r->state ? "true" : "false") + "}";
        mqtt.publish("relays/state", ack);
        return;
    }

    // ── WS2812 strip command ──────────────────────────────────────────
    auto stripIt = strips.find(ch);
    if (stripIt != strips.end()) {
        SerialRGBChannel* s = stripIt->second;
        if (action == "color") {
            uint8_t r = doc["r"] | 0;
            uint8_t g = doc["g"] | 0;
            uint8_t b = doc["b"] | 0;
            s->setAll(CRGB(r, g, b));
        } else if (action == "off") {
            s->setAll(CRGB::Black);
        } else if (action == "pixel") {
            uint8_t idx = doc["idx"] | 0;
            uint8_t r   = doc["r"] | 0;
            uint8_t g   = doc["g"] | 0;
            uint8_t b   = doc["b"] | 0;
            s->setPixel(idx, CRGB(r, g, b));
        }
        return;
    }

    // ── Marmotta trigger ──────────────────────────────────────────────
    auto marmIt = marmottas.find(ch);
    if (marmIt != marmottas.end()) {
        uint32_t ms = doc["ms"] | 200;
        marmIt->second->trigger(mux, ms);
    }
}

// ── mDNS (opzionale) — l'IP LAN è l'accesso affidabile ───────────────
static bool g_mdnsReady = false;

void ensureMdns(ConfigManager& cfgMgr) {
    if (!wifiHasStaIp()) return;
    if (g_mdnsReady) return;

    char host[32];
    wifiNormalizeHostname(cfgMgr.cfg.hostname, host, sizeof(host));
    WiFi.setHostname(host);
    if (MDNS.begin(host)) {
        MDNS.addService("http", "tcp", 80);
        g_mdnsReady = true;
        Serial.printf("[mDNS] http://%s.local/ (usa http://%s/)\n",
                      host, WiFi.localIP().toString().c_str());
    }
}

// ── Runtime WiFi reset (long-press during normal operation) ──────────
void checkResetButton() {
    static unsigned long pressStart = 0;
    static bool          wasPressed = false;

    bool pressed = (digitalRead(WIFI_RESET_PIN) == LOW);

    if (pressed && !wasPressed) {
        pressStart = millis();
        wasPressed = true;
    } else if (!pressed && wasPressed) {
        wasPressed = false;
    } else if (pressed && wasPressed) {
        if (millis() - pressStart >= WIFI_RESET_HOLD_MS) {
            Serial.println("[WIFI] Long press – resetting credentials");
            resetWifiAndRestart(cfgMgr);
        }
    }
}

SensorChannel* ensureSensorChannel(uint8_t ch) {
    auto it = sensors.find(ch);
    if (it != sensors.end()) return it->second;

    if (cfgMgr.cfg.pinMap[ch] != ChannelRole::UNUSED &&
        cfgMgr.cfg.pinMap[ch] != ChannelRole::SENSOR) {
        Serial.printf("[MAP] Ch%u busy (role %u) – sensor_rb skipped\n",
                      ch, static_cast<uint8_t>(cfgMgr.cfg.pinMap[ch]));
        return nullptr;
    }

    auto* s = new SensorChannel(ch);
    s->threshold = cfgMgr.cfg.sensorThreshold;
    sensors[ch] = s;
    channels.emplace_back(s);
    cfgMgr.cfg.pinMap[ch] = ChannelRole::SENSOR;
    Serial.printf("[MAP] Auto sensor on MUX ch%u (Rocrail block)\n", ch);
    return s;
}

uint8_t countConfiguredMuxChannels() {
    bool used[MUX_CHANNELS] = {};

    for (uint8_t i = 0; i < MUX_CHANNELS; i++) {
        switch (cfgMgr.cfg.pinMap[i]) {
            case ChannelRole::SENSOR:
            case ChannelRole::RELAY:
            case ChannelRole::MARMOTTA:
            case ChannelRole::SERIAL_RGB:
                used[i] = true;
                break;
            case ChannelRole::SIGNAL_RGB:
                for (uint8_t j = i; j < i + 3 && j < MUX_CHANNELS; j++) used[j] = true;
                i += 2;
                break;
            default:
                break;
        }
    }

    for (uint8_t i = 0; i < cfgMgr.cfg.numSignals; i++) {
        const SignalConfig& s = cfgMgr.cfg.signals[i];
        if (s.id[0] == '\0' || s.usePca) continue;
        if (s.chR < MUX_CHANNELS) used[s.chR] = true;
        if (s.chG < MUX_CHANNELS) used[s.chG] = true;
        if (s.chV < MUX_CHANNELS) used[s.chV] = true;
    }
    for (uint8_t i = 0; i < cfgMgr.cfg.numTurnouts; i++) {
        const TurnoutConfig& t = cfgMgr.cfg.turnouts[i];
        if (t.id[0] == '\0') continue;
        if (t.chS < MUX_CHANNELS) used[t.chS] = true;
        if (t.chD < MUX_CHANNELS) used[t.chD] = true;
    }
    for (uint8_t i = 0; i < cfgMgr.cfg.numSensorsRb; i++) {
        uint8_t c = cfgMgr.cfg.sensorsRb[i].muxCh;
        if (c < MUX_CHANNELS) used[c] = true;
    }
    for (uint8_t i = 0; i < cfgMgr.cfg.numAccessories; i++) {
        const AccessoryConfig& a = cfgMgr.cfg.accessories[i];
        if (a.id[0] == '\0') continue;
        if (a.muxCh < MUX_CHANNELS) used[a.muxCh] = true;
        if (a.muxChBar < MUX_CHANNELS) used[a.muxChBar] = true;
    }

    uint8_t n = 0;
    for (uint8_t i = 0; i < MUX_CHANNELS; i++) if (used[i]) n++;
    return n;
}
