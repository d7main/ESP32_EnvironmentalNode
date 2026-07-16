/**
 * @file hal_dht22.h
 * @brief Hardware Abstraction Layer for DHT22 (AM2302) Temp/Hum Sensor
 * @author d7main
 * @license MIT
 */

#ifndef HAL_DHT22_H
#define HAL_DHT22_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    float temperature; /* Celsius */
    float humidity;    /* Percentage % */
} dht22_data_t;

/**
 * @brief Read temperature and humidity from DHT22 via 1-Wire bitbanging.
 *        Blocks interrupts for ~5ms during readout.
 * @param out_data Pointer to output structure
 * @return true on valid checksum, false on timeout/error
 */
bool hal_dht22_read(dht22_data_t *out_data);

#endif /* HAL_DHT22_H */