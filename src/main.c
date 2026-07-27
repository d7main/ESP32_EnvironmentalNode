/**
 * This file is part of the ESP32_EnvironmentalNode project.
 *
 * @file    main.c
 * @brief   Application entry point.
 *
 *          Boot sequence
 *          ─────────────
 *          1. Initialize NVS flash (required by Wi-Fi and our config layer).
 *          2. Initialize the TCP/IP stack and default event loop ONCE — both
 *             sys_wifi_start_ap_and_server() and sys_wifi_connect_sta() depend
 *             on these singletons already being up.
 *          3. Load the persisted configuration from NVS.
 *          4. If the device is not yet configured → go straight into the
 *             configuration portal (blocks until the user submits + reboot).
 *          5. If the device IS configured → spawn the background button-
 *             monitor task and proceed with normal operation.
 *
 *          Button behaviour
 *          ────────────────
 *          Holding BUTTON_CONFIG_PIN (GPIO9 / physical BOOT button on the
 *          DevKitM-1) LOW for ≥ 3 seconds triggers the configuration portal.
 *
 *          ⚠  GPIO STRAPPING PIN WARNING (ESP32-C3)
 *          GPIO2 controls the boot mode on ESP32-C3:
 *            • HIGH (default, internal pull-up) → normal execution.
 *            • LOW  during power-on             → JTAG / serial-download mode.
 *          Shorting GPIO2 to GND with tweezers while the chip powers on
 *          prevents app_main from ever running — which was the original bug.
 *          GPIO9 (the on-board BOOT button) does not have this restriction.
 *
 * @author  d7main
 * @license MIT
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_sleep.h"
#include "driver/gpio.h"

#include "config.h"
#include "sys_nvs.h"
#include "sys_wifi.h"
#include "sys_mqtt.h"
#include "hal_moisture.h"
#include "hal_bmp280.h"
#include "hal_dht22.h"

/* ──────────────────────────────────────────────────────────────────────────
 * Configuration
 * ────────────────────────────────────────────────────────────────────────── */

/** Duration the button must be held to trigger the config portal (ms). */
#define BUTTON_HOLD_MS          3000U

/** Polling interval inside the button task (ms).
 *  Short enough to feel responsive, long enough not to waste CPU cycles. */
#define BUTTON_POLL_INTERVAL_MS 100U

/** Stack depth for the button monitor FreeRTOS task (words). */
#define BUTTON_TASK_STACK_WORDS 2048U

/** FreeRTOS priority for the button monitor task.
 *  Keep low — it only polls a GPIO, nothing time-critical. */
#define BUTTON_TASK_PRIORITY    1U

static const char *TAG = "APP_MAIN";

/* ──────────────────────────────────────────────────────────────────────────
 * Forward declarations
 * ────────────────────────────────────────────────────────────────────────── */
static void button_monitor_task(void *arg);

/* ──────────────────────────────────────────────────────────────────────────
 * Button monitor task
 * ────────────────────────────────────────────────────────────────────────── */

/**
 * @brief FreeRTOS task that monitors BUTTON_CONFIG_PIN.
 *
 *        The pin is configured as INPUT with the internal pull-up enabled,
 *        so it reads HIGH when the button is open and LOW when pressed
 *        (active-low, typical push-to-GND wiring or tweezers-to-GND).
 *
 *        When the button is held continuously for BUTTON_HOLD_MS ms the task
 *        calls sys_wifi_start_ap_and_server(), which blocks until the user
 *        submits the form and the chip reboots.
 *
 * @param arg  Unused (required by FreeRTOS task signature).
 */
