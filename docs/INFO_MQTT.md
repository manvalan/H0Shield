# Sistema informativo plastico (SIP) — protocollo MQTT

Un **unico topic MQTT** con payload **JSON** alimenta tutti i display del plastico.
Ogni scheda ShieldH0 si iscrive al topic e **applica solo i messaggi che corrispondono**
ai display configurati (stazione + binario, oppure `display_id`).

Topic default: **`railway/info`** (configurabile in `config.json` → `info_mqtt_topic`).

---

## Configurazione display

Ogni display in `config.json` definisce i filtri di matching:

```json
{
  "id": "bin3",
  "type": "platform",
  "platform_num": 3,
  "station_id": "castellina",
  "station_name": "Castellina sui Monti"
}
```

| Campo | Uso |
|-------|-----|
| `station_id` | Slug breve (es. `castellina`) — confronto case-insensitive |
| `station_name` | Nome esteso (es. `Castellina sui Monti`) |
| `platform_num` | Numero binario per display tipo `platform` |
| `id` | Usato se il messaggio contiene `display_id` |

Se stazione e binario nel messaggio coincidono con la config, il display si aggiorna.

---

## Aggiornamento binario (singolo)

**Topic:** `railway/info`

**Payload:**

```json
{
  "station_id": "castellina",
  "station": "Castellina sui Monti",
  "platform": 3,
  "destination": "Roma Termini",
  "departure_time": "14:32",
  "status": "in arrivo"
}
```

Campi accettati:

| Campo JSON | Alternativa | Descrizione |
|------------|-------------|-------------|
| `station_id` | — | Slug stazione |
| `station` | `station_name` | Nome stazione |
| `platform` | `platform_num` | Numero binario (1–99) |
| `destination` | `dest` | Destinazione treno |
| `departure_time` | `time` | Ora partenza (HH:MM) |
| `status` | — | `libero`, `in arrivo`, `in partenza`, `ritardo`, `soppresso` |
| `display_id` | — | Target diretto per ID display (ignora stazione/binario) |

---

## Aggiornamento multiplo binari

```json
{
  "station_id": "castellina",
  "platforms": [
    {
      "platform": 1,
      "destination": "Firenze SMN",
      "departure_time": "15:10",
      "status": "in partenza"
    },
    {
      "platform": 3,
      "destination": "Roma Termini",
      "departure_time": "14:32",
      "status": "in arrivo"
    }
  ]
}
```

`station_id` / `station_name` a livello radice vengono ereditati da ogni elemento dell'array.

---

## Tabellone orario

```json
{
  "type": "timetable",
  "station_id": "castellina",
  "station_name": "Castellina sui Monti",
  "rows": [
    { "time": "14:30", "destination": "Roma Termini", "platform": "3", "status": "" },
    { "time": "14:45", "destination": "Torino P.N.", "platform": "1", "status": "ritardo" }
  ]
}
```

Oppure annidato:

```json
{
  "timetable": {
    "station_id": "castellina",
    "rows": [ ... ]
  }
}
```

---

## Esempio da terminale (mosquitto)

Aggiorna binario 3 a Castellina:

```bash
mosquitto_pub -h BROKER -t railway/info -m '{
  "station_id": "castellina",
  "platform": 3,
  "destination": "Roma Termini",
  "departure_time": "14:32",
  "status": "in arrivo"
}'
```

Libera il binario:

```bash
mosquitto_pub -h BROKER -t railway/info -m '{
  "station_id": "castellina",
  "platform": 3,
  "destination": "",
  "departure_time": "",
  "status": "libero"
}'
```

---

## Compatibilità

Resta attivo anche il protocollo **per-display** su topic dedicati:

`railway/<hostname>/display/<id>/platform/…`

Vedi [DISPLAY.md](DISPLAY.md).

---

## Abilitazione

In `config.json`:

```json
"info_mqtt_topic": "railway/info",
"info_mqtt_enabled": true
```

Disabilitando `info_mqtt_enabled` la scheda non si iscrive al topic SIP (restano i topic per-display).
