#pragma once
#include <Arduino.h>
#include <ArduinoOTA.h>
#include "ConfigManager.h"

/**
 * Thin wrapper around ArduinoOTA.
 * Firmware update via:
 *   pio run -t upload --upload-port <hostname>.local
 * or the PlatformIO "Upload" button after setting upload_protocol = espota.
 *
 * Password = hostname (change if you need stronger security).
 */
class OTAManager {
public:
    void begin(ConfigManager& cfg) {
        ArduinoOTA.setHostname(cfg.cfg.hostname);
        ArduinoOTA.setPassword(cfg.cfg.hostname);   // simple default

        ArduinoOTA.onStart([]() {
            String type = (ArduinoOTA.getCommand() == U_FLASH) ? "firmware" : "filesystem";
            Serial.printf("[OTA] Start: %s\n", type.c_str());
        });
        ArduinoOTA.onEnd([]() {
            Serial.println("[OTA] Complete – rebooting");
        });
        ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
            Serial.printf("[OTA] %u%%\r", progress * 100 / total);
        });
        ArduinoOTA.onError([](ota_error_t e) {
            const char* msg;
            switch (e) {
                case OTA_AUTH_ERROR:    msg = "Auth failed";     break;
                case OTA_BEGIN_ERROR:   msg = "Begin failed";    break;
                case OTA_CONNECT_ERROR: msg = "Connect failed";  break;
                case OTA_RECEIVE_ERROR: msg = "Receive failed";  break;
                case OTA_END_ERROR:     msg = "End failed";      break;
                default:                msg = "Unknown";
            }
            Serial.printf("[OTA] Error[%u]: %s\n", e, msg);
        });

        ArduinoOTA.begin();
        Serial.printf("[OTA] Ready – password: %s\n", cfg.cfg.hostname);
    }

    void loop() { ArduinoOTA.handle(); }
};
