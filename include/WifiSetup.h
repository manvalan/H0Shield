#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include "Config.h"
#include "ConfigManager.h"
#include "SecureStore.h"

/**
 * Boot WiFi strategy (no WiFiManager captive portal UI):
 *   1. Try saved credentials (NVS + our SecureStore backup)
 *   2. If fail → AP+STA mode: softAP for setup, web UI handles scan/connect
 */
inline bool connectSavedWiFi(ConfigManager& cfg, uint16_t timeoutSec = 20) {
    if (WiFi.SSID().isEmpty() && cfg.cfg.wifiSsid[0] != '\0') {
        String pass = SecureStore::loadWifiPassword();
        WiFi.mode(WIFI_STA);
        WiFi.begin(cfg.cfg.wifiSsid, pass.c_str());
    } else if (!WiFi.SSID().isEmpty()) {
        WiFi.mode(WIFI_STA);
        WiFi.begin();
    } else {
        return false;
    }

    Serial.printf("[WIFI] Connessione a '%s' …\n", cfg.cfg.wifiSsid[0] ? cfg.cfg.wifiSsid : WiFi.SSID().c_str());
    const unsigned long deadline = millis() + timeoutSec * 1000UL;
    while (WiFi.status() != WL_CONNECTED && millis() < deadline) {
        delay(250);
        Serial.print('.');
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        strlcpy(cfg.cfg.wifiSsid, WiFi.SSID().c_str(), sizeof(cfg.cfg.wifiSsid));
        Serial.printf("[WIFI] Connesso – IP: %s\n", WiFi.localIP().toString().c_str());
        return true;
    }
    return false;
}

inline void startSetupAP(ConfigManager& cfg) {
    const String apName = String("ShieldH0-") + cfg.cfg.hostname;
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(apName.c_str());
    Serial.println("[WIFI] Modalità setup – nessuna rete configurata");
    Serial.printf("[WIFI] 1) Connetti al WiFi: %s\n", apName.c_str());
    Serial.printf("[WIFI] 2) Apri http://%s/\n", WiFi.softAPIP().toString().c_str());
}

inline bool setupWiFi(ConfigManager& cfg) {
    WiFi.persistent(true);

    if (digitalRead(WIFI_RESET_PIN) == LOW) {
        Serial.println("[WIFI] Reset button – cancello credenziali");
        SecureStore::clearWifi();
        cfg.cfg.wifiSsid[0] = '\0';
        cfg.save();
    }

    if (connectSavedWiFi(cfg)) return true;

    startSetupAP(cfg);
    return true;   // AP mode: web UI available for configuration
}

inline bool connectToNetwork(ConfigManager& cfg, const String& ssid, const String& pass) {
    SecureStore::saveWifiPassword(pass);
    strlcpy(cfg.cfg.wifiSsid, ssid.c_str(), sizeof(cfg.cfg.wifiSsid));
    cfg.save();

    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());

    const unsigned long deadline = millis() + 20000;
    while (WiFi.status() != WL_CONNECTED && millis() < deadline) delay(200);

    return WiFi.status() == WL_CONNECTED;
}

inline void resetWifiAndRestart() {
    SecureStore::clearWifi();
    delay(200);
    ESP.restart();
}
