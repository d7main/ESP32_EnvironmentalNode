/**
 * This file is part of the ESP32_EnvironmentalNode project.
 * Wifi, HTTP Server and Telegram API implementation
 * @file sys_wifi.c
 * @brief Network implementation (Wi-Fi STA/AP, HTTPD, Telegram API)
 * @author d7main
 * contact info: demianzaiats@gmail.com
 * @license MIT
 */

#include "sys_wifi.h"
#include "config.h"
#include "sys_nvs.h"
#include "hal_moisture.h"
#include "hal_bmp280.h"
#include "hal_dht22.h"

#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_http_client.h"
#include "cJSON.h"


static const char *TAG = "SYS_WIFI";
static EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

/* ==========================================================================
 * PORTAL-MODE SENSOR GLOBALS
 *
 * Written exclusively by portal_sensor_task() (every 20 s) and read by the
 * /api/status HTTP handler.  No lock is required or correct:
 *
 *  - On the ESP32-C3 (single-core RISC-V) all aligned 32-bit loads and
 *    stores are single instructions — they cannot be preempted mid-write.
 *    float and int32_t are both exactly 32 bits and naturally 4-byte aligned
 *    by the compiler, so reads and writes are already atomic.
 *
 *  - volatile tells the compiler it must re-fetch the value from memory on
 *    every access instead of keeping a stale copy in a register.
 *
 *  - Using a mutex here would cause the HTTP handler to BLOCK while the
 *    sensor task holds the lock during a 100 ms ADC conversion or a 5 ms
 *    DHT22 readout, starving the web server.  Using a spinlock
 *    (portENTER_CRITICAL) would freeze the entire scheduler for the same
 *    duration.  Either choice causes the exact deadlock/watchdog symptom.
 * ========================================================================== */

/** Polling period for the portal sensor task (ms). */
#define PORTAL_SENSOR_POLL_MS   20000U

/** Stack for the portal sensor task (words). */
#define PORTAL_SENSOR_STACK     3072U

/** Priority — above idle, below everything else. */
#define PORTAL_SENSOR_PRIORITY  2U

static volatile int32_t s_moisture_mv = -1;  /* -1 = not yet sampled        */
static volatile float   s_bmp_temp    = 0.0f;
static volatile float   s_bmp_press   = 0.0f;
static volatile float   s_dht_temp    = 0.0f;
static volatile float   s_dht_hum     = 0.0f;
static volatile bool    s_data_valid  = false;

/**
 * @brief FreeRTOS task: poll all sensors every PORTAL_SENSOR_POLL_MS ms.
 *        Exists ONLY during Config Mode (AP + Web Server).
 *        No locks are used — see comment block above.
 */
static void portal_sensor_task(void *arg) {
    ESP_LOGI(TAG, "[SENSOR_TASK] Portal sensor polling started "
                  "(period = %u ms).", PORTAL_SENSOR_POLL_MS);

    for (;;) {
        /* -- Soil moisture (HAL handles SENSOR_POWER_PIN gate internally) -- */
        int32_t soil_mv = hal_moisture_read_mv();

        /* -- BMP280 -- */
        bmp280_data_t bmp = {0};
        hal_bmp280_read(&bmp);

        /* -- DHT22 -- */
        dht22_data_t dht = {0};
        hal_dht22_read(&dht);

        /* -- Publish results; 32-bit aligned writes are atomic on RISC-V -- */
        s_moisture_mv = soil_mv;
        s_bmp_temp    = bmp.temperature;
        s_bmp_press   = bmp.pressure;
        s_dht_temp    = dht.temperature;
        s_dht_hum     = dht.humidity;
        s_data_valid  = true;

        ESP_LOGI(TAG, "[SENSOR_TASK] soil: %ld mV | "
                      "BMP: %.1f C / %.1f hPa | DHT: %.1f C / %.1f %%",
                 (long)soil_mv,
                 bmp.temperature, bmp.pressure,
                 dht.temperature, dht.humidity);

        /* Yield the CPU for the full poll period. */
        vTaskDelay(pdMS_TO_TICKS(PORTAL_SENSOR_POLL_MS));
    }
}

