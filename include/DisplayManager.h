#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <time.h>
#include "ConfigManager.h"
#include "HardwareProbe.h"
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
    void begin(ConfigManager& cfgMgr, const HardwareProbe& hw) {
        _cfg = &cfgMgr;
        _count = 0;

        for (uint8_t i = 0; i < cfgMgr.cfg.numDisplays && i < BoardConfig::MAX_DISPLAYS; i++) {
            RuntimeDisplay& rd = _rt[_count];
            rd.data   = cfgMgr.cfg.displays[i];
            rd.present = _probePresent(rd.data.i2cAddr, hw);
            rd.scrollRow = 0;
            rd.dirty = true;

#ifdef USE_DISPLAY
            if (rd.present) {
                rd.u8g2 = new U8G2_SH1106_128X64_NONAME_F_HW_I2C(
                    U8G2_R0, U8X8_PIN_NONE, I2C_SCL, I2C_SDA);
                rd.u8g2->setI2CAddress(static_cast<uint8_t>(rd.data.i2cAddr << 1));
                rd.u8g2->begin();
                rd.u8g2->setContrast(255);
                rd.u8g2->clearBuffer();
                rd.u8g2->sendBuffer();
                Serial.printf("[DISP] OLED '%s' @ 0x%02X (%s)\n",
                              rd.data.id, rd.data.i2cAddr,
                              rd.data.type == DisplayType::TIMETABLE ? "timetable" : "platform");
            }
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

            if (rd.dirty || now - _lastDrawMs >= 1000) {
                rd.dirty = false;
#ifdef USE_DISPLAY
                if (rd.u8g2) _draw(rd);
#endif
            }
        }
        _lastDrawMs = now;
    }

    void subscribeAll(MQTTManager& mqtt) {
        if (_count == 0 || !mqtt.isEnabled()) return;
        String topic = _cfg->topic("display/#");
        mqtt.subscribe(topic.c_str());
        Serial.printf("[DISP] Subscribed %s\n", topic.c_str());
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
    struct RuntimeDisplay {
        DisplayConfig data;
        bool     present    = false;
        bool     dirty      = true;
        uint8_t  scrollRow  = 0;
        unsigned long lastScrollMs = 0;
#ifdef USE_DISPLAY
        U8G2_SH1106_128X64_NONAME_F_HW_I2C* u8g2 = nullptr;
#endif
    };

    ConfigManager*  _cfg         = nullptr;
    RuntimeDisplay  _rt[BoardConfig::MAX_DISPLAYS];
    uint8_t         _count       = 0;
    unsigned long   _lastDrawMs  = 0;

    static void _setStr(char* dst, const String& src, size_t n) {
        strlcpy(dst, src.c_str(), n);
    }

    static bool _probePresent(uint8_t addr, const HardwareProbe& hw) {
        if (addr == 0x3C && hw.oled3c) return true;
        if (addr == 0x3D && hw.oled3d) return true;
        return HardwareProbe::deviceAt(addr);
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
    void _drawPlatform(U8G2& u8g2, const DisplayConfig& d) {
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
        if (rd.data.type == DisplayType::PLATFORM)
            _drawPlatform(*rd.u8g2, rd.data);
        else
            _drawTimetable(*rd.u8g2, rd);
    }
#endif
};
