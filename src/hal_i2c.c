/**
 * This file is part of the ESP32_EnvironmentalNode project.
 * @file hal_i2c.c
 * @brief I2C Master implementation
 * @author d7main
 * @license MIT
 * contact info: demianzaiats@gmail.com
 */

#include "hal_i2c.h"
#include "config.h"
#include "driver/i2c.h"
#include "esp_log.h"

static const char *TAG = "HAL_I2C";

bool hal_i2c_init(void) {
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_SDA_PIN,
        .scl_io_num = I2C_SCL_PIN,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = HAL_I2C_MASTER_FREQ_HZ,
    };

    if (i2c_param_config(HAL_I2C_PORT_NUM, &conf) != ESP_OK) {
        ESP_LOGE(TAG, "i2c_param_config_failed");
        return false;
    }

    if (i2c_driver_install(HAL_I2C_PORT_NUM, conf.mode, 0, 0, 0) != ESP_OK) {
        ESP_LOGE(TAG, "i2c_driver_install_failed");
        return false;
    }

    ESP_LOGI(TAG, "i2c_bus_initialized");
    return true;
}

void hal_i2c_deinit(void) {
    i2c_driver_delete(HAL_I2C_PORT_NUM);
    /* Reset pins to default state to avoid power draw */
    gpio_reset_pin(I2C_SDA_PIN);
    gpio_reset_pin(I2C_SCL_PIN);
}