/* ==========================================================================
 * EMBEDDED HTML PORTAL (Clean White & Orange UI)
 * ========================================================================== */
static const char *html_page =
"<!DOCTYPE html><html lang='en'><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>ESP32_EnvironmentalNode</title>"
"<style>"
":root{--primary:#FF6600;--bg:#f4f7f6;--card:#ffffff;--text:#1a1a1a;--muted:#666;--radius:16px;}"
"body{margin:0;padding:20px;font-family:'Segoe UI',system-ui,sans-serif;background:var(--bg);color:var(--text);display:flex;flex-direction:column;align-items:center;}"
".container{width:100%;max-width:500px;}"
".header{text-align:center;margin-bottom:30px;}"
".logo{font-size:3.8rem;font-weight:900;letter-spacing:-2px;margin:0;line-height:1;}"
".logo .d,.logo .m{color:#000;}"
".logo .num{color:var(--primary);}"
".subtitle{font-size:1rem;color:var(--muted);text-transform:uppercase;letter-spacing:2px;margin-top:8px;font-weight:700;}"
".card{background:var(--card);border-radius:var(--radius);padding:25px;box-shadow:0 10px 30px rgba(0,0,0,0.06);margin-bottom:20px;}"
"h3{margin-top:0;font-size:1.2rem;display:flex;align-items:center;gap:10px;border-bottom:2px solid #f0f0f0;padding-bottom:12px;color:#000;}"
"h3::before{content:'';display:block;width:12px;height:12px;background:var(--primary);border-radius:50%;}"
".grid{display:grid;gap:12px;margin-top:15px;}"
".metric{background:#fafafa;padding:16px;border-radius:12px;display:flex;justify-content:space-between;align-items:center;border:1px solid #eee;transition:transform 0.2s;}"
".metric:hover{transform:scale(1.02);}"
".m-title{font-size:0.95rem;color:var(--muted);font-weight:600;}"
".m-val{font-size:1.2rem;font-weight:800;color:var(--primary);}"
/* Progress bar for soil moisture percentage */
".pct-wrap{margin-top:8px;background:#f0f0f0;border-radius:6px;height:8px;overflow:hidden;}"
".pct-bar{height:100%;background:var(--primary);border-radius:6px;transition:width 0.6s ease;}"
/* Inline hint shown next to calibration inputs */
".live-hint{display:inline-block;margin-left:10px;font-size:0.82rem;font-weight:700;color:var(--primary);}"
".input-group{margin-bottom:18px;}"
"label{display:block;font-size:0.85rem;font-weight:700;color:var(--muted);margin-bottom:8px;text-transform:uppercase;letter-spacing:0.5px;}"
"input{width:100%;padding:14px;border:2px solid #e1e4e8;border-radius:10px;font-size:1rem;box-sizing:border-box;transition:all 0.3s;background:#fefefe;font-family:monospace;}"
"input:focus{border-color:var(--primary);outline:none;background:#fff;box-shadow:0 0 0 4px rgba(255,102,0,0.15);}"
".row{display:flex;gap:15px;}"
".row .input-group{flex:1;}"
".btn{width:100%;padding:18px;background:var(--primary);color:#fff;border:none;border-radius:12px;font-size:1.1rem;font-weight:800;cursor:pointer;transition:all 0.2s;margin-top:10px;text-transform:uppercase;letter-spacing:1px;box-shadow:0 4px 15px rgba(255,102,0,0.2);}"
".btn:hover{transform:translateY(-2px);box-shadow:0 8px 25px rgba(255,102,0,0.35);}"
".btn:active{transform:translateY(1px);box-shadow:none;}"
".footer{text-align:center;margin-top:25px;font-size:0.9rem;color:var(--muted);line-height:1.8;font-weight:500;padding-bottom:30px;}"
".footer a{color:var(--primary);text-decoration:none;font-weight:700;transition:color 0.2s;}"
".footer a:hover{color:#cc5200;}"
"</style>"
"<script>"
/* ── Global last-known soil reading (mV) so percentage can be recomputed
 *    whenever the user types new calibration bounds. ── */
