#pragma once
#include <Arduino.h>
#include <Wire.h>
#include "Config.h"

/**
 * Non-blocking hardware discovery.
 * The board must boot and serve the web UI even when:
 *   - I2C bus is empty (no VL6180X, no PCA9685)
 *   - MUX has only some channels wired
 *   - MQTT broker is not configured yet
 */
class HardwareProbe {
public:
    bool vl6180x  = false;
    bool pca9685  = false;   // reserved for optional PWM board @ 0x40

    void scanI2C() {
        Wire.begin(I2C_SDA, I2C_SCL);
        Serial.printf("[HW] I2C scan SDA=%u SCL=%u …\n", I2C_SDA, I2C_SCL);

        uint8_t found = 0;
        for (uint8_t addr = 1; addr < 127; addr++) {
            Wire.beginTransmission(addr);
            if (Wire.endTransmission() == 0) {
                Serial.printf("[HW]   device @ 0x%02X\n", addr);
                found++;
                if (addr == VL6180X_ADDR) vl6180x = true;
                if (addr == 0x40)           pca9685 = true;
            }
        }
        if (found == 0) {
            Serial.println("[HW]   (bus empty – OK, I2C optional)");
        }
    }

    static bool deviceAt(uint8_t addr) {
        Wire.beginTransmission(addr);
        return Wire.endTransmission() == 0;
    }
};
