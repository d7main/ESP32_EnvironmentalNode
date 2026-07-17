/**
 * This file is part of the ESP32_EnvironmentalNode project.
 * 
 * @file config.h
 * @brief Global Hardware & Software Configuration
 * @author d7main
 * contact info: demianzaiats@gmail.com
 * @license MIT
 */

#ifndef CONFIG_H
#define CONFIG_H

#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"

/* 
 * HARDWARE MAP (Target selected via platformio.ini)
*/
#if defined(CONFIG_TARGET_ESP32_S3)
    #define CHIP_NAME               "ESP32-S3"
    
    /* Soil Moisture (Capacitive via ADC) */
    #define SENSOR_POWER_PIN        GPIO_NUM_21
    #define SENSOR_ADC_CHANNEL      ADC_CHANNEL_3 /* GPIO4 */
    
    /* BMP280 (I2C) */
    #define I2C_SDA_PIN             GPIO_NUM_8
    #define I2C_SCL_PIN             GPIO_NUM_9
    
    /* DHT22 (1-Wire) */
    #define DHT22_DATA_PIN          GPIO_NUM_5

#elif defined(CONFIG_TARGET_ESP32_CLASSIC)
    #define CHIP_NAME               "ESP32-Classic"
    
    /* Soil Moisture (Capacitive via ADC) */
    #define SENSOR_POWER_PIN        GPIO_NUM_21
    #define SENSOR_ADC_CHANNEL      ADC_CHANNEL_6 /* GPIO34 */
    
    /* BMP280 (I2C) */
    #define I2C_SDA_PIN             GPIO_NUM_22
    #define I2C_SCL_PIN             GPIO_NUM_23
    
    /* DHT22 (1-Wire) */
    #define DHT22_DATA_PIN          GPIO_NUM_18

#elif defined(CONFIG_TARGET_ESP32_C3)
    #define CHIP_NAME               "ESP32-C3"
    
    /* Soil Moisture (Capacitive via ADC) */
    /* GPIO21 = USB D+ on ESP32-C3-DevKitM-1 — NOT a usable general-purpose pin.
     * Set this to the GPIO that actually controls your sensor's VCC transistor.
     * GPIO7 is a safe default on DevKitM-1; update to match your real wiring. */
    #define SENSOR_POWER_PIN        GPIO_NUM_7
    #define SENSOR_ADC_CHANNEL      ADC_CHANNEL_2 /* GPIO3 */
    
    /* BMP280 (I2C) */
    #define I2C_SDA_PIN             GPIO_NUM_8
    #define I2C_SCL_PIN             GPIO_NUM_9
    
    /* DHT22 (1-Wire) */
    #define DHT22_DATA_PIN          GPIO_NUM_10

#else
    #error "Target architecture not defined! Check build_flags in platformio.ini."
#endif

/* 
 * SYSTEM CONTROLS
 * */
/* NOTE: GPIO2 is a strapping pin on ESP32-C3 (selects JTAG vs. normal boot).
 * Shorting GPIO2 to GND during power-on holds the chip in JTAG/download mode
 * and prevents app_main from ever running. Use GPIO9 instead — it is the
 * physical BOOT button on the ESP32-C3-DevKitM-1 board and is safe to pull
 * LOW at any time (internal pull-up is always enabled in hardware). */
#define BUTTON_CONFIG_PIN           GPIO_NUM_9        /* BOOT Button for Web Portal */

#define DEFAULT_SLEEP_SEC           1800          /* 30 minutes */
#define NVS_NAMESPACE               "storage"

/* 
 * ACCESS POINT (CONFIG MODE)
 */
#define AP_WIFI_SSID                "d7main_sensor"
#define AP_WIFI_PASS                "admin123"    /* Min 8 chars for WPA2 */

#endif /* CONFIG_H */