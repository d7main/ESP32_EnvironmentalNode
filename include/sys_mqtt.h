/**
 * This file is part of the ESP32_EnvironmentalNode project.
 *
 * @file  sys_mqtt.h
 * @brief MQTT publish abstraction for Home Assistant Auto-Discovery.
 *
 *        On each operational wake-up cycle the firmware calls sys_mqtt_publish()
 *        once — after connecting to Wi-Fi STA — to:
 *
 *          1. Publish HA Auto-Discovery config payloads for all sensor entities
 *             (retained, so HA re-registers after broker restarts).
 *          2. Publish current sensor state values to their respective topics.
 *
 *        The function manages the full MQTT client lifecycle internally:
 *        connect → publish → disconnect.  A 5-second hard timeout prevents
 *        the MQTT path from stalling the deep-sleep cycle on an unreachable
 *        broker.
 *
 *        The entire function is a no-op when cfg->mqtt_broker_uri is empty,
 *        so the channel is safely disabled without any code changes.
 *
 * @author d7main
 * contact info: demianzaiats@gmail.com
 * @license MIT
 */

#ifndef SYS_MQTT_H
#define SYS_MQTT_H

#include <stdint.h>
#include "sys_nvs.h"     /* sys_config_t                          */
#include "hal_bmp280.h"  /* bmp280_data_t                         */
#include "hal_dht22.h"   /* dht22_data_t                          */

/**
 * @brief Connect to the configured MQTT broker, publish HA Auto-Discovery
 *        configs and sensor state values, then disconnect.
 *
 *        Safe to call even when MQTT is not configured — the function returns
 *        immediately (no-op) when cfg->mqtt_broker_uri is empty.
 *
 * @param cfg           Loaded device configuration (broker credentials, prefix).
 * @param soil_mv       Raw soil ADC reading in millivolts.
 * @param moisture_pct  Calculated soil moisture percentage (0–100).
 *                      Pass -1 when uncalibrated; the soil_pct topic is then
 *                      skipped to avoid sending invalid data to Home Assistant.
 * @param bmp           BMP280 data (temperature + pressure). Must not be NULL.
 * @param dht           DHT22 data (temperature + humidity).  Must not be NULL.
 */
void sys_mqtt_publish(const sys_config_t *cfg,
                      int32_t soil_mv, int moisture_pct,
                      const bmp280_data_t *bmp,
                      const dht22_data_t *dht);

#endif /* SYS_MQTT_H */
