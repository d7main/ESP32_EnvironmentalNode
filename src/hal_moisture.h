/**
 * 
 * This file is part of the ESP32_EnvironmentalNode project.
 * @file hal_moisture.h
 * @brief Hardware Abstraction Layer for Moisture Sensor
 * @author d7main (Demian Zaiats)
 * contact info: demianzaiats@gmail.com
 * license: MIT License
 */

#ifndef HAL_MOISTURE_H
#define HAL_MOISTURE_H

#include <stdint.h>

#define HAL_MOISTURE_SAMPLES 5
#define HAL_MOISTURE_DELAY_MS 50

/**
 * @brief Power up sensor, read calibrated ADC value, and power down sensor.
 * 
 * @return Calibrated voltage value in millivolts (mV), or -1 if an error occurred.
 */
int32_t hal_moisture_read_mv(void);

/**
 * @brief Convert measured millivolts to soil moisture percentage (0-100%).
 * 
 * @param current_mv Current ADC reading in mV
 * @param v_dry_mv Calibrated dry threshold in mV (e.g., 2600)
 * @param v_wet_mv Calibrated wet threshold in mV (e.g., 1200)
 * @return Moisture level in percent (0 to 100)
 */
uint8_t hal_moisture_mv_to_percent(int32_t current_mv, int16_t v_dry_mv, int16_t v_wet_mv);

#endif /* HAL_MOISTURE_H */