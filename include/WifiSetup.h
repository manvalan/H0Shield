#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include "Config.h"
#include "ConfigManager.h"
#include "SecureStore.h"

/**
 * Modalità setup  (nessun WiFi salvato / connessione fallita):
 *   AP ShieldH0-* → http://192.168.4.1/
 *
 * Modalità normale (WiFi salvato e connesso):
 *   Solo STA — niente AP che ruba il telefono.
 *   Accedi da http://<IP-LAN>/ sulla stessa rete di casa.
 *
 * Persistenza: SSID + last_sta_ip (LittleFS), password (NVS).
 */

inline bool wifiHasStaIp() {
    return WiFi.status() == WL_CONNECTED &&
           WiFi.localIP() != IPAddress(0, 0, 0, 0);
}

inline bool wifiApRunning() {
    return (WiFi.getMode() & WIFI_AP) && WiFi.softAPIP() != IPAddress(0, 0, 0, 0);
}

inline bool wifiInSetupMode(const ConfigManager& cfg) {
    return cfg.cfg.wifiSsid[0] == '\0' || !wifiHasStaIp();
}

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

inline void wifiSaveCredentials(ConfigManager& cfg, const char* ssid, const char* pass) {
    SecureStore::saveWifiPassword(pass ? pass : "");
    strlcpy(cfg.cfg.wifiSsid, ssid, sizeof(cfg.cfg.wifiSsid));
}

inline void wifiStopAp() {
    WiFi.softAPdisconnect(true);
    if (wifiHasStaIp())
        WiFi.mode(WIFI_STA);
}

inline void wifiStartSetupAp(ConfigManager& cfg) {
    const String apName = String("ShieldH0-") + cfg.cfg.hostname;
    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(
        IPAddress(192, 168, 4, 1),
        IPAddress(192, 168, 4, 1),
        IPAddress(255, 255, 255, 0));
    WiFi.softAP(apName.c_str());
    Serial.printf("[WIFI] Setup AP '%s' → http://192.168.4.1/\n", apName.c_str());
}

inline void wifiBeginSta(ConfigManager& cfg, const char* ssid, const char* pass) {
    wifiApplyHostname(cfg);
    WiFi.setAutoReconnect(true);
    WiFi.setSleep(false);
    WiFi.mode(WIFI_STA);
    if (pass && pass[0])
        WiFi.begin(ssid, pass);
    else
        WiFi.begin(ssid);
}

/** Durante setup: AP+STA così la pagina resta raggiungibile su 192.168.4.1 */
inline void wifiBeginStaWithAp(ConfigManager& cfg, const char* ssid, const char* pass) {
    const String apName = String("ShieldH0-") + cfg.cfg.hostname;
    WiFi.mode(WIFI_AP_STA);
    if (!wifiApRunning()) {
        WiFi.softAPConfig(
            IPAddress(192, 168, 4, 1),
            IPAddress(192, 168, 4, 1),
            IPAddress(255, 255, 255, 0));
        WiFi.softAP(apName.c_str());
    }
    wifiApplyHostname(cfg);
    WiFi.setAutoReconnect(true);
    WiFi.setSleep(false);
    if (pass && pass[0])
        WiFi.begin(ssid, pass);
    else
        WiFi.begin(ssid);
}

struct WiFiBootConnect {
    bool          active  = false;
    unsigned long started = 0;
    static constexpr uint32_t TIMEOUT_MS = 18000;
};

inline WiFiBootConnect wifiBoot;

class WiFiJobManager {
public:
    enum class Phase : uint8_t { IDLE, SCANNING, CONNECTING, SUCCESS, FAILED };

    static constexpr uint32_t CONNECT_TIMEOUT_MS = 25000;
    static constexpr uint32_t SCAN_TIMEOUT_MS    = 15000;

    void begin(ConfigManager* cfg) { _cfg = cfg; }

    bool busy() const {
        return _phase == Phase::SCANNING || _phase == Phase::CONNECTING;
    }

