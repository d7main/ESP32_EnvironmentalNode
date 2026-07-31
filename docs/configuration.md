# Configuration

All settings are managed through the **SoftAP Web Portal** — a self-hosted HTTP interface served directly by the ESP32-C3 over its own Wi-Fi access point. No mobile app, no cloud account, and no re-flashing is required to change any parameter.

---

## Entering the Portal

**On first boot** (or after a full erase flash), the node automatically detects that it is unconfigured and launches the portal.

**At any time during normal operation:**

1. Hold the **BOOT button (GPIO9)** for **3 seconds**.
2. The firmware halts the current operational cycle and starts the SoftAP.
3. From any device with Wi-Fi:

| Setting | Value |
|---|---|
| SSID | `EnvNode-Setup` |
| Password | `configure` |
| Portal URL | `http://192.168.4.1` |

!!! note "Portal exit"
    After submitting the form, the portal saves all values to NVS flash and performs a hard reboot.
    The device immediately re-enters normal operational mode with the new configuration.

---

## Wi-Fi Settings

| Field | Description | Required |
|---|---|---|
| **SSID** | Name of your 2.4 GHz Wi-Fi network | Yes |
| **Password** | Wi-Fi password (WPA2) | Yes (if secured) |

The node operates in **STA (client) mode** during normal cycles. The SoftAP is only active while the portal is open.

---

## Notification Settings

All three channels are **fully optional and independent**. A failure (or missing credentials) on any channel never blocks the others.

### Telegram

Delivered via the [Telegram Bot API](https://core.telegram.org/bots/api#sendmessage).

| Field | Description |
|---|---|
| **Bot Token** | Obtained from [@BotFather](https://t.me/BotFather) (`123456:ABC-abc…`) |
| **Chat ID** | Your personal or group chat numeric ID |

Leave both fields blank to disable Telegram alerts entirely.

### Discord

Sent as `{ "content": "<message>" }` via a [Discord Webhook](https://support.discord.com/hc/en-us/articles/228383668).

| Field | Description |
|---|---|
| **Discord Webhook URL** | Full webhook URL from your Discord channel settings |

Returns HTTP 204 on success.

### Custom Webhook

A `POST` request with a structured JSON body is sent to any URL you specify:

```json
{
  "event": "alert",
  "message": "Low soil moisture alert!...",
  "soil_mv": 2450,
  "temp": 22.5,
  "humidity": 61.0
}
```

| Field | Description |
|---|---|
| **Custom Webhook URL** | Any HTTPS endpoint that accepts a POST with `Content-Type: application/json` |

---

## MQTT Settings

Connects to any standard MQTT broker (Mosquitto, EMQX, HiveMQ, etc.) and publishes both Home Assistant Auto-Discovery configs and sensor state values on every wake-up cycle.

| Field | Description | Default |
|---|---|---|
| **Broker Host / IP** | Bare IP (`192.168.1.10`) or full URI (`mqtt://host:1883`) | `""` (disabled) |
| **Port** | TCP port | `1883` |
| **Topic Prefix** | Base path for all state topics | `d7main/sensor` |
| **Username** | Optional MQTT credentials | `""` |
| **Password** | Optional MQTT credentials | `""` |

!!! tip "Home Assistant Auto-Discovery"
    On each boot the node publishes one discovery config payload per entity to
    `homeassistant/sensor/<device_id>_<entity>/config` with `retain=1`.
    Home Assistant picks these up automatically — no manual entity configuration needed.

Leave **Broker Host / IP** blank to disable MQTT entirely. The rest of the wake-up cycle (sensors, alerts) is unaffected.

---

## Alert Threshold

| Field | Description | Default |
|---|---|---|
| **Soil Alert Threshold (%)** | Alerts fire when calculated moisture % falls below this value | `30%` |

If ADC calibration has not been set, moisture percentage cannot be calculated and threshold-based alerts are skipped. Raw mV values are still logged and published over MQTT.

---

## ADC Calibration

The capacitive soil moisture sensor outputs an analog voltage that varies by sensor batch, cable length, and soil composition. Two reference readings must be taken to map mV → %.

| Field | Description | How to measure |
|---|---|---|
| **Dry reference (mV)** | ADC reading with sensor fully in dry air | Read from the portal's live mV display with sensor out of soil |
| **Wet reference (mV)** | ADC reading with sensor fully submerged | Submerge sensor in a glass of water, read live mV |

The portal shows a **live mV reading** that updates every 2 seconds while the portal is open, making calibration straightforward.

!!! warning "Polarity"
    Capacitive sensors produce **lower voltage when wetter**.
    Ensure `Dry mV > Wet mV`. If the values are swapped the moisture % will calculate as 0% for all readings.

**Mapping formula:**

```
moisture_pct = (v_dry - current_mv) * 100 / (v_dry - v_wet)
```

Clamped to `[0, 100]`. Matches the JavaScript `calcPct()` function in the portal.

---

## Serial Monitor Output

Connect at **115200 baud** to observe the full operational log:

```
I APP_MAIN: ========================================
I APP_MAIN:  ESP32_EnvironmentalNode booting on ESP32-C3
I APP_MAIN: ========================================
I APP_MAIN: [HEAP] Boot baseline — Free: 263144 B
I APP_MAIN: Device is configured. Starting normal operation.
I APP_MAIN:   SSID       : MyNetwork
I APP_MAIN:   MQTT Broker: 192.168.1.10
I APP_MAIN:   Threshold  : 30%
I APP_MAIN: Reading sensors...
I APP_MAIN: Sensors: soil=2310 mV (47%) | BMP280: 22.3 C / 1012.4 hPa | DHT22: 22.1 C / 61.5%
I APP_MAIN: [HEAP] After sensor reads — Free: 262804 B
I SYS_MQTT: [MQTT] Connected to broker.
I SYS_MQTT: [MQTT] Discovery published: 6 entities.
I SYS_MQTT: [MQTT] State published: 7 topics.
I APP_MAIN: [HEAP] After MQTT cycle — Free: 263084 B
I APP_MAIN: Soil moisture OK (47% >= threshold 30%). No alert sent.
I APP_MAIN: [HEAP] Pre-sleep — Free: 263084 B | Min-ever: 218432 B
I APP_MAIN: Entering deep sleep for 1800 seconds.
```
