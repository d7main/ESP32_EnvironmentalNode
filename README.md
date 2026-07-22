# ESP32-C3 Environmental Monitoring Node (ESP-IDF)

A low-power environmental monitoring firmware built with native **ESP-IDF v5.x** and **PlatformIO** (no Arduino framework). Wakes from Deep Sleep, reads sensors, evaluates a configurable soil moisture threshold, dispatches alerts over one or more webhook channels, and returns to sleep.

An on-demand SoftAP Web Portal — launched by holding a physical button — handles all runtime configuration without serial re-flashing.

---

## Hardware & Screenshots

<div align="center">

<img src="https://github.com/user-attachments/assets/289511c9-2e68-44cc-9e47-d73a835792c4" width="300">
<img src="https://github.com/user-attachments/assets/02afec7f-1d40-4437-84a9-79e7a8dc2c39" width="300">
<img src="https://github.com/user-attachments/assets/80fdf836-1b7c-4d12-b02d-9d38fc25a386" width="300">

</div>




---

## Features

- **Pure ESP-IDF v5.x** — no Arduino wrappers, no bloat.
- **Deep Sleep** — timer wakeup via RTC; configurable interval (`DEFAULT_SLEEP_SEC`, default 30 min).
- **On-Demand Web Portal** — hold `GPIO9` (BOOT button) ≥ 3 s to launch the SoftAP configurator at `192.168.4.1`. Live sensor telemetry updates every 7 s while the portal is open.
- **Multi-Channel Alerts** — Telegram Bot API, Discord Webhook, and Custom JSON Webhook. Each channel is independent; a failed channel does not block the others.
- **Configurable Threshold** — soil alert threshold (%) stored in NVS, adjustable via the portal.
- **NVS Persistence** — Wi-Fi credentials, notification tokens/URLs, ADC calibration bounds, and alert threshold survive power cycles.
- **Sensors** — capacitive soil moisture (ADC), BMP280 (I²C, temperature + pressure), DHT22 (1-Wire, temperature + humidity).

---

## Target Hardware

| Parameter | Value |
|---|---|
| MCU | ESP32-C3 (`esp32-c3-devkitm-1`) |
| Flash | 2 MB, DIO mode |
| Framework | ESP-IDF 5.x |
| Toolchain | PlatformIO |

GPIO assignments are defined in [`include/config.h`](include/config.h) and selectable per target via `build_flags` in `platformio.ini`.

| Signal | ESP32-C3 GPIO |
|---|---|
| Soil sensor power gate | GPIO7 |
| Soil sensor ADC | GPIO2 (ADC CH1) |
| BMP280 SDA | GPIO8 |
| BMP280 SCL | GPIO9 |
| DHT22 data | GPIO10 |
| Config button (BOOT) | GPIO9 |

> **Strapping pin note:** GPIO2 on ESP32-C3 controls boot mode. Pulling it LOW during power-on enters JTAG/download mode. Use GPIO9 (BOOT button) for the config trigger instead — it is safe to hold LOW at any time.

---

## Quick Start

### Prerequisites

- [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/index.html) or the VS Code PlatformIO extension
- USB cable to ESP32-C3 dev board

### Build

```bash
git clone https://github.com/your-username/ESP32_EnvironmentalNode.git
cd ESP32_EnvironmentalNode
pio run --environment esp32_c3
```

### Upload & Monitor

```bash
pio run --target upload --target monitor --environment esp32_c3
```

Monitor baud rate: **115200**.

---

## First-Time Setup

1. Flash the firmware. On first boot `is_configured = false`, so the device immediately launches the SoftAP.
2. Connect a phone or laptop to Wi-Fi SSID **`d7main_sensor`** (password: `admin123`).
3. Open `http://192.168.4.1` in a browser.
4. Fill in the form and click **Write to NVS & Reboot**:

| Field | Required | Notes |
|---|---|---|
| Wi-Fi SSID | **Yes** | Your router SSID |
| Wi-Fi Password | No | Leave blank for open networks |
| Telegram Bot Token | No | Skip to disable Telegram alerts |
| Telegram Chat ID | No | Skip to disable Telegram alerts |
| Discord Webhook URL | No | Skip to disable Discord alerts |
| Custom Webhook URL | No | Skip to disable custom alerts |
| Soil Alert Threshold (%) | No | Default: 30% |
| Dry Air (mV) | **Yes** | Calibration — see below |
| Water (mV) | **Yes** | Calibration — see below |

