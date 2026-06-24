# ShieldH0 – Railway Automation Firmware

ESP32 (WeMos D32R1) firmware for H0-scale model railway automation.

## Features

| Feature | Details |
|---|---|
| **WiFi provisioning** | Captive portal (WiFiManager) – no hardcoded credentials |
| **Persistent config** | LittleFS `config.json` – survives reboots |
| **mDNS** | Reachable at `<hostname>.local` |
| **MUX driver** | CD74HC4067 × 16 channels (sensors, relays, signals, marmotta) |
| **I2C sensor** | VL6180X time-of-flight (dedicated bus, pins 21/22) |
| **MQTT** | Pub/Sub with LWT; topics under `railway/<name>/…` |
| **Web UI** | Visual channel mapping at `http://<hostname>.local/` |

## Hardware

```
WeMos D32R1 (ESP32)
│
├── MUX CD74HC4067
│   ├── A0 → GPIO 32
│   ├── A1 → GPIO 33
│   ├── A2 → GPIO 25
│   ├── A3 → GPIO 26
│   └── SIG → GPIO 34 (ADC)
│
└── VL6180X (I2C)
    ├── SDA → GPIO 21
    └── SCL → GPIO 22
```

## Quick Start

1. **Flash filesystem** first:  
   `PlatformIO: Upload Filesystem Image`  
   (uploads `data/index.html` + `data/config.json` to LittleFS)

2. **Flash firmware**:  
   `PlatformIO: Upload`

3. On first boot a WiFi AP named `ShieldH0-<hostname>` appears.  
   Connect → portal at **192.168.4.1** → enter WiFi + MQTT credentials.

4. After connection visit `http://<hostname>.local/` to map MUX channels.

## MQTT Topics

```
railway/<name>/sensors/state      ← published on change (JSON map ch→occupied)
railway/<name>/command/set        ← subscribe; payload: {"channel":N,"action":"on|off|toggle"}
railway/<name>/status/heartbeat   ← "online" / LWT "offline"
```

## Development Phases

- [x] Phase 1 – ConfigManager + WiFiManager + LittleFS
- [x] Phase 2 – CD74HC4067 MUX driver
- [x] Phase 3 – Channel object mapping
- [x] Phase 4 – MQTT integration
- [x] Phase 5 – Web config UI