"var g_soil_mv = null;"

/* ── Compute soil moisture % from mV + form calibration bounds.
 *    Mirrors the C logic in hal_moisture_mv_to_percent(). ── */
"function calcPct(mv){"
" var dry=parseInt(document.getElementById('v_dry').value)||0;"
" var wet=parseInt(document.getElementById('v_wet').value)||0;"
" if(dry<=wet||mv<0) return null;"
" if(mv>=dry) return 0;"
" if(mv<=wet) return 100;"
" return Math.round((dry-mv)*100/(dry-wet));"
"}"

/* ── Update the soil metric card (percentage + progress bar). ── */
"function updateSoilUI(){"
" if(g_soil_mv===null) return;"
" var pct=calcPct(g_soil_mv);"
" var pctTxt=(pct!==null)?(pct+'%'):'-- %';"
" document.getElementById('val_soil').innerText=pctTxt;"
" var bar=document.getElementById('pct_bar');"
" if(bar && pct!==null) bar.style.width=pct+'%';"
"}"

/* ── Fetch /api/status every 7 seconds and refresh all metric cards. ── */
"function fetchStatus(){"
" fetch('/api/status').then(r=>r.json()).then(d=>{"
"  g_soil_mv=d.soil_mv;"
"  updateSoilUI();"
"  document.getElementById('val_bmp').innerText=d.bmp_t+'\\u00b0C | '+d.bmp_p+' hPa';"
"  document.getElementById('val_dht').innerText=d.dht_t+'\\u00b0C | '+d.dht_h+'%';"
"  /* Update raw mV hint next to calibration inputs */"
"  var hint=document.getElementById('live_mv');"
"  if(hint) hint.innerText='live: '+d.soil_mv+' mV';"
" }).catch(e=>console.log('[status] fetch error:',e));"
"}"
"setInterval(fetchStatus,7000);"

/* ── Re-run percentage when the user edits calibration bounds live. ── */
"function onCalibInput(){ updateSoilUI(); }"

/* ── Submit handler (save config + reboot). ── */
"function saveCfg(e){"
" e.preventDefault();"
" let btn=document.getElementById('saveBtn');"
" btn.innerText='SAVING & REBOOTING...';btn.style.background='#333';btn.style.pointerEvents='none';"
" let cfg={"
"  ssid:document.getElementById('ssid').value,pass:document.getElementById('pass').value,"
"  tg_tok:document.getElementById('tg_tok').value,tg_chat:document.getElementById('tg_chat').value,"
"  disc_url:document.getElementById('disc_url').value,cust_url:document.getElementById('cust_url').value,"
"  soil_th_pct:parseInt(document.getElementById('soil_th_pct').value)||30,"
"  mqtt_uri:document.getElementById('mqtt_uri').value,"
"  mqtt_port:parseInt(document.getElementById('mqtt_port').value)||1883,"
"  mqtt_user:document.getElementById('mqtt_user').value,"
"  mqtt_pass:document.getElementById('mqtt_pass').value,"
"  mqtt_prefix:document.getElementById('mqtt_prefix').value,"
"  v_dry:parseInt(document.getElementById('v_dry').value),v_wet:parseInt(document.getElementById('v_wet').value)"
" };"
" fetch('/api/save',{method:'POST',body:JSON.stringify(cfg)}).then(()=>{"
"  setTimeout(()=>window.location.reload(),4000);"
" });"
"}"
"</script></head><body onload='fetchStatus()'>"
"<div class='container'>"
"<div class='header'>"
"<h1 class='logo'><span class='d'>d</span><span class='num'>7</span><span class='m'>main</span></h1>"
"<div class='subtitle'>ESP32_EnvironmentalNode</div>"
"</div>"