static void button_monitor_task(void *arg) {
    /* ── GPIO initialisation ─────────────────────────────────────────────── */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BUTTON_CONFIG_PIN),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,   /* active-low button → need pull-up */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,    /* we poll, not interrupt-driven */
    };

    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        /* Non-fatal: log and exit the task — do NOT crash the whole firmware. */
        ESP_LOGE(TAG, "[BTN] gpio_config failed for GPIO%d: %s. "
                       "Button monitoring disabled.",
                 BUTTON_CONFIG_PIN, esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "[BTN] Button monitor active on GPIO%d (hold %u ms to enter portal).",
             BUTTON_CONFIG_PIN, BUTTON_HOLD_MS);

    /* ── Polling loop ────────────────────────────────────────────────────── */
    uint32_t hold_ticks = 0; /* Accumulated hold time in ms. */

    for (;;) {
        int level = gpio_get_level(BUTTON_CONFIG_PIN);

        if (level == 0) {
            /* Button is pressed (pin pulled to GND). */
            hold_ticks += BUTTON_POLL_INTERVAL_MS;

            if (hold_ticks == BUTTON_POLL_INTERVAL_MS) {
                /* First detection — log once so the serial monitor shows
                 * that the press was registered. */
                ESP_LOGI(TAG, "[BTN] Button press detected on GPIO%d — "
                               "hold for %u ms to enter config portal.",
                         BUTTON_CONFIG_PIN, BUTTON_HOLD_MS);
            }

            /* Visual progress every 500 ms so the user sees something is
             * happening in the serial monitor. */
            if (hold_ticks % 500 == 0) {
                ESP_LOGI(TAG, "[BTN] Held for %lu ms / %u ms ...",
                         (unsigned long)hold_ticks, BUTTON_HOLD_MS);
            }

            if (hold_ticks >= BUTTON_HOLD_MS) {
                ESP_LOGW(TAG, "[BTN] 3-second hold confirmed! "
                               "Launching Wi-Fi AP + Configuration Portal...");

                /* sys_wifi_start_ap_and_server() blocks until the device
                 * reboots after a successful configuration save.  The task
                 * never returns from this call in normal circumstances. */
                sys_wifi_start_ap_and_server();

                /* If (unexpectedly) control returns, reset hold counter
                 * and keep monitoring — do not crash. */
                ESP_LOGW(TAG, "[BTN] AP/server returned unexpectedly. "
                               "Resuming button monitoring.");
                hold_ticks = 0;
            }
        } else {
            /* Button released — reset the counter. */
            if (hold_ticks > 0) {
                ESP_LOGI(TAG, "[BTN] Button released after %lu ms (threshold: %u ms). "
                               "Cancelled.",
                         (unsigned long)hold_ticks, BUTTON_HOLD_MS);
                hold_ticks = 0;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_INTERVAL_MS));
    }
}

/* ──────────────────────────────────────────────────────────────────────────
 * Application entry point
 * ────────────────────────────────────────────────────────────────────────── */

