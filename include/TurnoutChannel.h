#pragma once
#include <Arduino.h>
#include "MuxDriver.h"
#include "Config.h"

/**
 * MUX-based turnout driver.
 *
 * Mirrors the TurnoutDriver pattern from TurnoutBoard but uses two
 * consecutive MUX relay channels instead of two PCA9685 channels.
 *
 * Wiring (per turnout):
 *   chStraight → relay coil A (straight position)
 *   chDiverge  → relay coil B (diverging position)
 *
 * Both coils are driven by a timed pulse (default 300 ms) to avoid
 * burning the solenoid.  Only one coil fires at a time.
 *
 * Usage in the MUX polling loop: call update() every loop iteration.
 */
class TurnoutChannel {
public:
    String   rocrailId;
    uint8_t  chStraight;
    uint8_t  chDiverge;
    uint32_t pulseDurationMs;
    bool     isStraight = true;

    TurnoutChannel(const String& id, uint8_t chS, uint8_t chD, uint32_t pulseMs = 300)
        : rocrailId(id), chStraight(chS), chDiverge(chD), pulseDurationMs(pulseMs) {}

    void begin(MuxDriver& mux) {
        _drivePin(chStraight, false, mux);
        _drivePin(chDiverge,  false, mux);
    }

    bool isBusy() const { return _activeChannel != 255; }

    void commandStraight(MuxDriver& mux) {
        if (isBusy()) return;
        _startPulse(chStraight, mux);
        isStraight = true;
    }

    void commandDiverge(MuxDriver& mux) {
        if (isBusy()) return;
        _startPulse(chDiverge, mux);
        isStraight = false;
    }

    // Must be called every loop iteration to cut the pulse in time
    void update(MuxDriver& mux) {
        if (_activeChannel == 255) return;
        if (millis() - _pulseStart >= pulseDurationMs) {
            _drivePin(_activeChannel, false, mux);
            _activeChannel = 255;
        }
    }

    const char* stateStr() const { return isStraight ? "straight" : "turnout"; }

private:
    uint8_t       _activeChannel = 255;   // 255 = idle
    unsigned long _pulseStart    = 0;

    void _startPulse(uint8_t ch, MuxDriver& mux) {
        _drivePin(ch, true, mux);
        _activeChannel = ch;
        _pulseStart    = millis();
    }

    void _drivePin(uint8_t ch, bool on, MuxDriver& mux) {
        mux.selectChannel(ch);
        pinMode(MUX_PIN_SIG, OUTPUT);
        mux.writeDigital(on);
    }
};
