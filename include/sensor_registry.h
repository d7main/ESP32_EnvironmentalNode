/**
 * This file is part of the ESP32_EnvironmentalNode project.
 *
 * @file  sensor_registry.h
 * @brief Sensor interface contract (env_sensor_t) and global registry API.
 *
 *        Every sensor HAL must be compiled into the firmware and registered
 *        in sensor_registry.c.  main.c iterates the registry at runtime —
 *        it never includes individual sensor headers directly.
 *
 *        Lifecycle contract
 *        ──────────────────
 *        1. init()      — Configure hardware (bus, GPIO, calibration).
 *                         Called only when the sensor's bit is set in
 *                         cfg.active_sensors.  Must be idempotent.
 *        2. read_data() — Perform a measurement and append the result as
 *                         named keys to the provided cJSON object.
 *                         Must NOT free or delete the object.
 *        3. deinit()    — Release hardware resources (driver handles, GPIO
 *                         pins) so they do not leak current during deep sleep.
 *                         Called in reverse-init order before esp_deep_sleep_start().
 *
 * @author d7main
 * contact info: demianzaiats@gmail.com
 * @license MIT
 */

#ifndef SENSOR_REGISTRY_H
#define SENSOR_REGISTRY_H

#include <stddef.h>
#include "esp_err.h"
#include "cJSON.h"
#include "sys_nvs.h"   /* SENSOR_*_BIT constants */

/* ── Sensor interface vtable ──────────────────────────────────────────────── */

/**
 * @brief Virtual function table for a single sensor type.
 *
 *        All three function pointers must be non-NULL even for stub sensors.
 *        Stub implementations should log a warning and return
 *        ESP_ERR_NOT_SUPPORTED so the dispatcher can skip them gracefully.
 */
typedef struct {
    const char *name;          /**< Human-readable name used in log messages.   */
    uint32_t    sensor_bit;    /**< Bitmask constant (SENSOR_*_BIT) from sys_nvs.h. */

    /**
     * @brief Initialise the sensor hardware.
     * @return ESP_OK on success, or an esp_err_t code on failure.
     */
    esp_err_t (*init)(void);

    /**
     * @brief Read sensor data and append it to the JSON payload object.
     *
     * @param root  Non-NULL cJSON object to populate.  The function appends
     *              its keys; it must NOT create a new root or call cJSON_Delete().
     * @return ESP_OK on success, or an esp_err_t code on failure.
     */
    esp_err_t (*read_data)(cJSON *root);

    /**
     * @brief Release hardware resources (bus driver, GPIO pins).
     *        Called before deep sleep.  Must be safe to call even if init()
     *        was never called (e.g. if init() failed mid-way).
     */
    void (*deinit)(void);
} env_sensor_t;

/* ── Global registry ─────────────────────────────────────────────────────── */

/**
 * @brief Array of all registered sensor vtables, defined in sensor_registry.c.
 *
 *        Iterate using g_sensor_registry_count:
 *        @code
 *        for (size_t i = 0; i < g_sensor_registry_count; i++) {
 *            if (cfg.active_sensors & g_sensor_registry[i].sensor_bit) { ... }
 *        }
 *        @endcode
 */
extern const env_sensor_t g_sensor_registry[];

/** Number of entries in g_sensor_registry[]. */
extern const size_t g_sensor_registry_count;

/**
 * @brief Provide the loaded NVS configuration to the registry.
 *
 *        Must be called from app_main BEFORE iterating the registry so that
 *        sensor adapters (e.g. the moisture wrapper) can access calibration
 *        bounds from the config struct.
 *
 * @param cfg  Pointer to the loaded sys_config_t. Must remain valid for the
 *             entire wake cycle (caller owns the struct — typically on the
 *             app_main stack).
 */
void sensor_registry_set_config(const sys_config_t *cfg);

#endif /* SENSOR_REGISTRY_H */
