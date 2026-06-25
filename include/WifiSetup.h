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

struct WiFiAttemptResult {
    bool        ok     = false;
    wl_status_t status = WL_IDLE_STATUS;
    String      ip;
    int8_t      rssi   = 0;
};

inline const char* wifiStatusLabel(wl_status_t st) {
    switch (st) {
        case WL_CONNECTED:       return "connected";
        case WL_NO_SSID_AVAIL:   return "ssid_not_found";
        case WL_CONNECT_FAILED:  return "auth_failed";
        case WL_CONNECTION_LOST: return "connection_lost";
        case WL_DISCONNECTED:    return "disconnected";
        default:                 return "timeout";
    }
}

inline const char* wifiStatusMessageIt(wl_status_t st) {
    switch (st) {
        case WL_CONNECTED:       return "Connesso";
        case WL_NO_SSID_AVAIL:   return "Rete non trovata (SSID errato o fuori range)";
        case WL_CONNECT_FAILED:  return "Password errata o autenticazione fallita";
        case WL_CONNECTION_LOST: return "Connessione persa";
        case WL_DISCONNECTED:    return "Disconnesso";
        default:                 return "Timeout – la rete non risponde entro 18 s";
    }
}

inline bool _wifiApActive() {
    return WiFi.getMode() & WIFI_AP;
}

inline void _ensureSetupAp(ConfigManager& cfg) {
    if (_wifiApActive()) return;
    const String apName = String("ShieldH0-") + cfg.cfg.hostname;
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(apName.c_str());
}

/** Try SSID/password without saving. Keeps setup AP alive on failure. */
inline WiFiAttemptResult attemptWiFi(ConfigManager& cfg, const String& ssid,
                                     const String& pass) {
    WiFiAttemptResult r;

    _ensureSetupAp(cfg);

    WiFi.disconnect(false, true);
    delay(100);
    WiFi.begin(ssid.c_str(), pass.c_str());

    const unsigned long deadline = millis() + 18000;
    while (millis() < deadline) {
        wl_status_t st = WiFi.status();
        if (st == WL_CONNECTED) {
            r.ok     = true;
            r.status = st;
            r.ip     = WiFi.localIP().toString();
            r.rssi   = WiFi.RSSI();
            return r;
        }
        if (st == WL_CONNECT_FAILED || st == WL_NO_SSID_AVAIL) {
            r.status = st;
            WiFi.disconnect(false, true);
            _ensureSetupAp(cfg);
            return r;
        }
        delay(250);
    }

    r.status = WiFi.status();
    WiFi.disconnect(false, true);
    _ensureSetupAp(cfg);
    return r;
}

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
    WiFiAttemptResult r = attemptWiFi(cfg, ssid, pass);
    if (!r.ok) return false;

    SecureStore::saveWifiPassword(pass);
    strlcpy(cfg.cfg.wifiSsid, ssid.c_str(), sizeof(cfg.cfg.wifiSsid));
    cfg.save();
    return true;
}

/** Test credentials only – does not persist SSID/password. */
inline WiFiAttemptResult testWiFiNetwork(ConfigManager& cfg, const String& ssid,
                                         const String& pass) {
    WiFiAttemptResult r = attemptWiFi(cfg, ssid, pass);
    if (!r.ok) return r;
    // Stay connected so user can confirm; credentials saved only via connectToNetwork.
    return r;
}

inline void resetWifiAndRestart() {
    SecureStore::clearWifi();
    delay(200);
    ESP.restart();
}
