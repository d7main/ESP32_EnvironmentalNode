/**
 * This file is part of the ESP32_EnvironmentalNode project.
 * @file sys_wifi.h
 * @brief Wi-Fi, HTTP Server, and multi-channel alert delivery abstraction.
 * @author d7main
 * contact info: demianzaiats@gmail.com
 * @license MIT
 */

#ifndef SYS_WIFI_H
#define SYS_WIFI_H

#include <stdbool.h>
#include <stdint.h>
#include "sys_nvs.h"    /* sys_config_t */

/**
 * @brief Start Wi-Fi in Access Point (AP) mode and launch the Configuration Web Server.
 *        This function blocks indefinitely while serving the portal.
 */
void sys_wifi_start_ap_and_server(void);

/**
 * @brief Connect to external Wi-Fi Router (Station mode).
 *
 * @param ssid Network SSID
 * @param pass Network Password
 * @return true if connected successfully, false on timeout
 */
bool sys_wifi_connect_sta(const char *ssid, const char *pass);

/**
 * @brief Dispatch an alert message to all configured notification channels.
 *
 *        Each channel (Telegram, Discord, Custom Webhook) is attempted
 *        independently — a failure or missing credentials on one channel
 *        never prevents the others from running.
 *
 *        Channels with empty/blank credentials are silently skipped.
 *        All HTTP requests are subject to a 6-second timeout to prevent
 *        blocking the deep-sleep cycle on unreachable endpoints.
 *
 * @param cfg      Loaded device configuration (holds credentials/URLs).
 * @param message  Plain-text alert message to broadcast.
 * @param soil_mv  Raw soil ADC reading in mV (included in Custom Webhook payload).
 * @param temp     Temperature in °C (included in Custom Webhook payload).
 * @param humidity Relative humidity % (included in Custom Webhook payload).
 */
void sys_alerts_send(const sys_config_t *cfg, const char *message,
                     int32_t soil_mv, float temp, float humidity);

#endif /* SYS_WIFI_H */