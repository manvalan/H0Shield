#pragma once
#include <Arduino.h>
#include "Config.h"
#include "MuxDriver.h"

// ── Base channel abstraction ─────────────────────────────────────────
class IChannel {
public:
    uint8_t ch;
    explicit IChannel(uint8_t channel) : ch(channel) {}
    virtual ~IChannel() = default;
    virtual void update(MuxDriver& mux) = 0;
};

// ── Absorption / occupancy sensor ───────────────────────────────────
class SensorChannel : public IChannel {
public:
    static constexpr uint16_t THRESHOLD = 512;   // tune per layout

    uint16_t rawValue   = 0;
    bool     occupied   = false;

    using IChannel::IChannel;

    void update(MuxDriver& mux) override {
        mux.selectChannel(ch);
        pinMode(MUX_PIN_SIG, INPUT);
        rawValue = mux.readAnalog();
        occupied = rawValue > THRESHOLD;
    }
};

// ── Relay output ─────────────────────────────────────────────────────
class RelayChannel : public IChannel {
public:
    bool state = false;

    using IChannel::IChannel;

    void setState(bool on, MuxDriver& mux) {
        state = on;
        mux.selectChannel(ch);
        pinMode(MUX_PIN_SIG, OUTPUT);
        mux.writeDigital(state);
    }

    void update(MuxDriver& mux) override {
        // Relays are write-only; refresh output to handle MUX contention recovery
        mux.selectChannel(ch);
        pinMode(MUX_PIN_SIG, OUTPUT);
        mux.writeDigital(state);
    }
};

// ── RGB Semaphore (uses 3 consecutive MUX channels: ch, ch+1, ch+2) ─
class SignalRGBChannel : public IChannel {
public:
    uint8_t r = 0, g = 0, b = 0;   // 0 = off, 1 = on (digital)

    using IChannel::IChannel;

    void setColor(bool red, bool green, bool blue, MuxDriver& mux) {
        r = red; g = green; b = blue;
        _writePin(ch,   red,   mux);
        _writePin(ch+1, green, mux);
        _writePin(ch+2, blue,  mux);
    }

    void update(MuxDriver& mux) override {
        _writePin(ch,   r, mux);
        _writePin(ch+1, g, mux);
        _writePin(ch+2, b, mux);
    }

private:
    void _writePin(uint8_t c, bool val, MuxDriver& mux) {
        mux.selectChannel(c);
        pinMode(MUX_PIN_SIG, OUTPUT);
        mux.writeDigital(val);
    }
};

// ── Marmotta (audio trigger – single pulse) ──────────────────────────
class MarmottaChannel : public IChannel {
public:
    using IChannel::IChannel;

    void trigger(MuxDriver& mux, uint32_t pulseMs = 200) {
        mux.selectChannel(ch);
        pinMode(MUX_PIN_SIG, OUTPUT);
        mux.writeDigital(true);
        delay(pulseMs);
        mux.writeDigital(false);
    }

    void update(MuxDriver&) override {}  // no periodic action
};
