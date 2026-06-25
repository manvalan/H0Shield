#pragma once
#include <Arduino.h>

// ── MUX CD74HC4067 ──────────────────────────────────────────────────
#define MUX_PIN_A0   32
#define MUX_PIN_A1   33
#define MUX_PIN_A2   25
#define MUX_PIN_A3   26
#define MUX_PIN_SIG  34   // ADC input (reads from selected MUX channel)
#define MUX_CHANNELS 16

// ── I2C (VL6180X – dedicated bus, optional, NOT through MUX) ────────
// Bus may be empty at boot: firmware scans and enables only found devices.
#define I2C_SDA      21
#define I2C_SCL      22
#define VL6180X_ADDR 0x29

// ── WiFiManager reset button ─────────────────────────────────────────
// Hold LOW for WIFI_RESET_HOLD_MS to wipe saved credentials and reopen portal
#define WIFI_RESET_PIN      0      // BOOT button on most D32R1 boards
#define WIFI_RESET_HOLD_MS  3000

// ── Timing ──────────────────────────────────────────────────────────
#define SENSOR_POLL_MS      20    // MUX full scan period
#define MQTT_HEARTBEAT_MS   5000
#define WIFI_PORTAL_TIMEOUT 180   // seconds before captive portal gives up

// ── MQTT topic template  (use snprintf with board name) ─────────────
// railway/{name}/sensors/state
// railway/{name}/command/set
// railway/{name}/status/heartbeat

// ── Rocrail MQTT topics (standard, fixed – NOT per-board) ────────────
//   Signals:  subscribe command, publish feedback
#define ROCRAIL_TOPIC_SG_CMD      "rocrail/service/info/sg"
#define ROCRAIL_TOPIC_SG_FB       "rocrail/service/client"
#define ROCRAIL_TOPIC_SG_LWT      "railway/status/segnali"
//   Turnouts: subscribe command, publish feedback
#define ROCRAIL_TOPIC_SW_CMD      "rocrail/service/command"
#define ROCRAIL_TOPIC_SW_FB       "rocrail/service/info"
#define ROCRAIL_TOPIC_SW_LWT      "railway/status/scambi"

// ── Signal aspect mapping ─────────────────────────────────────────────
enum class SignalType : uint8_t { MAIN = 0, SHUNT = 1 };
enum class SignalAspect : uint8_t {
    RED = 0, GREEN = 1, YELLOW = 2,           // TYPE_MAIN
    STOP = 3, GO = 4, OBLIQUE = 5             // TYPE_SHUNT
};

// ── Filesystem paths ────────────────────────────────────────────────
#define CONFIG_PATH  "/config.json"

// ── WS2812 strips: MUX channel → physical GPIO ──────────────────────
// Add entries here as you wire more strips.
// Key = MUX config channel index, Value = ESP32 GPIO for the DATA line.
static const uint8_t WS2812_GPIO_MAP[][2] = {
    //  MUX ch,  GPIO
    {       8,    4 },
    {       9,    5 },
    {      10,   18 },
    {      11,   19 },
};
#define WS2812_MAP_SIZE (sizeof(WS2812_GPIO_MAP) / sizeof(WS2812_GPIO_MAP[0]))
#define WS2812_MAX_LEDS  30   // per-strip LED count (adjust per layout)

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
