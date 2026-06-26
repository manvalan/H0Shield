#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <time.h>
#include "ConfigManager.h"
#include "HardwareProbe.h"
#include "I2cBus.h"
#include "MQTTManager.h"

#ifdef USE_DISPLAY
#include <U8g2lib.h>
#endif

/**
 * Manages up to MAX_DISPLAYS OLED panels (SH1106 128×64 @ 0x3C / 0x3D).
 * Two layouts: platform (binario) and timetable (tabellone orario).
 *
 * MQTT: railway/<hostname>/display/<id>/platform/…  or  …/timetable/…
 * Build with -DUSE_DISPLAY and U8g2 for actual rendering.
 */
class DisplayManager {
public:
    static bool driverEnabled() {
#ifdef USE_DISPLAY
        return true;
#else
        return false;
#endif
    }

    void begin(ConfigManager& cfgMgr, const HardwareProbe& hw, I2cBus& bus) {
        _cfg = &cfgMgr;
        _hw  = &hw;
        _bus = &bus;
        _count = 0;

        for (uint8_t i = 0; i < cfgMgr.cfg.numDisplays && i < BoardConfig::MAX_DISPLAYS; i++) {
            RuntimeDisplay& rd = _rt[_count];
            rd.data   = cfgMgr.cfg.displays[i];
            uint8_t slot = rd.data.i2cSlot < I2C_SLOTS ? rd.data.i2cSlot : 0;
            rd.present = _probePresent(rd.data.i2cAddr, hw, bus, slot);
            rd.scrollRow = 0;
            rd.dirty = true;

#ifdef USE_DISPLAY
            if (rd.present) _initRuntimeOled(rd);
#endif
            _count++;
        }

        if (_hasTimetable()) {
            setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
            tzset();
            configTime(0, 0, "pool.ntp.org", "time.nist.gov");
        }

        Serial.printf("[DISP] %u display(s) configured%s\n", _count,
#ifdef USE_DISPLAY
                      ""
#else
                      " (USE_DISPLAY off – MQTT/config only)"
#endif
        );
    }

    void loop() {
        if (_count == 0) return;

        unsigned long now = millis();

        for (uint8_t i = 0; i < _count; i++) {
            RuntimeDisplay& rd = _rt[i];
            if (!rd.present) continue;

            if (rd.data.type == DisplayType::TIMETABLE &&
                now - rd.lastScrollMs >= 3000) {
                rd.lastScrollMs = now;
                uint8_t visible = _visibleRows(rd.data);
                if (visible > 3) {
                    rd.scrollRow = (rd.scrollRow + 1) % (visible - 2);
                    rd.dirty = true;
                }
            }

            if (rd.data.type == DisplayType::PLATFORM &&
                rd.platformView == PlatformView::DETAIL &&
                now - rd.lastPlatformScrollMs >= 40) {
                rd.lastPlatformScrollMs = now;
                const int tw = static_cast<int>(strlen(rd.scrollStops)) * 6;
                rd.scrollPos--;
                if (rd.scrollPos < -tw) rd.scrollPos = 128;
                rd.dirty = true;
            }

            if (rd.dirty || now - _lastDrawMs >= 1000) {
                rd.dirty = false;
#ifdef USE_DISPLAY
                if (rd.u8g2) _draw(rd);
#endif
            }
        }
        _lastDrawMs = now;
    }

    /** Disegna subito i display dirty (test UI / MQTT). */
    uint8_t flushDirty() {
#ifdef USE_DISPLAY
        uint8_t n = 0;
        for (uint8_t i = 0; i < _count; i++) {
            RuntimeDisplay& rd = _rt[i];
            if (!rd.dirty || !rd.present || !rd.u8g2) continue;
            rd.dirty = false;
            _draw(rd);
            n++;
        }
        return n;
#else
        return 0;
#endif
    }

    void subscribeAll(MQTTManager& mqtt) {
        if (_count == 0 || !mqtt.isEnabled()) return;
        String topic = _cfg->topic("display/#");
        mqtt.subscribe(topic.c_str());
        Serial.printf("[DISP] Subscribed %s\n", topic.c_str());
        if (_cfg->cfg.infoMqttEnabled && _cfg->cfg.infoMqttTopic[0]) {
            mqtt.subscribe(_cfg->cfg.infoMqttTopic);
            Serial.printf("[DISP/SIP] Subscribed %s\n", _cfg->cfg.infoMqttTopic);
        }
    }

