#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include "Config.h"
#include "ConfigManager.h"
#include "SecureStore.h"

/**
 * WiFi non volatile:
 *   - SSID + last_sta_ip → config.json (LittleFS)
 *   - Password           → NVS namespace "sh0sec"
 *   - Credenziali ESP    → NVS WiFi (persistent) dopo connessione OK
 *
 * Accesso web: preferire http://<IP-LAN>/ — mDNS *.local spesso non funziona.
 * Fallback: AP ShieldH0-<hostname> → http://192.168.4.1/
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
        case WL_DISCONNECTED:    return "Timeout – la rete non risponde entro 25 s";
        default:                 return "Timeout – la rete non risponde entro 25 s";
    }
}

inline bool wifiHasStaIp() {
    return WiFi.status() == WL_CONNECTED &&
           WiFi.localIP() != IPAddress(0, 0, 0, 0);
}

inline void wifiNormalizeHostname(const char* in, char* out, size_t n) {
    if (!in || !out || n < 2) return;
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 1 < n; i++) {
        char c = in[i];
        if (c == ' ') c = '-';
        else if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')
            out[j++] = c;
    }
    out[j] = '\0';
    if (j == 0) strlcpy(out, "shieldh0", n);
}

inline void wifiApplyHostname(ConfigManager& cfg) {
    char host[32];
    wifiNormalizeHostname(cfg.cfg.hostname, host, sizeof(host));
    WiFi.setHostname(host);
}

inline bool wifiSaveLastIp(ConfigManager& cfg) {
    if (!wifiHasStaIp()) return false;
    const String ip = WiFi.localIP().toString();
    if (ip == cfg.cfg.lastStaIp) return true;
    strlcpy(cfg.cfg.lastStaIp, ip.c_str(), sizeof(cfg.cfg.lastStaIp));
    return cfg.save();
}

inline void wifiPersistCredentials(const char* ssid, const char* pass) {
    SecureStore::saveWifiPassword(pass ? pass : "");
    WiFi.persistent(true);
    if (pass && pass[0])
        WiFi.begin(ssid, pass);
    else
        WiFi.begin(ssid);
}

inline void _startSetupAp(ConfigManager& cfg) {
    const String apName = String("ShieldH0-") + cfg.cfg.hostname;
    if (!(WiFi.getMode() & WIFI_AP))
        WiFi.mode(WIFI_AP_STA);
    else if (WiFi.getMode() != WIFI_AP_STA)
        WiFi.mode(WIFI_AP_STA);
    if (WiFi.softAPIP() == IPAddress(0, 0, 0, 0))
        WiFi.softAP(apName.c_str());
    Serial.printf("[WIFI] AP: %s → http://%s/\n",
                  apName.c_str(), WiFi.softAPIP().toString().c_str());
}

inline void _beginSta(const char* ssid, const char* pass) {
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.setSleep(false);
    if (pass && pass[0])
        WiFi.begin(ssid, pass);
    else
        WiFi.begin(ssid);
}

class WiFiJobManager {
public:
    enum class Phase : uint8_t { IDLE, SCANNING, CONNECTING, SUCCESS, FAILED };

    static constexpr uint32_t CONNECT_TIMEOUT_MS = 25000;
    static constexpr uint32_t SCAN_TIMEOUT_MS    = 15000;
    static constexpr uint32_t RESTART_DELAY_MS   = 2500;

    void begin(ConfigManager* cfg) { _cfg = cfg; }

    bool busy() const {
        return _phase == Phase::SCANNING || _phase == Phase::CONNECTING;
    }

    Phase phase() const { return _phase; }

    bool restartPending() const { return _restartAt != 0; }

    const char* phaseName() const {
        switch (_phase) {
            case Phase::SCANNING:   return "scanning";
            case Phase::CONNECTING: return "connecting";
            case Phase::SUCCESS:    return "success";
            case Phase::FAILED:     return "failed";
            default:                return "idle";
        }
    }

    void dismiss() {
        if (_phase == Phase::SUCCESS || _phase == Phase::FAILED) {
            _phase = Phase::IDLE;
            _restartAt = 0;
        }
    }

    bool startScan() {
        if (busy() || restartPending()) return false;
        if (_cfg) _startSetupAp(*_cfg);
        WiFi.scanDelete();
        WiFi.scanNetworks(true, true);
        _phase   = Phase::SCANNING;
        _started = millis();
        _message = "Scansione reti WiFi…";
        _saved   = false;
        _savedIp = "";
        _restartAt = 0;
        return true;
    }

    bool startConnect(const String& ssid, const String& pass, bool save) {
        if (busy() || restartPending() || !_cfg || ssid.isEmpty()) return false;

        _targetSsid    = ssid;
        _pendingPass   = pass;
        _saveOnSuccess = save;
        _saved         = false;
        _savedIp       = "";
        _restartAt     = 0;
        _lastStatus    = WL_IDLE_STATUS;
        _started       = millis();
        _deadline      = _started + CONNECT_TIMEOUT_MS;
        _phase         = Phase::CONNECTING;
        _message       = String("Connessione a ") + ssid + "…";

        wifiApplyHostname(*_cfg);
        WiFi.disconnect(true, false);
        delay(100);
        _beginSta(ssid.c_str(), pass.c_str());

        Serial.printf("[WIFI] Connect job → '%s' (save=%d)\n", ssid.c_str(), save);
        return true;
    }

    void loop() {
        if (_restartAt && millis() >= _restartAt) {
            Serial.println("[WIFI] Riavvio per applicare la nuova rete");
            delay(100);
            ESP.restart();
        }

        switch (_phase) {
            case Phase::SCANNING:   _loopScan(); break;
            case Phase::CONNECTING: _loopConnect(); break;
            default: break;
        }
    }

    void fillJobStatus(JsonObject doc) const {
        doc["job_phase"]      = phaseName();
        doc["job_busy"]       = busy();
        doc["job_message"]    = _message;
        doc["job_restart"]    = restartPending();
        doc["job_elapsed_ms"] = (_phase != Phase::IDLE && _started)
            ? static_cast<uint32_t>(millis() - _started) : 0;
        doc["job_timeout_ms"] = (_phase == Phase::CONNECTING) ? CONNECT_TIMEOUT_MS
                                 : (_phase == Phase::SCANNING) ? SCAN_TIMEOUT_MS : 0;
        if (_phase == Phase::SUCCESS) {
            const String ip = _savedIp.length() ? _savedIp : WiFi.localIP().toString();
            doc["job_ip"]    = ip;
            doc["job_rssi"]  = WiFi.RSSI();
            doc["job_saved"] = _saved;
            doc["job_web_url"] = String("http://") + ip + "/";
        }
        if (_phase == Phase::FAILED) {
            doc["job_status"]  = wifiStatusLabel(_lastStatus);
            doc["job_message"] = _message;
        }
    }

    bool fillScanResults(JsonDocument& doc) {
        int n = WiFi.scanComplete();
        if (n == -1) {
            doc["scanning"]   = true;
            doc["message"]    = _message;
            doc["elapsed_ms"] = millis() - _started;
            return false;
        }
        if (n == -2) return false;

        JsonArray arr = doc["networks"].to<JsonArray>();
        for (int i = 0; i < n; i++) {
            JsonObject o = arr.add<JsonObject>();
            o["ssid"]   = WiFi.SSID(i);
            o["rssi"]   = WiFi.RSSI(i);
            o["secure"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
            o["ch"]     = WiFi.channel(i);
        }
        doc["scanning"] = false;
        doc["count"]    = n;
        if (_phase == Phase::SCANNING)
            _phase = Phase::IDLE;
        return true;
    }

private:
    ConfigManager* _cfg           = nullptr;
    Phase          _phase         = Phase::IDLE;
    unsigned long  _started       = 0;
    unsigned long  _deadline      = 0;
    unsigned long  _restartAt     = 0;
    wl_status_t    _lastStatus    = WL_IDLE_STATUS;
    String         _message;
    String         _targetSsid;
    String         _pendingPass;
    String         _savedIp;
    bool           _saveOnSuccess = false;
    bool           _saved         = false;

    void _loopScan() {
        int n = WiFi.scanComplete();
        if (n == -1) {
            if (millis() - _started > SCAN_TIMEOUT_MS) {
                _phase    = Phase::FAILED;
                _message  = "Timeout scansione WiFi";
                _lastStatus = WL_DISCONNECTED;
            }
            return;
        }
        if (n == -2) {
            _phase   = Phase::FAILED;
            _message = "Scansione fallita";
            return;
        }
        _phase   = Phase::IDLE;
        _message = "";
        Serial.printf("[WIFI] Scan OK – %d reti\n", n);
    }

    void _loopConnect() {
        wl_status_t st = WiFi.status();
        _lastStatus = st;

        if (wifiHasStaIp()) {
            _savedIp = WiFi.localIP().toString();
            if (_saveOnSuccess && _cfg) {
                wifiPersistCredentials(_targetSsid.c_str(), _pendingPass.c_str());
                strlcpy(_cfg->cfg.wifiSsid, _targetSsid.c_str(), sizeof(_cfg->cfg.wifiSsid));
                strlcpy(_cfg->cfg.lastStaIp, _savedIp.c_str(), sizeof(_cfg->cfg.lastStaIp));
                if (!_cfg->save()) {
                    _phase   = Phase::FAILED;
                    _message = "Errore salvataggio configurazione";
                    _startSetupAp(*_cfg);
                    Serial.println("[WIFI] cfg.save() failed");
                    return;
                }
                _saved   = true;
                _phase   = Phase::SUCCESS;
                _message = String("Connesso — IP ") + _savedIp + " — riavvio…";
                _restartAt = millis() + RESTART_DELAY_MS;
                Serial.printf("[WIFI] Saved '%s' IP %s — restart %u ms\n",
                              _targetSsid.c_str(), _savedIp.c_str(), RESTART_DELAY_MS);
            } else {
                _phase   = Phase::SUCCESS;
                _message = String("Connesso — IP ") + _savedIp;
                if (_cfg) _startSetupAp(*_cfg);
            }
            return;
        }

        if (st == WL_CONNECT_FAILED || st == WL_NO_SSID_AVAIL) {
            WiFi.disconnect(true, false);
            if (_cfg) _startSetupAp(*_cfg);
            _phase   = Phase::FAILED;
            _message = wifiStatusMessageIt(st);
            return;
        }

        if (millis() >= _deadline) {
            WiFi.disconnect(true, false);
            if (_cfg) _startSetupAp(*_cfg);
            _phase      = Phase::FAILED;
            _lastStatus = WiFi.status();
            _message    = wifiStatusMessageIt(WL_DISCONNECTED);
            Serial.println("[WIFI] Connect timeout");
        }
    }
};

inline WiFiJobManager wifiJob;

inline bool connectSavedWiFi(ConfigManager& cfg, uint16_t timeoutSec = 25) {
    if (cfg.cfg.wifiSsid[0] == '\0') return false;

    wifiApplyHostname(cfg);
    String pass = SecureStore::loadWifiPassword();
    WiFi.persistent(true);
    WiFi.disconnect(true, false);
    delay(100);
    _beginSta(cfg.cfg.wifiSsid, pass.c_str());

    Serial.printf("[WIFI] Boot connect → '%s'\n", cfg.cfg.wifiSsid);
    const unsigned long deadline = millis() + timeoutSec * 1000UL;
    while (!wifiHasStaIp() && millis() < deadline) {
        delay(250);
        wl_status_t st = WiFi.status();
        if (st == WL_CONNECT_FAILED || st == WL_NO_SSID_AVAIL) break;
        Serial.print('.');
    }
    Serial.println();

    if (wifiHasStaIp()) {
        wifiSaveLastIp(cfg);
        Serial.printf("[WIFI] Connesso – IP: %s RSSI: %d\n",
                      WiFi.localIP().toString().c_str(), WiFi.RSSI());
        return true;
    }

    Serial.printf("[WIFI] Boot connect failed (status %d)\n", WiFi.status());
    WiFi.disconnect(true, false);
    return false;
}

inline void startSetupAP(ConfigManager& cfg) {
    _startSetupAp(cfg);
    Serial.println("[WIFI] Modalità setup AP");
}

inline bool setupWiFi(ConfigManager& cfg) {
    wifiJob.begin(&cfg);

    if (connectSavedWiFi(cfg)) {
        _startSetupAp(cfg);
        return true;
    }

    startSetupAP(cfg);
    return true;
}

inline void resetWifiAndRestart(ConfigManager& cfg) {
    SecureStore::clearWifi();
    cfg.cfg.wifiSsid[0] = '\0';
    cfg.cfg.lastStaIp[0] = '\0';
    cfg.save();
    delay(200);
    ESP.restart();
}
