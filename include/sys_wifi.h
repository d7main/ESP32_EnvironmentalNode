/**
 * This file is part of the ESP32_EnvironmentalNode project.
 * @file sys_wifi.h
 * @brief Wi-Fi, HTTP Server and Telegram API abstraction
 * @author d7main
 * contact info: demianzaiats@gmail.com
 * @license MIT
 */

#ifndef SYS_WIFI_H
#define SYS_WIFI_H

#include <stdbool.h>

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
 * @brief Send a JSON payload to the Telegram Bot API.
 * 
 * @param bot_token Telegram Bot Token
 * @param chat_id Target Chat ID
 * @param message Text message to send
 * @return true on HTTP 200 OK, false otherwise
 */
bool sys_wifi_send_telegram(const char *bot_token, const char *chat_id, const char *message);

#endif /* SYS_WIFI_H */