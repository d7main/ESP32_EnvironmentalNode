/**
 * @file hal_dht22.c
 * @brief DHT22 1-Wire bitbanging implementation with strict interrupt control
 */

#include "hal_dht22.h"
#include "config.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "HAL_DHT22";

#define DHT_TIMEOUT_US 1000

static int wait_for_pin_state(int expected_state) {
    int32_t start_time = (int32_t)esp_timer_get_time();
    while (gpio_get_level(DHT22_DATA_PIN) != expected_state) {
        if ((int32_t)esp_timer_get_time() - start_time > DHT_TIMEOUT_US) {
            return -1;
        }
    }
    return ((int32_t)esp_timer_get_time() - start_time);
}

bool hal_dht22_read(dht22_data_t *out_data) {
    if (!out_data) return false;

    uint8_t data[5] = {0};
    
    gpio_set_direction(DHT22_DATA_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(DHT22_DATA_PIN, 0);
    
    /* Host pull low for at least 18ms to signal start */
    vTaskDelay(pdMS_TO_TICKS(20));

    portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    portENTER_CRITICAL(&mux);

    gpio_set_level(DHT22_DATA_PIN, 1);
    esp_rom_delay_us(40);
    gpio_set_direction(DHT22_DATA_PIN, GPIO_MODE_INPUT);

    /* Sensor acknowledgment: 80us LOW, 80us HIGH */
    if (wait_for_pin_state(0) == -1) goto timeout;
    if (wait_for_pin_state(1) == -1) goto timeout;
    if (wait_for_pin_state(0) == -1) goto timeout;

    /* Read 40 bits (5 bytes) */
    for (int i = 0; i < 40; i++) {
        if (wait_for_pin_state(1) == -1) goto timeout;
        
        int32_t high_duration = wait_for_pin_state(0);
        if (high_duration == -1) goto timeout;

        /* High signal > 40us implies a binary '1', else '0' */
        if (high_duration > 40) {
            data[i / 8] |= (1 << (7 - (i % 8)));
        }
    }

    portEXIT_CRITICAL(&mux);

    /* Verify checksum */
    uint8_t checksum = data[0] + data[1] + data[2] + data[3];
    if (data[4] != checksum) {
        ESP_LOGE(TAG, "Checksum mismatch (Expected: 0x%02X, Got: 0x%02X)", checksum, data[4]);
        return false;
    }

    /* Convert raw bytes to physical values */
    uint16_t raw_humidity = (data[0] << 8) | data[1];
    uint16_t raw_temperature = ((data[2] & 0x7F) << 8) | data[3];

    out_data->humidity = (float)raw_humidity / 10.0f;
    out_data->temperature = (float)raw_temperature / 10.0f;
    
    if (data[2] & 0x80) {
        out_data->temperature *= -1.0f;
    }

    return true;

timeout:
    portEXIT_CRITICAL(&mux);
    ESP_LOGE(TAG, "Sensor bus timeout");
    return false;
}