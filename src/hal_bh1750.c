/**
 * @file  hal_bh1750.c
 * @brief BH1750 Ambient Light I2C HAL — STUB implementation.
 *
 *        Returns ESP_ERR_NOT_SUPPORTED until a full implementation is provided.
 *        To implement: send opcode 0x10 (Continuous H-Resolution Mode), wait
 *        180 ms, then read 2 bytes and compute lux = (byte0<<8|byte1) / 1.2.
 */
#include "hal_bh1750.h"
#include "esp_log.h"

static const char *TAG = "HAL_BH1750";

esp_err_t hal_bh1750_init(void)
{
    ESP_LOGW(TAG, "BH1750 HAL is a stub — not yet implemented");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t hal_bh1750_read_data(cJSON *root)
{
    (void)root;
    ESP_LOGW(TAG, "BH1750 read_data: stub — no data appended");
    return ESP_ERR_NOT_SUPPORTED;
}

void hal_bh1750_deinit(void)
{
    /* Nothing to release in stub */
}
