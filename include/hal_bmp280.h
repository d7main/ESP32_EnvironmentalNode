/**
 * @file hal_bmp280.h
 * @brief Hardware Abstraction Layer for BMP280 Barometric Pressure Sensor
 * @author d7main
 * contact info: demianzaiats@gmail.com
 * @license MIT
 * 16.07.2026
 */

#ifndef HAL_BMP280_H
#define HAL_BMP280_H

#include <stdint.h>
#include <stdbool.h>

#define BMP280_I2C_ADDR 0x76

typedef struct {
    float temperature; /* Celsius */
    float pressure;    /* hPa */
} bmp280_data_t;

/**
 * @brief Initialize BMP280 and read factory calibration data.
 * @return true if chip ID (0x58) is verified and calibrated.
 */
bool hal_bmp280_init(void);

/**
 * @brief Force measurement and read compensated data.
 * @param out_data Pointer to output structure
 * @return true on successful I2C transaction
 */
bool hal_bmp280_read(bmp280_data_t *out_data);

#endif /* HAL_BMP280_H */