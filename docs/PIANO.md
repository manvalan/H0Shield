# ShieldH0 — Piano di intervento firmware completo

Documento di riferimento per portare ShieldH0 a firmware **generale e completo**:
dashboard grafica, sensori, display 1,3″, accessori relè, integrazione Rocrail.

**Hardware di riferimento:** WeMos D32R1, MUX CD74HC4067, bus I2C (SDA 21 / SCL 22).

---

## Obiettivi finali

| Area | Obiettivo |
|------|-----------|
| Segnali | Grafica SVG semafori **MAIN** (3 lampade verticali) e **SHUNT** (3 lampade basse) |
| Scambi | Grafica SVG deviatoio (raddrizzato / deviato / busy) |
| Sensori assorbimento | Segmento binario + barra ADC + soglia configurabile |
| ToF VL6180X | Soglia mm: sopra = libero, sotto = occupato + feedback Rocrail |
| Display | OLED **1,3″ bianco/blu 128×64 I2C SH1106** — quadro orario |
| Accessori | Relè configurabili: luci, PL, campana, generico |
| Web UI | Minimalista, scura, 7 tab, test manuale |
| MQTT | Rocrail compatibile + topic ShieldH0 estesi |

---

## Display confermato

| Parametro | Valore |
|-----------|--------|
| Dimensione | 1,3″ |
| Tipo | OLED bianco/blu |
| Risoluzione | 128×64 |
| Bus | I2C (4 fili: VCC, GND, SDA, SCL) |
| Chip | SH1106 (fallback SSD1306 @ 0x3C) |
| Libreria | U8g2, flag compile `-DUSE_DISPLAY` |

Layout quadro orario:

```
┌──────────────────────────────┐
│ 14:32              Staz. H0  │
│──────────────────────────────│
│ Milano Centrale              │
│ Bin 3 · in partenza      ▶   │
└──────────────────────────────┘
```

Modalità: `clock` · `timetable` · `status` · `event`.

---

## Fasi di implementazione

### Fase 0 — Bug critici ✅ (completata)

| ID | Stato |
|----|-------|
| B1 | ✅ `SignalRGBChannel` usa chR/chG/chV indipendenti |
| B2 | ✅ Nota GPIO 34 in `Config.h` |
| B3 | ✅ `/api/test` richiede auth |
| B4 | ✅ `/api/auth/check` (sessione precedente) |
| B5 | ✅ Marmotta non bloccante |
| B6 | ✅ ToF status solo su cambio |
| B7 | ⏳ PCA9685 → Fase 6 |

---

### Fase 1 — Modello occupazione unificato ✅ (completata)

- `/api/status` con sensori MUX: `raw`, `threshold`, `id`, `occupied`
- ToF: `tof_threshold_mm`, `tof_enabled`, occupazione dist &lt; soglia
- `tof_blocks[]` → feedback Rocrail `<fb>`
- Tab Sensori: soglia ToF + blocchi Rocrail
- Dashboard: barre ADC + gauge ToF

---

### Fase 2 — Grafica segnali (MAIN / SHUNT) ✅ (completata)

- SVG inline: semaforo MAIN (3 lampade verticali) e SHUNT (A/B/C)
- `/api/status`: `{ type, aspect, lamps: { r, g, v } }`
- Dashboard: griglia segnali con lampade accese + glow
- Test manuale: pulsanti R/Y/V o Stop/Via/Obliq → `POST /api/test`

---

### Fase 3 — Grafica scambi ✅ (completata)

- SVG deviatoio con ramo evidenziato (raddrizzato / deviato)
- Animazione pulse sul fulcro durante impulso relè (`busy`)
- `/api/status`: `{ position, busy }`
- Test manuale: pulsanti → / ↗ → `POST /api/test`

---

### Fase 4 — Dashboard ridisegnata ✅ (completata)

- Layout wide (920px): stats 4 colonne, summary pill, binari full-width
- Griglia 2 colonne Segnali | Scambi (responsive)
- Segmenti binario visivi (verde/rosso) + barre ADC/ToF
- Badge contatori per sezione