    Phase phase() const { return _phase; }

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
        if (_phase == Phase::SUCCESS || _phase == Phase::FAILED)
            _phase = Phase::IDLE;
    }

    bool startScan() {
        if (busy() || !_cfg) return false;
        if (wifiInSetupMode(*_cfg))
            wifiStartSetupAp(*_cfg);
        WiFi.scanDelete();
        WiFi.scanNetworks(true, true);
        _phase   = Phase::SCANNING;
        _started = millis();
        _message = "Scansione reti WiFi…";
        return true;
    }

    bool startConnect(const String& ssid, const String& pass, bool save) {
        if (busy() || !_cfg || ssid.isEmpty()) return false;

        _targetSsid    = ssid;
        _pendingPass   = pass;
        _saveOnSuccess = save;
        _saved         = false;
        _savedIp       = "";
        _lastStatus    = WL_IDLE_STATUS;
        _started       = millis();
        _deadline      = _started + CONNECT_TIMEOUT_MS;
        _phase         = Phase::CONNECTING;
        _message       = String("Connessione a ") + ssid + "…";

        if (wifiInSetupMode(*_cfg) || wifiApRunning())
            wifiBeginStaWithAp(*_cfg, ssid.c_str(), pass.c_str());
        else
            wifiBeginSta(*_cfg, ssid.c_str(), pass.c_str());

        Serial.printf("[WIFI] Connect → '%s'\n", ssid.c_str());
        return true;
    }

    void loop() {
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
        doc["job_restart"]    = false;
        doc["job_elapsed_ms"] = (_phase != Phase::IDLE && _started)
            ? static_cast<uint32_t>(millis() - _started) : 0;
        doc["job_timeout_ms"] = (_phase == Phase::CONNECTING) ? CONNECT_TIMEOUT_MS
                                 : (_phase == Phase::SCANNING) ? SCAN_TIMEOUT_MS : 0;
        if (_phase == Phase::SUCCESS) {
            doc["job_ip"]      = _savedIp;
            doc["job_rssi"]    = WiFi.RSSI();
            doc["job_saved"]   = _saved;
            doc["job_web_url"] = String("http://") + _savedIp + "/";
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
                    return;
                }
                _saved = true;
            }
            wifiStopAp();
            wifiBoot.active = false;
            _phase   = Phase::SUCCESS;
            _message = String("Connesso — apri http://") + _savedIp + "/ dal WiFi di casa";
            Serial.printf("[WIFI] OK IP %s (AP spento)\n", _savedIp.c_str());
            return;
        }

        if (st == WL_CONNECT_FAILED || st == WL_NO_SSID_AVAIL) {
            _phase   = Phase::FAILED;
            _message = wifiStatusMessageIt(st);
            if (_cfg && wifiInSetupMode(*_cfg)) wifiStartSetupAp(*_cfg);
            return;
        }

        if (millis() >= _deadline) {
            _phase      = Phase::FAILED;
            _lastStatus = WiFi.status();
            _message    = wifiStatusMessageIt(WL_DISCONNECTED);
            if (_cfg && wifiInSetupMode(*_cfg)) wifiStartSetupAp(*_cfg);
        }
    }
};

inline WiFiJobManager wifiJob;

inline void wifiBootStart(ConfigManager& cfg) {
    if (cfg.cfg.wifiSsid[0] == '\0') return;
    String pass = SecureStore::loadWifiPassword();
    wifiBeginSta(cfg, cfg.cfg.wifiSsid, pass.c_str());
    wifiBoot.active  = true;
    wifiBoot.started = millis();
    Serial.printf("[WIFI] Boot → '%s' (solo STA)\n", cfg.cfg.wifiSsid);
}

inline void wifiBootLoop(ConfigManager& cfg) {
    if (!wifiBoot.active) return;

    if (wifiHasStaIp()) {
        wifiSaveLastIp(cfg);
        wifiStopAp();
        wifiBoot.active = false;
        Serial.printf("[WIFI] Boot OK – http://%s/\n", WiFi.localIP().toString().c_str());
        return;
    }

    wl_status_t st = WiFi.status();
    if (st == WL_CONNECT_FAILED || st == WL_NO_SSID_AVAIL ||
        millis() - wifiBoot.started > WiFiBootConnect::TIMEOUT_MS) {
        wifiBoot.active = false;
        Serial.println("[WIFI] Boot fallito — apro AP setup");
        wifiStartSetupAp(cfg);
    }
}

inline bool setupWiFi(ConfigManager& cfg) {
    WiFi.persistent(false);
    WiFi.disconnect(true, false);
    delay(50);
    wifiJob.begin(&cfg);

    if (cfg.cfg.wifiSsid[0] == '\0') {
        wifiStartSetupAp(cfg);
        return true;
    }

    wifiBootStart(cfg);
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

inline void wifiFillAccessStatus(const ConfigManager& cfg, JsonObject doc) {
    const bool sta  = wifiHasStaIp();
    const bool setup = wifiInSetupMode(cfg);
    String staIp = sta ? WiFi.localIP().toString() : String(cfg.cfg.lastStaIp);

    doc["connected"]   = sta;
    doc["setup_mode"]  = setup;
    doc["ap_active"]   = wifiApRunning();
    doc["ssid"]        = sta ? WiFi.SSID() : cfg.cfg.wifiSsid;
    doc["sta_ip"]      = staIp;
    doc["last_sta_ip"] = cfg.cfg.lastStaIp;
    doc["saved_ssid"]  = cfg.cfg.wifiSsid;
    if (staIp.length() && sta)
        doc["web_url"] = String("http://") + staIp + "/";
    if (wifiApRunning()) {
        doc["ap_ip"]  = WiFi.softAPIP().toString();
        doc["ap_url"] = String("http://") + WiFi.softAPIP().toString() + "/";
    }
    doc["rssi"] = WiFi.RSSI();
}