/* ── Telemetry card ── */
"<div class='card'>"
"<h3>Live Telemetry</h3>"
"<div class='grid'>"
/* Soil moisture: shows % in the main value, progress bar below */
"<div class='metric' style='flex-direction:column;align-items:stretch;'>"
" <div style='display:flex;justify-content:space-between;align-items:center;'>"
"  <span class='m-title'>&#127807; Soil Moisture</span>"
"  <span class='m-val' id='val_soil'>-- %</span>"
" </div>"
" <div class='pct-wrap'><div class='pct-bar' id='pct_bar' style='width:0%'></div></div>"
"</div>"
"<div class='metric'><span class='m-title'>&#127777;&#65039; BMP280 (T/P)</span><span class='m-val' id='val_bmp'>--</span></div>"
"<div class='metric'><span class='m-title'>&#128167; DHT22 (T/H)</span><span class='m-val' id='val_dht'>--</span></div>"
"</div>"
"</div>"

/* ── Config form card ── */
"<div class='card'>"
"<form onsubmit='saveCfg(event)'>"
"<h3>Network Configuration</h3>"
"<div class='input-group'><label>Wi-Fi SSID</label><input type='text' id='ssid' placeholder='IoT_Network' required></div>"
"<div class='input-group'><label>Wi-Fi Password</label><input type='password' id='pass' placeholder='Leave empty if open network'></div>"
"<h3 style='margin-top:30px;'>Notification Settings</h3>"
"<div class='input-group'><label>Telegram Bot Token <small style='font-weight:400;text-transform:none;'>(optional)</small></label><input type='text' id='tg_tok' placeholder='123456:ABC-DEF...'></div>"
"<div class='input-group'><label>Telegram Chat ID <small style='font-weight:400;text-transform:none;'>(optional)</small></label><input type='text' id='tg_chat' placeholder='-100...'></div>"
"<div class='input-group'><label>Discord Webhook URL <small style='font-weight:400;text-transform:none;'>(optional)</small></label><input type='url' id='disc_url' placeholder='https://discord.com/api/webhooks/...'></div>"
"<div class='input-group'><label>Custom Webhook URL <small style='font-weight:400;text-transform:none;'>(optional)</small></label><input type='url' id='cust_url' placeholder='https://your-api.example.com/alert'></div>"
"<h3 style='margin-top:30px;'>Alert Settings</h3>"
"<div class='input-group'><label>Soil Alert Threshold (%)</label><input type='number' id='soil_th_pct' placeholder='30' min='0' max='100'></div>"
"<h3 style='margin-top:30px;'>MQTT Settings</h3>"
"<div class='input-group'><label>Broker Host / IP <small style='font-weight:400;text-transform:none;'>(optional &mdash; leave blank to disable)</small></label>"
" <input type='text' id='mqtt_uri' placeholder='192.168.1.10 &nbsp; or &nbsp; mqtt://host:1883'></div>"
"<div class='row'>"
"<div class='input-group'><label>Port</label><input type='number' id='mqtt_port' placeholder='1883' min='1' max='65535'></div>"
"<div class='input-group'><label>Topic Prefix</label><input type='text' id='mqtt_prefix' placeholder='d7main/sensor'></div>"
"</div>"
"<div class='input-group'><label>Username <small style='font-weight:400;text-transform:none;'>(optional)</small></label>"
" <input type='text' id='mqtt_user' placeholder='homeassistant'></div>"
"<div class='input-group'><label>Password <small style='font-weight:400;text-transform:none;'>(optional)</small></label>"
" <input type='password' id='mqtt_pass' placeholder='(leave blank if not required)'></div>"
"<h3 style='margin-top:30px;'>ADC Calibration"
/* Live mV hint rendered right inside the heading */
" <span class='live-hint' id='live_mv'>live: -- mV</span>"
"</h3>"
"<div class='row'>"
"<div class='input-group'><label>Dry Air (mV)</label>"
" <input type='number' id='v_dry' placeholder='2600' required oninput='onCalibInput()'></div>"
"<div class='input-group'><label>Water (mV)</label>"
" <input type='number' id='v_wet' placeholder='1200' required oninput='onCalibInput()'></div>"
"</div>"
"<button type='submit' class='btn' id='saveBtn'>Write to NVS &amp; Reboot</button>"
"</form>"
"</div>"

