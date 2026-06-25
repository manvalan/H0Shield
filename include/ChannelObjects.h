#pragma once
#include <Arduino.h>
#include <FastLED.h>
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
    static constexpr uint16_t DEFAULT_THRESHOLD = 512;
    uint16_t threshold = DEFAULT_THRESHOLD;

    uint16_t rawValue   = 0;
    bool     occupied   = false;

    using IChannel::IChannel;

    void update(MuxDriver& mux) override {
        mux.selectChannel(ch);
        pinMode(MUX_PIN_SIG, INPUT);
        rawValue = mux.readAnalog();
        occupied = rawValue > threshold;
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

// ── RGB Semaphore (3 independent MUX channels: chR, chG, chV) ───────
class SignalRGBChannel : public IChannel {
public:
    uint8_t chR = 0, chG = 0, chV = 0;
    uint8_t r = 0, g = 0, b = 0;   // 0 = off, 1 = on (digital)

    SignalRGBChannel(uint8_t rCh, uint8_t gCh, uint8_t vCh)
        : IChannel(rCh), chR(rCh), chG(gCh), chV(vCh) {}

    void setColor(bool red, bool green, bool blue, MuxDriver& mux) {
        r = red; g = green; b = blue;
        _writePin(chR, red,   mux);
        _writePin(chG, green, mux);
        _writePin(chV, blue,  mux);
    }

    void update(MuxDriver& mux) override {
        _writePin(chR, r, mux);
        _writePin(chG, g, mux);
        _writePin(chV, b, mux);
    }

private:
    void _writePin(uint8_t c, bool val, MuxDriver& mux) {
        mux.selectChannel(c);
        pinMode(MUX_PIN_SIG, OUTPUT);
        mux.writeDigital(val);
    }
};

// ── WS2812 serial RGB strip (FastLED, direct GPIO – NOT via MUX) ────
//
// The MUX channel index is used only as a config tag.
// The actual DATA pin is looked up in WS2812_GPIO_MAP (Config.h).
// Each strip gets its own CRGB array allocated on the heap.
class SerialRGBChannel : public IChannel {
public:
    CRGB*   leds    = nullptr;
    uint8_t numLeds = WS2812_MAX_LEDS;
    uint8_t gpio    = 0;
    bool    enabled = false;

    explicit SerialRGBChannel(uint8_t muxCh) : IChannel(muxCh) {
        for (uint8_t i = 0; i < WS2812_MAP_SIZE; i++) {
            if (WS2812_GPIO_MAP[i][0] == muxCh) {
                gpio = WS2812_GPIO_MAP[i][1];
                break;
            }
        }
        if (gpio == 0) {
            Serial.printf("[RGB] Ch%u: no GPIO mapping – strip skipped\n", muxCh);
            return;
        }
        enabled = true;
        leds = new CRGB[numLeds];
    }

    ~SerialRGBChannel() override { delete[] leds; }

    void begin() {
        if (!enabled || !leds) return;
        switch (gpio) {
            case  4: FastLED.addLeds<WS2812B, 4,  GRB>(leds, numLeds); break;
            case  5: FastLED.addLeds<WS2812B, 5,  GRB>(leds, numLeds); break;
            case 18: FastLED.addLeds<WS2812B, 18, GRB>(leds, numLeds); break;
            case 19: FastLED.addLeds<WS2812B, 19, GRB>(leds, numLeds); break;
            default:
                enabled = false;
                Serial.printf("[RGB] GPIO %u unsupported – strip disabled\n", gpio);
                return;
        }
        fill_solid(leds, numLeds, CRGB::Black);
        FastLED.show();
    }

    void setAll(CRGB color) {
        if (!enabled || !leds) return;
        fill_solid(leds, numLeds, color);
        FastLED.show();
    }

    void setPixel(uint8_t idx, CRGB color) {
        if (!enabled || !leds || idx >= numLeds) return;
        leds[idx] = color;
        FastLED.show();
    }

    void update(MuxDriver&) override {}
};

// ── Marmotta (audio trigger – non-blocking pulse) ────────────────────
class MarmottaChannel : public IChannel {
public:
    using IChannel::IChannel;

    void trigger(MuxDriver& mux, uint32_t pulseMs = 200) {
        if (_active) return;
        mux.selectChannel(ch);
        pinMode(MUX_PIN_SIG, OUTPUT);
        mux.writeDigital(true);
        _active   = true;
        _startMs  = millis();
        _duration = pulseMs;
    }

    void update(MuxDriver& mux) override {
        if (!_active) return;
        if (millis() - _startMs >= _duration) {
            mux.selectChannel(ch);
            pinMode(MUX_PIN_SIG, OUTPUT);
            mux.writeDigital(false);
            _active = false;
        }
    }

private:
    bool          _active   = false;
    unsigned long _startMs  = 0;
    uint32_t      _duration = 0;
};
