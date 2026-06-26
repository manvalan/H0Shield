#pragma once
#include <Arduino.h>
#include <Wire.h>
#include "Config.h"

#define I2C_SLOTS             8
#define TCA9548A_ADDR_DEFAULT 0x70

enum class I2cDetectedType : uint8_t {
    DET_NONE = 0,
    DET_VL6180X = 1,
    DET_DISPLAY = 2,
    DET_UNKNOWN = 3,
};

struct I2cSlotDiscovery {
    bool              present = false;
    I2cDetectedType   type    = I2cDetectedType::DET_NONE;
    uint8_t           addr    = 0;
};

enum class I2cSlotMode : uint8_t {
    MODE_AUTO    = 0,
    MODE_SKIP    = 1,
    MODE_TOF     = 2,
    MODE_DISPLAY = 3,
};

/**
 * Manages the ShieldH0 I2C sub-buses (U9–U16) via optional TCA9548A @ 0x70.
 * Without mux, only slot 0 maps to the main ESP32 I2C bus.
 */
class I2cBus {
public:
    bool  hasMux  = false;
    uint8_t muxAddr = TCA9548A_ADDR_DEFAULT;
    I2cSlotDiscovery slots[I2C_SLOTS] = {};

    void begin(uint8_t tcaAddr = TCA9548A_ADDR_DEFAULT) {
        (void)tcaAddr;
        muxAddr = TCA9548A_ADDR_DEFAULT;
        hasMux  = false;

        Wire.begin(I2C_SDA, I2C_SCL);
        Wire.setClock(100000);
        Wire.setTimeout(50);

        _detectMux();
        if (!hasMux) {
            Serial.println("[I2C] No TCA9548A – slot 0 = main bus");
        }
        discoverAll();
    }

    void _detectMux() {
        for (uint8_t a = 0x70; a <= 0x77; a++) {
            Wire.beginTransmission(a);
            if (Wire.endTransmission() == 0) {
                muxAddr = a;
                hasMux  = true;
                Serial.printf("[I2C] TCA9548A @ 0x%02X (%u slots)\n", a, I2C_SLOTS);
                return;
            }
        }
    }

    void deselectMux() {
        if (!hasMux) return;
        Wire.beginTransmission(muxAddr);
        Wire.write(static_cast<uint8_t>(0));
        Wire.endTransmission();
        delayMicroseconds(300);
    }

    bool selectSlot(uint8_t slot) {
        if (slot >= I2C_SLOTS) return false;
        if (!hasMux) return slot == 0;
        deselectMux();
        Wire.beginTransmission(muxAddr);
        Wire.write(static_cast<uint8_t>(1 << slot));
        if (Wire.endTransmission() != 0) return false;
        delayMicroseconds(500);
        return true;
    }

    void discoverAll() {
        if (!hasMux) _detectMux();
        for (uint8_t s = 0; s < I2C_SLOTS; s++) {
            slots[s] = discoverSlot(s);
        }
        deselectMux();
    }

    I2cSlotDiscovery discoverSlot(uint8_t slot) {
        I2cSlotDiscovery d;
        if (!selectSlot(slot)) return d;
        delay(5);

        const uint8_t known[] = {VL6180X_ADDR, 0x3C, 0x3D, 0x40};
        for (uint8_t addr : known) {
            if (_probeAddr(addr)) {
                d.present = true;
                d.addr    = addr;
                d.type    = _classify(addr);
                return d;
            }
        }

        for (uint8_t addr = 0x08; addr < 0x78; addr++) {
            if (addr >= 0x70 && addr <= 0x77) continue;
            if (_probeAddr(addr)) {
                d.present = true;
                d.addr    = addr;
                d.type    = _classify(addr);
                return d;
            }
        }
        return d;
    }

    bool deviceAt(uint8_t addr, uint8_t slot = 0) {
        if (!selectSlot(slot)) return false;
        return _probeAddr(addr);
    }

    static const char* modeName(I2cSlotMode m) {
        switch (m) {
            case I2cSlotMode::MODE_SKIP:    return "none";
            case I2cSlotMode::MODE_TOF:     return "tof";
            case I2cSlotMode::MODE_DISPLAY: return "display";
            default:                        return "auto";
        }
    }

    static I2cSlotMode parseMode(const char* s) {
        if (strcmp(s, "none") == 0)    return I2cSlotMode::MODE_SKIP;
        if (strcmp(s, "tof") == 0)     return I2cSlotMode::MODE_TOF;
        if (strcmp(s, "display") == 0) return I2cSlotMode::MODE_DISPLAY;
        return I2cSlotMode::MODE_AUTO;
    }

    static const char* detectedName(I2cDetectedType t) {
        switch (t) {
            case I2cDetectedType::DET_VL6180X: return "vl6180x";
            case I2cDetectedType::DET_DISPLAY: return "display";
            case I2cDetectedType::DET_UNKNOWN: return "unknown";
            default:                             return "none";
        }
    }

    /** Resolve effective device for a slot given config mode and last scan. */
    I2cDetectedType effectiveType(I2cSlotMode mode, uint8_t slot) const {
        if (mode == I2cSlotMode::MODE_SKIP) return I2cDetectedType::DET_NONE;
        if (mode == I2cSlotMode::MODE_TOF)  return I2cDetectedType::DET_VL6180X;
        if (mode == I2cSlotMode::MODE_DISPLAY) return I2cDetectedType::DET_DISPLAY;
        if (slot < I2C_SLOTS) return slots[slot].type;
        return I2cDetectedType::DET_NONE;
    }

private:
    static bool _probeAddr(uint8_t addr) {
        for (uint8_t attempt = 0; attempt < 2; attempt++) {
            Wire.beginTransmission(addr);
            if (Wire.endTransmission() == 0) return true;
            delayMicroseconds(250);
        }
        return false;
    }

    static I2cDetectedType _classify(uint8_t addr) {
        if (addr == VL6180X_ADDR) return I2cDetectedType::DET_VL6180X;
        if (addr == 0x3C || addr == 0x3D) return I2cDetectedType::DET_DISPLAY;
        return I2cDetectedType::DET_UNKNOWN;
    }
};