"<div class='footer'>"
"&copy; 2026 ESP32_EnvironmentalNode<br>"
"by <b style='color:#000;'>d<span style='color:var(--primary);'>7</span>main</b> (Demian Zaiats)<br>"
"Contact: <a href='mailto:demianzaiats@gmail.com'>demianzaiats@gmail.com</a>"
"</div>"
"</div></body></html>";

/* ==========================================================================
 * HTTP SERVER HANDLERS
 * ========================================================================== */

static esp_err_t http_get_index_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html_page, HTTPD_RESP_USE_STRLEN);
}

/**
 * @brief GET /api/status
 *
 *        Returns the latest sensor readings from the volatile globals.
 *        The globals are updated every PORTAL_SENSOR_POLL_MS ms by
 *        portal_sensor_task().  No lock is taken — 32-bit reads are atomic
 *        on the single-core ESP32-C3 RISC-V core.
 *
 *        JSON format:
 *        {
 *          "soil_mv": <int>,     raw ADC in millivolts (-1 = not yet sampled)
 *          "bmp_t":   <float>,   BMP280 temperature degC
 *          "bmp_p":   <float>,   BMP280 pressure hPa
 *          "dht_t":   <float>,   DHT22 temperature degC
 *          "dht_h":   <float>,   DHT22 humidity %
 *          "valid":   <bool>     false until first poll completes
 *        }
 */
static esp_err_t http_get_status_handler(httpd_req_t *req) {
    /* Read volatile globals directly — lock-free, no blocking. */
    char json_buf[160];
    snprintf(json_buf, sizeof(json_buf),
             "{\"soil_mv\":%ld,\"bmp_t\":%.1f,\"bmp_p\":%.1f"
             ",\"dht_t\":%.1f,\"dht_h\":%.1f,\"valid\":%s}",
             (long)s_moisture_mv,
             (float)s_bmp_temp, (float)s_bmp_press,
             (float)s_dht_temp, (float)s_dht_hum,
             s_data_valid ? "true" : "false");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json_buf, HTTPD_RESP_USE_STRLEN);
}

/* BUG FIX #2 helpers: cJSON_GetObjectItem() returns NULL when a key is
 * missing, has the wrong type, or the browser sends NaN (from parseInt on
 * an empty field, serialised as null by JSON.stringify).  The original code
 * dereferenced the NULL pointer directly, causing an immediate panic and a
 * partial NVS write that corrupted the config and created a boot loop. */
static const char *safe_str(const cJSON *item) {
    if (!item || !cJSON_IsString(item) || !item->valuestring) return "";
    return item->valuestring;
}

static int safe_int(const cJSON *item, int fallback) {
    if (!item || !cJSON_IsNumber(item)) return fallback;
    return item->valueint;
}

