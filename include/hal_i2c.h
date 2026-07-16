/**
 * @file hal_i2c.h
 * @brief I2C Master Bus Driver
 * @author d7main
 * @license MIT
 */

#ifndef HAL_I2C_H
#define HAL_I2C_H

#include <stdbool.h>

#define HAL_I2C_PORT_NUM       0       /* I2C_NUM_0 */
#define HAL_I2C_MASTER_FREQ_HZ 400000  /* 400kHz Fast Mode */

/**
 * @brief Initialize I2C master bus.
 * @return true on success, false on failure
 */
bool hal_i2c_init(void);

/**
 * @brief Deinitialize I2C master bus to prevent deep sleep leakage.
 */
void hal_i2c_deinit(void);

#endif /* HAL_I2C_H */