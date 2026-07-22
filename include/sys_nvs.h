#ifndef SYS_NVS_H
#define SYS_NVS_H

#include <stdint.h>
#include <stdbool.h>

#define SYS_WIFI_SSID_MAX_LEN       32
#define SYS_WIFI_PASS_MAX_LEN       64
#define SYS_TG_TOKEN_MAX_LEN        64
#define SYS_TG_CHAT_MAX_LEN         32
#define SYS_WEBHOOK_URL_MAX_LEN     256  /* Discord & Custom Webhook URLs */

typedef struct {
    /* Network */
    char wifi_ssid[SYS_WIFI_SSID_MAX_LEN];
    char wifi_pass[SYS_WIFI_PASS_MAX_LEN];

    /* Notification channels (all optional — empty string = disabled) */
    char tg_token[SYS_TG_TOKEN_MAX_LEN];
    char tg_chat_id[SYS_TG_CHAT_MAX_LEN];
    char discord_webhook_url[SYS_WEBHOOK_URL_MAX_LEN];
    char custom_webhook_url[SYS_WEBHOOK_URL_MAX_LEN];

    /* ADC calibration */
    int16_t v_dry_mv;
    int16_t v_wet_mv;

    /* Alert threshold: send notification when soil moisture % drops below this */
    uint8_t soil_alert_threshold_pct;  /* 0-100, default 30 */

    bool is_configured;
} sys_config_t;

bool sys_nvs_init(void);
bool sys_nvs_load_config(sys_config_t *out_cfg);
bool sys_nvs_save_config(const sys_config_t *cfg);

#endif /* SYS_NVS_H */