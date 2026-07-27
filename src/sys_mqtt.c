/**
 * This file is part of the ESP32_EnvironmentalNode project.
 *
 * @file  sys_mqtt.c
 * @brief MQTT client implementation: HA Auto-Discovery + sensor state publish.
 *
 *        Architecture overview
 *        ─────────────────────
 *        This module owns the entire MQTT lifecycle for one publish cycle:
 *
 *          sys_mqtt_publish()
 *            ├─ Build broker URI and LWT topic strings
 *            ├─ Init esp-mqtt client with LWT ("offline", retain=1)
 *            ├─ Register event handler → sets EventGroup bits on CONNECTED/ERROR
 *            ├─ esp_mqtt_client_start()
 *            ├─ xEventGroupWaitBits(CONNECTED | ERROR, 5 s timeout)
 *            │   ├─ [CONNECTED] → publish_ha_discovery() → publish_sensor_states()
 *            │   └─ [ERROR / timeout] → log and skip publish
 *            ├─ vTaskDelay(500 ms)  — let QoS 0 outbound queue drain
 *            └─ esp_mqtt_client_stop() → esp_mqtt_client_destroy()
 *
 *        All publishes use QoS 0 (fire-and-forget) to minimise pre-sleep
 *        latency.  HA Auto-Discovery configs use retain=1; state topics
 *        use retain=0.
 *
 * @author d7main
 * contact info: demianzaiats@gmail.com
 * @license MIT
 */

#include "sys_mqtt.h"
#include "config.h"

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "mqtt_client.h"
#include "cJSON.h"

/* ── Module tag ─────────────────────────────────────────────────────────── */
static const char *TAG = "SYS_MQTT";

/* ── EventGroup bits used to signal the publish path ────────────────────── */
#define MQTT_CONNECTED_BIT  BIT0
#define MQTT_ERROR_BIT      BIT1

/* ── Timeouts ────────────────────────────────────────────────────────────── */
/** Maximum wait for MQTT_EVENT_CONNECTED before giving up (ms). */
#define MQTT_CONNECT_TIMEOUT_MS  5000U

/** Delay after last publish call to let the MQTT task drain its outbound
 *  queue before we stop the client.  QoS 0 frames are small; 500 ms is
 *  generous for any home-lab broker on a local Wi-Fi link. */
#define MQTT_DRAIN_DELAY_MS       500U

/* ── Default topic prefix (used when the NVS field is blank) ────────────── */
#define MQTT_DEFAULT_PREFIX  "d7main/sensor"

/* ── Software version string embedded in the HA device block ─────────────── */
#define FIRMWARE_VERSION  "2.0"

/* ==========================================================================
 * INTERNAL TYPES
 * ========================================================================== */

/**
 * @brief Context passed to the MQTT event handler via handler_args.
 *        Lives on the stack of sys_mqtt_publish() for its full duration.
 */
typedef struct {
    EventGroupHandle_t evt_group;  /**< Bits set by the event handler. */
} mqtt_evt_ctx_t;

/**
 * @brief Descriptor for a single Home Assistant sensor entity.
 */
typedef struct {
    const char *suffix;       /**< Topic suffix, e.g. "soil_pct"          */
    const char *name;         /**< HA entity display name                 */
    const char *device_class; /**< HA device_class (NULL = omit field)    */
    const char *unit;         /**< unit_of_measurement                    */
} ha_entity_t;

/* ==========================================================================
 * MQTT EVENT HANDLER
 * ========================================================================== */

/**
 * @brief Minimal MQTT event handler: sets EventGroup bits on connect/error.
 *        All heavy work (publishing) is done outside this handler to avoid
 *        blocking the MQTT internal task.
 */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    mqtt_evt_ctx_t *ctx = (mqtt_evt_ctx_t *)handler_args;
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {

        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "[MQTT] Connected to broker.");
            xEventGroupSetBits(ctx->evt_group, MQTT_CONNECTED_BIT);
            break;

        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "[MQTT] Connection error (error_type=%d).",
                     event->error_handle ? (int)event->error_handle->error_type : -1);
            xEventGroupSetBits(ctx->evt_group, MQTT_ERROR_BIT);
            break;

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGD(TAG, "[MQTT] Disconnected.");
            break;

        default:
            break;
    }
}

/* ==========================================================================
 * HOME ASSISTANT AUTO-DISCOVERY
 * ========================================================================== */