### Fase 5 — ToF completo in UI ✅ (completata in Fase 1)

---

### Fase 6 — PCA9685 e polish

- Integrare `PCA9685Signal.h` in `main.cpp` (env `wemos_d1_mini32_pca`) **oppure** rimuovere codice morto
- Aspect di boot configurabile (default rosso/stop)
- README allineato al codice
- Env `shieldh0_minimal` vs `shieldh0_full`

---

### Fase 7 — Display I2C (SH1106 128×64)

- `DisplayManager.h` + U8g2
- `HardwareProbe`: rilevamento 0x3C / 0x3D
- NTP (`Europe/Rome` configurabile)
- MQTT: `railway/<host>/display/line/N`
- Tab web **Display** + anteprima ASCII 128×64

---

### Fase 8 — Accessori relè configurabili

Profili (`AccessoryProfile`):

| Profilo | Uso |
|---------|-----|
| `generic` | ON/OFF libero |
| `light` | Illuminazione layout (relè → strip 12V) |
| `level_xing` | Passaggio a livello (lampeggio + barra) |
| `bell` | Campana (impulso) |
| `gate` | Sbarra |

Config `accessories[]` in `config.json`.  
MQTT: `{ "accessory": "id", "action": "on" }` (retrocompat. `channel`).

---

### Fase 9 — Integrazione scenari

- PL attivato da sensore occupazione → relè luci + messaggio display `event`
- Dashboard unificata tutti gli output
- Documentazione cablaggio H0

---

## Schema config target (evoluzione)

```json
{
  "hostname": "ShieldH0",
  "sensor_threshold": 512,
  "tof_enabled": true,
  "tof_threshold_mm": 35,
  "display": {
    "enabled": true,
    "type": "sh1106_128x64",
    "i2c_addr": 60,
    "mode": "timetable",
    "station_name": "Stazione H0",
    "lines": [
      { "text": "Milano Centrale", "scroll": false },
      { "text": "Bin 3 · in partenza", "scroll": true }
    ],
    "ntp_timezone": "Europe/Rome"
  },
  "accessories": [
    { "id": "luci_sala", "mux_ch": 4, "profile": "light" },
    { "id": "pl_nord", "profile": "level_xing", "mux_ch_lights": 5, "mux_ch_bar": 6 }
  ],
  "signals": [],
  "turnouts": [],
  "sensors_rb": [],
  "tof_blocks": []
}
```

---

## Tab web UI finali

| Tab | Contenuto |
|-----|-----------|
| Dashboard | Monitoraggio grafico live + test rapido |
| Rete | WiFi, auth, scan reti |
| Sensori | ADC, ToF, mapping Rocrail |
| Segnali & Scambi | Config + test |
| Accessori | Relè e profili |
| Display | Quadro orario |
| Sistema | Hostname, MQTT, MUX |

---

## Budget flash (stima)

| Componente | Flash |
|------------|-------|
| Stato attuale | ~94% |
| Fase 0–4 (SVG + API) | +0 KB (HTML in LittleFS) |
| Fase 7 U8g2 SH1106 | +20–25 KB |
| Fase 8 Accessori | +5–8 KB |

**Strategia:** `-Os`, `CORE_DEBUG_LEVEL=1`, flag opzionali `USE_DISPLAY`, `USE_ACCESSORIES`.

---

## Ordine di esecuzione

```
Fase 0 → Fase 1 → Fase 2 → Fase 3 → Fase 4 → Fase 5
                                              ↓
                    Fase 8 → Fase 7 → Fase 9 → Fase 6
```

---

## Note hardware

- **MUX SIG (GPIO 34):** solo input su ESP32; output digitali MUX richiedono shield con driver appropriato o pin output dedicato.
- **I2C condiviso:** VL6180X @ 0x29, SH1106 @ 0x3C, PCA9685 @ 0x40 (opz.).
- **WS2812:** GPIO diretti (4, 5, 18, 19), non via MUX.

---

*Ultimo aggiornamento: giugno 2025 — display 1,3″ 128×64 I2C bianco/blu confermato.*
