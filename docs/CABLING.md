# Cablaggio ShieldH0 — plastico H0

Riferimento per collegare WeMos D32R1, MUX, sensori, relè, I2C e Rocrail.

> Schema elettrico della PCB: [HARDWARE.md](HARDWARE.md) · [SCHEDA-H0.pdf](SCHEDA-H0.pdf)

---

## WeMos D32R1 — pin principali

| Funzione | GPIO | Note |
|----------|------|------|
| I2C SDA | 21 | Bus condiviso VL6180X + OLED |
| I2C SCL | 22 | |
| MUX indirizzo A0 | 32 | |
| MUX indirizzo A1 | 33 | |
| MUX indirizzo A2 | 25 | |
| MUX indirizzo A3 | 26 | |
| MUX segnale COM | 34 | **Solo INPUT** — lettura ADC sensori |
| WiFi reset | 0 | BOOT, tenere premuto 3 s |
| WS2812 strip 1 | 4 | Canale MUX 8 |
| WS2812 strip 2 | 5 | Canale MUX 9 |
| WS2812 strip 3 | 18 | Canale MUX 10 |
| WS2812 strip 4 | 19 | Canale MUX 11 |

> GPIO 34–39 sono input-only. Relè e semafori via MUX richiedono hardware che
> piloti uscite digitali (driver/transistor), non lettura diretta su SIG.

---

## Multiplexer CD74HC4067 (16 canali)

```
WeMos          MUX CD74HC4067
GPIO 32 ────── A0
GPIO 33 ────── A1
GPIO 25 ────── A2
GPIO 26 ────── A3
GPIO 34 ────── SIG (COM) ← sensore ADC o uscita relè*
3.3V/GND ───── VCC/GND

* Uscite relè: il segnale torna al COM del MUX; ogni canale Y0–Y15 va al carico.
```

Assegna i canali in **Sensori** / **Sistema** (web) o in `config.json` → `pin_map`.

---

## Bus I2C (4 fili)

| Dispositivo | Indirizzo | Filo |
|-------------|-----------|------|
| VL6180X ToF | 0x29 | VCC, GND, SDA, SCL |
| OLED binario SH1106 | 0x3C | idem |
| OLED tabellone | 0x3D | idem |
| PCA9685 (opz.) | 0x40 | idem |

```
WeMos SDA (21) ──┬── VL6180X
WeMos SCL (22) ──┤── OLED 0x3C (binario)
3.3V / GND ──────┴── OLED 0x3D (tabellone)
```

---

## Esempio layout stazione H0

| MUX | Ruolo | Oggetto plastico |
|-----|-------|------------------|
| 0–2 | Segnale RGB | Semaforo ingresso MAIN |
| 3–4 | Scambio | chS / chD deviatoio |
| 2 | Sensore assorbimento | Blocco approccio binario 3 |
| 4 | Relè | Luci sala |
| 5 | Relè | Luci PL (lampeggio) |
| 6 | Relè | Sbarra PL |
| 7 | Relè | Campana |

### Scenario tipico (Fase 9)

Sensore MUX **2** occupato →

1. PL `pl_nord`: luci lampeggiano + sbarra giù  
2. Luci `luci_sala`: ON  
3. Campana: impulso  
4. Display `bin3`: stato **in arrivo**, destinazione **Treno in transito**

Sensore libero → PL aperto, luci OFF, display **libero**.

Config in tab **Scenari** o:

```json
"scenarios": [{
  "id": "approccio_bin3",
  "mux_ch": 2,
  "accessory_pl": "pl_nord",
  "accessory_light": "luci_sala",
  "accessory_bell": "campana",
  "display_id": "bin3",
  "status_occupied": "in arrivo",
  "dest_occupied": "Treno in transito"
}]
```

---

## Rocrail MQTT

| Oggetto | Topic |
|---------|--------|
| Segnali | `rocrail/service/info/sg` |
| Scambi | `rocrail/service/command` |
| Feedback | `rocrail/service/client` / `info` |
| Board | `railway/ShieldH0/...` |

---

## Alimentazione

- WeMos: USB 5 V o regolatore 5 V → pin 5V  
- Relè modulo: bobina 5 V, **GND comune** con ESP32  
- Luci H0 12 V: alimentazione separata, relè in serie sul +12 V  
- OLED: **3,3 V** (non 5 V)

---

*Vedi anche `docs/HARDWARE.md`, `docs/DISPLAY.md`, `docs/PIANO.md`, `README.md`.*
