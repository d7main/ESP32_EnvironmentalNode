/**
 * This file is part of the ESP32_EnvironmentalNode project.
 *
 * @file  hal_mhz19.c
 * @brief MH-Z19B CO₂ sensor HAL — UART binary protocol implementation.
 *
 *        Protocol: 9-byte binary (9600 baud, 8N1, no flow control).
 *        Read command:  FF 01 86 00 00 00 00 00 79
 *        Response:      FF 86 <HI> <LO> <STAT> 00 00 00 <CRC>
 *        CO₂ ppm = (HI << 8) | LO
 *        Checksum = ~(byte1 + ... + byte7) + 1  (inverted sum of bytes 1–7)
 *
 * @author d7main
 * contact info: demianzaiats@gmail.com
 * @license MIT
 */

#include "hal_mhz19.h"
#include "config.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "HAL_MHZ19";

#define MHZ19_UART_PORT     UART_NUM_1
#define MHZ19_UART_BAUD     9600
#define MHZ19_BUF_SIZE      256
#define MHZ19_RESPONSE_LEN  9
#define MHZ19_READ_TIMEOUT_MS 1000

/* 9-byte "Read CO2" command */
static const uint8_t CMD_READ_CO2[9] = {
    0xFF, 0x01, 0x86, 0x00, 0x00, 0x00, 0x00, 0x00, 0x79
};

static bool s_inited = false;

/* ── Checksum ─────────────────────────────────────────────────────────────── */

/**
 * @brief MH-Z19 checksum: invert the sum of bytes [1..7], add 1.
 *        Applied to both outgoing commands and validated on incoming responses.
 */
static uint8_t mhz19_checksum(const uint8_t *buf)
{
    uint8_t crc = 0;
    for (int i = 1; i <= 7; i++) {
        crc += buf[i];
    }
    return (~crc) + 1;
}

/* ── Public interface ─────────────────────────────────────────────────────── */

esp_err_t hal_mhz19_init(void)
{
    if (s_inited) return ESP_OK;

    uart_config_t uart_cfg = {
        .baud_rate  = MHZ19_UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_param_config(MHZ19_UART_PORT, &uart_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config failed: %s", esp_err_to_name(err));
        return err;
    }

    err = uart_set_pin(MHZ19_UART_PORT,
                       MHZ19_UART_TX_PIN,  /* TX — connects to sensor RX */
                       MHZ19_UART_RX_PIN,  /* RX — connects to sensor TX */
                       UART_PIN_NO_CHANGE, /* RTS not used */
                       UART_PIN_NO_CHANGE  /* CTS not used */);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin failed: %s", esp_err_to_name(err));
        return err;
    }

    err = uart_driver_install(MHZ19_UART_PORT,
                              MHZ19_BUF_SIZE, /* RX ring buffer */
                              0,              /* TX ring buffer (0 = blocking) */
                              0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }

    s_inited = true;
    ESP_LOGI(TAG, "UART%d initialised (TX=GPIO%d RX=GPIO%d, 9600 baud)",
             MHZ19_UART_PORT, MHZ19_UART_TX_PIN, MHZ19_UART_RX_PIN);
    return ESP_OK;
}

esp_err_t hal_mhz19_read_data(cJSON *root)
{
    if (!s_inited) {
        ESP_LOGW(TAG, "read_data called before init — calling init now");
        esp_err_t err = hal_mhz19_init();
        if (err != ESP_OK) return err;
    }

    /* Flush any stale bytes in the RX buffer before sending a new command */
    uart_flush_input(MHZ19_UART_PORT);

    /* Send the 9-byte read command */
    int written = uart_write_bytes(MHZ19_UART_PORT,
                                   (const char *)CMD_READ_CO2,
                                   sizeof(CMD_READ_CO2));
    if (written != (int)sizeof(CMD_READ_CO2)) {
        ESP_LOGE(TAG, "uart_write_bytes: expected %d, sent %d",
                 (int)sizeof(CMD_READ_CO2), written);
        return ESP_FAIL;
    }

    /* Wait for the 9-byte response */
    uint8_t response[MHZ19_RESPONSE_LEN];
    int received = uart_read_bytes(MHZ19_UART_PORT, response,
                                   MHZ19_RESPONSE_LEN,
                                   pdMS_TO_TICKS(MHZ19_READ_TIMEOUT_MS));
    if (received < MHZ19_RESPONSE_LEN) {
        ESP_LOGE(TAG, "Timeout: expected %d bytes, got %d",
                 MHZ19_RESPONSE_LEN, received);
        return ESP_ERR_TIMEOUT;
    }

    /* Validate header and checksum */
    if (response[0] != 0xFF || response[1] != 0x86) {
        ESP_LOGE(TAG, "Bad response header: 0x%02X 0x%02X",
                 response[0], response[1]);
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint8_t expected_crc = mhz19_checksum(response);
    if (response[8] != expected_crc) {
        ESP_LOGE(TAG, "CRC mismatch: got 0x%02X, expected 0x%02X",
                 response[8], expected_crc);
        return ESP_ERR_INVALID_CRC;
    }

    int co2_ppm = ((int)response[2] << 8) | response[3];
    cJSON_AddNumberToObject(root, "mhz19_co2_ppm", (double)co2_ppm);

    ESP_LOGI(TAG, "CO2 = %d ppm", co2_ppm);
    return ESP_OK;
}

void hal_mhz19_deinit(void)
{
    if (!s_inited) return;
    uart_driver_delete(MHZ19_UART_PORT);
    /* Return UART pins to Hi-Z to avoid leakage current during deep sleep */
    gpio_reset_pin(MHZ19_UART_TX_PIN);
    gpio_reset_pin(MHZ19_UART_RX_PIN);
    s_inited = false;
    ESP_LOGD(TAG, "Deinitialised");
}
