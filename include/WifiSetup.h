#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include "Config.h"
#include "ConfigManager.h"
#include "SecureStore.h"

/**
 * Regola d'oro: l'AP ShieldH0-* è SEMPRE attivo → http://192.168.4.1/
 * La connessione alla rete di casa avviene in background (AP+STA).
 *
 * Persistenza: SSID + last_sta_ip (LittleFS), password (NVS sh0sec).
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

/** AP sempre su 192.168.4.1 — accesso garantito alla scheda. */
inline void wifiEnsureAp(ConfigManager& cfg) {
    const String apName = String("ShieldH0-") + cfg.cfg.hostname;
    WiFi.mode(WIFI_AP_STA);
    if (WiFi.softAPIP() == IPAddress(0, 0, 0, 0)) {
        WiFi.softAPConfig(
            IPAddress(192, 168, 4, 1),
            IPAddress(192, 168, 4, 1),
            IPAddress(255, 255, 255, 0));
        WiFi.softAP(apName.c_str());
    }
    Serial.printf("[WIFI] AP '%s' → http://%s/\n",
                  apName.c_str(), WiFi.softAPIP().toString().c_str());
}

/** Avvia/riprende STA senza spegnere l'AP. */
inline void wifiBeginSta(ConfigManager& cfg, const char* ssid, const char* pass) {
    wifiEnsureAp(cfg);
    wifiApplyHostname(cfg);
    WiFi.setAutoReconnect(true);
    WiFi.setSleep(false);
    if (pass && pass[0])
        WiFi.begin(ssid, pass);
    else
        WiFi.begin(ssid);
}

inline void wifiSaveCredentials(ConfigManager& cfg, const char* ssid, const char* pass) {
    SecureStore::saveWifiPassword(pass ? pass : "");
    strlcpy(cfg.cfg.wifiSsid, ssid, sizeof(cfg.cfg.wifiSsid));
}

// ── Boot: connessione STA in background (AP già attivo) ───────────────
struct WiFiBootConnect {
    bool          active  = false;
    unsigned long started = 0;
    static constexpr uint32_t TIMEOUT_MS = 20000;
};

inline WiFiBootConnect wifiBoot;

inline void wifiBootStart(ConfigManager& cfg) {
    if (cfg.cfg.wifiSsid[0] == '\0') return;
    String pass = SecureStore::loadWifiPassword();
    wifiBeginSta(cfg, cfg.cfg.wifiSsid, pass.c_str());
    wifiBoot.active  = true;
    wifiBoot.started = millis();
    Serial.printf("[WIFI] Boot STA async → '%s'\n", cfg.cfg.wifiSsid);
}

inline void wifiBootLoop(ConfigManager& cfg) {
    if (!wifiBoot.active) return;

    if (wifiHasStaIp()) {
        wifiSaveLastIp(cfg);
        wifiBoot.active = false;
        Serial.printf("[WIFI] Boot STA OK – IP %s\n", WiFi.localIP().toString().c_str());
        return;
    }

    wl_status_t st = WiFi.status();
    if (st == WL_CONNECT_FAILED || st == WL_NO_SSID_AVAIL ||
        millis() - wifiBoot.started > WiFiBootConnect::TIMEOUT_MS) {
        wifiBoot.active = false;
        Serial.printf("[WIFI] Boot STA fallita (status %d) — usa AP http://192.168.4.1/\n", st);
        wifiEnsureAp(cfg);
    }
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
        if (busy() || restartPending() || !_cfg) return false;
        wifiEnsureAp(*_cfg);
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

        wifiBeginSta(*_cfg, ssid.c_str(), pass.c_str());
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
            doc["job_ip"]      = ip;
            doc["job_rssi"]    = WiFi.RSSI();
            doc["job_saved"]   = _saved;
            doc["job_web_url"] = String("http://") + ip + "/";
            doc["job_ap_url"]  = String("http://") + WiFi.softAPIP().toString() + "/";
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
                wifiSaveCredentials(*_cfg, _targetSsid.c_str(), _pendingPass.c_str());
                strlcpy(_cfg->cfg.lastStaIp, _savedIp.c_str(), sizeof(_cfg->cfg.lastStaIp));
                if (!_cfg->save()) {
                    _phase   = Phase::FAILED;
                    _message = "Errore salvataggio configurazione";
                    Serial.println("[WIFI] cfg.save() failed");
                    return;
                }
                _saved     = true;
                _phase     = Phase::SUCCESS;
                _message   = String("Connesso IP ") + _savedIp + " — riavvio…";
                _restartAt = millis() + RESTART_DELAY_MS;
            } else {
                _phase   = Phase::SUCCESS;
                _message = String("Connesso IP ") + _savedIp;
            }
            wifiEnsureAp(*_cfg);
            return;
        }

        if (st == WL_CONNECT_FAILED || st == WL_NO_SSID_AVAIL) {
            _phase   = Phase::FAILED;
            _message = wifiStatusMessageIt(st);
            wifiEnsureAp(*_cfg);
            return;
        }

        if (millis() >= _deadline) {
            _phase      = Phase::FAILED;
            _lastStatus = WiFi.status();
            _message    = wifiStatusMessageIt(WL_DISCONNECTED);
            wifiEnsureAp(*_cfg);
            Serial.println("[WIFI] Connect timeout");
        }
    }
};

inline WiFiJobManager wifiJob;

inline bool setupWiFi(ConfigManager& cfg) {
    WiFi.persistent(false);
    WiFi.disconnect(true, false);
    delay(50);
    wifiJob.begin(&cfg);

    wifiEnsureAp(cfg);
    wifiBootStart(cfg);

    Serial.println("[WIFI] Pronto — connettiti all'AP ShieldH0 e apri http://192.168.4.1/");
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
