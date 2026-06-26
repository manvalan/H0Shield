#pragma once
/**
 * Unica fonte di verità per config.json ↔ BoardConfig ↔ API /api/cfg.
 * Stile snello: niente duplicazione in WebConfig.
 */
#include <ArduinoJson.h>
#include "Config.h"
#include "I2cBus.h"

namespace ConfigJson {

inline AccessoryProfile parseProfile(const char* s) {
    if (strcmp(s, "light") == 0)       return AccessoryProfile::LIGHT;
    if (strcmp(s, "level_xing") == 0)  return AccessoryProfile::LEVEL_XING;
    if (strcmp(s, "bell") == 0)        return AccessoryProfile::BELL;
    if (strcmp(s, "gate") == 0)        return AccessoryProfile::GATE;
    return AccessoryProfile::GENERIC;
}

inline const char* profileName(AccessoryProfile p) {
    switch (p) {
        case AccessoryProfile::LIGHT:      return "light";
        case AccessoryProfile::LEVEL_XING: return "level_xing";
        case AccessoryProfile::BELL:       return "bell";
        case AccessoryProfile::GATE:       return "gate";
        default:                           return "generic";
    }
}

inline void write(const BoardConfig& c, JsonObject doc, bool forApi = false) {
    doc["hostname"] = c.hostname;
    doc["wifi_ssid"] = c.wifiSsid;
    if (c.lastStaIp[0]) doc["last_sta_ip"] = c.lastStaIp;
    doc["web_user"] = c.webUser;
    if (forApi)
        doc["has_web_pass"] = c.webPassHash[0] != '\0';
    else
        doc["web_pass_hash"] = c.webPassHash;

    doc["mqtt_broker"] = c.mqttBroker;
    doc["mqtt_port"] = c.mqttPort;
    doc["mqtt_user"] = c.mqttUser;
    doc["info_mqtt_topic"] = c.infoMqttTopic;
    doc["info_mqtt_enabled"] = c.infoMqttEnabled;
    doc["sensor_threshold"] = c.sensorThreshold;
    doc["tof_enabled"] = c.tofEnabled;
    doc["tof_threshold_mm"] = c.tofThresholdMm;
    doc["boot_aspect_main"] = c.bootAspectMain;
    doc["boot_aspect_shunt"] = c.bootAspectShunt;
    doc["tca9548_addr"] = c.tca9548Addr;
    doc["tof_i2c_slot"] = c.tofI2cSlot;

    JsonArray i2Arr = doc["i2c_slots"].to<JsonArray>();
    for (uint8_t s = 0; s < I2C_SLOTS; s++) {
        JsonObject sl = i2Arr.add<JsonObject>();
        sl["slot"] = s;
        sl["mode"] = I2cBus::modeName(c.i2cSlots[s].mode);
        if (c.i2cSlots[s].elementId[0])
            sl["element_id"] = c.i2cSlots[s].elementId;
    }

    JsonArray pm = doc["pin_map"].to<JsonArray>();
    for (uint8_t i = 0; i < MUX_CHANNELS; i++)
        pm.add(static_cast<uint8_t>(c.pinMap[i]));

    JsonArray sgArr = doc["signals"].to<JsonArray>();
    for (uint8_t i = 0; i < c.numSignals; i++) {
        JsonObject o = sgArr.add<JsonObject>();
        o["id"] = c.signals[i].id;
        o["type"] = c.signals[i].type;
        o["chR"] = c.signals[i].chR;
        o["chG"] = c.signals[i].chG;
        o["chV"] = c.signals[i].chV;
        if (c.signals[i].usePca) o["use_pca"] = true;
    }

    JsonArray swArr = doc["turnouts"].to<JsonArray>();
    for (uint8_t i = 0; i < c.numTurnouts; i++) {
        JsonObject o = swArr.add<JsonObject>();
        o["id"] = c.turnouts[i].id;
        o["chS"] = c.turnouts[i].chS;
        o["chD"] = c.turnouts[i].chD;
        o["pulse"] = c.turnouts[i].pulse;
    }

    JsonArray rbArr = doc["sensors_rb"].to<JsonArray>();
    for (uint8_t i = 0; i < c.numSensorsRb; i++) {
        JsonObject o = rbArr.add<JsonObject>();
        o["rocrail_id"] = c.sensorsRb[i].rocrailId;
        o["mux_ch"] = c.sensorsRb[i].muxCh;
    }

    JsonArray tbArr = doc["tof_blocks"].to<JsonArray>();
    for (uint8_t i = 0; i < c.numTofBlocks; i++) {
        JsonObject o = tbArr.add<JsonObject>();
        o["rocrail_id"] = c.tofBlocks[i].rocrailId;
        o["threshold_mm"] = c.tofBlocks[i].thresholdMm;
    }

    JsonArray dpArr = doc["displays"].to<JsonArray>();
    for (uint8_t i = 0; i < c.numDisplays; i++) {
        const DisplayConfig& d = c.displays[i];
        JsonObject o = dpArr.add<JsonObject>();
        o["id"] = d.id;
        o["type"] = (d.type == DisplayType::TIMETABLE) ? "timetable" : "platform";
        if (d.i2cSlot < I2C_SLOTS) o["i2c_slot"] = d.i2cSlot;
        o["i2c_addr"] = d.i2cAddr;
        o["platform_num"] = d.platformNum;
        if (d.stationId[0]) o["station_id"] = d.stationId;
        o["station_name"] = d.stationName;
        o["destination"] = d.destination;
        o["departure_time"] = d.departureTime;
        o["status"] = d.status;
        JsonArray rows = o["rows"].to<JsonArray>();
        for (uint8_t r = 0; r < d.numRows; r++) {
            JsonObject ro = rows.add<JsonObject>();
            ro["time"] = d.rows[r].timeStr;
            ro["destination"] = d.rows[r].destination;
            ro["platform"] = d.rows[r].platformBin;
            ro["status"] = d.rows[r].status;
        }
    }

    JsonArray acArr = doc["accessories"].to<JsonArray>();
    for (uint8_t i = 0; i < c.numAccessories; i++) {
        const AccessoryConfig& a = c.accessories[i];
        JsonObject o = acArr.add<JsonObject>();
        o["id"] = a.id;
        o["profile"] = profileName(a.profile);
        if (a.profile == AccessoryProfile::LEVEL_XING) {
            o["mux_ch_lights"] = a.muxCh;
            o["mux_ch_bar"] = a.muxChBar;
        } else {
            o["mux_ch"] = a.muxCh;
        }
        o["pulse_ms"] = a.pulseMs;
    }

    JsonArray scArr = doc["scenarios"].to<JsonArray>();
    for (uint8_t i = 0; i < c.numScenarios; i++) {
        const ScenarioConfig& s = c.scenarios[i];
        JsonObject o = scArr.add<JsonObject>();
        o["id"] = s.id;
        o["enabled"] = s.enabled;
        if (s.muxCh != 255) o["mux_ch"] = s.muxCh;
        o["tof_trigger"] = s.tofTrigger;
        if (s.rocrailId[0]) o["rocrail_id"] = s.rocrailId;
        if (s.accessoryPl[0]) o["accessory_pl"] = s.accessoryPl;
        if (s.accessoryLight[0]) o["accessory_light"] = s.accessoryLight;
        if (s.accessoryBell[0]) o["accessory_bell"] = s.accessoryBell;
        if (s.displayId[0]) o["display_id"] = s.displayId;
        o["status_occupied"] = s.statusOcc;
        o["status_free"] = s.statusFree;
        if (s.destOcc[0]) o["dest_occupied"] = s.destOcc;
        if (s.destFree[0]) o["dest_free"] = s.destFree;
    }
}

inline void readFile(BoardConfig& c, JsonObject doc) {
    strlcpy(c.hostname, doc["hostname"] | c.hostname, sizeof(c.hostname));
    strlcpy(c.wifiSsid, doc["wifi_ssid"] | c.wifiSsid, sizeof(c.wifiSsid));
    strlcpy(c.lastStaIp, doc["last_sta_ip"] | c.lastStaIp, sizeof(c.lastStaIp));
    strlcpy(c.webUser, doc["web_user"] | c.webUser, sizeof(c.webUser));
    strlcpy(c.webPassHash, doc["web_pass_hash"] | c.webPassHash, sizeof(c.webPassHash));
    strlcpy(c.mqttBroker, doc["mqtt_broker"] | c.mqttBroker, sizeof(c.mqttBroker));
    c.mqttPort = doc["mqtt_port"] | c.mqttPort;
    c.sensorThreshold = doc["sensor_threshold"] | c.sensorThreshold;
    c.tofEnabled = doc["tof_enabled"] | c.tofEnabled;
    c.tofThresholdMm = doc["tof_threshold_mm"] | c.tofThresholdMm;
    strlcpy(c.bootAspectMain, doc["boot_aspect_main"] | c.bootAspectMain, sizeof(c.bootAspectMain));
    strlcpy(c.bootAspectShunt, doc["boot_aspect_shunt"] | c.bootAspectShunt, sizeof(c.bootAspectShunt));
    c.tca9548Addr = doc["tca9548_addr"] | c.tca9548Addr;
    c.tofI2cSlot = doc["tof_i2c_slot"] | c.tofI2cSlot;
    strlcpy(c.mqttUser, doc["mqtt_user"] | c.mqttUser, sizeof(c.mqttUser));
    c.mqttPass[0] = '\0';
    strlcpy(c.infoMqttTopic, doc["info_mqtt_topic"] | c.infoMqttTopic, sizeof(c.infoMqttTopic));
    c.infoMqttEnabled = doc["info_mqtt_enabled"] | c.infoMqttEnabled;

    for (uint8_t s = 0; s < I2C_SLOTS; s++) {
        I2cSlotConfig def = {};
        JsonArray i2Slots = doc["i2c_slots"].as<JsonArray>();
        if (s < i2Slots.size()) {
            JsonObject sl = i2Slots[s];
            def.mode = I2cBus::parseMode(sl["mode"] | "auto");
            strlcpy(def.elementId, sl["element_id"] | "", sizeof(def.elementId));
        }
        c.i2cSlots[s] = def;
    }

    JsonArray pm = doc["pin_map"].as<JsonArray>();
    for (uint8_t i = 0; i < MUX_CHANNELS && i < pm.size(); i++)
        c.pinMap[i] = static_cast<ChannelRole>(pm[i].as<uint8_t>());

    c.numSignals = 0;
    for (JsonObject sg : doc["signals"].as<JsonArray>()) {
        if (c.numSignals >= BoardConfig::MAX_SIGNALS) break;
        SignalConfig& s = c.signals[c.numSignals++];
        strlcpy(s.id, sg["id"] | "", sizeof(s.id));
        s.type = sg["type"] | 0;
        s.chR = sg["chR"] | 0;
        s.chG = sg["chG"] | 1;
        s.chV = sg["chV"] | 2;
        s.usePca = sg["use_pca"] | false;
    }

    c.numTurnouts = 0;
    for (JsonObject sw : doc["turnouts"].as<JsonArray>()) {
        if (c.numTurnouts >= BoardConfig::MAX_TURNOUTS) break;
        TurnoutConfig& t = c.turnouts[c.numTurnouts++];
        strlcpy(t.id, sw["id"] | "", sizeof(t.id));
        t.chS = sw["chS"] | 0;
        t.chD = sw["chD"] | 1;
        t.pulse = sw["pulse"] | 300;
    }

    c.numSensorsRb = 0;
    for (JsonObject rb : doc["sensors_rb"].as<JsonArray>()) {
        if (c.numSensorsRb >= BoardConfig::MAX_SENSORS_RB) break;
        SensorRbConfig& sr = c.sensorsRb[c.numSensorsRb++];
        strlcpy(sr.rocrailId, rb["rocrail_id"] | "", sizeof(sr.rocrailId));
        sr.muxCh = rb["mux_ch"] | 0;
    }

    c.numTofBlocks = 0;
    for (JsonObject tb : doc["tof_blocks"].as<JsonArray>()) {
        if (c.numTofBlocks >= BoardConfig::MAX_TOF_BLOCKS) break;
        ToFBlockConfig& t = c.tofBlocks[c.numTofBlocks++];
        strlcpy(t.rocrailId, tb["rocrail_id"] | "", sizeof(t.rocrailId));
        t.thresholdMm = tb["threshold_mm"] | 0;
    }

    c.numDisplays = 0;
    for (JsonObject dp : doc["displays"].as<JsonArray>()) {
        if (c.numDisplays >= BoardConfig::MAX_DISPLAYS) break;
        DisplayConfig& d = c.displays[c.numDisplays++];
        strlcpy(d.id, dp["id"] | "", sizeof(d.id));
        const char* typeStr = dp["type"] | "platform";
        d.type = (strcmp(typeStr, "timetable") == 0) ? DisplayType::TIMETABLE : DisplayType::PLATFORM;
        d.i2cSlot = dp["i2c_slot"].isNull() ? 255 : static_cast<uint8_t>(dp["i2c_slot"].as<uint8_t>());
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

    c.numAccessories = 0;
    for (JsonObject ac : doc["accessories"].as<JsonArray>()) {
        if (c.numAccessories >= BoardConfig::MAX_ACCESSORIES) break;
        AccessoryConfig& a = c.accessories[c.numAccessories++];
        strlcpy(a.id, ac["id"] | "", sizeof(a.id));
        a.profile = parseProfile(ac["profile"] | "generic");
        a.muxCh = ac["mux_ch"] | ac["mux_ch_lights"] | 0;
        a.muxChBar = ac["mux_ch_bar"] | 0;
        a.pulseMs = ac["pulse_ms"] | 300;
    }

    c.numScenarios = 0;
    for (JsonObject sc : doc["scenarios"].as<JsonArray>()) {
        if (c.numScenarios >= BoardConfig::MAX_SCENARIOS) break;
        ScenarioConfig& s = c.scenarios[c.numScenarios++];
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
}

inline void readApi(BoardConfig& c, JsonObject doc) {
    if (doc["hostname"].is<const char*>())
        strlcpy(c.hostname, doc["hostname"], sizeof(c.hostname));
    if (doc["web_user"].is<const char*>())
        strlcpy(c.webUser, doc["web_user"], sizeof(c.webUser));
    if (doc["sensor_threshold"].is<uint16_t>())
        c.sensorThreshold = doc["sensor_threshold"];
    if (doc["tof_enabled"].is<bool>())
        c.tofEnabled = doc["tof_enabled"];
    if (doc["tof_threshold_mm"].is<uint8_t>() || doc["tof_threshold_mm"].is<uint16_t>())
        c.tofThresholdMm = doc["tof_threshold_mm"].as<uint8_t>();
    if (doc["boot_aspect_main"].is<const char*>())
        strlcpy(c.bootAspectMain, doc["boot_aspect_main"], sizeof(c.bootAspectMain));
    if (doc["boot_aspect_shunt"].is<const char*>())
        strlcpy(c.bootAspectShunt, doc["boot_aspect_shunt"], sizeof(c.bootAspectShunt));
    if (doc["tca9548_addr"].is<uint8_t>() || doc["tca9548_addr"].is<uint16_t>())
        c.tca9548Addr = doc["tca9548_addr"].as<uint8_t>();
    if (doc["tof_i2c_slot"].is<uint8_t>())
        c.tofI2cSlot = doc["tof_i2c_slot"];
    if (doc["mqtt_broker"].is<const char*>())
        strlcpy(c.mqttBroker, doc["mqtt_broker"], sizeof(c.mqttBroker));
    if (doc["mqtt_port"].is<uint16_t>())
        c.mqttPort = doc["mqtt_port"];
    if (doc["mqtt_user"].is<const char*>())
        strlcpy(c.mqttUser, doc["mqtt_user"], sizeof(c.mqttUser));
    if (doc["info_mqtt_topic"].is<const char*>())
        strlcpy(c.infoMqttTopic, doc["info_mqtt_topic"], sizeof(c.infoMqttTopic));
    if (doc["info_mqtt_enabled"].is<bool>())
        c.infoMqttEnabled = doc["info_mqtt_enabled"];

    if (doc["i2c_slots"].is<JsonArray>()) {
        uint8_t si = 0;
        for (JsonObject sl : doc["i2c_slots"].as<JsonArray>()) {
            if (si >= I2C_SLOTS) break;
            c.i2cSlots[si].mode = I2cBus::parseMode(sl["mode"] | "auto");
            strlcpy(c.i2cSlots[si].elementId, sl["element_id"] | "",
                    sizeof(c.i2cSlots[si].elementId));
            si++;
        }
    }

    if (doc["pin_map"].is<JsonArray>()) {
        JsonArray pm = doc["pin_map"].as<JsonArray>();
        for (uint8_t i = 0; i < MUX_CHANNELS && i < pm.size(); i++)
            c.pinMap[i] = static_cast<ChannelRole>(pm[i].as<uint8_t>());
    }

    if (doc["signals"].is<JsonArray>()) {
        c.numSignals = 0;
        for (JsonObject sg : doc["signals"].as<JsonArray>()) {
            if (c.numSignals >= BoardConfig::MAX_SIGNALS) break;
            SignalConfig& s = c.signals[c.numSignals++];
            strlcpy(s.id, sg["id"] | "", sizeof(s.id));
            s.type = sg["type"] | 0;
            s.chR = sg["chR"] | 0;
            s.chG = sg["chG"] | 1;
            s.chV = sg["chV"] | 2;
            s.usePca = sg["use_pca"] | false;
        }
    }

    if (doc["turnouts"].is<JsonArray>()) {
        c.numTurnouts = 0;
        for (JsonObject sw : doc["turnouts"].as<JsonArray>()) {
            if (c.numTurnouts >= BoardConfig::MAX_TURNOUTS) break;
            TurnoutConfig& t = c.turnouts[c.numTurnouts++];
            strlcpy(t.id, sw["id"] | "", sizeof(t.id));
            t.chS = sw["chS"] | 0;
            t.chD = sw["chD"] | 1;
            t.pulse = sw["pulse"] | 300;
        }
    }

    if (doc["sensors_rb"].is<JsonArray>()) {
        c.numSensorsRb = 0;
        for (JsonObject rb : doc["sensors_rb"].as<JsonArray>()) {
            if (c.numSensorsRb >= BoardConfig::MAX_SENSORS_RB) break;
            SensorRbConfig& sr = c.sensorsRb[c.numSensorsRb++];
            strlcpy(sr.rocrailId, rb["rocrail_id"] | "", sizeof(sr.rocrailId));
            sr.muxCh = rb["mux_ch"] | 0;
        }
    }

    if (doc["tof_blocks"].is<JsonArray>()) {
        c.numTofBlocks = 0;
        for (JsonObject tb : doc["tof_blocks"].as<JsonArray>()) {
            if (c.numTofBlocks >= BoardConfig::MAX_TOF_BLOCKS) break;
            ToFBlockConfig& t = c.tofBlocks[c.numTofBlocks++];
            strlcpy(t.rocrailId, tb["rocrail_id"] | "", sizeof(t.rocrailId));
            t.thresholdMm = tb["threshold_mm"] | 0;
        }
    }

    if (doc["displays"].is<JsonArray>()) {
        c.numDisplays = 0;
        for (JsonObject dp : doc["displays"].as<JsonArray>()) {
            if (c.numDisplays >= BoardConfig::MAX_DISPLAYS) break;
            DisplayConfig& d = c.displays[c.numDisplays++];
            strlcpy(d.id, dp["id"] | "", sizeof(d.id));
            const char* typeStr = dp["type"] | "platform";
            d.type = (strcmp(typeStr, "timetable") == 0) ? DisplayType::TIMETABLE : DisplayType::PLATFORM;
            d.i2cSlot = dp["i2c_slot"].isNull() ? 255 : static_cast<uint8_t>(dp["i2c_slot"].as<uint8_t>());
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
    }

    if (doc["accessories"].is<JsonArray>()) {
        c.numAccessories = 0;
        for (JsonObject ac : doc["accessories"].as<JsonArray>()) {
            if (c.numAccessories >= BoardConfig::MAX_ACCESSORIES) break;
            AccessoryConfig& a = c.accessories[c.numAccessories++];
            strlcpy(a.id, ac["id"] | "", sizeof(a.id));
            a.profile = parseProfile(ac["profile"] | "generic");
            a.muxCh = ac["mux_ch"] | ac["mux_ch_lights"] | 0;
            a.muxChBar = ac["mux_ch_bar"] | 0;
            a.pulseMs = ac["pulse_ms"] | 300;
        }
    }

    if (doc["scenarios"].is<JsonArray>()) {
        c.numScenarios = 0;
        for (JsonObject sc : doc["scenarios"].as<JsonArray>()) {
            if (c.numScenarios >= BoardConfig::MAX_SCENARIOS) break;
            ScenarioConfig& s = c.scenarios[c.numScenarios++];
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
    }
}

}  // namespace ConfigJson
