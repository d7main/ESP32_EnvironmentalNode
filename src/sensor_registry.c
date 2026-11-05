/**
 * This file is part of the ESP32_EnvironmentalNode project.
 *
 * @file  sensor_registry.c
 * @brief Global sensor registry — the single place that wires all HALs together.
 *
 *        To add a new sensor:
 *          1. Implement its HAL (init / read_data / deinit) in a new hal_*.c file.
 *          2. Add its header #include below.
 *          3. Add a row to g_sensor_registry[].
 *          4. Define its SENSOR_*_BIT macro in sys_nvs.h.
 *
 *        The moisture sensor does not follow the env_sensor_t init/deinit
 *        signature natively (its init is internal and lazy), so thin adapter
 *        functions are defined here.  read_data uses a file-static pointer to
 *        the loaded config so it can access the ADC calibration bounds — set
 *        by sensor_registry_set_config() called from app_main before the loop.
 *
 * @author d7main
 * contact info: demianzaiats@gmail.com
 * @license MIT
 */

#include "sensor_registry.h"
#include "sys_nvs.h"
#include "hal_bme280.h"
#include "hal_sht3x.h"
#include "hal_bh1750.h"
#include "hal_mhz19.h"
#include "hal_pms5003.h"
#include "hal_moisture.h"
#include "hal_i2c.h"
#include "hal_ds18b20.h"
#include "config.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "SENSOR_REG";

/* ── Moisture sensor adapter ──────────────────────────────────────────────────
 * The moisture HAL exposes hal_moisture_read_mv() which returns int32_t, and
 * hal_moisture_mv_to_percent() for calibrated %.  Wrap both into env_sensor_t
 * signatures.  The config pointer is set by sensor_registry_set_config()
 * before the first read cycle.
 * ─────────────────────────────────────────────────────────────────────────── */

static const sys_config_t *s_cfg = NULL;

/**
 * @brief Call this from app_main before iterating the registry so that the
 *        moisture adapter can access ADC calibration bounds from NVS config.
 */
void sensor_registry_set_config(const sys_config_t *cfg)
{
    s_cfg = cfg;
}

static esp_err_t moisture_init(void)
{
    /* The ADC handle is initialised lazily inside hal_moisture_read_mv().
     * Configure the power gate GPIO now so it is ready for the first read. */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << SENSOR_POWER_PIN),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[Moisture] gpio_config failed: %s", esp_err_to_name(err));
        return err;
    }
    gpio_set_level(SENSOR_POWER_PIN, 0); /* keep gate off until read */
    ESP_LOGI(TAG, "[Moisture] Power gate GPIO%d configured", SENSOR_POWER_PIN);
    return ESP_OK;
}

static esp_err_t moisture_read_data(cJSON *root)
{
    int32_t mv = hal_moisture_read_mv();
    if (mv < 0) {
        ESP_LOGE(TAG, "[Moisture] ADC read failed");
        return ESP_FAIL;
    }

    cJSON_AddNumberToObject(root, "soil_mv", (double)mv);

    /* Include calibrated % only when calibration bounds are valid */
    if (s_cfg && s_cfg->v_dry_mv > s_cfg->v_wet_mv) {
        uint8_t pct = hal_moisture_mv_to_percent(mv, s_cfg->v_dry_mv, s_cfg->v_wet_mv);
        cJSON_AddNumberToObject(root, "soil_moisture_pct", (double)pct);
        ESP_LOGI(TAG, "[Moisture] %ld mV → %d%%", (long)mv, (int)pct);
    } else {
        ESP_LOGI(TAG, "[Moisture] %ld mV (uncalibrated)", (long)mv);
    }
    return ESP_OK;
}

static void moisture_deinit(void)
{
    gpio_set_level(SENSOR_POWER_PIN, 0);
    gpio_reset_pin(SENSOR_POWER_PIN);
    ESP_LOGD(TAG, "[Moisture] Power gate released");
}

/* ── Registry definition ──────────────────────────────────────────────────── */

/**
 * @brief All supported sensors in registration order.
 *
 *        Sensors are initialised in this order and deinitialised in REVERSE
 *        order (not enforced here — app_main iterates forward for both, which
 *        is safe because no sensor's deinit depends on another sensor's state).
 */
const env_sensor_t g_sensor_registry[] = {
    {
        .name       = "BME280",
        .sensor_bit = SENSOR_BME280_BIT,
        .init       = hal_bme280_init,
        .read_data  = hal_bme280_read_data,
        .deinit     = hal_bme280_deinit,
    },
    {
        .name       = "SHT3x",
        .sensor_bit = SENSOR_SHT3X_BIT,
        .init       = hal_sht3x_init,
        .read_data  = hal_sht3x_read_data,
        .deinit     = hal_sht3x_deinit,
    },
    {
        .name       = "BH1750",
        .sensor_bit = SENSOR_BH1750_BIT,
        .init       = hal_bh1750_init,
        .read_data  = hal_bh1750_read_data,
        .deinit     = hal_bh1750_deinit,
    },
    {
        .name       = "MH-Z19",
        .sensor_bit = SENSOR_MHZ19_BIT,
        .init       = hal_mhz19_init,
        .read_data  = hal_mhz19_read_data,
        .deinit     = hal_mhz19_deinit,
    },
    {
        .name       = "PMS5003",
        .sensor_bit = SENSOR_PMS5003_BIT,
        .init       = hal_pms5003_init,
        .read_data  = hal_pms5003_read_data,
        .deinit     = hal_pms5003_deinit,
    },
    {
        .name       = "Moisture",
        .sensor_bit = SENSOR_MOISTURE_BIT,
        .init       = moisture_init,
        .read_data  = moisture_read_data,
        .deinit     = moisture_deinit,
    },
    {
        .name       = "DS18B20",
        .sensor_bit = SENSOR_DS18B20_BIT,
        .init       = hal_ds18b20_init,
        .read_data  = hal_ds18b20_read_data,
        .deinit     = hal_ds18b20_deinit,
    },
};

const size_t g_sensor_registry_count =
    sizeof(g_sensor_registry) / sizeof(g_sensor_registry[0]);
