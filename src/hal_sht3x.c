/**
 * @file  hal_sht3x.c
 * @brief SHT3x I2C HAL — STUB implementation.
 *
 *        Returns ESP_ERR_NOT_SUPPORTED until a full implementation is provided.
 *        To implement: use I2C single-shot command 0x2C06 (high-repeatability,
 *        clock-stretching enabled) and read 6 bytes (temp[2] CRC hum[2] CRC).
 */
#include "hal_sht3x.h"
#include "esp_log.h"

static const char *TAG = "HAL_SHT3X";

esp_err_t hal_sht3x_init(void)
{
    ESP_LOGW(TAG, "SHT3x HAL is a stub — not yet implemented");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t hal_sht3x_read_data(cJSON *root)
{
    (void)root;
    ESP_LOGW(TAG, "SHT3x read_data: stub — no data appended");
    return ESP_ERR_NOT_SUPPORTED;
}

void hal_sht3x_deinit(void)
{
    /* Nothing to release in stub */
}
