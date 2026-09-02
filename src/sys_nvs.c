/**
 * @file sys_nvs.c
 * @brief Non-Volatile Storage (NVS) abstraction layer for configuration persistence
 * @author d7main
 * @license MIT
 */

#include "sys_nvs.h"
#include "config.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "SYS_NVS";

/**
 * @brief Initializes the default NVS partition.
 *        If the partition is truncated or has a new version, it will be erased and reinitialized.
 * 
 * @return true on success, false otherwise
 */
bool sys_nvs_init(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition truncated or upgraded. Erasing and retrying...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return (err == ESP_OK);
}

/**
 * @brief Loads the system configuration from NVS.
 *        If a key is missing, it falls back to a safe default (empty string or 0)
 *        to prevent reading garbage memory.
 * 
 * @param out_cfg Pointer to the configuration structure to populate
 * @return true if NVS handle opened successfully, false otherwise
 */
bool sys_nvs_load_config(sys_config_t *out_cfg) {
    // Zero out the structure to prevent any random RAM garbage
    memset(out_cfg, 0, sizeof(sys_config_t));

    nvs_handle_t handle;
    // Open in READWRITE mode. If the namespace doesn't exist yet, 
    // it will be created automatically instead of failing instantly.
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS namespace (%s). Brand new chip?", esp_err_to_name(err));
        out_cfg->is_configured = false;
        return false;
    }

    size_t len;

    // Load strings safely. If the key is missing, write a null-terminator.
    len = SYS_WIFI_SSID_MAX_LEN;
    if (nvs_get_str(handle, "ssid", out_cfg->wifi_ssid, &len) != ESP_OK) {
        out_cfg->wifi_ssid[0] = '\0';
    }

    len = SYS_WIFI_PASS_MAX_LEN;
    if (nvs_get_str(handle, "pass", out_cfg->wifi_pass, &len) != ESP_OK) {
        out_cfg->wifi_pass[0] = '\0';
    }

    len = SYS_TG_TOKEN_MAX_LEN;
    if (nvs_get_str(handle, "tg_tok", out_cfg->tg_token, &len) != ESP_OK) {
        out_cfg->tg_token[0] = '\0';
    }

    len = SYS_TG_CHAT_MAX_LEN;
    if (nvs_get_str(handle, "tg_chat", out_cfg->tg_chat_id, &len) != ESP_OK) {
        out_cfg->tg_chat_id[0] = '\0';
    }

    len = SYS_WEBHOOK_URL_MAX_LEN;
    if (nvs_get_str(handle, "disc_url", out_cfg->discord_webhook_url, &len) != ESP_OK) {
        out_cfg->discord_webhook_url[0] = '\0';
    }

    len = SYS_WEBHOOK_URL_MAX_LEN;
    if (nvs_get_str(handle, "cust_url", out_cfg->custom_webhook_url, &len) != ESP_OK) {
        out_cfg->custom_webhook_url[0] = '\0';
    }

    // ── MQTT (all optional — default to empty / standard defaults) ────────
    len = SYS_MQTT_URI_MAX_LEN;
    if (nvs_get_str(handle, "mqtt_uri", out_cfg->mqtt_broker_uri, &len) != ESP_OK) {
        out_cfg->mqtt_broker_uri[0] = '\0';  /* empty = MQTT disabled */
    }

    len = SYS_MQTT_USER_MAX_LEN;
    if (nvs_get_str(handle, "mqtt_user", out_cfg->mqtt_username, &len) != ESP_OK) {
        out_cfg->mqtt_username[0] = '\0';
    }

    len = SYS_MQTT_PASS_MAX_LEN;
    if (nvs_get_str(handle, "mqtt_pass", out_cfg->mqtt_password, &len) != ESP_OK) {
        out_cfg->mqtt_password[0] = '\0';
    }

    len = SYS_MQTT_PREFIX_MAX_LEN;
    if (nvs_get_str(handle, "mqtt_prefix", out_cfg->mqtt_topic_prefix, &len) != ESP_OK) {
        /* Sensible default so HA topics work out-of-the-box */
        strncpy(out_cfg->mqtt_topic_prefix, "d7main/sensor", SYS_MQTT_PREFIX_MAX_LEN - 1);
        out_cfg->mqtt_topic_prefix[SYS_MQTT_PREFIX_MAX_LEN - 1] = '\0';
    }

    if (nvs_get_u16(handle, "mqtt_port", &out_cfg->mqtt_port) != ESP_OK) {
        out_cfg->mqtt_port = 1883;
    }

    // Load soil alert threshold; default to 30% if not previously saved
    if (nvs_get_u8(handle, "soil_th_pct", &out_cfg->soil_alert_threshold_pct) != ESP_OK) {
        out_cfg->soil_alert_threshold_pct = 30;
    }

    // Load integer values safely. If missing, default to 0.
    if (nvs_get_i16(handle, "v_dry", &out_cfg->v_dry_mv) != ESP_OK) {
        out_cfg->v_dry_mv = 0;
    }

    if (nvs_get_i16(handle, "v_wet", &out_cfg->v_wet_mv) != ESP_OK) {
        out_cfg->v_wet_mv = 0;
    }

    // Load the configuration status flag
    uint8_t conf = 0;
    if (nvs_get_u8(handle, "conf", &conf) != ESP_OK) {
        conf = 0;
    }
    out_cfg->is_configured = (conf == 1);

    ESP_LOGI(TAG, "Configuration loaded. is_configured = %d", out_cfg->is_configured);

    nvs_close(handle);
    return true;
}