/**
 * @brief Allocate and return a cJSON object representing the HA "device" block.
 *        The caller owns the returned object (cJSON_Delete or attach to a
 *        parent and let the parent take ownership).
 *
 * @param device_id  Unique device identifier string, e.g. "d7node_a1b2c3".
 */
static cJSON *build_device_block(const char *device_id)
{
    cJSON *device = cJSON_CreateObject();
    if (!device) return NULL;

    cJSON *ids = cJSON_CreateArray();
    if (ids) {
        cJSON_AddItemToArray(ids, cJSON_CreateString(device_id));
        cJSON_AddItemToObject(device, "identifiers", ids);
    }

    cJSON_AddStringToObject(device, "name",         "ESP32 Environmental Node");
    cJSON_AddStringToObject(device, "manufacturer",  "d7main");
    cJSON_AddStringToObject(device, "model",         "ESP32-C3 Environmental Node");
    cJSON_AddStringToObject(device, "sw_version",    FIRMWARE_VERSION);

    return device;
}

/**
 * @brief Build and publish one HA Auto-Discovery config payload.
 *
 * @param client     Active MQTT client handle.
 * @param prefix     Topic prefix string (never NULL, never empty).
 * @param device_id  Unique device identifier string.
 * @param entity     Entity descriptor (suffix, name, device_class, unit).
 */
static void publish_one_discovery(esp_mqtt_client_handle_t client,
                                  const char *prefix,
                                  const char *device_id,
                                  const ha_entity_t *entity)
{
    /* ── Build topic strings ─────────────────────────────────────────────── */
    char unique_id[56];
    char state_topic[100];
    char avail_topic[100];
    char disc_topic[140];

    snprintf(unique_id,   sizeof(unique_id),   "%s_%s",                 device_id, entity->suffix);
    snprintf(state_topic, sizeof(state_topic), "%s/%s",                 prefix,    entity->suffix);
    snprintf(avail_topic, sizeof(avail_topic), "%s/status",             prefix);
    snprintf(disc_topic,  sizeof(disc_topic),  "homeassistant/sensor/%s/config", unique_id);

    /* ── Build cJSON payload ─────────────────────────────────────────────── */
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        ESP_LOGE(TAG, "[MQTT] cJSON alloc failed for entity '%s'.", entity->suffix);
        return;
    }

    cJSON_AddStringToObject(root, "name",                  entity->name);
    cJSON_AddStringToObject(root, "unique_id",             unique_id);
    cJSON_AddStringToObject(root, "state_topic",           state_topic);

    if (entity->device_class) {
        cJSON_AddStringToObject(root, "device_class",      entity->device_class);
    }

    cJSON_AddStringToObject(root, "unit_of_measurement",   entity->unit);
    cJSON_AddStringToObject(root, "value_template",        "{{ value }}");
    cJSON_AddStringToObject(root, "availability_topic",    avail_topic);
    cJSON_AddStringToObject(root, "payload_available",     "online");
    cJSON_AddStringToObject(root, "payload_not_available", "offline");

    /* build_device_block() allocates a new cJSON object; cJSON_AddItemToObject
     * takes ownership so it is freed when root is deleted. */
    cJSON *device = build_device_block(device_id);
    if (device) {
        cJSON_AddItemToObject(root, "device", device);
    }

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);  /* frees device block too */

    if (!payload) {
        ESP_LOGE(TAG, "[MQTT] cJSON print failed for entity '%s'.", entity->suffix);
        return;
    }

    /* ── Publish with retain=1, QoS=0 ───────────────────────────────────── */
    int msg_id = esp_mqtt_client_publish(client, disc_topic, payload,
                                         0 /* len = auto */, 0 /* QoS */, 1 /* retain */);
    if (msg_id < 0) {
        ESP_LOGE(TAG, "[MQTT] Failed to queue discovery for '%s'.", entity->suffix);
    } else {
        ESP_LOGD(TAG, "[MQTT] Discovery queued: %s", disc_topic);
    }

    free(payload);
}

/**
 * @brief Publish HA Auto-Discovery config payloads for all 6 sensor entities.
 *
 * @param client    Active MQTT client handle.
 * @param prefix    Topic prefix string.
 * @param device_id Unique device identifier string.
 */
