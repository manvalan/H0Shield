#pragma once
/**
 * Optional PCA9685-based signal driver.
 *
 * Compile-time guard: only included when USE_PCA9685 is defined in
 * platformio.ini build_flags (-D USE_PCA9685).
 */
#ifdef USE_PCA9685

#include <Arduino.h>
#include <Adafruit_PWMServoDriver.h>
#include "Config.h"

class PCA9685Signal {
public:
    String     rocrailId;
    SignalType type;

    uint16_t brR = 4095, brG = 4095, brV = 4095;
    bool     lampR = false, lampG = false, lampV = false;

    PCA9685Signal(const String& id, SignalType t,
                  Adafruit_PWMServoDriver* pca,
                  uint8_t pinR, uint8_t pinG, uint8_t pinV,
                  uint16_t brightnessR = 4095,
                  uint16_t brightnessG = 4095,
                  uint16_t brightnessV = 4095)
        : rocrailId(id), type(t), brR(brightnessR), brG(brightnessG), brV(brightnessV),
          _pca(pca), _pinR(pinR), _pinG(pinG), _pinV(pinV) {}

    void begin(SignalAspect bootAsp) {
        setAspect(bootAsp);
    }

    void setAspect(SignalAspect asp) {
        _aspect = asp;
        uint16_t r = 0, g = 0, v = 0;

        if (type == SignalType::MAIN) {
            switch (asp) {
                case SignalAspect::RED:    r = brR; break;
                case SignalAspect::GREEN:             v = brV; break;
                case SignalAspect::YELLOW:           g = brG; break;
                default:                  r = brR; break;
            }
        } else {
            switch (asp) {
                case SignalAspect::STOP:    g = brG; v = brV; break;
                case SignalAspect::GO:      r = brR; g = brG; break;
                case SignalAspect::OBLIQUE: r = brR;          v = brV; break;
                default:                   g = brG; v = brV; break;
            }
        }

        lampR = r > 0;
        lampG = g > 0;
        lampV = v > 0;
        _pca->setPWM(_pinR, 0, r);
        _pca->setPWM(_pinG, 0, g);
        _pca->setPWM(_pinV, 0, v);
    }

    SignalAspect currentAspect() const { return _aspect; }

    const char* aspectName() const {
        switch (_aspect) {
            case SignalAspect::RED:     return "red";
            case SignalAspect::GREEN:   return "green";
            case SignalAspect::YELLOW:  return "yellow";
            case SignalAspect::STOP:    return "stop";
            case SignalAspect::GO:      return "go";
            case SignalAspect::OBLIQUE: return "oblique";
            default:                    return "unknown";
        }
    }

private:
    Adafruit_PWMServoDriver* _pca;
    uint8_t  _pinR, _pinG, _pinV;
    SignalAspect _aspect = SignalAspect::RED;
};

#endif  // USE_PCA9685