    /** Applica payload SIP (topic railway/info) — anche da test UI senza broker. */
    bool injectInfoFromJson(JsonObject doc, String& err) {
        if (doc["display"].is<JsonObject>()) {
            if (!_upsertRuntimeFromJson(doc["display"], err))
                return false;
        }
        JsonDocument payload;
        JsonObject root = payload.to<JsonObject>();
        for (JsonPair kv : doc) {
            if (strcmp(kv.key().c_str(), "display") == 0) continue;
            if (strcmp(kv.key().c_str(), "type") == 0) {
                const char* tv = kv.value().as<const char*>();
                if (tv && strcmp(tv, "display_info") == 0) continue;
            }
            root[kv.key().c_str()] = kv.value();
        }
        const uint8_t n = _processInfoPayload(root);
        if (n == 0) {
            err = "Nessun display corrisponde (controlla station_id, binario, display_id)";
            return false;
        }
        flushDirty();
        return true;
    }

    /** Sistema informativo plastico — topic unico JSON (railway/info). */
    bool handleInfoMqtt(const String& topic, const String& payload) {
        if (_count == 0 || !_cfg->cfg.infoMqttEnabled) return false;
        if (!_cfg->cfg.infoMqttTopic[0] || !topic.equals(_cfg->cfg.infoMqttTopic))
            return false;

        JsonDocument doc;
        if (deserializeJson(doc, payload)) {
            Serial.println("[DISP/SIP] Bad JSON");
            return true;
        }

        const uint8_t updated = _processInfoPayload(doc.as<JsonObject>());
        if (updated > 0) {
            Serial.printf("[DISP/SIP] Updated %u display(s)\n", updated);
            flushDirty();
        }
        return true;
    }

    // Returns true if topic was handled (display namespace).
    bool handleMqtt(const String& topic, const String& payload) {
        String prefix = String("railway/") + _cfg->cfg.hostname + "/display/";
        if (!topic.startsWith(prefix)) return false;

        String rest = topic.substring(prefix.length());
        int slash = rest.indexOf('/');
        if (slash < 0) return true;

        String id   = rest.substring(0, slash);
        String path = rest.substring(slash + 1);
        int idx = _findById(id);
        if (idx < 0) return true;

        RuntimeDisplay& rd = _rt[idx];

        if (path.startsWith("platform/")) {
            if (rd.data.type != DisplayType::PLATFORM) return true;
            String field = path.substring(9);
            if      (field == "destination")  _setStr(rd.data.destination, payload, sizeof(rd.data.destination));
            else if (field == "time")           _setStr(rd.data.departureTime, payload, sizeof(rd.data.departureTime));
            else if (field == "status")         _setStr(rd.data.status, payload, sizeof(rd.data.status));
            else if (field == "num")            rd.data.platformNum = constrain(payload.toInt(), 1, 99);
        } else if (path.startsWith("timetable/")) {
            if (rd.data.type != DisplayType::TIMETABLE) return true;
            if (path == "timetable/station") {
                _setStr(rd.data.stationName, payload, sizeof(rd.data.stationName));
            } else if (path.startsWith("timetable/row/")) {
                // timetable/row/N/field
                int p2 = path.indexOf('/', 15);
                if (p2 < 0) return true;
                uint8_t row = static_cast<uint8_t>(path.substring(15, p2).toInt());
                String field = path.substring(p2 + 1);
                if (row >= DisplayConfig::MAX_ROWS) return true;
                if (row >= rd.data.numRows) rd.data.numRows = row + 1;
                TimetableRowConfig& r = rd.data.rows[row];
                if      (field == "time") _setStr(r.timeStr, payload, sizeof(r.timeStr));
                else if (field == "dest") _setStr(r.destination, payload, sizeof(r.destination));
                else if (field == "bin")  _setStr(r.platformBin, payload, sizeof(r.platformBin));
                else if (field == "status") _setStr(r.status, payload, sizeof(r.status));
            }
        }

        rd.dirty = true;
        return true;
    }