static esp_err_t http_post_save_handler(httpd_req_t *req) {
    char buf[1800];  /* Sized for 5 MQTT fields + 2 webhook URLs + JSON overhead */
    int ret, remaining = req->content_len;
    if (remaining >= (int)sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Payload too large");
        return ESP_FAIL;
    }

    if ((ret = httpd_req_recv(req, buf, remaining)) <= 0) {
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    sys_config_t cfg;
    memset(&cfg, 0, sizeof(sys_config_t));

    /* Use safe accessors — no raw pointer dereference on potentially-NULL items. */
    strncpy(cfg.wifi_ssid,           safe_str(cJSON_GetObjectItem(root, "ssid")),         SYS_WIFI_SSID_MAX_LEN   - 1);
    strncpy(cfg.wifi_pass,           safe_str(cJSON_GetObjectItem(root, "pass")),         SYS_WIFI_PASS_MAX_LEN   - 1);
    strncpy(cfg.tg_token,            safe_str(cJSON_GetObjectItem(root, "tg_tok")),       SYS_TG_TOKEN_MAX_LEN    - 1);
    strncpy(cfg.tg_chat_id,          safe_str(cJSON_GetObjectItem(root, "tg_chat")),      SYS_TG_CHAT_MAX_LEN     - 1);
    strncpy(cfg.discord_webhook_url, safe_str(cJSON_GetObjectItem(root, "disc_url")),     SYS_WEBHOOK_URL_MAX_LEN - 1);
    strncpy(cfg.custom_webhook_url,  safe_str(cJSON_GetObjectItem(root, "cust_url")),     SYS_WEBHOOK_URL_MAX_LEN - 1);
    strncpy(cfg.mqtt_broker_uri,     safe_str(cJSON_GetObjectItem(root, "mqtt_uri")),     SYS_MQTT_URI_MAX_LEN    - 1);
    strncpy(cfg.mqtt_username,       safe_str(cJSON_GetObjectItem(root, "mqtt_user")),    SYS_MQTT_USER_MAX_LEN   - 1);
    strncpy(cfg.mqtt_password,       safe_str(cJSON_GetObjectItem(root, "mqtt_pass")),    SYS_MQTT_PASS_MAX_LEN   - 1);
    strncpy(cfg.mqtt_topic_prefix,   safe_str(cJSON_GetObjectItem(root, "mqtt_prefix")),  SYS_MQTT_PREFIX_MAX_LEN - 1);
    cfg.v_dry_mv                 = (int16_t) safe_int(cJSON_GetObjectItem(root, "v_dry"),        0);
    cfg.v_wet_mv                 = (int16_t) safe_int(cJSON_GetObjectItem(root, "v_wet"),        0);
    cfg.soil_alert_threshold_pct = (uint8_t) safe_int(cJSON_GetObjectItem(root, "soil_th_pct"),  30);
    cfg.mqtt_port                = (uint16_t)safe_int(cJSON_GetObjectItem(root, "mqtt_port"),   1883);
    cfg.is_configured = true;

    /* Require at minimum a non-empty SSID; reject the save if it is blank. */
    if (cfg.wifi_ssid[0] == '\0') {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID cannot be empty");
        ESP_LOGE(TAG, "Save rejected: SSID field is empty.");
        return ESP_FAIL;
    }

    sys_nvs_save_config(&cfg);
    cJSON_Delete(root);

    httpd_resp_sendstr(req, "OK");

    ESP_LOGW(TAG, "Configuration saved. Rebooting in 2 seconds...");
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();

    return ESP_OK;
}

/* ==========================================================================
 * WI-FI CONTROL LOGIC
 * ========================================================================== */

static void wifi_event_handler(void* arg, esp_event_base_t base, int32_t id, void* data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
    }
}

void sys_wifi_start_ap_and_server(void) {
    /* IMPORTANT: esp_netif_init() and esp_event_loop_create_default() must
     * be called ONCE by the caller (app_main) before invoking this function.
     * Calling them here a second time would return ESP_ERR_INVALID_STATE and
     * crash via ESP_ERROR_CHECK. */
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = AP_WIFI_SSID,
            .ssid_len = strlen(AP_WIFI_SSID),
            .password = AP_WIFI_PASS,
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA2_PSK
        },
    };
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "AP Started. SSID: %s, PASS: %s", AP_WIFI_SSID, AP_WIFI_PASS);

    /* ── Spawn the portal sensor task (Config Mode ONLY) ─────────────────────
     * This task does NOT exist during the normal measure → deep-sleep path.
     * It is created right here, inside sys_wifi_start_ap_and_server(), so it
     * is guaranteed to be running iff we are serving the web portal. */
    BaseType_t task_ok = xTaskCreate(
        portal_sensor_task,
        "portal_sensor",
        PORTAL_SENSOR_STACK,
        NULL,
        PORTAL_SENSOR_PRIORITY,
        NULL
    );
    if (task_ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create portal_sensor_task! "
                      "/api/status will return zeros until heap is available.");
    } else {
        ESP_LOGI(TAG, "portal_sensor_task spawned (poll every %u ms).",
                 PORTAL_SENSOR_POLL_MS);
    }

    /* ── Start HTTP server and register all URI handlers ─────────────────── */
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t uri_index  = { .uri = "/",           .method = HTTP_GET,  .handler = http_get_index_handler,  .user_ctx = NULL };
        httpd_uri_t uri_status = { .uri = "/api/status", .method = HTTP_GET,  .handler = http_get_status_handler, .user_ctx = NULL };
        httpd_uri_t uri_save   = { .uri = "/api/save",   .method = HTTP_POST, .handler = http_post_save_handler,  .user_ctx = NULL };
        
        httpd_register_uri_handler(server, &uri_index);
        httpd_register_uri_handler(server, &uri_status);
        httpd_register_uri_handler(server, &uri_save);
        ESP_LOGI(TAG, "HTTP Server started. Endpoints: GET /, GET /api/status, POST /api/save");
    }

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

