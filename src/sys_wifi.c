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

#include "freertos/semphr.h"

static const char *TAG = "SYS_WIFI";
static EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

/* ==========================================================================
 * PORTAL-MODE SENSOR CACHE
 *
 * These globals are written exclusively by the portal_sensor_task (every 20 s)
 * and read by the /api/status HTTP handler.  Access is protected by a spinlock
 * so a partial read can never observe a torn write.
 *
 * The task is created ONLY inside sys_wifi_start_ap_and_server() and therefore
 * only exists during Config Mode.  It is never spawned in the normal
 * measure → deep-sleep path.
 * ========================================================================== */

/** Polling period for the portal sensor task (ms). */
#define PORTAL_SENSOR_POLL_MS   20000U

/** Stack for the portal sensor task (words). */
#define PORTAL_SENSOR_STACK     3072U

/** Priority — above idle, below everything else. */
#define PORTAL_SENSOR_PRIORITY  2U

typedef struct {
    int32_t  soil_mv;      /* Raw ADC reading in millivolts */
    float    bmp_t;        /* BMP280 temperature (°C)       */
    float    bmp_p;        /* BMP280 pressure (hPa)         */
    float    dht_t;        /* DHT22 temperature (°C)        */
    float    dht_h;        /* DHT22 humidity (%)            */
    bool     valid;        /* true once a reading exists    */
} portal_sensor_cache_t;

static portal_sensor_cache_t s_cache = { .valid = false };

/* BUG FIX #3: replaced portMUX_TYPE spinlock with a proper FreeRTOS mutex.
 * The spinlock (portENTER_CRITICAL) disables the entire scheduler for its
 * duration.  When hal_dht22_read() runs its own portENTER_CRITICAL section
 * (~5 ms of blocked interrupts) inside the sensor task, and the HTTPD task
 * simultaneously tries to read the cache, the combined critical section time
 * exceeds the Task Watchdog Timer threshold and causes a watchdog reset.
 * A mutex lets the HTTPD task block (yield CPU) instead of spin-waiting. */
static SemaphoreHandle_t s_cache_mutex = NULL;

/**
 * @brief FreeRTOS task: poll all sensors every PORTAL_SENSOR_POLL_MS ms.
 *        Exists ONLY during Config Mode (AP + Web Server).
 *        hal_moisture_read_mv() powers the sensor on/off internally.
 */
