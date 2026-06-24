#pragma once
#include <Arduino.h>

// ── MUX CD74HC4067 ──────────────────────────────────────────────────
#define MUX_PIN_A0   32
#define MUX_PIN_A1   33
#define MUX_PIN_A2   25
#define MUX_PIN_A3   26
#define MUX_PIN_SIG  34   // ADC input (reads from selected MUX channel)
#define MUX_CHANNELS 16

// ── I2C (VL6180X – dedicated bus, NOT through MUX) ──────────────────
#define I2C_SDA      21
#define I2C_SCL      22
#define VL6180X_ADDR 0x29

// ── Timing ──────────────────────────────────────────────────────────
#define SENSOR_POLL_MS      20    // MUX full scan period
#define MQTT_HEARTBEAT_MS   5000
#define WIFI_PORTAL_TIMEOUT 180   // seconds before captive portal gives up

// ── MQTT topic template  (use snprintf with board name) ─────────────
// railway/{name}/sensors/state
// railway/{name}/command/set
// railway/{name}/status/heartbeat

// ── Filesystem paths ────────────────────────────────────────────────
#define CONFIG_PATH  "/config.json"

// ── Channel roles ───────────────────────────────────────────────────
enum class ChannelRole : uint8_t {
    UNUSED        = 0,
    SENSOR        = 1,   // absorption/current sensor
    RELAY         = 2,
    SIGNAL_RGB    = 3,   // RGB semaphore (common-anode/cathode via 3 consecutive ch)
    MARMOTTA      = 4,   // Marmot sound trigger
    SERIAL_RGB    = 5    // WS2812-style serial LED strip
};

constexpr const char* channelRoleName(ChannelRole r) {
    switch (r) {
        case ChannelRole::SENSOR:     return "Sensore";
        case ChannelRole::RELAY:      return "Relè";
        case ChannelRole::SIGNAL_RGB: return "Semaforo RGB";
        case ChannelRole::MARMOTTA:   return "Marmotta";
        case ChannelRole::SERIAL_RGB: return "RGB Seriale";
        default:                      return "Non usato";
    }
}
