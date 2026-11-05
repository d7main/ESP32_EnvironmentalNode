/**
 * This file is part of the ESP32_EnvironmentalNode project.
 *
 * @file  hal_mhz19.h
 * @brief MH-Z19B CO₂ sensor HAL — UART binary protocol, env_sensor_t interface.
 *
 *        The MH-Z19B communicates at 9600 baud (8N1) over a 9-byte binary
 *        command/response protocol.  This HAL uses UART1 on pins defined in
 *        config.h (MHZ19_UART_TX_PIN / MHZ19_UART_RX_PIN).
 *
 *        Note: The sensor requires a warm-up period of ~3 minutes after power-on
 *        before CO₂ readings stabilise.  Early readings may be returned as 0 or
 *        400 ppm — this is normal hardware behaviour, not a driver bug.
 *
 * @author d7main
 * contact info: demianzaiats@gmail.com
 * @license MIT
 */

#ifndef HAL_MHZ19_H
#define HAL_MHZ19_H

#include "esp_err.h"
#include "cJSON.h"

/**
 * @brief Initialise UART1 for MH-Z19 communication (9600 baud, 8N1).
 *
 *        Installs the UART driver with a 256-byte RX ring buffer.
 *        Safe to call multiple times — subsequent calls are a no-op if the
 *        driver is already installed.
 *
 * @return ESP_OK on success, or an esp_err_t code on UART driver failure.
 */
esp_err_t hal_mhz19_init(void);

/**
 * @brief Send a CO₂ read command and append the result to a cJSON object.
 *
 *        Keys appended:
 *          "mhz19_co2_ppm" — integer, CO₂ concentration in parts-per-million
 *
 *        The function validates the 9-byte response checksum before accepting
 *        the reading.  Returns ESP_ERR_INVALID_CRC on checksum mismatch.
 *
 * @param root  Non-NULL cJSON object to append values to.
 * @return ESP_OK on success.
 *         ESP_ERR_TIMEOUT if the sensor does not respond within 1000 ms.
 *         ESP_ERR_INVALID_CRC on checksum mismatch.
 */
esp_err_t hal_mhz19_read_data(cJSON *root);

/**
 * @brief Delete the UART1 driver and release its resources.
 *        Safe to call even if init() was never invoked.
 */
void hal_mhz19_deinit(void);

#endif /* HAL_MHZ19_H */
