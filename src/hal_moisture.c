/**
 * @file hal_moisture.c
 * @brief ADC Oneshot Implementation for Moisture Sensor
 * @author d7main (Demian Zaiats)
 * contact info: demianzaiats@gmail.com
 */

#include "hal_moisture.h"
#include "config.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "HAL_MOISTURE";

/* ── Static ADC singleton ─────────────────────────────────────────────────
 * The ADC unit and calibration handles are created once on first read and
 * reused for all subsequent calls.  This prevents the per-call alloc/free
 * cycle that could leave handles leaked on early-return error paths, and
 * avoids the ESP_ERR_NOT_FOUND fault caused by opening a second handle to
 * ADC1 while a previous handle is still alive (e.g. in portal sensor task). */
static adc_oneshot_unit_handle_t s_adc_handle  = NULL;
static adc_cali_handle_t         s_cali_handle = NULL;
static bool                      s_adc_inited  = false;

/**
 * @brief Initialise the ADC1 unit and calibration scheme exactly once.
 *        Subsequent calls return immediately via the s_adc_inited guard.
 * @return true on success (or already initialised), false on hardware error.
 */
static bool ensure_adc_init(void) {
    if (s_adc_inited) return true;

    adc_oneshot_unit_init_cfg_t init_cfg = {
        .unit_id = ADC_UNIT_1,
        .clk_src = 0, /* ADC_RTC_CLK_SRC_DEFAULT */
    };
    if (adc_oneshot_new_unit(&init_cfg, &s_adc_handle) != ESP_OK) {
        ESP_LOGE(TAG, "[MOISTURE] adc_oneshot_new_unit() failed.");
        return false;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten    = ADC_ATTEN_DB_12,
    };
    /* Check the return value — an unchecked failure here would silently
     * sample the wrong channel on every subsequent read. */
    if (adc_oneshot_config_channel(s_adc_handle, SENSOR_ADC_CHANNEL, &chan_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "[MOISTURE] adc_oneshot_config_channel() failed.");
        adc_oneshot_del_unit(s_adc_handle);
        s_adc_handle = NULL;
        return false;
    }

#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t cali_cfg = {
        .unit_id  = ADC_UNIT_1,
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_line_fitting(&cali_cfg, &s_cali_handle) != ESP_OK) {
        /* Not fatal — fall back to raw linear conversion. */
        ESP_LOGW(TAG, "[MOISTURE] Calibration scheme unavailable; using raw linear conversion.");
        s_cali_handle = NULL;
    }
#endif

    s_adc_inited = true;
    return true;
}

static void pwr_gate_enable(void) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << SENSOR_POWER_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);
    gpio_set_level(SENSOR_POWER_PIN, 1);

    /* BUG FIX #1 — was 5 ms; too short for the capacitive sensor's oscillator
     * to stabilise.  At 5 ms the ADC sampled the rising supply rail and always
     * returned ~3300 mV.  HAL_MOISTURE_DELAY_MS is defined as 50 ms in the
     * header; using 2× (100 ms) gives comfortable margin for all sensor variants. */
    vTaskDelay(pdMS_TO_TICKS(HAL_MOISTURE_DELAY_MS * 2));
}

static void pwr_gate_disable(void) {
    gpio_set_level(SENSOR_POWER_PIN, 0);
    gpio_reset_pin(SENSOR_POWER_PIN); 
}

int32_t hal_moisture_read_mv(void) {
    pwr_gate_enable();

    if (!ensure_adc_init()) {
        pwr_gate_disable();
        return -1;
    }

    int raw = 0, mv = 0, sum_mv = 0;
    uint8_t valid_samples = 0;

    for (uint8_t i = 0; i < HAL_MOISTURE_SAMPLES; i++) {
        if (adc_oneshot_read(s_adc_handle, SENSOR_ADC_CHANNEL, &raw) != ESP_OK) {
            ESP_LOGW(TAG, "[MOISTURE] ADC read failed on sample %d — skipping.", i);
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        if (s_cali_handle) {
            adc_cali_raw_to_voltage(s_cali_handle, raw, &mv);
        } else {
            mv = (raw * 3300) / 4095;
        }
        sum_mv += mv;
        valid_samples++;
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    pwr_gate_disable();

    if (valid_samples == 0) {
        ESP_LOGE(TAG, "[MOISTURE] All samples failed — returning -1.");
        return -1;
    }
    return (sum_mv / valid_samples);
}

uint8_t hal_moisture_mv_to_percent(int32_t current_mv, int16_t v_dry_mv, int16_t v_wet_mv) {
    if (v_dry_mv <= v_wet_mv) return 0; 
    if (current_mv >= v_dry_mv) return 0;   
    if (current_mv <= v_wet_mv) return 100; 

    int32_t percentage = ((v_dry_mv - current_mv) * 100) / (v_dry_mv - v_wet_mv);
    if (percentage < 0) return 0;
    if (percentage > 100) return 100;
    return (uint8_t)percentage;
}