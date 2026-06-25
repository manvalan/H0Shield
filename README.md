# ShieldH0 – Railway Automation Firmware

ESP32 (WeMos D32R1) firmware for H0-scale model railway automation.  
Compatible with **Rocrail** via MQTT (same protocol as [SignalBoard](https://github.com/manvalan/SignalBoard) and [TurnoutBoard](https://github.com/manvalan/TurnoutBoard)).

---

## Features

| Area | Details |
|------|---------|
| **WiFi** | Captive portal setup, credentials in NVS (encrypted password) |
| **Web UI** | Dashboard live, 7 tab (Dashboard · Rete · Sensori · Display · Accessori · Scenari · Sistema) |
| **MUX** | CD74HC4067 × 16 ch — sensori, relè, semafori RGB, marmotta, WS2812 |
| **ToF** | VL6180X @ 0x29, soglia mm, feedback blocchi Rocrail |
| **Display** | OLED SH1106 128×64 — binario + tabellone orario (opz. U8g2) |
| **Accessori** | Relè nominati: luci, PL, campana, sbarra (`accessories[]`) |
| **Scenari** | Sensore occupato → PL + luci + display (`scenarios[]`) |
| **Rocrail** | Segnali, scambi, blocchi via MQTT XML |
| **MQTT** | Topic `railway/<hostname>/…`, LWT, heartbeat |
| **OTA** | ArduinoOTA |

---

## Build environments

| Env | Uso |
|-----|-----|
| `wemos_d1_mini32_usb` | Default USB — firmware base (~98% flash) |
| `wemos_d1_mini32_usb_display` | + OLED U8g2 (`USE_DISPLAY`) |
| `wemos_d1_mini32_pca_usb` | + PCA9685 PWM segnali (`USE_PCA9685`) |
| `shieldh0_minimal` | Alias di `wemos_d1_mini32_usb` |
| `shieldh0_full` | Display + PCA9685 insieme |

```bash
# Filesystem (prima volta o dopo modifica data/)
pio run -e wemos_d1_mini32_usb -t uploadfs

# Firmware USB
pio run -e wemos_d1_mini32_usb -t upload

# Con display OLED
pio run -e wemos_d1_mini32_usb_display -t upload

# Con PCA9685 (SignalBoard-style PWM)
pio run -e wemos_d1_mini32_pca_usb -t upload

# Tutto abilitato
pio run -e shieldh0_full -t upload
```

---

## Hardware

```
WeMos D32R1
├── MUX CD74HC4067  (A0–A3: 32,33,25,26 · SIG: GPIO 34 ADC)
├── I2C SDA 21 / SCL 22
│   ├── VL6180X @ 0x29
│   ├── OLED binario @ 0x3C
│   ├── OLED tabellone @ 0x3D
│   └── PCA9685 @ 0x40 (opz.)
├── WS2812: GPIO 4, 5, 18, 19
└── Reset WiFi: GPIO 0 (BOOT, 3 s)
```

Vedi [docs/HARDWARE.md](docs/HARDWARE.md) per lo schema elettrico PCB e
[docs/CABLING.md](docs/CABLING.md) per il cablaggio plastico H0.

---

## Config (`config.json`)

```json
{
  "hostname": "ShieldH0",
  "boot_aspect_main": "red",
  "boot_aspect_shunt": "stop",
  "signals": [
    { "id": "sg1", "type": 0, "chR": 0, "chG": 1, "chV": 2 },
    { "id": "sg2", "type": 0, "chR": 3, "chG": 4, "chV": 5, "use_pca": true }
  ],
  "accessories": [
    { "id": "luci_sala", "profile": "light", "mux_ch": 4 },
    { "id": "pl_nord", "profile": "level_xing", "mux_ch_lights": 5, "mux_ch_bar": 6 }
  ],
  "displays": [
    { "id": "bin3", "type": "platform", "i2c_addr": 60, "platform_num": 3 }
  ],
  "scenarios": [
    {
      "id": "approccio_bin3",
      "mux_ch": 2,
      "accessory_pl": "pl_nord",
      "accessory_light": "luci_sala",
      "display_id": "bin3",
      "status_occupied": "in arrivo"
    }
  ]
}
```

**Aspect di boot:** configurabile in tab Sistema (`boot_aspect_main` / `boot_aspect_shunt`).  
Applicate a tutti i segnali MUX e PCA9685 all'avvio.

**Segnali PCA9685:** `"use_pca": true` — `chR/chG/chV` sono pin PCA (0–15), non MUX. Richiede build `-D USE_PCA9685`.

---

## MQTT

### Rocrail (fissi)

| Topic | Uso |
|-------|-----|
| `rocrail/service/info/sg` | Comandi semafori |
| `rocrail/service/command` | Comandi scambi |
| `rocrail/service/client` | Feedback segnali |

### Board (`railway/<hostname>/…`)

| Topic | Payload |
|-------|---------|
| `command/set` | `{"accessory":"luci_sala","action":"on"}` |
| `display/<id>/platform/status` | `in arrivo` |
| `railway/info` | JSON SIP — aggiorna display per stazione/binario ([INFO_MQTT.md](docs/INFO_MQTT.md)) |
| `accessories/state` | JSON stato relè |
| `sensors/state` | JSON occupazione MUX |

---

## Web UI

`http://<hostname>.local/` — configurazione, dashboard SVG (segnali/scambi/binari), test manuale.

---

## Documentazione

| File | Contenuto |
|------|-----------|
| [docs/PIANO.md](docs/PIANO.md) | Roadmap fasi 0–9 |
| [docs/DISPLAY.md](docs/DISPLAY.md) | Binario e tabellone OLED |
| [docs/INFO_MQTT.md](docs/INFO_MQTT.md) | Sistema informativo plastico (topic `railway/info`) |
| [docs/HARDWARE.md](docs/HARDWARE.md) | Schema elettrico PCB ([PDF](docs/SCHEDA-H0.pdf)) |
| [docs/CABLING.md](docs/CABLING.md) | Cablaggio plastico H0 |

---

## File structure

```
include/
  ConfigManager.h      Config + load/save
  RocrailProtocol.h    XML MQTT segnali/scambi
  DisplayManager.h     OLED SH1106 (USE_DISPLAY)
  AccessoryManager.h   Relè profilati
  ScenarioManager.h    Automazioni sensore→output
  PCA9685Signal.h      PWM segnali (USE_PCA9685)
src/main.cpp
data/index.html        Web UI
data/config.json       Default LittleFS
```
