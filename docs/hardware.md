# Hardware

## Bill of Materials

| Component | Part | Notes |
|---|---|---|
| Microcontroller | Espressif ESP32-C3-DevKitM-1 | RISC-V, 160 MHz, 400 KB RAM, 4 MB flash |
| Temperature & Pressure | Bosch BMP280 (I²C breakout) | Forced mode — powers down between readings |
| Temp & Humidity | AOSONG DHT22 / AM2302 | Single-wire bit-bang; add 10 kΩ pull-up on data pin |
| Soil Moisture | Capacitive sensor (STEMMA QT or equivalent) | Outputs 0–3.3 V; connected to ADC GPIO |
| USB-UART | CH340/CH341 (on-board DevKit) | Required for serial monitor and non-stub flash |

---

## GPIO Pin Map

Defined in [`include/config.h`](https://github.com/d7main/ESP32_EnvironmentalNode/blob/main/include/config.h).

| GPIO | Function | Direction | Notes |
|---|---|---|---|
| `GPIO0` | Soil moisture ADC | Input (ADC1 CH0) | Capacitive sensor signal |
| `GPIO1` | Soil sensor power | Output | HIGH to power sensor, LOW to cut power between reads |
| `GPIO3` | I²C SDA | Bidirectional | BMP280 data line |
| `GPIO4` | I²C SCL | Output | BMP280 clock line |
| `GPIO5` | DHT22 data | Bidirectional | 1-wire bit-bang; 10 kΩ pull-up to 3.3 V |
| `GPIO9` | BOOT / Config button | Input (pull-up) | Hold 3 s → launch portal |

!!! note "ADC on ESP32-C3"
    The ESP32-C3 has one 12-bit ADC controller (ADC1) with channels on GPIO0–GPIO4.
    GPIO0 is used for the moisture sensor. ADC2 channels are unavailable when Wi-Fi is active,
    which is why ADC1 (GPIO0) was chosen — it remains functional throughout the entire cycle.

---

## I²C Bus

| Parameter | Value |
|---|---|
| SDA | GPIO3 |
| SCL | GPIO4 |
| Speed | 400 kHz (Fast Mode) |
| Pull-up | 4.7 kΩ to 3.3 V (on breakout board or add externally) |

**I²C Devices:**

| Device | Address | Notes |
|---|---|---|
| BMP280 | `0x76` or `0x77` | SDO pin LOW → 0x76 (default); HIGH → 0x77 |

---

## BMP280 Wiring

| BMP280 Pin | ESP32-C3 Pin | Notes |
|---|---|---|
| VCC | 3V3 | 3.3 V supply |
| GND | GND | Common ground |
| SDA | GPIO3 | I²C data |
| SCL | GPIO4 | I²C clock |
| SDO | GND | Sets I²C address to 0x76 |
| CSB | 3V3 | Selects I²C mode (tie HIGH) |

The BMP280 is read in **forced mode**: the sensor takes one measurement, then returns to sleep (< 5 µA). This matches the Deep Sleep duty cycle of the firmware — no continuous sensor polling.

---

## DHT22 Wiring

| DHT22 Pin | ESP32-C3 Pin | Notes |
|---|---|---|
| VCC | 3V3 | 3.3 V supply |
| GND | GND | Common ground |
| DATA | GPIO5 | Bit-bang single-wire; 10 kΩ pull-up to 3.3 V required |

!!! warning "DHT22 timing sensitivity"
    The DHT22 protocol uses 1-wire with tight µs-level timing.
    The firmware acquires the bus with `portENTER_CRITICAL()` during the 40-bit read to prevent FreeRTOS context switches from corrupting the signal.
    Do not place the DHT22 data pin near high-frequency signals (SPI, PWM).

---

## Capacitive Soil Moisture Sensor Wiring

| Sensor Pin | ESP32-C3 Pin | Notes |
|---|---|---|
| VCC | GPIO1 (power control) | Powered on only during ADC read; saves ~3 mA in sleep |
| GND | GND | Common ground |
| AOUT | GPIO0 | Analog voltage output |

The sensor is powered via a GPIO rather than a fixed 3.3 V rail so the firmware can cut power between reads, reducing idle current during the Deep Sleep interval.

**Typical ADC range (3.3 V supply):**

| State | Approximate mV |
|---|---|
| Sensor in dry air | 2400 – 2800 mV |
| Sensor in water (100% wet) | 1000 – 1400 mV |

These ranges vary by sensor batch. Use the portal's live mV display to determine your exact calibration points.

---

## Power Budget (Estimate)

| State | Current | Duration |
|---|---|---|
| Deep Sleep | ~5 µA | 30 min (default) |
| Wi-Fi association | ~80 mA peak | ~2–3 s |
| MQTT publish cycle | ~70 mA | ~1–2 s |
| Sensor reads (all) | ~20 mA | ~100 ms |
| SoftAP portal (idle) | ~60 mA | User-controlled |

On a 3000 mAh LiPo at the default 30-minute sleep interval, runtime exceeds **6 months** — assuming no persistent alerts and a fast broker connection each cycle.

!!! tip "Extending battery life"
    Increase `DEFAULT_SLEEP_SEC` in [`include/config.h`](https://github.com/d7main/ESP32_EnvironmentalNode/blob/main/include/config.h)
    to reduce the wake-up frequency.
    The MQTT Auto-Discovery payloads are published on every boot (retained=1),
    so HA always has fresh entity registrations even with long sleep intervals.