static void publish_ha_discovery(esp_mqtt_client_handle_t client,
                                 const char *prefix,
                                 const char *device_id)
{
    /* Entity table — edit here to add/remove sensors from HA. */
    static const ha_entity_t entities[] = {
        { "soil_pct",     "Soil Moisture",       "moisture",              "%" },
        { "soil_mv",      "Soil Raw ADC",         NULL,                   "mV" },
        { "bmp_temp",     "Temperature (BMP280)", "temperature",          "°C" },
        { "bmp_pressure", "Pressure",             "atmospheric_pressure", "hPa" },
        { "dht_temp",     "Temperature (DHT22)",  "temperature",          "°C" },
        { "dht_humidity", "Humidity",             "humidity",             "%" },
    };

    const int count = (int)(sizeof(entities) / sizeof(entities[0]));

    for (int i = 0; i < count; i++) {
        publish_one_discovery(client, prefix, device_id, &entities[i]);
    }

    ESP_LOGI(TAG, "[MQTT] Discovery published: %d entities.", count);
}

/* ==========================================================================
 * SENSOR STATE PUBLISH
 * ========================================================================== */

/**
 * @brief Publish current sensor readings to their state topics (QoS 0, no retain).
 *        Also publishes "online" to the availability topic (retain=1).
 *
 * @param client       Active MQTT client handle.
 * @param prefix       Topic prefix string.
 * @param soil_mv      Raw ADC millivolts.
 * @param moisture_pct Soil moisture percentage (skipped if -1).
 * @param bmp          BMP280 data.
 * @param dht          DHT22 data.
 */
