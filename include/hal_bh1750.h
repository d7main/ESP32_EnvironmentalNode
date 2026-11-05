/**
 * @file  hal_bh1750.h
 * @brief BH1750 Ambient Light I2C HAL — env_sensor_t interface stub.
 * @author d7main  @license MIT
 */
#ifndef HAL_BH1750_H
#define HAL_BH1750_H
#include "esp_err.h"
#include "cJSON.h"
esp_err_t hal_bh1750_init(void);
esp_err_t hal_bh1750_read_data(cJSON *root);
void      hal_bh1750_deinit(void);
#endif /* HAL_BH1750_H */