    /** Update platform display fields (scenario / automation). */
    bool updatePlatform(const String& id, const char* status,
                        const char* destination = nullptr,
                        const char* time = nullptr) {
        int idx = _findById(id);
        if (idx < 0) return false;
        RuntimeDisplay& rd = _rt[idx];
        if (rd.data.type != DisplayType::PLATFORM) return false;
        if (status && status[0])
            strlcpy(rd.data.status, status, sizeof(rd.data.status));
        if (destination)
            strlcpy(rd.data.destination, destination, sizeof(rd.data.destination));
        if (time)
            strlcpy(rd.data.departureTime, time, sizeof(rd.data.departureTime));
        rd.dirty = true;
        return true;
    }

    void appendStatus(JsonObject parent) const {
        JsonArray arr = parent["displays"].to<JsonArray>();
        for (uint8_t i = 0; i < _count; i++) {
            const RuntimeDisplay& rd = _rt[i];
            JsonObject o = arr.add<JsonObject>();
            o["id"]       = rd.data.id;
            o["type"]       = (rd.data.type == DisplayType::TIMETABLE) ? "timetable" : "platform";
            o["i2c_addr"]   = rd.data.i2cAddr;
            o["present"]    = rd.present;
            o["platform_num"] = rd.data.platformNum;
            if (rd.data.stationId[0]) o["station_id"] = rd.data.stationId;
            o["destination"]  = rd.data.destination;
            o["departure_time"] = rd.data.departureTime;
            o["status"]       = rd.data.status;
            o["station_name"] = rd.data.stationName;
            if (rd.data.type == DisplayType::TIMETABLE) {
                JsonArray rows = o["rows"].to<JsonArray>();
                for (uint8_t r = 0; r < rd.data.numRows; r++) {
                    JsonObject ro = rows.add<JsonObject>();
                    ro["time"]        = rd.data.rows[r].timeStr;
                    ro["destination"] = rd.data.rows[r].destination;
                    ro["platform"]    = rd.data.rows[r].platformBin;
                    ro["status"]      = rd.data.rows[r].status;
                }
            }
        }
    }

private:
    enum class PlatformView : uint8_t {
        SIMPLE = 0,
        DETAIL,
        DEPARTURES,
        WAITING
    };

    struct RuntimeDisplay {
        DisplayConfig data;
        bool     present    = false;
        bool     dirty      = true;
        uint8_t  scrollRow  = 0;
        unsigned long lastScrollMs = 0;
        PlatformView platformView = PlatformView::SIMPLE;
        char     scrollStops[96] = "";
        int16_t  scrollPos = 128;
        unsigned long lastPlatformScrollMs = 0;
#ifdef USE_DISPLAY
        U8G2_SH1106_128X64_NONAME_F_HW_I2C* u8g2 = nullptr;
#endif
    };

    ConfigManager*  _cfg         = nullptr;
    const HardwareProbe* _hw     = nullptr;
    I2cBus*         _bus         = nullptr;
    RuntimeDisplay  _rt[BoardConfig::MAX_DISPLAYS];
    uint8_t         _count       = 0;
    unsigned long   _lastDrawMs  = 0;

    static void _setStr(char* dst, const String& src, size_t n) {
        strlcpy(dst, src.c_str(), n);
    }

    static bool _probePresent(uint8_t addr, const HardwareProbe& hw,
                            I2cBus& bus, uint8_t slot) {
        if (bus.deviceAt(addr, slot)) return true;
        if (slot == 0) {
            if (addr == 0x3C && hw.oled3c) return true;
            if (addr == 0x3D && hw.oled3d) return true;
        }
        return false;
    }

    bool _hasTimetable() const {
        for (uint8_t i = 0; i < _count; i++)
            if (_rt[i].data.type == DisplayType::TIMETABLE) return true;
        return false;
    }

    int _findById(const String& id) const {
        for (uint8_t i = 0; i < _count; i++)
            if (id.equalsIgnoreCase(_rt[i].data.id)) return i;
        return -1;
    }