static void publish_sensor_states(esp_mqtt_client_handle_t client,
                                  const char *prefix,
                                  int32_t soil_mv, int moisture_pct,
                                  const bmp280_data_t *bmp,
                                  const dht22_data_t *dht)
{
    char topic[100];
    char value[24];
    int  published = 0;

/* Internal helper macro: build topic, format value, publish, count. */
#define MQTT_PUB(suffix, fmt, ...)                                              \
    do {                                                                        \
        snprintf(topic, sizeof(topic), "%s/%s", prefix, (suffix));             \
        snprintf(value, sizeof(value), (fmt), ##__VA_ARGS__);                  \
        int r = esp_mqtt_client_publish(client, topic, value,                   \
                                        0 /* len */, 0 /* QoS */, 0 /* retain */); \
        if (r < 0) {                                                            \
            ESP_LOGW(TAG, "[MQTT] Failed to queue state: %s", suffix);         \
        } else {                                                                \
            published++;                                                        \
            ESP_LOGD(TAG, "[MQTT] Queued %s = %s", suffix, value);             \
        }                                                                       \
    } while (0)

    /* ── Soil moisture ───────────────────────────────────────────────────── */
    if (moisture_pct >= 0) {
        MQTT_PUB("soil_pct", "%d", moisture_pct);
    }
    MQTT_PUB("soil_mv", "%ld", (long)soil_mv);

    /* ── BMP280 ──────────────────────────────────────────────────────────── */
    MQTT_PUB("bmp_temp",     "%.1f", bmp->temperature);
    MQTT_PUB("bmp_pressure", "%.1f", bmp->pressure);

    /* ── DHT22 ───────────────────────────────────────────────────────────── */
    MQTT_PUB("dht_temp",     "%.1f", dht->temperature);
    MQTT_PUB("dht_humidity", "%.1f", dht->humidity);

#undef MQTT_PUB

    /* ── Availability: publish "online" with retain=1 ────────────────────── */
    snprintf(topic, sizeof(topic), "%s/status", prefix);
    int r = esp_mqtt_client_publish(client, topic, "online", 6,
                                    0 /* QoS */, 1 /* retain */);
    if (r >= 0) published++;

    ESP_LOGI(TAG, "[MQTT] State published: %d topics.", published);
}

/* ==========================================================================
 * PUBLIC ENTRY POINT
 * ========================================================================== */

void sys_mqtt_publish(const sys_config_t *cfg,
                      int32_t soil_mv, int moisture_pct,
                      const bmp280_data_t *bmp,
                      const dht22_data_t *dht)
{
    /* ── Guard: skip entirely if broker is not configured ────────────────── */
    if (cfg->mqtt_broker_uri[0] == '\0') {
        ESP_LOGD(TAG, "[MQTT] Broker URI not configured. Skipping.");
        return;
    }

    /* ── Derive unique device_id from the last 3 bytes of the Wi-Fi MAC ─── */
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char device_id[16];
    snprintf(device_id, sizeof(device_id), "d7node_%02x%02x%02x",
             mac[3], mac[4], mac[5]);

    /* ── Resolve topic prefix (never leave it blank) ─────────────────────── */
    const char *prefix = cfg->mqtt_topic_prefix[0]
                         ? cfg->mqtt_topic_prefix
                         : MQTT_DEFAULT_PREFIX;

    /* ── Build the effective broker URI ──────────────────────────────────── */
    /* Accept a bare hostname/IP ("192.168.1.10") OR a full URI
     * ("mqtt://192.168.1.10:1883").  If the scheme is missing we prepend
     * "mqtt://" and append the configured port. */
    char uri[SYS_MQTT_URI_MAX_LEN + 30];
    if (strncmp(cfg->mqtt_broker_uri, "mqtt", 4) == 0) {
        /* User supplied a full URI — use it verbatim. */
        snprintf(uri, sizeof(uri), "%s", cfg->mqtt_broker_uri);
    } else {
        /* Bare host/IP — construct the URI ourselves. */
        uint16_t port = cfg->mqtt_port ? cfg->mqtt_port : 1883;
        snprintf(uri, sizeof(uri), "mqtt://%s:%u", cfg->mqtt_broker_uri, port);
    }

    /* ── Build the Last-Will topic ───────────────────────────────────────── */
    char lwt_topic[SYS_MQTT_PREFIX_MAX_LEN + 8];
    snprintf(lwt_topic, sizeof(lwt_topic), "%s/status", prefix);

    /* ── Resolve credentials (NULL = anonymous) ──────────────────────────── */
    const char *username = cfg->mqtt_username[0] ? cfg->mqtt_username : NULL;
    const char *password = cfg->mqtt_password[0] ? cfg->mqtt_password : NULL;

    ESP_LOGI(TAG, "[MQTT] Connecting to %s as device_id=%s prefix=%s",
             uri, device_id, prefix);

    /* ── Create EventGroup for connection signaling ──────────────────────── */
    EventGroupHandle_t evt_group = xEventGroupCreate();
    if (!evt_group) {
        ESP_LOGE(TAG, "[MQTT] Failed to create EventGroup. Skipping.");
        return;
    }

    /* ── Configure and start the MQTT client ─────────────────────────────── */
    mqtt_evt_ctx_t ctx = { .evt_group = evt_group };

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri                        = uri,
        .credentials.username                      = username,
        .credentials.authentication.password       = password,
        .session.last_will.topic                   = lwt_topic,
        .session.last_will.msg                     = "offline",
        .session.last_will.msg_len                 = 7,
        .session.last_will.retain                  = 1,
        .session.last_will.qos                     = 1,
        .network.timeout_ms                        = MQTT_CONNECT_TIMEOUT_MS,
    };

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    if (!client) {
        ESP_LOGE(TAG, "[MQTT] esp_mqtt_client_init() failed. Skipping.");
        vEventGroupDelete(evt_group);
        return;
    }

    esp_mqtt_client_register_event(client,
                                   (esp_mqtt_event_id_t)ESP_EVENT_ANY_ID,
                                   mqtt_event_handler,
                                   &ctx);
    esp_mqtt_client_start(client);

    /* ── Wait for connection (or timeout) ────────────────────────────────── */
    EventBits_t bits = xEventGroupWaitBits(
        evt_group,
        MQTT_CONNECTED_BIT | MQTT_ERROR_BIT,
        pdFALSE,   /* do not clear on exit */
        pdFALSE,   /* wait for ANY bit, not all */
        pdMS_TO_TICKS(MQTT_CONNECT_TIMEOUT_MS)
    );

    if (bits & MQTT_CONNECTED_BIT) {
        /* ── Connection established — publish discovery, then state ──── */
        publish_ha_discovery(client, prefix, device_id);
        publish_sensor_states(client, prefix, soil_mv, moisture_pct, bmp, dht);

        /* Give the MQTT internal task time to drain its outbound queue
         * before we pull the plug.  QoS 0 frames on a local LAN are
         * typically sent within a few milliseconds; 500 ms is very safe. */
        vTaskDelay(pdMS_TO_TICKS(MQTT_DRAIN_DELAY_MS));
        ESP_LOGI(TAG, "[MQTT] Publish cycle complete.");

    } else if (bits & MQTT_ERROR_BIT) {
        ESP_LOGE(TAG, "[MQTT] Broker connection error. Skipping MQTT this cycle.");
    } else {
        ESP_LOGE(TAG, "[MQTT] Connection timed out after %u ms. Skipping MQTT this cycle.",
                 MQTT_CONNECT_TIMEOUT_MS);
    }

    /* ── Clean up: stop client, free all resources ───────────────────────── */
    esp_mqtt_client_stop(client);
    esp_mqtt_client_destroy(client);
    vEventGroupDelete(evt_group);
}