bool sys_wifi_connect_sta(const char *ssid, const char *pass) {
    /* IMPORTANT: esp_netif_init() and esp_event_loop_create_default() must
     * be called ONCE by the caller (app_main) before invoking this function. */
    wifi_event_group = xEventGroupCreate();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    strncpy((char *)wifi_config.sta.password, pass, sizeof(wifi_config.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(8000));
    
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to STA");
        return true;
    }
    ESP_LOGE(TAG, "Failed to connect to STA");
    return false;
}

/* ==========================================================================
 * MULTI-CHANNEL ALERT DELIVERY
 *
 * Three independent static helpers — one per channel — plus the public
 * sys_alerts_send() dispatcher that calls them all.  A failure (or missing
 * credentials) on any one channel never prevents the others from running.
 *
 * All HTTP requests use cJSON for payload serialisation so that any special
 * characters in the message (quotes, newlines, etc.) are safely escaped.
 * Timeout is capped at 6 s per channel to avoid stalling the deep-sleep cycle.
 * ========================================================================== */

/**
 * @brief Send via Telegram Bot API (internal helper, do not call directly).
 */
static bool send_telegram(const char *bot_token, const char *chat_id, const char *message) {
    char url[256];
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendMessage", bot_token);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "chat_id", chat_id);
    cJSON_AddStringToObject(root, "text",    message);
    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!payload) {
        ESP_LOGE(TAG, "[TELEGRAM] cJSON serialisation failed.");
        return false;
    }

    esp_http_client_config_t config = {
        .url                      = url,
        .method                   = HTTP_METHOD_POST,
        .timeout_ms               = 6000,
        .skip_cert_common_name_check = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "[TELEGRAM] esp_http_client_init() failed — dropping payload.");
        free(payload);
        return false;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, payload, strlen(payload));

    esp_err_t err   = esp_http_client_perform(client);
    int       status = esp_http_client_get_status_code(client);
    bool      success = (err == ESP_OK && status == 200);

    if (success) {
        ESP_LOGI(TAG, "[TELEGRAM] Delivered OK (HTTP %d).", status);
    } else {
        ESP_LOGE(TAG, "[TELEGRAM] Failed: %s (HTTP %d).", esp_err_to_name(err), status);
    }

    esp_http_client_cleanup(client);
    free(payload);
    return success;
}

/**
 * @brief POST to a Discord Webhook with payload {"content": message}.
 */
static bool send_discord(const char *webhook_url, const char *message) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "content", message);
    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!payload) {
        ESP_LOGE(TAG, "[DISCORD] cJSON serialisation failed.");
        return false;
    }

    esp_http_client_config_t config = {
        .url                      = webhook_url,
        .method                   = HTTP_METHOD_POST,
        .timeout_ms               = 6000,
        .skip_cert_common_name_check = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "[DISCORD] esp_http_client_init() failed — dropping payload.");
        free(payload);
        return false;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, payload, strlen(payload));

    esp_err_t err    = esp_http_client_perform(client);
    int       status = esp_http_client_get_status_code(client);
    /* Discord returns 204 No Content on success for webhook POSTs */
    bool      success = (err == ESP_OK && status >= 200 && status < 300);

    if (success) {
        ESP_LOGI(TAG, "[DISCORD] Delivered OK (HTTP %d).", status);
    } else {
        ESP_LOGE(TAG, "[DISCORD] Failed: %s (HTTP %d).", esp_err_to_name(err), status);
    }

    esp_http_client_cleanup(client);
    free(payload);
    return success;
}

