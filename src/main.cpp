#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <ESPmDNS.h>
#include <Wire.h>
#include <VL6180X.h>

#include "Config.h"
#include "ConfigManager.h"
#include "MuxDriver.h"
#include "ChannelObjects.h"
#include "MQTTManager.h"
#include "WebConfig.h"

// ── Globals ──────────────────────────────────────────────────────────
ConfigManager cfgMgr;
MuxDriver     mux;
MQTTManager   mqtt;
WebConfig     webConfig;
VL6180X       tof;

// Channel object pools (populated from pin_map during setup)
std::vector<std::unique_ptr<IChannel>> channels;

// Relay lookup (quick access for MQTT commands)
std::map<uint8_t, RelayChannel*> relays;
std::map<uint8_t, SensorChannel*> sensors;

// ── Scan state machine ───────────────────────────────────────────────
static unsigned long lastScanMs = 0;

// ── Forward declarations ─────────────────────────────────────────────
void buildChannelObjects();
void onMqttCommand(const String& topic, const String& payload);
void publishSensorStates();

// ─────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    Serial.println("\n\n==== ShieldH0 booting ====");

    // ── Phase 1: Config + Filesystem ─────────────────────────────────
    cfgMgr.begin();   // mounts LittleFS, loads config.json

    // ── Phase 1: WiFi portal ──────────────────────────────────────────
    WiFiManager wm;
    wm.setTimeout(WIFI_PORTAL_TIMEOUT);

    // Expose MQTT fields in the captive portal
    WiFiManagerParameter p_host("hostname",    "Board name",       cfgMgr.cfg.hostname,   32);
    WiFiManagerParameter p_broker("broker",    "MQTT Broker IP",   cfgMgr.cfg.mqttBroker, 64);
    WiFiManagerParameter p_port("port",        "MQTT Port",        "1883",                 6);
    WiFiManagerParameter p_user("mqtt_user",   "MQTT User",        cfgMgr.cfg.mqttUser,   32);
    WiFiManagerParameter p_pass("mqtt_pass",   "MQTT Password",    cfgMgr.cfg.mqttPass,   32);

    wm.addParameter(&p_host);
    wm.addParameter(&p_broker);
    wm.addParameter(&p_port);
    wm.addParameter(&p_user);
    wm.addParameter(&p_pass);

    // Called after the portal saves credentials
    wm.setSaveParamsCallback([&]() {
        strlcpy(cfgMgr.cfg.hostname,   p_host.getValue(),   sizeof(cfgMgr.cfg.hostname));
        strlcpy(cfgMgr.cfg.mqttBroker, p_broker.getValue(), sizeof(cfgMgr.cfg.mqttBroker));
        cfgMgr.cfg.mqttPort = atoi(p_port.getValue());
        strlcpy(cfgMgr.cfg.mqttUser,   p_user.getValue(),   sizeof(cfgMgr.cfg.mqttUser));
        strlcpy(cfgMgr.cfg.mqttPass,   p_pass.getValue(),   sizeof(cfgMgr.cfg.mqttPass));
        cfgMgr.save();
    });

    String apName = String("ShieldH0-") + cfgMgr.cfg.hostname;
    if (!wm.autoConnect(apName.c_str())) {
        Serial.println("[WIFI] Portal timed out – restarting");
        ESP.restart();
    }
    Serial.printf("[WIFI] Connected – IP: %s\n", WiFi.localIP().toString().c_str());

    // ── mDNS ──────────────────────────────────────────────────────────
    if (MDNS.begin(cfgMgr.cfg.hostname)) {
        MDNS.addService("http", "tcp", 80);
        Serial.printf("[mDNS] Hostname: http://%s.local/\n", cfgMgr.cfg.hostname);
    }

    // ── Phase 2: Hardware init ────────────────────────────────────────
    mux.begin();

    Wire.begin(I2C_SDA, I2C_SCL);
    tof.init();
    tof.configureDefault();
    tof.setTimeout(500);
    Serial.println("[HW] VL6180X ready");

    // ── Phase 3: Build channel objects from pin_map ───────────────────
    buildChannelObjects();

    // ── Phase 4: MQTT ────────────────────────────────────────────────
    mqtt.begin(cfgMgr, onMqttCommand);

    // ── Phase 5: Web config UI ────────────────────────────────────────
    webConfig.begin(cfgMgr);

    Serial.println("==== Boot complete ====\n");
}

// ─────────────────────────────────────────────────────────────────────
void loop() {
    webConfig.loop();
    mqtt.loop();

    // MUX polling: full channel scan every SENSOR_POLL_MS
    if (millis() - lastScanMs >= SENSOR_POLL_MS) {
        lastScanMs = millis();

        for (auto& ch : channels) {
            ch->update(mux);
        }
        publishSensorStates();
    }
}

// ── Phase 3: populate channel objects ────────────────────────────────
void buildChannelObjects() {
    channels.clear();
    relays.clear();
    sensors.clear();

    for (uint8_t i = 0; i < MUX_CHANNELS; i++) {
        switch (cfgMgr.cfg.pinMap[i]) {
            case ChannelRole::SENSOR: {
                auto* s = new SensorChannel(i);
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
                auto* sig = new SignalRGBChannel(i);
                channels.emplace_back(sig);
                Serial.printf("[MAP] Ch%02u → Signal RGB (occupies %u-%u)\n", i, i, i+2);
                i += 2;  // skip the next 2 channels consumed by R,G,B
                break;
            }
            case ChannelRole::MARMOTTA: {
                auto* m = new MarmottaChannel(i);
                channels.emplace_back(m);
                Serial.printf("[MAP] Ch%02u → Marmotta\n", i);
                break;
            }
            default:
                break;
        }
    }
    Serial.printf("[MAP] %u channel objects created\n", channels.size());
}

// ── Phase 4: publish sensor states ───────────────────────────────────
void publishSensorStates() {
    if (sensors.empty()) return;

    JsonDocument doc;
    JsonObject states = doc.to<JsonObject>();
    bool anyChange = false;

    for (auto& [ch, s] : sensors) {
        static bool prevState[MUX_CHANNELS] = {};
        if (s->occupied != prevState[ch]) {
            prevState[ch] = s->occupied;
            anyChange = true;
        }
        states[String(ch)] = s->occupied;
    }

    if (anyChange) {
        String payload;
        serializeJson(doc, payload);
        mqtt.publish("sensors/state", payload);
    }
}

// ── Phase 4: handle incoming MQTT commands ───────────────────────────
void onMqttCommand(const String& topic, const String& payload) {
    Serial.printf("[MQTT] ← %s : %s\n", topic.c_str(), payload.c_str());

    JsonDocument doc;
    if (deserializeJson(doc, payload)) {
        Serial.println("[MQTT] Bad JSON in command");
        return;
    }

    // Expected payload: { "channel": 3, "action": "on" | "off" | "toggle" }
    if (!doc["channel"].is<uint8_t>()) return;
    uint8_t ch     = doc["channel"];
    String  action = doc["action"] | "toggle";

    auto it = relays.find(ch);
    if (it == relays.end()) {
        Serial.printf("[MQTT] Ch%u is not a relay\n", ch);
        return;
    }

    RelayChannel* r = it->second;
    if      (action == "on")     r->setState(true,  mux);
    else if (action == "off")    r->setState(false, mux);
    else if (action == "toggle") r->setState(!r->state, mux);

    // Echo state back
    String ack = String("{\"channel\":") + ch +
                 ",\"state\":" + (r->state ? "true" : "false") + "}";
    mqtt.publish("sensors/state", ack);
}
