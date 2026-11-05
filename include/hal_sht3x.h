/**
 * @file  hal_sht3x.h
 * @brief SHT3x (SHT30/SHT31/SHT35) I2C HAL — env_sensor_t interface stub.
 * @author d7main  @license MIT
 */
#ifndef HAL_SHT3X_H
#define HAL_SHT3X_H
#include "esp_err.h"
#include "cJSON.h"
esp_err_t hal_sht3x_init(void);
esp_err_t hal_sht3x_read_data(cJSON *root);
void      hal_sht3x_deinit(void);
#endif /* HAL_SHT3X_H */
