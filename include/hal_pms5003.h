/**
 * @file  hal_pms5003.h
 * @brief PMS5003 Particulate Matter UART HAL — env_sensor_t interface stub.
 * @author d7main  @license MIT
 */
#ifndef HAL_PMS5003_H
#define HAL_PMS5003_H
#include "esp_err.h"
#include "cJSON.h"
esp_err_t hal_pms5003_init(void);
esp_err_t hal_pms5003_read_data(cJSON *root);
void      hal_pms5003_deinit(void);
#endif /* HAL_PMS5003_H */
