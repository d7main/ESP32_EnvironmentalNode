/**
 * This file is part of the ESP32_EnvironmentalNode project.
 *
 * @file  hal_bme280.c
 * @brief BME280 / BMP280 I2C HAL implementing the env_sensor_t interface.
 *
 *        Supports both variants transparently:
 *          - BMP280 (chip ID 0x58): temperature + pressure only.
 *          - BME280 (chip ID 0x60): temperature + pressure + humidity.
 *
 *        The Bosch datasheet compensation formulas (float variant) are used
 *        verbatim for maximum accuracy on the ESP32's FPU.
 *
 * @author d7main
 * contact info: demianzaiats@gmail.com
 * @license MIT
 */

#include "hal_bme280.h"
#include "hal_i2c.h"
#include "config.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "HAL_BME280";

/* ── Register map ─────────────────────────────────────────────────────────── */
#define REG_CHIP_ID      0xD0
#define REG_RESET        0xE0
#define REG_CTRL_HUM     0xF2   /* BME280 only */
#define REG_CTRL_MEAS    0xF4
#define REG_CONFIG       0xF5
#define REG_DATA_START   0xF7   /* 8 bytes: press[2:0] temp[2:0] hum[1:0] */
#define REG_CALIB_TP     0x88   /* Temp + Pressure calibration start (24 B) */
#define REG_CALIB_H1     0xA1   /* Humidity calib byte 1                    */
#define REG_CALIB_H2     0xE1   /* Humidity calib bytes 2-6 (6 B)           */

/* ── Calibration data (loaded once in init) ───────────────────────────────── */
static struct {
    /* Temperature */
    uint16_t dig_T1; int16_t dig_T2; int16_t dig_T3;
    /* Pressure */
    uint16_t dig_P1; int16_t dig_P2; int16_t dig_P3;
    int16_t  dig_P4; int16_t dig_P5; int16_t dig_P6;
    int16_t  dig_P7; int16_t dig_P8; int16_t dig_P9;
    /* Humidity (BME280 only) */
    uint8_t  dig_H1; int16_t dig_H2; uint8_t dig_H3;
    int16_t  dig_H4; int16_t dig_H5; int8_t  dig_H6;
} s_calib;

static int32_t s_t_fine;          /* Shared between temp and pressure/humidity compensations */
static uint8_t s_chip_id  = 0;
static bool    s_inited   = false;

/* ── Low-level I2C helpers ────────────────────────────────────────────────── */

static esp_err_t bme280_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_write_to_device(HAL_I2C_PORT_NUM, BME280_I2C_ADDR,
                                      buf, 2, pdMS_TO_TICKS(100));
}

static esp_err_t bme280_read(uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_write_read_device(HAL_I2C_PORT_NUM, BME280_I2C_ADDR,
                                        &reg, 1, data, len, pdMS_TO_TICKS(100));
}

/* ── Compensation formulas (Bosch datasheet, float variant) ──────────────── */

static float compensate_temperature(int32_t adc_T)
{
    float v1 = (((float)adc_T) / 16384.0f - ((float)s_calib.dig_T1) / 1024.0f)
               * (float)s_calib.dig_T2;
    float v2 = ((((float)adc_T) / 131072.0f - ((float)s_calib.dig_T1) / 8192.0f)
               * (((float)adc_T) / 131072.0f - ((float)s_calib.dig_T1) / 8192.0f))
               * (float)s_calib.dig_T3;
    s_t_fine = (int32_t)(v1 + v2);
    return (v1 + v2) / 5120.0f;
}

static float compensate_pressure(int32_t adc_P)
{
    float v1 = ((float)s_t_fine / 2.0f) - 64000.0f;
    float v2 = v1 * v1 * ((float)s_calib.dig_P6) / 32768.0f;
    v2 = v2 + v1 * ((float)s_calib.dig_P5) * 2.0f;
    v2 = (v2 / 4.0f) + (((float)s_calib.dig_P4) * 65536.0f);
    v1 = (((float)s_calib.dig_P3) * v1 * v1 / 524288.0f
          + ((float)s_calib.dig_P2) * v1) / 524288.0f;
    v1 = (1.0f + v1 / 32768.0f) * ((float)s_calib.dig_P1);
    if (v1 == 0.0f) return 0.0f;
    float p = 1048576.0f - (float)adc_P;
    p = (p - (v2 / 4096.0f)) * 6250.0f / v1;
    v1 = ((float)s_calib.dig_P9) * p * p / 2147483648.0f;
    v2 = p * ((float)s_calib.dig_P8) / 32768.0f;
    p = p + (v1 + v2 + ((float)s_calib.dig_P7)) / 16.0f;
    return p / 100.0f; /* Pa → hPa */
}

static float compensate_humidity(int32_t adc_H)
{
    float h = (float)s_t_fine - 76800.0f;
    if (h == 0.0f) return 0.0f;
    h = ((float)adc_H - (((float)s_calib.dig_H4) * 64.0f
         + ((float)s_calib.dig_H5) / 16384.0f * h))
        * ((float)s_calib.dig_H2 / 65536.0f
           * (1.0f + ((float)s_calib.dig_H6) / 67108864.0f * h
              * (1.0f + ((float)s_calib.dig_H3) / 67108864.0f * h)));
    h = h * (1.0f - ((float)s_calib.dig_H1) * h / 524288.0f);
    if (h > 100.0f) h = 100.0f;
    if (h < 0.0f)   h = 0.0f;
    return h;
}

/* ── Public interface ─────────────────────────────────────────────────────── */

