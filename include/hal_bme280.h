/**
 * This file is part of the ESP32_EnvironmentalNode project.
 *
 * @file  hal_bme280.h
 * @brief BME280 / BMP280 sensor HAL — env_sensor_t interface.
 *
 *        Supports both BMP280 (chip ID 0x58, no humidity) and
 *        BME280 (chip ID 0x60, adds humidity channel).
 *        The init function probes the chip ID and configures the driver
 *        accordingly — no compile-time switch required.
 *
 * @author d7main
 * contact info: demianzaiats@gmail.com
 * @license MIT
 */

#ifndef HAL_BME280_H
#define HAL_BME280_H

#include "esp_err.h"
#include "cJSON.h"

#define BME280_I2C_ADDR       0x76   /**< SDO pulled LOW  (default)   */
#define BME280_I2C_ADDR_ALT   0x77   /**< SDO pulled HIGH (alternate) */

#define BME280_CHIP_ID_BMP280 0x58   /**< BMP280 — pressure + temp only */
#define BME280_CHIP_ID_BME280 0x60   /**< BME280 — adds humidity         */

/**
 * @brief Initialise the BME280/BMP280: verify chip ID and read factory
 *        calibration coefficients from the device NVM.
 *
 *        Initialises the I2C bus via hal_i2c_init() if it has not been
 *        started already.
 *
 * @return ESP_OK on success.
 *         ESP_ERR_NOT_FOUND if no device responds at BME280_I2C_ADDR.
 *         Other esp_err_t codes on I2C communication failure.
 */
esp_err_t hal_bme280_init(void);

/**
 * @brief Trigger a forced-mode measurement and append results to a cJSON object.
 *
 *        Keys appended:
 *          "bme280_temp_c"  — float, degrees Celsius
 *          "bme280_press_hpa" — float, hectopascal
 *          "bme280_hum_pct" — float, % RH  (BME280 only; omitted for BMP280)
 *
 * @param root  Non-NULL cJSON object to append values to.
 * @return ESP_OK on success, or an esp_err_t code on I2C failure.
 */
esp_err_t hal_bme280_read_data(cJSON *root);

/**
 * @brief Release the I2C bus driver and reset GPIO pins to Hi-Z.
 *        Safe to call even if init() was never invoked.
 */
void hal_bme280_deinit(void);

#endif /* HAL_BME280_H */
