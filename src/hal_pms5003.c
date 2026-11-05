/**
 * @file  hal_pms5003.c
 * @brief PMS5003 UART Particulate Matter HAL — STUB implementation.
 *
 *        Returns ESP_ERR_NOT_SUPPORTED until a full implementation is provided.
 *        To implement: configure UART2 at 9600 baud; parse the 32-byte passive
 *        read response (start bytes 0x42 0x4D), extract PM1.0/PM2.5/PM10 from
 *        bytes [10:11], [12:13], [14:15] (standard environment values).
 */
#include "hal_pms5003.h"
#include "esp_log.h"

static const char *TAG = "HAL_PMS5003";

esp_err_t hal_pms5003_init(void)
{
    ESP_LOGW(TAG, "PMS5003 HAL is a stub — not yet implemented");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t hal_pms5003_read_data(cJSON *root)
{
    (void)root;
    ESP_LOGW(TAG, "PMS5003 read_data: stub — no data appended");
    return ESP_ERR_NOT_SUPPORTED;
}

void hal_pms5003_deinit(void)
{
    /* Nothing to release in stub */
}
