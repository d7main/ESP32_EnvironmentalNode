/**
 * This file is part of the ESP32_EnvironmentalNode project.
 * @file hal_dht22.c
 * @brief DHT22 1-Wire bitbanging implementation with strict interrupt control
 * @author d7main
 * contact info: demianzaiats@gmail.com
 * @license MIT
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
    /* Use int64_t to avoid overflow: int32_t wraps after ~35 min of uptime,
     * which would cause permanent false timeouts in long AP portal sessions. */
    int64_t start_time = esp_timer_get_time();
    while (gpio_get_level(DHT22_DATA_PIN) != expected_state) {
        if ((esp_timer_get_time() - start_time) > DHT_TIMEOUT_US) {
            return -1;
        }
    }
    return (int)(esp_timer_get_time() - start_time);
}

bool hal_dht22_read(dht22_data_t *out_data) {
    if (!out_data) return false;

    uint8_t data[5] = {0};

    gpio_set_direction(DHT22_DATA_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(DHT22_DATA_PIN, 0);

    /* Host pull low for at least 18 ms to signal start */
    vTaskDelay(pdMS_TO_TICKS(20));

    /* Release the bus and switch to input — outside any critical section.
     * The 40 µs delay here is a busy-wait; FreeRTOS only preempts at tick
     * boundaries so the probability of preemption in 40 µs is negligible,
     * and DHT22 is tolerant of minor timing variation in the start sequence. */
    gpio_set_level(DHT22_DATA_PIN, 1);
    esp_rom_delay_us(40);
    gpio_set_direction(DHT22_DATA_PIN, GPIO_MODE_INPUT);

    /* Sensor acknowledgment: 80 µs LOW then 80 µs HIGH.
     * Timing here is loose enough that no critical section is needed. */
    if (wait_for_pin_state(0) == -1) goto timeout;
    if (wait_for_pin_state(1) == -1) goto timeout;
    if (wait_for_pin_state(0) == -1) goto timeout;

    /* Read 40 bits (5 bytes).
     *
     * The HIGH-duration measurement of each bit is the only window where
     * microsecond accuracy matters (~26 µs = '0', ~70 µs = '1').  Wrap
     * ONLY that measurement in a critical section so the scheduler is
     * frozen for at most ~70 µs per bit instead of the full ~5 ms total.
     *
     * The LOW→HIGH edge wait (wait_for_pin_state(1)) runs without a lock
     * because jitter there does not affect bit value determination.
     *
     * After portEXIT_CRITICAL() the mux is unlocked before any goto, so
     * the timeout label is always reached with no critical section active. */
    {
        portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
        for (int i = 0; i < 40; i++) {
            /* Wait for the LOW→HIGH bit edge — no critical section needed */
            if (wait_for_pin_state(1) == -1) goto timeout;

            /* Measure the HIGH pulse duration under a critical section */
            portENTER_CRITICAL(&mux);
            int high_duration = wait_for_pin_state(0);
            portEXIT_CRITICAL(&mux); /* always unlocked before any branch */

            if (high_duration == -1) goto timeout;

            /* HIGH > 40 µs → binary '1', otherwise binary '0' */
            if (high_duration > 40) {
                data[i / 8] |= (1 << (7 - (i % 8)));
            }
        }
    } /* mux scope ends here; lock is guaranteed unlocked on all paths above */

    /* Verify checksum */
    uint8_t checksum = data[0] + data[1] + data[2] + data[3];
    if (data[4] != checksum) {
        ESP_LOGE(TAG, "Checksum mismatch (Expected: 0x%02X, Got: 0x%02X)", checksum, data[4]);
        return false;
    }

    /* Convert raw bytes to physical values */
    uint16_t raw_humidity    = (data[0] << 8) | data[1];
    uint16_t raw_temperature = ((data[2] & 0x7F) << 8) | data[3];

    out_data->humidity    = (float)raw_humidity    / 10.0f;
    out_data->temperature = (float)raw_temperature / 10.0f;

    if (data[2] & 0x80) {
        out_data->temperature *= -1.0f;
    }

    return true;

timeout:
    /* No critical section is active on any path that reaches this label:
     *  - ACK-phase gotos jump from outside any CS.
     *  - Bit-loop gotos execute only after portEXIT_CRITICAL() has returned. */
    ESP_LOGE(TAG, "Sensor bus timeout");
    return false;
}