void app_main(void) {
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, " ESP32_EnvironmentalNode booting on %s", CHIP_NAME);
    ESP_LOGI(TAG, "========================================");

    /* ── Step 1: NVS flash ───────────────────────────────────────────────── */
    /* Must be the very first call — Wi-Fi stack and our config layer both
     * require NVS to be initialised before anything else touches it. */
    if (!sys_nvs_init()) {
        /* Without NVS the device cannot remember its configuration.
         * Log the error but do NOT abort — we will fall through to the
         * config portal so the user can at least flash a new setup. */
        ESP_LOGE(TAG, "NVS initialisation failed! Config cannot be saved/loaded.");
    } else {
        ESP_LOGI(TAG, "NVS flash initialised OK.");
    }

    /* ── Step 2: TCP/IP stack and event loop ─────────────────────────────── */
    /* These are global IDF singletons.  They MUST be initialised exactly once.
     * Both sys_wifi_start_ap_and_server() and sys_wifi_connect_sta() depend
     * on them being already up when they are called.  Initialising here
     * prevents the ESP_ERR_INVALID_STATE crash that occurs if either WiFi
     * function tries to initialise them a second time. */
    esp_err_t err;

    err = esp_netif_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_init() failed: %s", esp_err_to_name(err));
        /* Non-recoverable for network operation, but let the boot continue
         * so we can at least see the serial output and investigate. */
    } else {
        ESP_LOGI(TAG, "TCP/IP network interface initialised OK.");
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_event_loop_create_default() failed: %s",
                 esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Default event loop created OK.");
    }

    /* ── Step 3: Load configuration from NVS ────────────────────────────── */
    sys_config_t cfg;
    bool load_ok = sys_nvs_load_config(&cfg);

    if (!load_ok) {
        ESP_LOGW(TAG, "Could not load config from NVS (brand new device?). "
                       "Defaulting to unconfigured state.");
        cfg.is_configured = false;
    }

    ESP_LOGI(TAG, "Device is_configured = %s",
             cfg.is_configured ? "TRUE" : "FALSE");

    /* ── Step 4: Branch on configuration state ───────────────────────────── */
    if (!cfg.is_configured) {
        /* ── Path A: First boot / unconfigured ──────────────────────────── */
        ESP_LOGW(TAG, "Device is NOT configured. Launching Wi-Fi AP and "
                       "configuration portal immediately.");
        ESP_LOGI(TAG, "Connect to SSID: \"%s\"  Password: \"%s\"  "
                       "then open http://192.168.4.1 in a browser.",
                 AP_WIFI_SSID, AP_WIFI_PASS);

        /* This call blocks indefinitely (serves the portal) until the user
         * submits the form, at which point the firmware reboots. */
        sys_wifi_start_ap_and_server();

        /* Should never be reached — but if it is, just log and sit here. */
        ESP_LOGE(TAG, "sys_wifi_start_ap_and_server() returned unexpectedly!");
        while (1) { vTaskDelay(pdMS_TO_TICKS(5000)); }

    } else {
        /* ── Path B: Device is configured — normal operation ─────────────── */
        ESP_LOGI(TAG, "Device is configured. Starting normal operation.");
        ESP_LOGI(TAG, "  SSID       : %s", cfg.wifi_ssid);
        ESP_LOGI(TAG, "  TG Token   : %s", cfg.tg_token[0]            ? "[SET]" : "[not configured]");
        ESP_LOGI(TAG, "  TG Chat    : %s", cfg.tg_chat_id[0]          ? "[SET]" : "[not configured]");
        ESP_LOGI(TAG, "  Discord WH : %s", cfg.discord_webhook_url[0]  ? "[SET]" : "[not configured]");
        ESP_LOGI(TAG, "  Custom WH  : %s", cfg.custom_webhook_url[0]   ? "[SET]" : "[not configured]");
        ESP_LOGI(TAG, "  MQTT Broker: %s", cfg.mqtt_broker_uri[0]      ? cfg.mqtt_broker_uri : "[not configured]");
        ESP_LOGI(TAG, "  MQTT Prefix: %s", cfg.mqtt_topic_prefix[0]    ? cfg.mqtt_topic_prefix : "(default)");
        ESP_LOGI(TAG, "  Threshold  : %d%%", (int)cfg.soil_alert_threshold_pct);
        ESP_LOGI(TAG, "  V_dry      : %d mV", cfg.v_dry_mv);
        ESP_LOGI(TAG, "  V_wet      : %d mV", cfg.v_wet_mv);

        /* ── Step 5: Spawn the button monitor task ────────────────────────── */
        /* The task runs in the background at low priority, polling GPIO9.
         * If the user holds the button for 3 s the portal launches. */
        BaseType_t task_created = xTaskCreate(
            button_monitor_task,        /* Task function                */
            "btn_monitor",              /* Human-readable task name     */
            BUTTON_TASK_STACK_WORDS,    /* Stack size in words          */
            NULL,                       /* Task parameter               */
            BUTTON_TASK_PRIORITY,       /* Priority                     */
            NULL                        /* Task handle (not needed)     */
        );

        if (task_created != pdPASS) {
            ESP_LOGE(TAG, "Failed to create button_monitor_task! "
                           "(insufficient heap?) Button monitoring disabled.");
        } else {
            ESP_LOGI(TAG, "Button monitor task spawned. "
                           "Hold GPIO%d for %u ms to enter config portal.",
                     BUTTON_CONFIG_PIN, BUTTON_HOLD_MS);
        }

        /* ── Step 6: Read sensors ─────────────────────────────────────────── */
        /* Sensors self-initialise on first call (HAL layer manages handles). */
        ESP_LOGI(TAG, "Reading sensors...");

        int32_t    soil_mv = hal_moisture_read_mv();
        bmp280_data_t bmp  = {0};
        hal_bmp280_read(&bmp);
        dht22_data_t  dht  = {0};
        hal_dht22_read(&dht);

        /* Calculate soil moisture % from user-calibrated ADC bounds.
         * Mirrors the JavaScript calcPct() logic in the web portal.
         * moisture_pct == -1 means calibration is not yet set. */
        int moisture_pct = -1;
        if (cfg.v_dry_mv > cfg.v_wet_mv && soil_mv >= 0) {
            if      (soil_mv >= cfg.v_dry_mv) { moisture_pct = 0;   }
            else if (soil_mv <= cfg.v_wet_mv) { moisture_pct = 100; }
            else {
                moisture_pct = (int32_t)(
                    ((int32_t)cfg.v_dry_mv - soil_mv) * 100L /
                    ((int32_t)cfg.v_dry_mv - (int32_t)cfg.v_wet_mv)
                );
            }
        }

        ESP_LOGI(TAG, "Sensors: soil=%ld mV (%d%%) | "
                      "BMP280: %.1f C / %.1f hPa | DHT22: %.1f C / %.1f%%",
                 (long)soil_mv, moisture_pct,
                 bmp.temperature, bmp.pressure,
                 dht.temperature, dht.humidity);

        /* ── Step 7: Connect WiFi, publish MQTT, dispatch threshold alerts ── */
        if (sys_wifi_connect_sta(cfg.wifi_ssid, cfg.wifi_pass)) {

            /* Step 7a: MQTT telemetry — runs every cycle regardless of threshold.
             * Publishes HA Auto-Discovery configs + sensor state topics.
             * No-op if cfg.mqtt_broker_uri is empty. */
            sys_mqtt_publish(&cfg, soil_mv, moisture_pct, &bmp, &dht);

            /* Step 7b: Threshold-based webhook alerts — independent of MQTT. */
            if (moisture_pct < 0) {
                /* Calibration not configured — log a hint, send anyway so the
                 * user knows the node is alive, but skip threshold evaluation. */
                ESP_LOGW(TAG, "Moisture %% uncalibrated (v_dry=%d mV, v_wet=%d mV). "
                              "Set calibration in the portal to enable threshold alerts.",
                         cfg.v_dry_mv, cfg.v_wet_mv);

            } else if (moisture_pct < (int)cfg.soil_alert_threshold_pct) {

                ESP_LOGW(TAG, "ALERT: Soil moisture %d%% is BELOW threshold %d%%!",
                         moisture_pct, (int)cfg.soil_alert_threshold_pct);

                /* Build a human-readable alert message.  cJSON will re-escape
                 * it properly when building the webhook payloads. */
                char alert_msg[256];
                snprintf(alert_msg, sizeof(alert_msg),
                         "Low soil moisture alert!\n"
                         "Moisture: %d%% (threshold: %d%%)\n"
                         "Raw ADC : %ld mV\n"
                         "DHT22   : %.1f C / %.1f%% RH\n"
                         "BMP280  : %.1f C / %.1f hPa",
                         moisture_pct, (int)cfg.soil_alert_threshold_pct,
                         (long)soil_mv,
                         dht.temperature, dht.humidity,
                         bmp.temperature, bmp.pressure);

                sys_alerts_send(&cfg, alert_msg, soil_mv,
                                dht.temperature, dht.humidity);

            } else {
                ESP_LOGI(TAG, "Soil moisture OK (%d%% >= threshold %d%%). No alert sent.",
                         moisture_pct, (int)cfg.soil_alert_threshold_pct);
            }

        } else {
            ESP_LOGE(TAG, "WiFi connection failed. Skipping notifications this cycle.");
        }

        /* ── Step 8: Enter deep sleep ──────────────────────────────────────── */
        /* Give the TCP/IP stack a brief moment to flush before powering down. */
        vTaskDelay(pdMS_TO_TICKS(200));
        ESP_LOGI(TAG, "Entering deep sleep for %d seconds.", DEFAULT_SLEEP_SEC);
        esp_sleep_enable_timer_wakeup((uint64_t)DEFAULT_SLEEP_SEC * 1000000ULL);
        esp_deep_sleep_start();

        /* ── Unreachable — esp_deep_sleep_start() does not return. ──────────── */
        ESP_LOGE(TAG, "esp_deep_sleep_start() returned unexpectedly!");
        while (1) { vTaskDelay(pdMS_TO_TICKS(5000)); }
    }
}