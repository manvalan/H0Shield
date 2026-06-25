#pragma once
#include <Arduino.h>
#include <VL6180X.h>
#include <vector>
#include "MQTTManager.h"
#include "ConfigManager.h"
#include "LiveStatus.h"

class RocrailProtocol;

/**
 * VL6180X ToF on dedicated I2C bus.
 * Occupancy: distance_mm < threshold_mm → occupied (object close).
 * Above threshold → free.
 */
class ToFManager {
public:
    static constexpr unsigned long POLL_MS = 100;

    void begin(VL6180X& sensor, MQTTManager& mqtt, ConfigManager& cfg,
               RocrailProtocol* rocrail, LiveStatus* live, bool hwPresent) {
        _tof      = &sensor;
        _mqtt     = &mqtt;
        _cfg      = &cfg;
        _rocrail  = rocrail;
        _live     = live;
        _present  = hwPresent;
        if (_live) {
            _live->tof.present = hwPresent;
            _live->tof.enabled = cfg.cfg.tofEnabled && hwPresent;
            _live->tof.thresholdMm = cfg.cfg.tofThresholdMm;
        }
    }

    void loop() {
        if (!_present || !_tof || !_cfg->cfg.tofEnabled) return;
        if (millis() - _lastMs < POLL_MS) return;
        _lastMs = millis();

        uint8_t dist = _tof->readRangeSingleMillimeters();

        if (_tof->timeoutOccurred()) {
            _setValid(false, "timeout");
            return;
        }

        uint8_t errCode = _tof->readReg(VL6180X::RESULT__RANGE_STATUS) >> 4;
        if (errCode != 0) {
            _setValid(false, String("error:") + errCode);
            return;
        }

        _setValid(true, "ok");
        _dist = dist;

        if (abs((int)dist - (int)_prevDist) > 1) {
            _prevDist = dist;
            _mqtt->publish("tof/distance", String(dist));
        }

        if (millis() - _lastAmbientMs >= 1000) {
            _lastAmbientMs = millis();
            uint16_t als = _tof->readAmbientSingle();
            _mqtt->publish("tof/ambient", String(als));
        }

        const uint8_t th = _cfg->cfg.tofThresholdMm;
        const bool occ = (dist < th);
        if (occ != _occupied) {
            _occupied = occ;
            _publishOccupancy(occ);
        }

        if (_live) {
            _live->tof.distanceMm  = dist;
            _live->tof.thresholdMm = th;
            _live->tof.occupied    = occ;
            _live->tof.valid       = true;
            _live->tof.enabled     = true;
        }
    }

    bool occupied() const { return _occupied; }
    uint8_t distanceMm() const { return _dist; }

private:
    VL6180X*          _tof          = nullptr;
    MQTTManager*      _mqtt         = nullptr;
    ConfigManager*    _cfg          = nullptr;
    RocrailProtocol*  _rocrail      = nullptr;
    LiveStatus*       _live         = nullptr;
    bool              _present      = false;
    unsigned long     _lastMs       = 0;
    unsigned long     _lastAmbientMs= 0;
    uint8_t           _prevDist     = 255;
    uint8_t           _dist         = 0;
    bool              _occupied     = false;
    String            _prevStatus;

    void _setValid(bool ok, const String& status) {
        _publishStatus(status);
        if (_live) {
            _live->tof.valid   = ok;
            _live->tof.status  = status;
            _live->tof.enabled = _cfg->cfg.tofEnabled;
        }
    }

    void _publishStatus(const String& status) {
        if (status == _prevStatus) return;
        _prevStatus = status;
        _mqtt->publish("tof/status", status);
    }

    void _publishOccupancy(bool occupied);
};
