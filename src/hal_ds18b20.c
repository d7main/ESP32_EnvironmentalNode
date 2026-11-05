/**
 * @file  hal_ds18b20.c
 * @brief DS18B20 1-Wire Temperature HAL — STUB implementation.
 *
 *        Returns ESP_ERR_NOT_SUPPORTED until a full implementation is provided.
 *        To implement: use the ESP-IDF onewire_bus component (available in IDF 5.x
 *        via idf_component_manager) or bit-bang the 1-Wire protocol on DS18B20_DATA_PIN.
 *        ROM command 0xCC (Skip ROM) + 0x44 (Convert T), wait 750 ms, then
 *        0xCC + 0xBE (Read Scratchpad) → 9 bytes, temp in bytes [0:1].
 */
#include "hal_ds18b20.h"
#include "esp_log.h"

static const char *TAG = "HAL_DS18B20";

esp_err_t hal_ds18b20_init(void)
{
    ESP_LOGW(TAG, "DS18B20 HAL is a stub — not yet implemented");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t hal_ds18b20_read_data(cJSON *root)
{
    (void)root;
    ESP_LOGW(TAG, "DS18B20 read_data: stub — no data appended");
    return ESP_ERR_NOT_SUPPORTED;
}

void hal_ds18b20_deinit(void)
{
    /* Nothing to release in stub */
}
