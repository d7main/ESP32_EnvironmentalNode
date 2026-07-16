#ifndef SYS_NVS_H
#define SYS_NVS_H

#include <stdint.h>
#include <stdbool.h>

#define SYS_WIFI_SSID_MAX_LEN 32
#define SYS_WIFI_PASS_MAX_LEN 64
#define SYS_TG_TOKEN_MAX_LEN  64
#define SYS_TG_CHAT_MAX_LEN   32

typedef struct {
    char wifi_ssid[SYS_WIFI_SSID_MAX_LEN];
    char wifi_pass[SYS_WIFI_PASS_MAX_LEN];
    char tg_token[SYS_TG_TOKEN_MAX_LEN];
    char tg_chat_id[SYS_TG_CHAT_MAX_LEN];
    int16_t v_dry_mv;
    int16_t v_wet_mv;
    bool is_configured;
} sys_config_t;

bool sys_nvs_init(void);
bool sys_nvs_load_config(sys_config_t *out_cfg);
bool sys_nvs_save_config(const sys_config_t *cfg);

#endif /* SYS_NVS_H */