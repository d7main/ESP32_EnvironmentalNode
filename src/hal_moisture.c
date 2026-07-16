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
    vTaskDelay(pdMS_TO_TICKS(5)); 
}

static void pwr_gate_disable(void) {
    gpio_set_level(SENSOR_POWER_PIN, 0);
    gpio_reset_pin(SENSOR_POWER_PIN); 
}

int32_t hal_moisture_read_mv(void) {
    pwr_gate_enable();

    adc_oneshot_unit_handle_t adc_handle;
    adc_oneshot_unit_init_cfg_t init_cfg = {
        .unit_id = ADC_UNIT_1,
        .clk_src = 0,
    };
    if (adc_oneshot_new_unit(&init_cfg, &adc_handle) != ESP_OK) {
        pwr_gate_disable();
        return -1;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = ADC_ATTEN_DB_12,
    };
    adc_oneshot_config_channel(adc_handle, SENSOR_ADC_CHANNEL, &chan_cfg);

    adc_cali_handle_t cali_handle = NULL;
    bool has_cali = false;

#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t cali_config = {
        .unit_id = ADC_UNIT_1,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_line_fitting(&cali_config, &cali_handle) == ESP_OK) {
        has_cali = true;
    }
#endif

    int raw = 0, mv = 0, sum_mv = 0;
    for (uint8_t i = 0; i < HAL_MOISTURE_SAMPLES; i++) {
        adc_oneshot_read(adc_handle, SENSOR_ADC_CHANNEL, &raw);
        if (has_cali) {
            adc_cali_raw_to_voltage(cali_handle, raw, &mv);
            sum_mv += mv;
        } else {
            sum_mv += (raw * 3300) / 4095;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    adc_oneshot_del_unit(adc_handle);
#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    if (has_cali) {
        adc_cali_delete_scheme_line_fitting(cali_handle);
    }
#endif

    pwr_gate_disable();
    return (sum_mv / HAL_MOISTURE_SAMPLES);
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