/**
 * This file is part of the ESP32_EnvironmentalNode project.
 * @file hal_bmp280.c
 * @brief BMP280 implementation with forced mode and float compensation
 * @author d7main
 * contact info: demianzaiats@gmail.com
 * @license MIT
 */

#include "hal_bmp280.h"
#include "hal_i2c.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "HAL_BMP280";

#define REG_CHIP_ID      0xD0
#define REG_CTRL_MEAS    0xF4
#define REG_DATA_START   0xF7
#define EXPECTED_CHIP_ID 0x58

static struct {
    uint16_t dig_T1; int16_t  dig_T2; int16_t  dig_T3;
    uint16_t dig_P1; int16_t  dig_P2; int16_t  dig_P3;
    int16_t  dig_P4; int16_t  dig_P5; int16_t  dig_P6;
    int16_t  dig_P7; int16_t  dig_P8; int16_t  dig_P9;
} calib;

static int32_t t_fine;

static esp_err_t bmp280_write_reg(uint8_t reg, uint8_t val) {
    uint8_t buf[2] = {reg, val};
    return i2c_master_write_to_device(HAL_I2C_PORT_NUM, BMP280_I2C_ADDR, buf, 2, pdMS_TO_TICKS(100));
}

static esp_err_t bmp280_read_regs(uint8_t reg, uint8_t *data, size_t len) {
    return i2c_master_write_read_device(HAL_I2C_PORT_NUM, BMP280_I2C_ADDR, &reg, 1, data, len, pdMS_TO_TICKS(100));
}

bool hal_bmp280_init(void) {
    uint8_t chip_id = 0;
    if (bmp280_read_regs(REG_CHIP_ID, &chip_id, 1) != ESP_OK) {
        ESP_LOGE(TAG, "I2C read failed");
        return false;
    }
    
    if (chip_id != EXPECTED_CHIP_ID) {
        ESP_LOGE(TAG, "Invalid Chip ID: 0x%02X (Expected 0x%02X)", chip_id, EXPECTED_CHIP_ID);
        return false;
    }

    uint8_t buf[24];
    if (bmp280_read_regs(0x88, buf, 24) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read calibration data");
        return false;
    }

    calib.dig_T1 = (buf[1] << 8)  | buf[0];
    calib.dig_T2 = (buf[3] << 8)  | buf[2];
    calib.dig_T3 = (buf[5] << 8)  | buf[4];
    calib.dig_P1 = (buf[7] << 8)  | buf[6];
    calib.dig_P2 = (buf[9] << 8)  | buf[8];
    calib.dig_P3 = (buf[11] << 8) | buf[10];
    calib.dig_P4 = (buf[13] << 8) | buf[12];
    calib.dig_P5 = (buf[15] << 8) | buf[14];
    calib.dig_P6 = (buf[17] << 8) | buf[16];
    calib.dig_P7 = (buf[19] << 8) | buf[18];
    calib.dig_P8 = (buf[21] << 8) | buf[20];
    calib.dig_P9 = (buf[23] << 8) | buf[22];

    ESP_LOGI(TAG, "Initialized successfully");
    return true;
}

bool hal_bmp280_read(bmp280_data_t *out_data) {
    if (!out_data) return false;

    /* OS_T=1x, OS_P=1x, Mode=Forced */
    if (bmp280_write_reg(REG_CTRL_MEAS, 0x25) != ESP_OK) {
        return false;
    }

    /* Max measurement time for 1x oversampling is ~6.4ms */
    vTaskDelay(pdMS_TO_TICKS(10));

    uint8_t data[6];
    if (bmp280_read_regs(REG_DATA_START, data, 6) != ESP_OK) {
        return false;
    }

    int32_t adc_P = (data[0] << 12) | (data[1] << 4) | (data[2] >> 4);
    int32_t adc_T = (data[3] << 12) | (data[4] << 4) | (data[5] >> 4);

    /* Temperature compensation */
    float var1_T = (((float)adc_T) / 16384.0f - ((float)calib.dig_T1) / 1024.0f) * ((float)calib.dig_T2);
    float var2_T = ((((float)adc_T) / 131072.0f - ((float)calib.dig_T1) / 8192.0f) * 
                    (((float)adc_T) / 131072.0f - ((float)calib.dig_T1) / 8192.0f)) * ((float)calib.dig_T3);
    t_fine = (int32_t)(var1_T + var2_T);
    out_data->temperature = (var1_T + var2_T) / 5120.0f;

    /* Pressure compensation */
    float var1_P = ((float)t_fine / 2.0f) - 64000.0f;
    float var2_P = var1_P * var1_P * ((float)calib.dig_P6) / 32768.0f;
    var2_P = var2_P + var1_P * ((float)calib.dig_P5) * 2.0f;
    var2_P = (var2_P / 4.0f) + (((float)calib.dig_P4) * 65536.0f);
    var1_P = (((float)calib.dig_P3) * var1_P * var1_P / 524288.0f + ((float)calib.dig_P2) * var1_P) / 524288.0f;
    var1_P = (1.0f + var1_P / 32768.0f) * ((float)calib.dig_P1);
    
    float p = 0.0f;
    if (var1_P != 0.0f) {
        p = 1048576.0f - (float)adc_P;
        p = (p - (var2_P / 4096.0f)) * 6250.0f / var1_P;
        var1_P = ((float)calib.dig_P9) * p * p / 2147483648.0f;
        var2_P = p * ((float)calib.dig_P8) / 32768.0f;
        p = p + (var1_P + var2_P + ((float)calib.dig_P7)) / 16.0f;
    }
    out_data->pressure = p / 100.0f;

    return true;
}