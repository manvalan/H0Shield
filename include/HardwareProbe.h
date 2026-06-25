#pragma once
#include <Arduino.h>
#include <Wire.h>
#include "Config.h"
#include "I2cBus.h"

/**
 * Non-blocking hardware discovery on the ShieldH0 I2C tree.
 */
class HardwareProbe {
public:
    bool vl6180x  = false;
    bool pca9685  = false;
    bool oled3c   = false;
    bool oled3d   = false;

    void scanI2C(I2cBus& bus) {
        bus.discoverAll();
        vl6180x = false;
        pca9685 = false;
        oled3c  = false;
        oled3d  = false;

        Serial.printf("[HW] I2C scan (mux=%s addr=0x%02X) …\n",
                      bus.hasMux ? "yes" : "no", bus.muxAddr);

        for (uint8_t s = 0; s < I2C_SLOTS; s++) {
            const I2cSlotDiscovery& d = bus.slots[s];
            if (!d.present) continue;
            Serial.printf("[HW]   slot U%d @ 0x%02X (%s)\n",
                          s + 9, d.addr, I2cBus::detectedName(d.type));
            if (d.type == I2cDetectedType::DET_VL6180X) vl6180x = true;
            if (d.addr == 0x3C) oled3c = true;
            if (d.addr == 0x3D) oled3d  = true;
            if (d.addr == 0x40) pca9685 = true;
        }

        if (!vl6180x && !oled3c && !oled3d && !pca9685) {
            Serial.println("[HW]   (no known devices – OK, I2C optional)");
        }
    }

    static bool deviceAt(uint8_t addr, I2cBus& bus, uint8_t slot = 0) {
        return bus.deviceAt(addr, slot);
    }
};
