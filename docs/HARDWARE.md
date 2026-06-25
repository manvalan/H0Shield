# ShieldH0 — schema elettrico PCB

Schema ufficiale della scheda: **[SCHEDA-H0.pdf](SCHEDA-H0.pdf)**  
EasyEDA · revisione **V1.0** · 2026-05-29

---

## Panoramica

La PCB **ShieldH0** espande un microcontrollore in formato **Arduino UNO** con:

| Blocco | Componente | Funzione |
|--------|------------|----------|
| **P1** | CD74HC4067 | Multiplexer analogico/digitale 16 canali (Y0–Y15) |
| **U1** | Multiplexer I2C | 1 bus SDA/SCL → 8 bus indipendenti SC0/SD0 … SC7/SD7 |
| **H1, H2** | Connettori 1×10 (2,54 mm) | Uscite MUX + alimentazione 3,3 V |
| **U9–U16** | Connettori 1×4 (2,54 mm) | Bus I2C dedicati per display, sensori, PCA9685, ecc. |
| **U8** | Arduino UNO (footprint) | Host di riferimento sullo schema |

> Il firmware in questo repository è compilato per **WeMos D32R1 (ESP32)**.  
> I GPIO effettivi usati dal codice sono in `include/Config.h` e in [CABLING.md](CABLING.md).  
> Lo schema PDF descrive la **topologia della PCB** (MUX, bus I2C, connettori); il mapping pin
> Arduino UNO → ESP32 dipende da come la scheda è montata o adattata.

---

## CD74HC4067 (P1) — collegamenti MCU

| Segnale MUX | Pin Arduino (schema) | Ruolo |
|-------------|----------------------|-------|
| **S0** | D3 | Bit 0 selezione canale |
| **S1** | D8 | Bit 1 |
| **S2** | D9 | Bit 2 |
| **S3** | D10 | Bit 3 |
| **SIG** (COM) | D2 **e** A2 | Linea comune: digitale o analogica |
| **EN** | — | Enable (verificare sul PCB se a GND o routato) |
| **VCC / GND** | 3,3 V / GND | Alimentazione logica |

Per selezionare il canale *N* (0–15): impostare S3…S0 al valore binario di *N*, poi leggere o
scrivere su **SIG**.

---

## Connettori MUX — H1 e H2

Entrambi i connettori **2,54-1×10** espongono alimentazione e 8 canali del MUX:

| Pin header | Segnale |
|------------|---------|
| 1 | **V33** (3,3 V) |
| 2 | **GND** |
| 3–10 | Canali MUX (C15…C8 su H1, C7…C0 su H2) |

Ogni pin 3–10 corrisponde a un canale **Y** del CD74HC4067 (indice 0–15 nel firmware → `pin_map` /
`config.json`).

---

## Bus I2C — U1 e connettori U9–U16

Il bus principale **SDA / SCL** (Arduino A4 / A5 sullo schema) entra nel multiplexer I2C **U1**,
che genera 8 coppie indipendenti:

| Coppia | Connettore | Uso tipico |
|--------|------------|------------|
| SC0 / SD0 | U9 | Display / sensore |
| SC1 / SD1 | U10 | |
| … | … | |
| SC7 / SD7 | U16 | |

**Pinout connettore 1×4 (U9–U16):**

| Pin | Segnale |
|-----|---------|
| 1 | **SCx** (SCL del sotto-bus) |
| 2 | **SDx** (SDA del sotto-bus) |
| 3 | **GND** |
| 4 | **V33** (3,3 V) |

Prima di parlare con un dispositivo su SC*n*/SD*n*, il firmware (o un driver TCA9548A) deve
**selezionare il sotto-bus** corrispondente. Utile per più OLED con lo stesso indirizzo I2C
(0x3C / 0x3D) su bus separati.

---

## Firmware ESP32 (WeMos D32R1)

Mapping attuale nel codice (`include/Config.h`):

| Funzione | GPIO ESP32 |
|----------|------------|
| MUX A0–A3 | 32, 33, 25, 26 |
| MUX SIG (ADC) | 34 (solo input) |
| I2C SDA / SCL | 21 / 22 |
| Reset WiFi (BOOT) | 0 |
| WS2812 (strip dirette) | 4, 5, 18, 19 |

Vedi [CABLING.md](CABLING.md) per cablaggio plastico H0, indirizzi I2C e scenari.

---

## Note progettuali

1. **SIG su D2 e A2** (schema UNO): consente lettura digitale o ADC sullo stesso nodo COM del MUX.
2. **GPIO 34** su ESP32 è input-only: relè e uscite digitali via MUX richiedono hardware di
   pilotaggio (transistor/MOSFET) come previsto dalla PCB, non `digitalWrite` diretto su SIG.
3. **Alimentazione**: logica 3,3 V (**V33**); non alimentare OLED o sensori I2C a 5 V sui
   connettori U9–U16.

---

*Schema sorgente: `docs/SCHEDA-H0.pdf` · Cablaggio layout: [CABLING.md](CABLING.md)*