/**
 * @brief POST to a Custom Webhook with full sensor JSON payload.
 *
 * Payload schema:
 *   {"event":"alert","message":"...","soil_mv":1234,"temp":22.5,"humidity":65.0}
 */
static bool send_custom_webhook(const char *webhook_url, const char *message,
                                int32_t soil_mv, float temp, float humidity) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "event",    "alert");
    cJSON_AddStringToObject(root, "message",  message);
    cJSON_AddNumberToObject(root, "soil_mv",  (double)soil_mv);
    cJSON_AddNumberToObject(root, "temp",     (double)temp);
    cJSON_AddNumberToObject(root, "humidity", (double)humidity);
    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!payload) {
        ESP_LOGE(TAG, "[CUSTOM_WH] cJSON serialisation failed.");
        return false;
    }

    esp_http_client_config_t config = {
        .url                      = webhook_url,
        .method                   = HTTP_METHOD_POST,
        .timeout_ms               = 6000,
        .skip_cert_common_name_check = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "[CUSTOM_WH] esp_http_client_init() failed — dropping payload.");
        free(payload);
        return false;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, payload, strlen(payload));

    esp_err_t err    = esp_http_client_perform(client);
    int       status = esp_http_client_get_status_code(client);
    bool      success = (err == ESP_OK && status >= 200 && status < 300);

    if (success) {
        ESP_LOGI(TAG, "[CUSTOM_WH] Delivered OK (HTTP %d).", status);
    } else {
        ESP_LOGE(TAG, "[CUSTOM_WH] Failed: %s (HTTP %d).", esp_err_to_name(err), status);
    }

    esp_http_client_cleanup(client);
    free(payload);
    return success;
}

/* --------------------------------------------------------------------------
 * Public dispatcher
 * -------------------------------------------------------------------------- */

void sys_alerts_send(const sys_config_t *cfg, const char *message,
                     int32_t soil_mv, float temp, float humidity)
{
    ESP_LOGI(TAG, "[ALERTS] Dispatching: \"%s\"", message);
    bool any_ok = false;

    /* ── Channel 1: Telegram ───────────────────────────────────────────── */
    if (cfg->tg_token[0] != '\0' && cfg->tg_chat_id[0] != '\0') {
        ESP_LOGI(TAG, "[ALERTS] -> Telegram: attempting delivery...");
        if (send_telegram(cfg->tg_token, cfg->tg_chat_id, message)) any_ok = true;
    } else {
        ESP_LOGD(TAG, "[ALERTS] -> Telegram: skipped (credentials not configured).");
    }

    /* ── Channel 2: Discord Webhook ────────────────────────────────────── */
    if (cfg->discord_webhook_url[0] != '\0') {
        ESP_LOGI(TAG, "[ALERTS] -> Discord: attempting delivery...");
        if (send_discord(cfg->discord_webhook_url, message)) any_ok = true;
    } else {
        ESP_LOGD(TAG, "[ALERTS] -> Discord: skipped (webhook URL not configured).");
    }

    /* ── Channel 3: Custom Webhook ─────────────────────────────────────── */
    if (cfg->custom_webhook_url[0] != '\0') {
        ESP_LOGI(TAG, "[ALERTS] -> Custom Webhook: attempting delivery...");
        if (send_custom_webhook(cfg->custom_webhook_url, message,
                                soil_mv, temp, humidity)) any_ok = true;
    } else {
        ESP_LOGD(TAG, "[ALERTS] -> Custom Webhook: skipped (URL not configured).");
    }

    if (!any_ok) {
        ESP_LOGW(TAG, "[ALERTS] No channels delivered the alert. "
                      "Check credentials/URLs and network connectivity.");
    } else {
        ESP_LOGI(TAG, "[ALERTS] Dispatch complete.");
    }
}