    static void _parseDisplayJson(DisplayConfig& d, JsonObject dp) {
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

    void _initRuntimeOled(RuntimeDisplay& rd) {
#ifdef USE_DISPLAY
        if (!rd.present) return;
        uint8_t slot = rd.data.i2cSlot < I2C_SLOTS ? rd.data.i2cSlot : 0;
        if (_bus) _bus->selectSlot(slot);

        if (!rd.u8g2) {
            rd.u8g2 = new U8G2_SH1106_128X64_NONAME_F_HW_I2C(
                U8G2_R0, U8X8_PIN_NONE, I2C_SCL, I2C_SDA);
        }
        rd.u8g2->setI2CAddress(static_cast<uint8_t>(rd.data.i2cAddr << 1));
        rd.u8g2->begin();
        rd.u8g2->setContrast(255);
        rd.u8g2->clearBuffer();
        rd.u8g2->sendBuffer();
        Serial.printf("[DISP] OLED '%s' slot %u @ 0x%02X (%s)\n",
                      rd.data.id, slot, rd.data.i2cAddr,
                      rd.data.type == DisplayType::TIMETABLE ? "timetable" : "platform");
#endif
    }

    bool _upsertRuntimeFromJson(JsonObject dp, String& err) {
        if (!_hw || !_bus) {
            err = "Display manager non inizializzato";
            return false;
        }
        DisplayConfig tmp = {};
        _parseDisplayJson(tmp, dp);
        if (!tmp.id[0]) {
            err = "ID display mancante";
            return false;
        }

        int idx = _findById(tmp.id);
        if (idx < 0) {
            if (_count >= BoardConfig::MAX_DISPLAYS) {
                err = "Troppi display (salva e riavvia)";
                return false;
            }
            idx = _count++;
            _rt[idx] = {};
        }

        RuntimeDisplay& rd = _rt[idx];
        const bool addrChanged = rd.data.i2cAddr != tmp.i2cAddr ||
                                 rd.data.i2cSlot != tmp.i2cSlot;
        rd.data = tmp;
        uint8_t slot = rd.data.i2cSlot < I2C_SLOTS ? rd.data.i2cSlot : 0;
        if (!rd.present || addrChanged)
            rd.present = _probePresent(rd.data.i2cAddr, *_hw, *_bus, slot);
        rd.dirty = true;
        _initRuntimeOled(rd);
        return true;
    }

    static bool _ieq(const char* a, const char* b) {
        if (!a || !b || !a[0] || !b[0]) return false;
        return strcasecmp(a, b) == 0;
    }

    static bool _matchStation(const DisplayConfig& d, JsonObject msg) {
        const char* sid = msg["station_id"] | "";
        const char* sn  = msg["station_name"] | msg["station"] | "";

        if (sid[0] && d.stationId[0] && _ieq(sid, d.stationId)) return true;
        if (sn[0] && d.stationName[0] && _ieq(sn, d.stationName)) return true;
        if (sid[0] && d.stationName[0] && _ieq(sid, d.stationName)) return true;
        return false;
    }

    static bool _displayHasStationFilter(const DisplayConfig& d) {
        return d.stationId[0] != '\0' || d.stationName[0] != '\0';
    }

    bool _matchInfoPlatform(const DisplayConfig& d, JsonObject msg) const {
        if (d.type != DisplayType::PLATFORM) return false;

        if (msg["display_id"].is<const char*>()) {
            const char* id = msg["display_id"];
            return id[0] && _ieq(id, d.id);
        }

        int plat = -1;
        if (msg["platform"].is<const char*>())
            plat = atoi(msg["platform"].as<const char*>());
        else if (msg["platform_num"].is<const char*>())
            plat = atoi(msg["platform_num"].as<const char*>());
        else
            plat = msg["platform"] | msg["platform_num"] | -1;
        if (plat < 0 || static_cast<uint8_t>(plat) != d.platformNum) return false;

        if (msg["station_id"].is<const char*>() || msg["station"].is<const char*>() ||
            msg["station_name"].is<const char*>()) {
            return _matchStation(d, msg);
        }

        return !_displayHasStationFilter(d);
    }

    bool _matchInfoTimetable(const DisplayConfig& d, JsonObject msg) const {
        if (d.type != DisplayType::TIMETABLE) return false;

        if (msg["display_id"].is<const char*>()) {
            const char* id = msg["display_id"];
            return id[0] && _ieq(id, d.id);
        }

        bool msgHasStation = msg["station_id"].is<const char*>() ||
                             msg["station"].is<const char*>() ||
                             msg["station_name"].is<const char*>();
        if (msgHasStation) return _matchStation(d, msg);
        return !_displayHasStationFilter(d);
    }

    static void _applyPlatformFields(DisplayConfig& d, JsonObject msg) {
        const char* dest = msg["destination"] | msg["dest"] | nullptr;
        const char* time = msg["departure_time"] | msg["time"] | nullptr;
        const char* st   = msg["status"] | nullptr;
        int plat         = -1;
        if (msg["platform"].is<const char*>())
            plat = atoi(msg["platform"].as<const char*>());
        else if (msg["platform_num"].is<const char*>())
            plat = atoi(msg["platform_num"].as<const char*>());
        else
            plat = msg["platform"] | msg["platform_num"] | -1;

        if (dest) strlcpy(d.destination, dest, sizeof(d.destination));
        if (time)  strlcpy(d.departureTime, time, sizeof(d.departureTime));
        if (st)    strlcpy(d.status, st, sizeof(d.status));
        if (plat >= 1 && plat <= 99) d.platformNum = static_cast<uint8_t>(plat);
    }

    static void _resetPlatformView(RuntimeDisplay& rd) {
        rd.platformView = PlatformView::SIMPLE;
        rd.scrollStops[0] = '\0';
        rd.scrollPos = 128;
    }

    static bool _matchBinario(const DisplayConfig& d, JsonObject f) {
        if (f["binario"].is<const char*>())
            return static_cast<uint8_t>(atoi(f["binario"])) == d.platformNum;
        if (f["binario"].is<int>())
            return f["binario"].as<int>() == static_cast<int>(d.platformNum);
        return false;
    }

    static bool _matchFermata(const DisplayConfig& d, JsonObject f) {
        const char* st = f["stazione"] | "";
        if (!st[0]) return false;
        if (!_matchBinario(d, f)) return false;
        if (d.stationName[0] && _ieq(st, d.stationName)) return true;
        if (d.stationId[0] && _ieq(st, d.stationId)) return true;
        return false;
    }

    static void _buildScrollStops(const DisplayConfig& d, JsonObject fermataLocale,
                                  JsonObject treno, char* buf, size_t n) {
        strlcpy(buf, "Ferma a: ", n);
        bool dopoDiMe = false;
        for (JsonObject f : treno["fermate"].as<JsonArray>()) {
            const char* nome = f["stazione"] | "";
            if (dopoDiMe) {
                if (strcmp(buf, "Ferma a: ") != 0) strlcat(buf, " - ", n);
                const char* orario = f["orario"] | "";
                if (orario[0]) {
                    strlcat(buf, orario, n);
                    strlcat(buf, " ", n);
                }
                strlcat(buf, nome, n);
            }
            if (_ieq(nome, d.stationName) ||
                (d.stationId[0] && _ieq(nome, d.stationId)) ||
                _ieq(nome, fermataLocale["stazione"] | ""))
                dopoDiMe = true;
        }
    }

    static void _fillDeparturesAtStation(DisplayConfig& d, JsonArray treni) {
        d.numRows = 0;
        for (JsonObject t : treni) {
            for (JsonObject f : t["fermate"].as<JsonArray>()) {
                const char* st = f["stazione"] | "";
                if (!st[0]) continue;
                if (d.stationName[0] && !_ieq(st, d.stationName) &&
                    !(d.stationId[0] && _ieq(st, d.stationId)))
                    continue;
                if (d.numRows >= DisplayConfig::MAX_ROWS) return;
                TimetableRowConfig& r = d.rows[d.numRows++];
                strlcpy(r.timeStr, f["orario"] | "--:--", sizeof(r.timeStr));
                strlcpy(r.destination, t["destinazione"] | "Treno", sizeof(r.destination));
                const char* bin = f["binario"] | "-";
                strlcpy(r.platformBin, bin, sizeof(r.platformBin));
                r.status[0] = '\0';
            }
        }
    }

    uint8_t _applyInfoRailTreni(JsonArray treni) {
        if (treni.isNull()) return 0;
        uint8_t n = 0;
        for (uint8_t i = 0; i < _count; i++) {
            RuntimeDisplay& rd = _rt[i];
            if (rd.data.type != DisplayType::PLATFORM) continue;

            bool trovato = false;
            for (JsonObject treno : treni) {
                for (JsonObject f : treno["fermate"].as<JsonArray>()) {
                    if (!_matchFermata(rd.data, f)) continue;
                    const int mins = treno["minutiMancanti"] | 99;
                    const char* dest = treno["destinazione"] | "DEST.";
                    const char* orario = f["orario"] | "--:--";
                    strlcpy(rd.data.destination, dest, sizeof(rd.data.destination));
                    strlcpy(rd.data.departureTime, orario, sizeof(rd.data.departureTime));
                    rd.data.status[0] = '\0';

                    if (mins <= 10) {
                        rd.platformView = PlatformView::DETAIL;
                        _buildScrollStops(rd.data, f, treno, rd.scrollStops, sizeof(rd.scrollStops));
                        rd.scrollPos = 128;
                    } else {
                        rd.platformView = PlatformView::DEPARTURES;
                        rd.scrollStops[0] = '\0';
                        _fillDeparturesAtStation(rd.data, treni);
                    }
                    trovato = true;
                    break;
                }
                if (trovato) break;
            }
            if (!trovato) {
                rd.platformView = PlatformView::WAITING;
                rd.scrollStops[0] = '\0';
                rd.data.destination[0] = '\0';
                rd.data.departureTime[0] = '\0';
                rd.data.status[0] = '\0';
            }
            rd.dirty = true;
            n++;
        }
        return n;
    }

    static void _applyTimetableRow(TimetableRowConfig& row, JsonObject ro) {
        const char* t = ro["time"] | "";
        const char* dest = ro["destination"] | ro["dest"] | "";
        const char* bin = ro["platform"] | ro["bin"] | ro["platform_num"] | "";
        const char* st = ro["status"] | "";
        if (t[0])    strlcpy(row.timeStr, t, sizeof(row.timeStr));
        if (dest[0]) strlcpy(row.destination, dest, sizeof(row.destination));
        if (bin[0])  strlcpy(row.platformBin, bin, sizeof(row.platformBin));
        if (st[0])   strlcpy(row.status, st, sizeof(row.status));
    }

    uint8_t _applyInfoPlatform(JsonObject msg) {
        uint8_t n = 0;
        for (uint8_t i = 0; i < _count; i++) {
            RuntimeDisplay& rd = _rt[i];
            if (!_matchInfoPlatform(rd.data, msg)) continue;
            _applyPlatformFields(rd.data, msg);
            _resetPlatformView(rd);
            rd.dirty = true;
            n++;
        }
        return n;
    }

    uint8_t _applyInfoTimetable(JsonObject msg) {
        const char* stName = msg["station_name"] | msg["station"] | nullptr;
        JsonArray rows = msg["rows"].as<JsonArray>();
        if (rows.isNull()) return 0;

        uint8_t n = 0;
        for (uint8_t i = 0; i < _count; i++) {
            RuntimeDisplay& rd = _rt[i];
            if (!_matchInfoTimetable(rd.data, msg)) continue;

            if (stName)
                strlcpy(rd.data.stationName, stName, sizeof(rd.data.stationName));

            uint8_t rowIdx = 0;
            for (JsonObject ro : rows) {
                if (rowIdx >= DisplayConfig::MAX_ROWS) break;
                _applyTimetableRow(rd.data.rows[rowIdx], ro);
                rowIdx++;
            }
            rd.data.numRows = rowIdx ? rowIdx : rd.data.numRows;
            rd.dirty = true;
            n++;
        }
        return n;
    }

    uint8_t _processInfoPayload(JsonObject doc) {
        uint8_t updated = 0;

        if (doc["platforms"].is<JsonArray>()) {
            const char* stId = doc["station_id"] | doc["station"] | "";
            const char* stName = doc["station_name"] | doc["station"] | "";
            for (JsonObject item : doc["platforms"].as<JsonArray>()) {
                if (stId[0] && !item["station_id"].is<const char*>())
                    item["station_id"] = stId;
                if (stName[0] && !item["station_name"].is<const char*>() &&
                    !item["station"].is<const char*>())
                    item["station_name"] = stName;
                updated += _applyInfoPlatform(item);
            }
        }

        JsonObject tt = doc["timetable"].is<JsonObject>()
            ? doc["timetable"].as<JsonObject>() : doc;
        const char* type = doc["type"] | tt["type"] | "";
        if (strcmp(type, "timetable") == 0 || tt["rows"].is<JsonArray>() ||
            doc["rows"].is<JsonArray>()) {
            updated += _applyInfoTimetable(tt);
        }

        if (doc["platform"].is<int>() || doc["platform"].is<const char*>() ||
            doc["platform_num"].is<int>() || doc["platform_num"].is<const char*>() ||
            doc["display_id"].is<const char*>()) {
            updated += _applyInfoPlatform(doc);
        }

        if (doc["treni"].is<JsonArray>())
            updated += _applyInfoRailTreni(doc["treni"].as<JsonArray>());

        return updated;
    }

    static uint8_t _visibleRows(const DisplayConfig& d) {
        uint8_t n = d.numRows;
        if (n == 0) n = 1;
        return n;
    }

    static void _truncate(char* s, size_t maxLen) {
        if (strlen(s) <= maxLen) return;
        if (maxLen < 4) { s[maxLen] = '\0'; return; }
        s[maxLen - 1] = '.';
        s[maxLen - 2] = '.';
        s[maxLen - 3] = '.';
        s[maxLen] = '\0';
    }

    static bool _clockStr(char* buf, size_t n) {
        struct tm ti;
        if (!getLocalTime(&ti, 0)) {
            strlcpy(buf, "--:--", n);
            return false;
        }
        snprintf(buf, n, "%02d:%02d", ti.tm_hour, ti.tm_min);
        return true;
    }

#ifdef USE_DISPLAY
    void _drawPlatformSimple(U8G2& u8g2, const DisplayConfig& d) {
        u8g2.clearBuffer();
        u8g2.setFont(u8g2_font_6x10_tf);
        u8g2.drawStr(0, 10, "BINARIO");
        u8g2.setFont(u8g2_font_logisoso16_tf);
        char num[8];
        snprintf(num, sizeof(num), "%u", d.platformNum);
        u8g2.drawStr(72, 22, num);
        u8g2.setFont(u8g2_font_6x10_tf);
        char dest[24];
        strlcpy(dest, d.destination[0] ? d.destination : "—", sizeof(dest));
        _truncate(dest, 20);
        u8g2.drawStr(0, 36, dest);
        char line[32];
        snprintf(line, sizeof(line), "%s  %s",
                 d.departureTime[0] ? d.departureTime : "--:--",
                 d.status[0] ? d.status : "libero");
        _truncate(line, 20);
        u8g2.drawStr(0, 58, line);
        u8g2.sendBuffer();
    }

    void _drawPlatformDetail(U8G2& u8g2, const RuntimeDisplay& rd) {
        const DisplayConfig& d = rd.data;
        u8g2.clearBuffer();
        u8g2.setFont(u8g2_font_6x10_tf);
        char hdr[40];
        const char* st = d.stationName[0] ? d.stationName : d.stationId;
        snprintf(hdr, sizeof(hdr), "%s - BIN %u", st[0] ? st : "?", d.platformNum);
        _truncate(hdr, 20);
        u8g2.drawStr(0, 10, hdr);
        u8g2.drawHLine(0, 12, 128);

        u8g2.setFont(u8g2_font_logisoso16_tf);
        char dest[32];
        strlcpy(dest, d.destination[0] ? d.destination : "DEST.", sizeof(dest));
        _truncate(dest, 14);
        u8g2.drawStr(0, 30, dest);

        u8g2.setFont(u8g2_font_6x10_tf);
        u8g2.drawStr(0, 46, d.departureTime[0] ? d.departureTime : "--:--");
        u8g2.drawHLine(0, 50, 128);
        u8g2.drawStr(rd.scrollPos, 62, rd.scrollStops[0] ? rd.scrollStops : "");
        u8g2.sendBuffer();
    }

    void _drawPlatformDepartures(U8G2& u8g2, const DisplayConfig& d) {
        u8g2.clearBuffer();
        u8g2.setFont(u8g2_font_6x10_tf);
        u8g2.drawStr(0, 10, "PROSSIME PARTENZE");
        u8g2.drawHLine(0, 12, 128);
        int y = 24;
        const uint8_t rows = d.numRows ? d.numRows : 0;
        for (uint8_t i = 0; i < rows && y <= 60; i++) {
            const TimetableRowConfig& r = d.rows[i];
            char line[40];
            snprintf(line, sizeof(line), "%s %-10s %s",
                     r.timeStr[0] ? r.timeStr : "00:00",
                     r.destination[0] ? r.destination : "Treno",
                     r.platformBin[0] ? r.platformBin : "-");
            _truncate(line, 21);
            u8g2.drawStr(0, y, line);
            y += 12;
        }
        u8g2.sendBuffer();
    }

    void _drawPlatformWaiting(U8G2& u8g2) {
        u8g2.clearBuffer();
        u8g2.setFont(u8g2_font_6x10_tf);
        u8g2.drawStr(16, 28, "IN ATTESA DI");
        u8g2.drawStr(12, 42, "INFORMAZIONI");
        u8g2.sendBuffer();
    }

    void _drawPlatform(U8G2& u8g2, const RuntimeDisplay& rd) {
        switch (rd.platformView) {
            case PlatformView::DETAIL:     _drawPlatformDetail(u8g2, rd); break;
            case PlatformView::DEPARTURES: _drawPlatformDepartures(u8g2, rd.data); break;
            case PlatformView::WAITING:    _drawPlatformWaiting(u8g2); break;
            default:                       _drawPlatformSimple(u8g2, rd.data); break;
        }
    }

    void _drawTimetable(U8G2& u8g2, RuntimeDisplay& rd) {
        u8g2.clearBuffer();
        char clk[8];
        _clockStr(clk, sizeof(clk));
        u8g2.setFont(u8g2_font_6x10_tf);
        char hdr[32];
        snprintf(hdr, sizeof(hdr), "%s", rd.data.stationName);
        _truncate(hdr, 14);
        u8g2.drawStr(0, 10, hdr);
        u8g2.drawStr(90, 10, clk);
        u8g2.drawHLine(0, 13, 128);

        uint8_t y = 26;
        uint8_t start = rd.scrollRow;
        uint8_t rows = rd.data.numRows ? rd.data.numRows : 1;
        for (uint8_t i = 0; i < 3 && (start + i) < rows; i++) {
            const TimetableRowConfig& row = rd.data.rows[start + i];
            char line[32];
            if (row.timeStr[0] || row.destination[0]) {
                snprintf(line, sizeof(line), "%s %s",
                         row.timeStr[0] ? row.timeStr : "--:--",
                         row.destination[0] ? row.destination : "—");
            } else {
                strlcpy(line, "—", sizeof(line));
            }
            _truncate(line, 18);
            u8g2.drawStr(0, y, line);
            if (row.platformBin[0]) {
                char bin[8];
                snprintf(bin, sizeof(bin), "B%s", row.platformBin);
                u8g2.drawStr(100, y, bin);
            }
            y += 14;
        }
        u8g2.sendBuffer();
    }

    void _draw(RuntimeDisplay& rd) {
        if (!rd.u8g2 || !rd.present) return;
        uint8_t slot = rd.data.i2cSlot < I2C_SLOTS ? rd.data.i2cSlot : 0;
        if (_bus && !_bus->selectSlot(slot)) return;
        if (rd.data.type == DisplayType::PLATFORM)
            _drawPlatform(*rd.u8g2, rd);
        else
            _drawTimetable(*rd.u8g2, rd);
    }
#endif
};
