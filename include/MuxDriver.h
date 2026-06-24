#pragma once
#include <Arduino.h>
#include "Config.h"

/**
 * Driver for CD74HC4067 – 16-channel single-ended analog/digital MUX.
 *
 * Pin assignments (A0–A3) are defined in Config.h.
 * selectChannel() is a fast 4-bit GPIO write; subsequent reads/writes
 * on MUX_PIN_SIG apply to the selected channel.
 *
 * NOTE: because the MUX is shared between sensor reads and relay drives,
 * every caller MUST bracket its use: selectChannel() → read/write → done.
 * Do NOT hold a channel selected across yield()/delay() calls.
 */
class MuxDriver {
public:
    void begin() {
        pinMode(MUX_PIN_A0, OUTPUT);
        pinMode(MUX_PIN_A1, OUTPUT);
        pinMode(MUX_PIN_A2, OUTPUT);
        pinMode(MUX_PIN_A3, OUTPUT);
        // SIG pin direction is set per operation by callers
        selectChannel(0);
    }

    inline void selectChannel(uint8_t ch) {
        ch &= 0x0F;   // clamp to 0-15
        _currentChannel = ch;
        digitalWrite(MUX_PIN_A0, (ch >> 0) & 1);
        digitalWrite(MUX_PIN_A1, (ch >> 1) & 1);
        digitalWrite(MUX_PIN_A2, (ch >> 2) & 1);
        digitalWrite(MUX_PIN_A3, (ch >> 3) & 1);
        // Datasheet propagation delay: typ 9 ns – no explicit delay needed at Arduino speeds
    }

    // Read 12-bit ADC value from the currently selected channel
    uint16_t readAnalog() const {
        return analogRead(MUX_PIN_SIG);
    }

    // Read digital (HIGH/LOW) from currently selected channel
    bool readDigital() const {
        return digitalRead(MUX_PIN_SIG);
    }

    // Drive a digital output on the currently selected channel
    // (caller must set MUX_PIN_SIG as OUTPUT before using this)
    void writeDigital(bool state) const {
        digitalWrite(MUX_PIN_SIG, state ? HIGH : LOW);
    }

    uint8_t currentChannel() const { return _currentChannel; }

private:
    uint8_t _currentChannel = 0;
};
