/**
 * @file  hal_ds18b20.h
 * @brief DS18B20 1-Wire Temperature HAL — env_sensor_t interface stub.
 * @author d7main  @license MIT
 */
#ifndef HAL_DS18B20_H
#define HAL_DS18B20_H
#include "esp_err.h"
#include "cJSON.h"
esp_err_t hal_ds18b20_init(void);
esp_err_t hal_ds18b20_read_data(cJSON *root);
void      hal_ds18b20_deinit(void);
#endif /* HAL_DS18B20_H */