esp_err_t hal_bme280_init(void)
{
    if (s_inited) return ESP_OK;

    /* Ensure I2C bus is up */
    hal_i2c_init();

    /* Probe chip ID */
    esp_err_t err = bme280_read(REG_CHIP_ID, &s_chip_id, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C read failed (addr=0x%02X): %s", BME280_I2C_ADDR, esp_err_to_name(err));
        return ESP_ERR_NOT_FOUND;
    }
    if (s_chip_id != BME280_CHIP_ID_BMP280 && s_chip_id != BME280_CHIP_ID_BME280) {
        ESP_LOGE(TAG, "Unexpected chip ID: 0x%02X (expected 0x58 or 0x60)", s_chip_id);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "Detected %s (ID=0x%02X)",
             s_chip_id == BME280_CHIP_ID_BME280 ? "BME280" : "BMP280", s_chip_id);

    /* Read temperature + pressure calibration (0x88, 24 bytes) */
    uint8_t buf[26];
    if (bme280_read(REG_CALIB_TP, buf, 24) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read T/P calibration data");
        return ESP_FAIL;
    }
    s_calib.dig_T1 = (uint16_t)((buf[1]  << 8) | buf[0]);
    s_calib.dig_T2 = (int16_t) ((buf[3]  << 8) | buf[2]);
    s_calib.dig_T3 = (int16_t) ((buf[5]  << 8) | buf[4]);
    s_calib.dig_P1 = (uint16_t)((buf[7]  << 8) | buf[6]);
    s_calib.dig_P2 = (int16_t) ((buf[9]  << 8) | buf[8]);
    s_calib.dig_P3 = (int16_t) ((buf[11] << 8) | buf[10]);
    s_calib.dig_P4 = (int16_t) ((buf[13] << 8) | buf[12]);
    s_calib.dig_P5 = (int16_t) ((buf[15] << 8) | buf[14]);
    s_calib.dig_P6 = (int16_t) ((buf[17] << 8) | buf[16]);
    s_calib.dig_P7 = (int16_t) ((buf[19] << 8) | buf[18]);
    s_calib.dig_P8 = (int16_t) ((buf[21] << 8) | buf[20]);
    s_calib.dig_P9 = (int16_t) ((buf[23] << 8) | buf[22]);

    /* Read humidity calibration if BME280 */
    if (s_chip_id == BME280_CHIP_ID_BME280) {
        uint8_t h1 = 0;
        bme280_read(REG_CALIB_H1, &h1, 1);
        s_calib.dig_H1 = h1;

        uint8_t hbuf[7];
        if (bme280_read(REG_CALIB_H2, hbuf, 7) != ESP_OK) {
            ESP_LOGW(TAG, "Failed to read humidity calibration — humidity disabled");
            s_chip_id = BME280_CHIP_ID_BMP280; /* downgrade to BMP280 mode */
        } else {
            s_calib.dig_H2 = (int16_t)((hbuf[1] << 8) | hbuf[0]);
            s_calib.dig_H3 = hbuf[2];
            s_calib.dig_H4 = (int16_t)((hbuf[3] << 4) | (hbuf[4] & 0x0F));
            s_calib.dig_H5 = (int16_t)((hbuf[5] << 4) | (hbuf[4] >> 4));
            s_calib.dig_H6 = (int8_t)hbuf[6];

            /* Enable humidity 1x oversampling before ctrl_meas write */
            bme280_write(REG_CTRL_HUM, 0x01);
        }
    }

    s_inited = true;
    ESP_LOGI(TAG, "Initialised successfully");
    return ESP_OK;
}

esp_err_t hal_bme280_read_data(cJSON *root)
{
    if (!s_inited) {
        ESP_LOGW(TAG, "read_data called before init — calling init now");
        esp_err_t err = hal_bme280_init();
        if (err != ESP_OK) return err;
    }

    /* Forced mode: OS_T=1x, OS_P=1x, Mode=Forced (0b00100101) */
    bme280_write(REG_CTRL_MEAS, 0x25);
    vTaskDelay(pdMS_TO_TICKS(10)); /* ≥6.4 ms for 1× oversampling */

    /* Read 8 raw bytes: press[2:0] temp[2:0] hum[1:0] */
    uint8_t data[8];
    esp_err_t err = bme280_read(REG_DATA_START, data, 8);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read measurement registers: %s", esp_err_to_name(err));
        return err;
    }

    int32_t adc_P = ((int32_t)data[0] << 12) | ((int32_t)data[1] << 4) | (data[2] >> 4);
    int32_t adc_T = ((int32_t)data[3] << 12) | ((int32_t)data[4] << 4) | (data[5] >> 4);
    int32_t adc_H = ((int32_t)data[6] << 8)  |  (int32_t)data[7];

    float temp  = compensate_temperature(adc_P == 0x80000 ? 0 : adc_T);
    float press = (adc_P == 0x80000) ? 0.0f : compensate_pressure(adc_P);

    cJSON_AddNumberToObject(root, "bme280_temp_c",    (double)temp);
    cJSON_AddNumberToObject(root, "bme280_press_hpa", (double)press);

    if (s_chip_id == BME280_CHIP_ID_BME280 && adc_H != 0x8000) {
        float hum = compensate_humidity(adc_H);
        cJSON_AddNumberToObject(root, "bme280_hum_pct", (double)hum);
        ESP_LOGI(TAG, "T=%.2f°C P=%.2f hPa H=%.2f%%", temp, press, hum);
    } else {
        ESP_LOGI(TAG, "T=%.2f°C P=%.2f hPa", temp, press);
    }

    return ESP_OK;
}

void hal_bme280_deinit(void)
{
    if (!s_inited) return;
    hal_i2c_deinit();
    s_inited  = false;
    s_chip_id = 0;
    ESP_LOGD(TAG, "Deinitialised");
}
