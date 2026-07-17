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
"function fetchLive(){"
" fetch('/api/live').then(r=>r.json()).then(d=>{"
"  document.getElementById('val_soil').innerText = d.soil_mv + ' mV';"
"  document.getElementById('val_bmp').innerText = d.bmp_t + '°C | ' + d.bmp_p + ' hPa';"
"  document.getElementById('val_dht').innerText = d.dht_t + '°C | ' + d.dht_h + '%';"
" }).catch(e=>console.log(e));"
"}"
"setInterval(fetchLive, 2000);"
"function saveCfg(e){"
" e.preventDefault();"
" let btn = document.getElementById('saveBtn');"
" btn.innerText = 'SAVING & REBOOTING...'; btn.style.background = '#333'; btn.style.pointerEvents = 'none';"
" let cfg = {"
"  ssid: document.getElementById('ssid').value, pass: document.getElementById('pass').value,"
"  tg_tok: document.getElementById('tg_tok').value, tg_chat: document.getElementById('tg_chat').value,"
"  v_dry: parseInt(document.getElementById('v_dry').value), v_wet: parseInt(document.getElementById('v_wet').value)"
" };"
" fetch('/api/save',{method:'POST',body:JSON.stringify(cfg)}).then(()=>{"
"  setTimeout(() => window.location.reload(), 4000);"
" });"
"}"
"</script></head><body onload='fetchLive()'>"
"<div class='container'>"
"<div class='header'>"
"<h1 class='logo'><span class='d'>d</span><span class='num'>7</span><span class='m'>main</span></h1>"
"<div class='subtitle'>ESP32_EnvironmentalNode</div>"
"</div>"
"<div class='card'>"
"<h3>Live Telemetry</h3>"
"<div class='grid'>"
"<div class='metric'><span class='m-title'>🌱 Soil Moisture</span><span class='m-val' id='val_soil'>-- mV</span></div>"
"<div class='metric'><span class='m-title'>🌡️ BMP280 (T/P)</span><span class='m-val' id='val_bmp'>--</span></div>"
"<div class='metric'><span class='m-title'>💧 DHT22 (T/H)</span><span class='m-val' id='val_dht'>--</span></div>"
"</div>"
"</div>"
"<div class='card'>"
"<form onsubmit='saveCfg(event)'>"
"<h3>Network Configuration</h3>"
"<div class='input-group'><label>Wi-Fi SSID</label><input type='text' id='ssid' placeholder='IoT_Network' required></div>"
"<div class='input-group'><label>Wi-Fi Password</label><input type='password' id='pass' placeholder='Leave empty if open network'></div>"
"<h3 style='margin-top:30px;'>Telegram Integrations</h3>"
"<div class='input-group'><label>Bot Token</label><input type='text' id='tg_tok' placeholder='123456:ABC-DEF...' required></div>"
"<div class='input-group'><label>Chat ID</label><input type='text' id='tg_chat' placeholder='-100...' required></div>"
"<h3 style='margin-top:30px;'>ADC Calibration</h3>"
"<div class='row'>"
"<div class='input-group'><label>Dry Air (mV)</label><input type='number' id='v_dry' placeholder='2600' required></div>"
"<div class='input-group'><label>Water (mV)</label><input type='number' id='v_wet' placeholder='1200' required></div>"
"</div>"
"<button type='submit' class='btn' id='saveBtn'>Write to NVS & Reboot</button>"
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

static esp_err_t http_get_live_handler(httpd_req_t *req) {
    int32_t soil_mv = hal_moisture_read_mv();
    
    bmp280_data_t bmp = {0};
    hal_bmp280_read(&bmp); 

    dht22_data_t dht = {0};
    hal_dht22_read(&dht);

    char json_buf[128];
    snprintf(json_buf, sizeof(json_buf), 
             "{\"soil_mv\":%ld,\"bmp_t\":%.1f,\"bmp_p\":%.1f,\"dht_t\":%.1f,\"dht_h\":%.1f}",
             soil_mv, bmp.temperature, bmp.pressure, dht.temperature, dht.humidity);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json_buf, HTTPD_RESP_USE_STRLEN);
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
    
    strncpy(cfg.wifi_ssid, cJSON_GetObjectItem(root, "ssid")->valuestring, SYS_WIFI_SSID_MAX_LEN - 1);
    strncpy(cfg.wifi_pass, cJSON_GetObjectItem(root, "pass")->valuestring, SYS_WIFI_PASS_MAX_LEN - 1);
    strncpy(cfg.tg_token, cJSON_GetObjectItem(root, "tg_tok")->valuestring, SYS_TG_TOKEN_MAX_LEN - 1);
    strncpy(cfg.tg_chat_id, cJSON_GetObjectItem(root, "tg_chat")->valuestring, SYS_TG_CHAT_MAX_LEN - 1);
    cfg.v_dry_mv = (int16_t)cJSON_GetObjectItem(root, "v_dry")->valueint;
    cfg.v_wet_mv = (int16_t)cJSON_GetObjectItem(root, "v_wet")->valueint;
    cfg.is_configured = true;

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

    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t uri_index = { .uri = "/", .method = HTTP_GET, .handler = http_get_index_handler, .user_ctx = NULL };
        httpd_uri_t uri_live  = { .uri = "/api/live", .method = HTTP_GET, .handler = http_get_live_handler, .user_ctx = NULL };
        httpd_uri_t uri_save  = { .uri = "/api/save", .method = HTTP_POST, .handler = http_post_save_handler, .user_ctx = NULL };
        
        httpd_register_uri_handler(server, &uri_index);
        httpd_register_uri_handler(server, &uri_live);
        httpd_register_uri_handler(server, &uri_save);
        ESP_LOGI(TAG, "HTTP Server started on port 80");
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