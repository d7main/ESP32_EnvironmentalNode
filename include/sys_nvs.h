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

    bool is_configured;
} sys_config_t;

/* ── Public API ──────────────────────────────────────────────────────────── */
bool sys_nvs_init(void);
bool sys_nvs_load_config(sys_config_t *out_cfg);
bool sys_nvs_save_config(const sys_config_t *cfg);

#endif /* SYS_NVS_H */