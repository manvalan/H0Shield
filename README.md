# ShieldH0 – Railway Automation Firmware

ESP32 (WeMos D32R1) firmware for H0-scale model railway automation.  
Fully compatible with **Rocrail** via MQTT (same protocol as [SignalBoard](https://github.com/manvalan/SignalBoard) and [TurnoutBoard](https://github.com/manvalan/TurnoutBoard)).

---

## Features

| Feature | Details |
|---|---|
| **WiFi provisioning** | Captive portal (WiFiManager) — no hardcoded credentials |
| **Persistent config** | LittleFS `config.json` — survives reboots |
| **mDNS** | Reachable at `<hostname>.local`; also resolves broker by `*.local` hostname |
| **MUX driver** | CD74HC4067 × 16 channels — sensors, relays, signals (R/G/Y), marmotta, WS2812 |
| **VL6180X** | Time-of-flight sensor on dedicated I2C bus (GPIO 21/22) |
| **MQTT** | PubSub with LWT, heartbeat, mDNS broker resolution, reconnect back-off |
| **Rocrail** | Signal aspect + turnout commands via standard XML topics |
| **OTA** | ArduinoOTA (password = hostname) |
| **Web UI** | Visual MUX mapping, signal/turnout config, live dashboard, manual test |

---

## Hardware

```
WeMos D32R1 (ESP32)
│
├── MUX CD74HC4067
│   ├── A0  → GPIO 32        ├── A2  → GPIO 25
│   ├── A1  → GPIO 33        ├── A3  → GPIO 26
│   └── SIG → GPIO 34 (ADC)
│
├── VL6180X (I2C, dedicated)
│   ├── SDA → GPIO 21
│   └── SCL → GPIO 22
│
├── WS2812B strips (direct GPIO, NOT through MUX)
│   ├── Strip 0 → GPIO 4    ├── Strip 2 → GPIO 18
│   └── Strip 1 → GPIO 5    └── Strip 3 → GPIO 19
│
└── Reset button  → GPIO 0 (BOOT, hold 3 s to clear WiFi credentials)
```

---

## MUX Channel Roles

| Role (config value) | Description | MUX channels used |
|---|---|---|
| `0` — UNUSED | Not configured | 1 |
| `1` — SENSOR | Absorption/occupancy sensor (ADC) | 1 |
| `2` — RELAY | Digital relay output | 1 |
| `3` — SIGNAL_RGB | Semaphore RGB (digital: R, G/Y, V) | **3 consecutive** |
| `4` — MARMOTTA | Audio trigger (pulse) | 1 |
| `5` — SERIAL_RGB | WS2812 strip (data on dedicated GPIO) | 1 (tag only) |

---

## Rocrail MQTT Protocol

### Signal commands (`TYPE_MAIN` and `TYPE_SHUNT`)

| Direction | Topic | Payload |
|---|---|---|
| ← subscribe | `rocrail/service/info/sg` | `<sg id="sg1" cmd="aspect" aspect="green"/>` |
| → feedback | `rocrail/service/client` | same XML |
| retained LWT | `railway/status/segnali` | `{"module":"…","status":"online\|offline"}` |

**Aspects:** `red` · `green` · `yellow` (MAIN) — `stop` · `go` · `oblique` (SHUNT)

### Turnout commands

| Direction | Topic | Payload |
|---|---|---|
| ← subscribe | `rocrail/service/command` | `<sw id="sw1" cmd="straight" addr1="1"/>` |
| → feedback | `rocrail/service/info` | `<fb id="sw1" addr1="1" cmd="straight"/>` |
| retained LWT | `railway/status/scambi` | `{"module":"…","status":"online\|offline"}` |

**Commands:** `straight` · `turnout`  
Turnouts use a **timed pulse** on two MUX relay channels (default 300 ms).

### Block detector feedback

When a sensor mapped to a Rocrail ID changes state:

```
→ rocrail/service/info    <fb id="bk1" state="free|occupied"/>
```

### Board-specific topics

```
railway/<hostname>/sensors/state      ← retained JSON {ch: occupied, …}
railway/<hostname>/command/set        ← {"channel":N,"action":"on|off|toggle"}
railway/<hostname>/status/heartbeat   ← "online" every 5 s
railway/<hostname>/tof/distance       ← mm (on change)
railway/<hostname>/tof/ambient        ← ALS counts (every 1 s)
```

---

## Configuration (Web UI)

Visit `http://<hostname>.local/` after connecting to WiFi.

### Sections

1. **Rete & MQTT** — hostname, broker IP or `*.local`, port, credentials
2. **Mapping canali MUX** — assign role to each of the 16 MUX channels
3. **Segnali Rocrail** — map Rocrail signal IDs to MUX channel triplets (R/G/V)
4. **Scambi Rocrail** — map Rocrail turnout IDs to two MUX relay channels + pulse duration
5. **Dashboard live** — real-time sensor occupancy, signal aspects, turnout positions, MQTT status
6. **Test manuale** — send signal aspects and turnout commands directly from the browser

The server validates **channel conflicts** (duplicate MUX assignment) before saving.

### `config.json` structure

```json
{
  "hostname": "ShieldH0",
  "mqtt_broker": "192.168.1.x",
  "mqtt_port": 1883,
  "mqtt_user": "",
  "mqtt_pass": "",
  "pin_map": [1, 2, 3, 3, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0],
  "signals": [
    { "id": "sg1", "type": 0, "chR": 2, "chG": 3, "chV": 4 }
  ],
  "turnouts": [
    { "id": "sw1", "chS": 5, "chD": 6, "pulse": 300 }
  ],
  "sensors_rb": [
    { "rocrail_id": "bk1", "mux_ch": 0 }
  ]
}
```

---

## Flash Instructions

```bash
# 1. Upload filesystem (first time, or after changing data/)
pio run -t uploadfs

# 2. Upload firmware (USB)
pio run -t upload

# 3. Subsequent OTA updates
pio run -t upload --upload-port ShieldH0.local
```

First boot opens WiFi AP `ShieldH0-<hostname>` → connect → configure at `http://192.168.4.1`.

---

## Optional: PCA9685 Signal Driver

If you want PWM brightness control for signals (as in SignalBoard), build with:

```bash
pio run -e wemos_d1_mini32_pca
```

This enables `PCA9685Signal.h` (I2C addr 0x40) alongside the standard MUX channels.  
See `include/PCA9685Signal.h` for wiring and usage.

---

## File Structure

```
ShieldH0/
├── platformio.ini
├── include/
│   ├── Config.h            pin defs, Rocrail topics, enums
│   ├── ConfigManager.h     LittleFS load/save, BoardConfig struct
│   ├── MuxDriver.h         CD74HC4067 channel select + read/write
│   ├── ChannelObjects.h    IChannel hierarchy (Sensor, Relay, SignalRGB, Marmotta, SerialRGB)
│   ├── TurnoutChannel.h    MUX-based timed relay turnout driver
│   ├── RocrailProtocol.h   XML parser, signal/turnout/sensor dispatcher
│   ├── MQTTManager.h       PubSubClient wrapper, LWT, mDNS broker resolution
│   ├── WebConfig.h         REST API + live status + test handler
│   ├── OTAManager.h        ArduinoOTA wrapper
│   ├── ToFManager.h        VL6180X polling + MQTT publish
│   └── PCA9685Signal.h     Optional PWM signal driver (USE_PCA9685)
├── src/
│   └── main.cpp            setup/loop, object factories, MQTT callbacks
└── data/
    ├── config.json         default config (uploaded to LittleFS)
    └── index.html          single-page config + dashboard UI
```