To re-enter the portal at any time: hold `GPIO9` (BOOT button) for ≥ 3 seconds.

---

## Sensor Calibration

The moisture percentage is computed linearly from two ADC calibration points:

```
moisture_pct = (v_dry - soil_mv) * 100 / (v_dry - v_wet)
```

| Step | Action |
|---|---|
| **v_dry** | Expose the sensor to dry open air. Read `soil_mv` from the Live Telemetry card in the portal, or from the serial monitor. Enter that value. |
| **v_wet** | Submerge the sensor tip ~1 cm in water. Read and enter `soil_mv`. |

Typical values: `v_dry ≈ 2600 mV`, `v_wet ≈ 1200 mV` (varies by sensor and supply voltage).

---

## Alert Channels

### Telegram

Requires a bot token and chat ID. Alert text is sent as a `text` message via `sendMessage`.

### Discord Webhook

POST to the configured URL with:
```json
{ "content": "<alert message>" }
```

### Custom Webhook

POST to the configured URL with a full sensor payload:
```json
{
  "event":    "alert",
  "message":  "Low soil moisture alert!\nMoisture: 18% ...",
  "soil_mv":  2350,
  "temp":     23.1,
  "humidity": 58.0
}
```

All channels: `Content-Type: application/json`, 6-second timeout per request.

---

## Firmware Boot Cycle

```
Power on / Deep Sleep wakeup
│
├─ NVS init → load config
├─ is_configured?
│   ├─ No  → launch SoftAP + Web Portal (blocks until form submit + reboot)
│   └─ Yes →
│       ├─ Spawn button monitor task (GPIO9 hold = portal escape hatch)
│       ├─ Read sensors (soil ADC, BMP280, DHT22)
│       ├─ Calculate moisture %
│       ├─ Connect Wi-Fi STA
│       │   ├─ moisture < threshold → sys_alerts_send() [Telegram / Discord / Custom]
│       │   └─ moisture OK         → log, skip alerts
│       └─ Deep Sleep (DEFAULT_SLEEP_SEC = 1800 s)
```

---

## NVS Key Map

| Key | Type | Field |
|---|---|---|
| `ssid` | str | `wifi_ssid` |
| `pass` | str | `wifi_pass` |
| `tg_tok` | str | `tg_token` |
| `tg_chat` | str | `tg_chat_id` |
| `disc_url` | str | `discord_webhook_url` |
| `cust_url` | str | `custom_webhook_url` |
| `v_dry` | i16 | `v_dry_mv` |
| `v_wet` | i16 | `v_wet_mv` |
| `soil_th_pct` | u8 | `soil_alert_threshold_pct` |
| `conf` | u8 | `is_configured` |

NVS namespace: `storage` (see `NVS_NAMESPACE` in `config.h`).

---

## Project Structure

```
ESP32_EnvironmentalNode/
├── include/
│   ├── config.h          # GPIO map, sleep interval, AP credentials
│   ├── sys_nvs.h         # Config struct + NVS API
│   ├── sys_wifi.h        # Wi-Fi, Web Portal, sys_alerts_send()
│   ├── hal_bmp280.h
│   ├── hal_dht22.h
│   ├── hal_i2c.h
│   └── hal_moisture.h
├── src/
│   ├── main.c            # App entry, boot cycle, operational loop
│   ├── sys_nvs.c         # NVS load/save
│   ├── sys_wifi.c        # AP/STA, HTTP server, alert dispatch
│   ├── hal_bmp280.c
│   ├── hal_dht22.c
│   ├── hal_i2c.c
│   └── hal_moisture.c
├── partitions.csv        # Custom partition table (2 MB flash)
└── platformio.ini
```

---

## License

MIT — see [`LICENSE`](LICENSE) for details.

Author: **d7main** (Demian Zaiats) — demianzaiats@gmail.com
