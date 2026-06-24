#pragma once
#include <Arduino.h>
#include <VL6180X.h>
#include "MQTTManager.h"
#include "ConfigManager.h"

/**
 * Manages a single VL6180X sensor on the dedicated I2C bus.
 * Publishes:
 *   railway/<name>/tof/distance   → mm (uint, 0-255)
 *   railway/<name>/tof/ambient    → raw ALS counts
 *   railway/<name>/tof/status     → "ok" | "timeout" | "error:<code>"
 */
class ToFManager {
public:
    static constexpr unsigned long POLL_MS = 100;   // 10 Hz

    void begin(VL6180X& sensor, MQTTManager& mqtt, ConfigManager& cfg) {
        _tof  = &sensor;
        _mqtt = &mqtt;
        _cfg  = &cfg;
    }

    void loop() {
        if (millis() - _lastMs < POLL_MS) return;
        _lastMs = millis();

        uint8_t dist = _tof->readRangeSingleMillimeters();

        if (_tof->timeoutOccurred()) {
            _mqtt->publish("tof/status", "timeout");
            return;
        }

        // VL6180X range: 0–100 mm typical; >200 usually means no target
        uint8_t errCode = _tof->readReg(VL6180X::RESULT__RANGE_STATUS) >> 4;
        if (errCode != 0) {
            String s = "error:" + String(errCode);
            _mqtt->publish("tof/status", s);
            return;
        }

        // Publish only on change (±1 mm hysteresis)
        if (abs((int)dist - (int)_prevDist) > 1) {
            _prevDist = dist;
            _mqtt->publish("tof/distance", String(dist));
        }

        // Ambient light every 1 s
        if (millis() - _lastAmbientMs >= 1000) {
            _lastAmbientMs = millis();
            uint16_t als = _tof->readAmbientSingle();
            _mqtt->publish("tof/ambient", String(als));
        }

        _mqtt->publish("tof/status", "ok");
    }

private:
    VL6180X*       _tof          = nullptr;
    MQTTManager*   _mqtt         = nullptr;
    ConfigManager* _cfg          = nullptr;
    unsigned long  _lastMs       = 0;
    unsigned long  _lastAmbientMs= 0;
    uint8_t        _prevDist     = 255;
};