static void portal_sensor_task(void *arg) {
    ESP_LOGI(TAG, "[SENSOR_TASK] Portal sensor polling started "
                  "(period = %u ms).", PORTAL_SENSOR_POLL_MS);

    /* Create the cache mutex on first run (task is only ever created once). */
    if (s_cache_mutex == NULL) {
        s_cache_mutex = xSemaphoreCreateMutex();
        if (s_cache_mutex == NULL) {
            ESP_LOGE(TAG, "[SENSOR_TASK] Failed to create cache mutex! Aborting task.");
            vTaskDelete(NULL);
            return;
        }
    }

    for (;;) {
        /* -- Soil moisture (HAL handles SENSOR_POWER_PIN gate internally) -- */
        int32_t soil_mv = hal_moisture_read_mv();

        /* -- BMP280 -- */
        bmp280_data_t bmp = {0};
        hal_bmp280_read(&bmp);

        /* -- DHT22 -- */
        dht22_data_t dht = {0};
        hal_dht22_read(&dht);

        /* -- Commit to cache under mutex (not spinlock — see BUG FIX #3) -- */
        if (xSemaphoreTake(s_cache_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            s_cache.soil_mv = soil_mv;
            s_cache.bmp_t   = bmp.temperature;
            s_cache.bmp_p   = bmp.pressure;
            s_cache.dht_t   = dht.temperature;
            s_cache.dht_h   = dht.humidity;
            s_cache.valid   = true;
            xSemaphoreGive(s_cache_mutex);
        } else {
            ESP_LOGW(TAG, "[SENSOR_TASK] Could not acquire cache mutex — skipping update.");
        }

        ESP_LOGI(TAG, "[SENSOR_TASK] Cache updated — soil: %ld mV | "
                      "BMP: %.1f \xc2\xb0C / %.1f hPa | DHT: %.1f \xc2\xb0C / %.1f %%",
                 (long)soil_mv, bmp.temperature, bmp.pressure,
                 dht.temperature, dht.humidity);

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
"<h3 style='margin-top:30px;'>Telegram Integrations</h3>"
"<div class='input-group'><label>Bot Token</label><input type='text' id='tg_tok' placeholder='123456:ABC-DEF...' required></div>"
"<div class='input-group'><label>Chat ID</label><input type='text' id='tg_chat' placeholder='-100...' required></div>"
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
 *        Returns the latest sensor readings from the portal sensor cache.
 *        The cache is populated every PORTAL_SENSOR_POLL_MS ms by
 *        portal_sensor_task().  If no reading exists yet (first boot,
 *        task hasn't fired), all numeric values will be 0 and "valid"
 *        will be false.
 *
 *        JSON format:
 *        {
 *          "soil_mv": <int>,     raw ADC in millivolts
 *          "bmp_t":   <float>,   BMP280 temperature °C
 *          "bmp_p":   <float>,   BMP280 pressure hPa
 *          "dht_t":   <float>,   DHT22 temperature °C
 *          "dht_h":   <float>,   DHT22 humidity %
 *          "valid":   <bool>     false until first poll completes
 *        }
 */
static esp_err_t http_get_status_handler(httpd_req_t *req) {
    /* Snapshot the cache under mutex — the handler runs in the HTTPD task. */
    portal_sensor_cache_t snap = { .valid = false };

    if (s_cache_mutex != NULL &&
        xSemaphoreTake(s_cache_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        snap = s_cache;
        xSemaphoreGive(s_cache_mutex);
    } else {
        ESP_LOGW(TAG, "/api/status: could not acquire cache mutex, returning stale data.");
    }

    char json_buf[160];
    snprintf(json_buf, sizeof(json_buf),
             "{\"soil_mv\":%ld,\"bmp_t\":%.1f,\"bmp_p\":%.1f"
             ",\"dht_t\":%.1f,\"dht_h\":%.1f,\"valid\":%s}",
             (long)snap.soil_mv,
             snap.bmp_t, snap.bmp_p,
             snap.dht_t, snap.dht_h,
             snap.valid ? "true" : "false");

    httpd_resp_set_type(req, "application/json");
    /* Prevent browser caching — we always want fresh data. */
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
    char buf[512];
    int ret, remaining = req->content_len;
    if (remaining >= sizeof(buf)) {
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
    strncpy(cfg.wifi_ssid,  safe_str(cJSON_GetObjectItem(root, "ssid")),    SYS_WIFI_SSID_MAX_LEN - 1);
    strncpy(cfg.wifi_pass,  safe_str(cJSON_GetObjectItem(root, "pass")),    SYS_WIFI_PASS_MAX_LEN - 1);
    strncpy(cfg.tg_token,   safe_str(cJSON_GetObjectItem(root, "tg_tok")),  SYS_TG_TOKEN_MAX_LEN  - 1);
    strncpy(cfg.tg_chat_id, safe_str(cJSON_GetObjectItem(root, "tg_chat")), SYS_TG_CHAT_MAX_LEN   - 1);
    cfg.v_dry_mv = (int16_t)safe_int(cJSON_GetObjectItem(root, "v_dry"), 0);
    cfg.v_wet_mv = (int16_t)safe_int(cJSON_GetObjectItem(root, "v_wet"), 0);
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

bool sys_wifi_send_telegram(const char *bot_token, const char *chat_id, const char *message) {
    char url[256];
    char payload[256];
    
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendMessage", bot_token);
    snprintf(payload, sizeof(payload), "{\"chat_id\":\"%s\",\"text\":\"%s\"}", chat_id, message);

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 5000,
        .skip_cert_common_name_check = true 
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, payload, strlen(payload));

    esp_err_t err = esp_http_client_perform(client);
    bool success = (err == ESP_OK && esp_http_client_get_status_code(client) == 200);
    
    esp_http_client_cleanup(client);
    return success;
}