/**
 * This file is part of the ESP32_EnvironmentalNode project.
 *
 * @file  sys_nvs.h
 * @brief NVS abstraction: config struct and load/save API.
 * @author d7main
 * contact info: demianzaiats@gmail.com
 * @license MIT
 */

#ifndef SYS_NVS_H
#define SYS_NVS_H

#include <stdint.h>
#include <stdbool.h>

/* ── Active-sensor bitmask constants ─────────────────────────────────────────
 *  Use these macros everywhere a specific sensor must be identified:
 *  in the registry, the web portal JavaScript, and NVS load/save.
 *  Adding a new sensor = assign the next free bit and add a HAL entry.
 * ─────────────────────────────────────────────────────────────────────────── */
#define SENSOR_BME280_BIT    (1UL << 0)   /**< BME280  — I2C  (Temp/Pressure/Humidity) */
#define SENSOR_SHT3X_BIT     (1UL << 1)   /**< SHT3x   — I2C  (Temp/Humidity)          */
#define SENSOR_BH1750_BIT    (1UL << 2)   /**< BH1750  — I2C  (Ambient Light)           */
#define SENSOR_MHZ19_BIT     (1UL << 3)   /**< MH-Z19  — UART (CO2 ppm)                */
#define SENSOR_PMS5003_BIT   (1UL << 4)   /**< PMS5003 — UART (PM2.5/PM10)             */
#define SENSOR_MOISTURE_BIT  (1UL << 5)   /**< Capacitive — ADC (Soil Moisture)        */
#define SENSOR_DS18B20_BIT   (1UL << 6)   /**< DS18B20 — 1-Wire (Temperature)          */

/* ── String field length limits ──────────────────────────────────────────── */
#define SYS_WIFI_SSID_MAX_LEN       32
#define SYS_WIFI_PASS_MAX_LEN       64
#define SYS_TG_TOKEN_MAX_LEN        64
#define SYS_TG_CHAT_MAX_LEN         32
#define SYS_WEBHOOK_URL_MAX_LEN    256  /* Discord & Custom Webhook URLs    */
#define SYS_MQTT_URI_MAX_LEN       128  /* Broker hostname/IP or full URI   */
#define SYS_MQTT_USER_MAX_LEN       64
#define SYS_MQTT_PASS_MAX_LEN       64
#define SYS_MQTT_PREFIX_MAX_LEN     64  /* e.g. "d7main/sensor"             */

/**
 * @brief Persisted device configuration.
 *
 *        All notification / MQTT fields are optional.
 *        An empty string (first byte == '\0') means the channel is disabled.
 *        Default values are applied by sys_nvs_load_config() when a key is
 *        missing from NVS (e.g. first boot after a firmware upgrade).
 */
typedef struct {
    /* ── Wi-Fi ──────────────────────────────────────────────────────────── */
    char wifi_ssid[SYS_WIFI_SSID_MAX_LEN];
    char wifi_pass[SYS_WIFI_PASS_MAX_LEN];

    /* ── Webhook / push notification channels (all optional) ────────────── */
    char tg_token[SYS_TG_TOKEN_MAX_LEN];
    char tg_chat_id[SYS_TG_CHAT_MAX_LEN];
    char discord_webhook_url[SYS_WEBHOOK_URL_MAX_LEN];
    char custom_webhook_url[SYS_WEBHOOK_URL_MAX_LEN];

    /* ── MQTT (optional — empty broker URI = channel disabled) ───────────── */
    char     mqtt_broker_uri[SYS_MQTT_URI_MAX_LEN];   /* host, IP, or URI   */
    char     mqtt_username[SYS_MQTT_USER_MAX_LEN];
    char     mqtt_password[SYS_MQTT_PASS_MAX_LEN];
    char     mqtt_topic_prefix[SYS_MQTT_PREFIX_MAX_LEN]; /* default: d7main/sensor */
    uint16_t mqtt_port;                                /* default: 1883      */

    /* ── ADC calibration ─────────────────────────────────────────────────── */
    int16_t v_dry_mv;
    int16_t v_wet_mv;

    /* ── Alert threshold ─────────────────────────────────────────────────── */
    uint8_t soil_alert_threshold_pct;  /* 0-100 %; default 30 */

    /* ── Active sensor bitmask ────────────────────────────────────────────── */
    /** Each bit corresponds to a SENSOR_*_BIT constant.  A set bit means the
     *  sensor's init/read_data/deinit functions will be called this wake cycle.
     *  Default (0) = all sensors off; user must opt-in via the portal.        */
    uint32_t active_sensors;

    bool is_configured;
} sys_config_t;

/* ── Public API ──────────────────────────────────────────────────────────── */
bool sys_nvs_init(void);
bool sys_nvs_load_config(sys_config_t *out_cfg);
bool sys_nvs_save_config(const sys_config_t *cfg);

#endif /* SYS_NVS_H */