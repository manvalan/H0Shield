#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include <mbedtls/sha256.h>
#include <WiFi.h>

/**
 * Sensitive data storage:
 *   - WiFi SSID + password + last IP → NVS namespace "sh0sec" (sopravvive a uploadfs)
 *   - WiFi SSID (copia) + last IP     → config.json
 *
 * Note: ESP32 NVS is isolated from the filesystem; this is the standard approach
 * on embedded devices without a secure element.
 */
class SecureStore {
public:
    static String hashPassword(const String& plain) {
        if (plain.isEmpty()) return "";
        uint8_t hash[32];
        mbedtls_sha256_context ctx;
        mbedtls_sha256_init(&ctx);
        mbedtls_sha256_starts(&ctx, 0);
        mbedtls_sha256_update(&ctx,
            reinterpret_cast<const uint8_t*>(plain.c_str()), plain.length());
        mbedtls_sha256_finish(&ctx, hash);
        mbedtls_sha256_free(&ctx);

        char hex[65];
        for (int i = 0; i < 32; i++) sprintf(hex + i * 2, "%02x", hash[i]);
        hex[64] = '\0';
        return String(hex);
    }

    static bool verifyPassword(const String& plain, const char* hashHex) {
        if (!hashHex || hashHex[0] == '\0') return plain.isEmpty();
        return hashPassword(plain) == String(hashHex);
    }

    static void saveWifiPassword(const String& pass) {
        Preferences p;
        p.begin("sh0sec", false);
        p.putString("wifi_pass", pass);
        p.end();
    }

    static void saveWifiSsid(const String& ssid) {
        Preferences p;
        p.begin("sh0sec", false);
        p.putString("wifi_ssid", ssid);
        p.end();
    }

    static void saveLastStaIp(const String& ip) {
        Preferences p;
        p.begin("sh0sec", false);
        p.putString("last_sta_ip", ip);
        p.end();
    }

    static String loadWifiSsid() {
        Preferences p;
        p.begin("sh0sec", true);
        String s = p.getString("wifi_ssid", "");
        p.end();
        return s;
    }

    static String loadLastStaIp() {
        Preferences p;
        p.begin("sh0sec", true);
        String s = p.getString("last_sta_ip", "");
        p.end();
        return s;
    }

    static String loadWifiPassword() {
        Preferences p;
        p.begin("sh0sec", true);
        String s = p.getString("wifi_pass", "");
        p.end();
        return s;
    }

    static void saveMqttPassword(const String& pass) {
        Preferences p;
        p.begin("sh0sec", false);
        p.putString("mqtt_pass", pass);
        p.end();
    }

    static String loadMqttPassword() {
        Preferences p;
        p.begin("sh0sec", true);
        String s = p.getString("mqtt_pass", "");
        p.end();
        return s;
    }

    static void clearWifi() {
        Preferences p;
        p.begin("sh0sec", false);
        p.remove("wifi_pass");
        p.remove("wifi_ssid");
        p.remove("last_sta_ip");
        p.end();
        WiFi.disconnect(true, true);
    }

    /** Config completa in NVS — sopravvive a uploadfs (LittleFS viene riscritto). */
    static bool saveConfigJson(const String& json) {
        if (json.isEmpty()) return false;
        Preferences p;
        p.begin("sh0cfg", false);
        const bool ok = p.putBytes("data", json.c_str(), json.length()) == json.length();
        if (ok) p.putUInt("len", json.length());
        p.end();
        return ok;
    }

    static String loadConfigJson() {
        Preferences p;
        p.begin("sh0cfg", true);
        const size_t len = p.getUInt("len", 0);
        if (len == 0 || len > 8192) {
            p.end();
            return "";
        }
        char* buf = static_cast<char*>(malloc(len + 1));
        if (!buf) {
            p.end();
            return "";
        }
        const size_t got = p.getBytes("data", buf, len);
        p.end();
        if (got != len) {
            free(buf);
            return "";
        }
        buf[len] = '\0';
        String out(buf);
        free(buf);
        return out;
    }
};
