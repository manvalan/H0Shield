# Display I2C — Binario e Tabellone Orario

Hardware: **OLED 1,3″ bianco/blu 128×64 I2C SH1106** (4 fili, SDA/SCL).

> **Nota:** il progetto `~/Documents/Develop/Arduino/RailwayInfoSystem` contiene solo
> lo scaffold PlatformIO (cartella `src/` vuota). Il codice va ricostruito/integrato
> qui in ShieldH0 seguendo questa specifica.

---

## Due tipi di display

| Tipo | ID config | Uso sul plastico | Contenuto tipico |
|------|-----------|------------------|------------------|
| **`platform`** | `binario` | Cartello sul marciapiede | Numero binario grande, prossimo treno, ora, stato |
| **`timetable`** | `tabellone` | Quadro partenze in stazione | Nome stazione, orologio, elenco partenze scroll |

Più display sullo stesso bus I2C con **indirizzi diversi** (`0x3C`, `0x3D`).

---

## Layout 128×64

### Binario (`platform`)

```
┌────────────────────────────┐
│ BINARIO        3           │  ← numero binario grande
│ Milano Centrale            │  ← destinazione
│ 14:32        in partenza   │  ← ora + stato
└────────────────────────────┘
```

Stati: `libero` · `in arrivo` · `in partenza` · `ritardo` · `soppresso`

### Tabellone (`timetable`)

```
┌────────────────────────────┐
│ STAZIONE H0       14:32    │  ← header + NTP
│ 14:30 Roma Termini    B3   │
│ 14:45 Torino P.N.     B1   │  ← scroll verticale
│ 15:10 Firenze SMN     B2   │
└────────────────────────────┘
```

---

## Config (`config.json`)

```json
"displays": [
  {
    "id": "bin3",
    "type": "platform",
    "i2c_addr": 60,
    "platform_num": 3,
    "destination": "",
    "departure_time": "",
    "status": "libero"
  },
  {
    "id": "tab1",
    "type": "timetable",
    "i2c_addr": 61,
    "station_name": "Stazione H0",
    "ntp_timezone": "Europe/Rome",
    "rows": [
      { "time": "14:30", "dest": "Roma Termini", "bin": "3", "status": "" },
      { "time": "14:45", "dest": "Torino P.N.", "bin": "1", "status": "ritardo" }
    ]
  }
]
```

---

## MQTT

Due modalità (compatibili):

1. **SIP — topic unico** `railway/info` con JSON: ogni display filtra per `station_id` + `platform` (o `display_id`). Vedi **[INFO_MQTT.md](INFO_MQTT.md)**.
2. **Per-display** — topic dedicati sotto `railway/<hostname>/display/<id>/…`

Topic base per-display: `railway/<hostname>/display/<id>/…`

### Binario

| Topic | Payload | Esempio |
|-------|---------|---------|
| `…/platform/destination` | testo | `Milano Centrale` |
| `…/platform/time` | HH:MM | `14:32` |
| `…/platform/status` | stato | `in partenza` |
| `…/platform/num` | 1–99 | `3` |

### Tabellone

| Topic | Payload |
|-------|---------|
| `…/timetable/row/N/time` | `14:30` |
| `…/timetable/row/N/dest` | `Roma Termini` |
| `…/timetable/row/N/bin` | `3` |
| `…/timetable/row/N/status` | `ritardo` |
| `…/timetable/station` | nome stazione |

Integrazione Rocrail: opzionale via automazione MQTT esterna o parser XML futuro.

---

## Cablaggio

```
WeMos D32R1          Display 1 (binario)    Display 2 (tabellone)
GPIO 21 SDA ─────────┬───────────────────────
GPIO 22 SCL ─────────┤
3.3V / GND ──────────┴── @0x3C          @0x3D
```

Stesso bus del VL6180X @ `0x29`.

---

## Build

Display opzionale (U8g2 ~25 KB flash):

```bash
pio run -e wemos_d1_mini32_usb_display -t upload
```

Env standard **senza** `-DUSE_DISPLAY` resta sotto il limite flash.