/**
 * @brief Saves the given configuration structure back to NVS.
 * 
 * @param cfg Pointer to the configuration to save
 * @return true on successful write and commit, false otherwise
 */
bool sys_nvs_save_config(const sys_config_t *cfg) {
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace for writing!");
        return false;
    }

    /* Check every write individually.  If any nvs_set_* call fails (e.g.
     * ESP_ERR_NVS_NOT_ENOUGH_SPACE or a flash ECC error) we close without
     * calling nvs_commit() so that a partial write is never persisted.
     * A partial commit would leave is_configured = true with an empty SSID,
     * creating an unrecoverable boot loop. */
    esp_err_t err = ESP_OK;

#define NVS_SET(fn, key, val)                                                    \
    if (err == ESP_OK) {                                                         \
        err = fn(handle, key, val);                                              \
        if (err != ESP_OK) {                                                     \
            ESP_LOGE(TAG, "nvs write failed [%s]: %s", key, esp_err_to_name(err)); \
        }                                                                        \
    }

    NVS_SET(nvs_set_str, "ssid",        cfg->wifi_ssid)
    NVS_SET(nvs_set_str, "pass",        cfg->wifi_pass)
    NVS_SET(nvs_set_str, "tg_tok",      cfg->tg_token)
    NVS_SET(nvs_set_str, "tg_chat",     cfg->tg_chat_id)
    NVS_SET(nvs_set_str, "disc_url",    cfg->discord_webhook_url)
    NVS_SET(nvs_set_str, "cust_url",    cfg->custom_webhook_url)

    // ── MQTT ──────────────────────────────────────────────────────────────
    NVS_SET(nvs_set_str, "mqtt_uri",    cfg->mqtt_broker_uri)
    NVS_SET(nvs_set_str, "mqtt_user",   cfg->mqtt_username)
    NVS_SET(nvs_set_str, "mqtt_pass",   cfg->mqtt_password)
    NVS_SET(nvs_set_str, "mqtt_prefix", cfg->mqtt_topic_prefix)
    NVS_SET(nvs_set_u16, "mqtt_port",   cfg->mqtt_port)

    NVS_SET(nvs_set_i16, "v_dry",       cfg->v_dry_mv)
    NVS_SET(nvs_set_i16, "v_wet",       cfg->v_wet_mv)
    NVS_SET(nvs_set_u8,  "soil_th_pct", cfg->soil_alert_threshold_pct)
    NVS_SET(nvs_set_u8,  "conf",        cfg->is_configured ? 1 : 0)

#undef NVS_SET

    if (err != ESP_OK) {
        /* At least one write failed — do NOT commit the partial state. */
        nvs_close(handle);
        ESP_LOGE(TAG, "Aborting NVS save due to write error. Config unchanged.");
        return false;
    }

    // Commit changes to flash memory
    err = nvs_commit(handle);
    nvs_close(handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Configuration saved and committed to flash.");
        return true;
    }
    ESP_LOGE(TAG, "Failed to commit configuration to NVS! Error: %s", esp_err_to_name(err));
    return false;
}