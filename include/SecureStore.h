#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include <mbedtls/sha256.h>
#include <WiFi.h>

/**
 * Sensitive data storage:
 *   - WiFi password, MQTT password → NVS namespace "sh0sec" (not in config.json)
 *   - Web login password           → SHA-256 hash in config.json (never stored plain)
 *   - WiFi SSID (display only)     → config.json
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
        p.end();
        WiFi.disconnect(true, true);
    }
};
