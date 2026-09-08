#include "maintenance_portal.h"

#include "app_version.h"
#include "board_config.h"

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BUTTON_POLL_MS          20
#define BUTTON_DEBOUNCE_MS      50
#define BUTTON_LONG_PRESS_MS    3000
#define OTA_RECEIVE_BUFFER_SIZE 4096

static const char *TAG = "MAINTENANCE";
static httpd_handle_t s_http_server;
static esp_netif_t *s_sta_netif;
static char s_ap_ssid[33];
static char s_device_mac[18];
static bool s_ap_started;
static bool s_sta_configured;
static bool s_maintenance_handlers_registered;

static const char INDEX_PAGE[] =
    "<!doctype html><html lang='zh-CN'><head>"
    "<meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>UART Bridge Maintenance</title><style>"
    "body{margin:0;background:#eef2f7;color:#172033;font-family:system-ui,sans-serif}"
    ".card{max-width:560px;margin:7vh auto;padding:28px;background:#fff;border-radius:16px;"
    "box-shadow:0 12px 35px #18315322}h1{margin-top:0;font-size:24px}"
    ".info{display:grid;grid-template-columns:1fr 1fr;gap:12px;margin:22px 0}"
    ".item{padding:14px;background:#f5f7fb;border-radius:10px}.item small{color:#697386}"
    ".item b{display:block;margin-top:5px}input{width:100%%;box-sizing:border-box;padding:12px;"
    "border:1px solid #ccd4e0;border-radius:8px}button{width:100%%;margin-top:12px;padding:12px;"
    "border:0;border-radius:8px;background:#1769e0;color:#fff;font-weight:700}"
    "button:disabled{opacity:.5}.state{min-height:24px;margin:10px 0;color:#46536a}"
    "</style></head><body><main class='card'><h1>设备维护</h1>"
    "<div class='info'><div class='item'><small>软件版本</small><b>%s</b></div>"
    "<div class='item'><small>硬件版本</small><b>%s</b></div>"
    "<div class='item'><small>SKU</small><b>%s</b></div>"
    "<div class='item'><small>AP 热点</small><b>%s</b></div></div>"
    "<h2>连接路由器 Wi-Fi</h2><div id='wifiState' class='state'>正在读取状态…</div>"
    "<input id='ssid' maxlength='32' placeholder='Wi-Fi 名称（SSID）'>"
    "<input id='password' maxlength='63' type='password' placeholder='Wi-Fi 密码'>"
    "<button id='connect' onclick='connectWifi()'>保存并连接</button>"
    "<h2>OTA 固件升级</h2><p>请选择为本设备编译的 .bin 应用固件。升级完成后设备会自动重启。</p>"
    "<input id='fw' type='file' accept='.bin,application/octet-stream'>"
    "<button id='upload' onclick='upgrade()'>上传并升级</button><div id='status' class='state'></div>"
    "<script>async function wifiStatus(){try{const r=await fetch('/wifi/status'),j=await r.json();"
    "document.getElementById('wifiState').textContent=j.connected?"
    "'已连接：'+j.ssid+'，IP：'+j.ip:(j.configured?'正在连接或连接失败':'尚未配置');}"
    "catch(e){document.getElementById('wifiState').textContent='无法读取状态'}}"
    "async function connectWifi(){const b=document.getElementById('connect'),"
    "s=document.getElementById('wifiState'),ssid=document.getElementById('ssid').value,"
    "password=document.getElementById('password').value;if(!ssid){s.textContent='请输入 Wi-Fi 名称';return}"
    "b.disabled=true;s.textContent='正在保存并连接…';try{const body=new URLSearchParams({ssid,password});"
    "const r=await fetch('/wifi/connect',{method:'POST',headers:{'Content-Type':"
    "'application/x-www-form-urlencoded'},body});const t=await r.text();if(!r.ok)throw new Error(t);"
    "s.textContent=t;setTimeout(wifiStatus,3000)}catch(e){s.textContent='连接失败：'+e.message}"
    "finally{b.disabled=false}}"
    "async function upgrade(){const f=document.getElementById('fw').files[0],"
    "b=document.getElementById('upload'),s=document.getElementById('status');"
    "if(!f){s.textContent='请先选择固件文件';return}"
    "if(!confirm('确定升级固件？升级过程中请勿断电。'))return;"
    "b.disabled=true;s.textContent='正在上传，请勿断电…';try{const r=await fetch('/ota',"
    "{method:'POST',headers:{'Content-Type':'application/octet-stream'},body:f});"
    "const t=await r.text();if(!r.ok)throw new Error(t);s.textContent=t}"
    "catch(e){s.textContent='升级失败：'+e.message;b.disabled=false}}"
    "wifiStatus();setInterval(wifiStatus,5000);</script>"
    "</main></body></html>";

static esp_err_t index_handler(httpd_req_t *request)
{
    char *page = malloc(sizeof(INDEX_PAGE) + 160);
    if (page == NULL) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Insufficient memory");
    }

    int length = snprintf(page, sizeof(INDEX_PAGE) + 160, INDEX_PAGE,
                          APP_SOFTWARE_VERSION, APP_HARDWARE_VERSION,
                          APP_SKU, s_ap_ssid);
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    esp_err_t err = httpd_resp_send(request, page, length);
    free(page);
    return err;
}

static int hex_value(char character)
{
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }
    return -1;
}

static bool form_value(const char *body, const char *key,
                       char *output, size_t output_size)
{
    size_t key_length = strlen(key);
    const char *field = body;

    while (*field != '\0') {
        const char *field_end = strchr(field, '&');
        if (field_end == NULL) {
            field_end = field + strlen(field);
        }
        if ((size_t)(field_end - field) > key_length &&
            strncmp(field, key, key_length) == 0 && field[key_length] == '=') {
            const char *source = field + key_length + 1;
            size_t written = 0;
            while (source < field_end && written + 1 < output_size) {
                if (*source == '+') {
                    output[written++] = ' ';
                    source++;
                } else if (*source == '%' && source + 2 < field_end) {
                    int high = hex_value(source[1]);
                    int low = hex_value(source[2]);
                    if (high < 0 || low < 0) {
                        return false;
                    }
                    output[written++] = (char)((high << 4) | low);
                    source += 3;
                } else {
                    output[written++] = *source++;
                }
            }
            if (source != field_end) {
                return false;
            }
            output[written] = '\0';
            return true;
        }
        field = *field_end == '&' ? field_end + 1 : field_end;
    }
    return false;
}

static void json_escape_ssid(const uint8_t *ssid, char *output,
                             size_t output_size)
{
    size_t written = 0;
    for (size_t index = 0; ssid[index] != '\0' && written + 1 < output_size;
         ++index) {
        if ((ssid[index] == '"' || ssid[index] == '\\') &&
            written + 2 < output_size) {
            output[written++] = '\\';
            output[written++] = (char)ssid[index];
        } else if (ssid[index] >= 0x20) {
            output[written++] = (char)ssid[index];
        }
    }
    output[written] = '\0';
}

static esp_err_t wifi_status_handler(httpd_req_t *request)
{
    wifi_ap_record_t record = {0};
    esp_netif_ip_info_t ip_info = {0};
    bool connected = esp_wifi_sta_get_ap_info(&record) == ESP_OK;
    if (connected) {
        (void)esp_netif_get_ip_info(s_sta_netif, &ip_info);
    }

    char escaped_ssid[65];
    json_escape_ssid(record.ssid, escaped_ssid, sizeof(escaped_ssid));
    char response[192];
    int length = snprintf(response, sizeof(response),
                          "{\"configured\":%s,\"connected\":%s,"
                          "\"ssid\":\"%s\",\"ip\":\"" IPSTR "\"}",
                          s_sta_configured ? "true" : "false",
                          connected ? "true" : "false", escaped_ssid,
                          IP2STR(&ip_info.ip));
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_send(request, response, length);
}

static esp_err_t wifi_connect_handler(httpd_req_t *request)
{
    if (request->content_len <= 0 || request->content_len > 255) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                                   "Invalid Wi-Fi settings");
    }

    char body[256];
    int received_total = 0;
    while (received_total < request->content_len) {
        int received = httpd_req_recv(request, body + received_total,
                                      request->content_len - received_total);
        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (received <= 0) {
            return ESP_FAIL;
        }
        received_total += received;
    }
    body[received_total] = '\0';

    char ssid[33];
    char password[64];
    if (!form_value(body, "ssid", ssid, sizeof(ssid)) || ssid[0] == '\0' ||
        !form_value(body, "password", password, sizeof(password))) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                                   "Invalid SSID or password");
    }

    wifi_config_t station_config = {0};
    strlcpy((char *)station_config.sta.ssid, ssid,
            sizeof(station_config.sta.ssid));
    strlcpy((char *)station_config.sta.password, password,
            sizeof(station_config.sta.password));
    station_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    station_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &station_config);
    if (err != ESP_OK) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Unable to save Wi-Fi settings");
    }
    s_sta_configured = true;
    (void)esp_wifi_disconnect();
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi connection start failed: %s",
                 esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "Saved station Wi-Fi SSID: %s", ssid);
    httpd_resp_set_type(request, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(request, "配置已保存，正在连接路由器。");
}

static void restart_task(void *argument)
{
    (void)argument;
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

static esp_err_t ota_error(httpd_req_t *request, esp_ota_handle_t handle,
                           char *buffer, const char *message)
{
    if (handle != 0) {
        (void)esp_ota_abort(handle);
    }
    free(buffer);
    ESP_LOGE(TAG, "OTA failed: %s", message);
    return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                               message);
}

static bool ota_password_valid(httpd_req_t *request)
{
    const size_t expected_length = strlen(BOARD_OTA_PASSWORD);
    const size_t actual_length =
        httpd_req_get_hdr_value_len(request, "X-OTA-Password");
    if (actual_length != expected_length || actual_length == 0) {
        return false;
    }

    char password[sizeof(BOARD_OTA_PASSWORD)];
    if (httpd_req_get_hdr_value_str(request, "X-OTA-Password", password,
                                    sizeof(password)) != ESP_OK) {
        return false;
    }
    return strcmp(password, BOARD_OTA_PASSWORD) == 0;
}

static esp_err_t api_ota_error(httpd_req_t *request, const char *status,
                               const char *message)
{
    char response[160];
    int length = snprintf(response, sizeof(response),
                          "{\"error\":\"%s\"}", message);
    httpd_resp_set_status(request, status);
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_send(request, response, length);
}

static esp_err_t ota_handler(httpd_req_t *request)
{
    const bool is_api_request = strcmp(request->uri, "/api/ota") == 0;
    if (is_api_request && !ota_password_valid(request)) {
        return api_ota_error(request, "401 Unauthorized",
                             "OTA password is incorrect");
    }

    const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
    if (partition == NULL) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "No OTA partition available");
    }
    if (request->content_len <= 0 ||
        request->content_len > (int)partition->size) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                                   "Invalid firmware size");
    }

    char *buffer = malloc(OTA_RECEIVE_BUFFER_SIZE);
    if (buffer == NULL) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Insufficient memory");
    }

    esp_ota_handle_t ota_handle = 0;
    esp_err_t err = esp_ota_begin(partition, request->content_len, &ota_handle);
    if (err != ESP_OK) {
        free(buffer);
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Unable to begin OTA");
    }

    ESP_LOGI(TAG, "OTA upload started: %d bytes -> %s",
             request->content_len, partition->label);
    int remaining = request->content_len;
    while (remaining > 0) {
        int chunk_size = remaining < OTA_RECEIVE_BUFFER_SIZE
                             ? remaining
                             : OTA_RECEIVE_BUFFER_SIZE;
        int received = httpd_req_recv(request, buffer, chunk_size);
        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (received <= 0) {
            return ota_error(request, ota_handle, buffer,
                             "Firmware upload interrupted");
        }
        err = esp_ota_write(ota_handle, buffer, received);
        if (err != ESP_OK) {
            return ota_error(request, ota_handle, buffer,
                             "Unable to write firmware");
        }
        remaining -= received;
    }
    free(buffer);

    err = esp_ota_end(ota_handle);
    ota_handle = 0;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA image validation failed: %s", esp_err_to_name(err));
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                                   "Firmware image validation failed");
    }
    err = esp_ota_set_boot_partition(partition);
    if (err != ESP_OK) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Unable to select new firmware");
    }

    ESP_LOGI(TAG, "OTA successful; restarting");
    if (is_api_request) {
        httpd_resp_set_type(request, "application/json");
        err = httpd_resp_sendstr(
            request, "{\"ok\":true,\"rebooting\":true}");
    } else {
        httpd_resp_set_type(request, "text/plain; charset=utf-8");
        err = httpd_resp_sendstr(
            request, "升级成功，设备即将重启。请稍候重新连接。");
    }
    if (xTaskCreate(restart_task, "ota_restart", 2048, NULL, 5, NULL) != pdPASS) {
        ESP_LOGW(TAG, "Restart task creation failed; restarting immediately");
        esp_restart();
    }
    return err;
}

static esp_err_t api_status_handler(httpd_req_t *request)
{
    char response[256];
    int length = snprintf(response, sizeof(response),
                          "{\"mac\":\"%s\",\"sku\":\"%s\","
                          "\"software_version\":\"%s\","
                          "\"hardware_version\":\"%s\","
                          "\"relay_count\":0,\"input_count\":0,"
                          "\"relays\":0,\"inputs\":0}",
                          s_device_mac, APP_SKU, APP_SOFTWARE_VERSION,
                          APP_HARDWARE_VERSION);
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, response, length);
}

static esp_err_t start_status_http_server(void)
{
    if (s_http_server != NULL) {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 6144;

    esp_err_t err = httpd_start(&s_http_server, &config);
    if (err != ESP_OK) {
        return err;
    }

    const httpd_uri_t status_uri = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = api_status_handler,
    };
    err = httpd_register_uri_handler(s_http_server, &status_uri);
    if (err != ESP_OK) {
        (void)httpd_stop(s_http_server);
        s_http_server = NULL;
    }
    return err;
}

static esp_err_t register_maintenance_handlers(void)
{
    if (s_maintenance_handlers_registered) {
        return ESP_OK;
    }

    const httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = index_handler,
    };
    const httpd_uri_t ota_uri = {
        .uri = "/ota",
        .method = HTTP_POST,
        .handler = ota_handler,
    };
    const httpd_uri_t api_ota_uri = {
        .uri = "/api/ota",
        .method = HTTP_POST,
        .handler = ota_handler,
    };
    const httpd_uri_t wifi_status_uri = {
        .uri = "/wifi/status",
        .method = HTTP_GET,
        .handler = wifi_status_handler,
    };
    const httpd_uri_t wifi_connect_uri = {
        .uri = "/wifi/connect",
        .method = HTTP_POST,
        .handler = wifi_connect_handler,
    };
    esp_err_t err = httpd_register_uri_handler(s_http_server, &index_uri);
    if (err == ESP_OK) {
        err = httpd_register_uri_handler(s_http_server, &ota_uri);
    }
    if (err == ESP_OK) {
        err = httpd_register_uri_handler(s_http_server, &api_ota_uri);
    }
    if (err == ESP_OK) {
        err = httpd_register_uri_handler(s_http_server, &wifi_status_uri);
    }
    if (err == ESP_OK) {
        err = httpd_register_uri_handler(s_http_server, &wifi_connect_uri);
    }
    if (err != ESP_OK) {
        (void)httpd_unregister_uri_handler(s_http_server, "/", HTTP_GET);
        (void)httpd_unregister_uri_handler(s_http_server, "/ota", HTTP_POST);
        (void)httpd_unregister_uri_handler(s_http_server, "/api/ota",
                                           HTTP_POST);
        (void)httpd_unregister_uri_handler(s_http_server, "/wifi/status",
                                           HTTP_GET);
        (void)httpd_unregister_uri_handler(s_http_server, "/wifi/connect",
                                           HTTP_POST);
        return err;
    }
    s_maintenance_handlers_registered = true;
    return ESP_OK;
}

static void unregister_maintenance_handlers(void)
{
    if (!s_maintenance_handlers_registered) {
        return;
    }

    (void)httpd_unregister_uri_handler(s_http_server, "/", HTTP_GET);
    (void)httpd_unregister_uri_handler(s_http_server, "/ota", HTTP_POST);
    (void)httpd_unregister_uri_handler(s_http_server, "/api/ota", HTTP_POST);
    (void)httpd_unregister_uri_handler(s_http_server, "/wifi/status", HTTP_GET);
    (void)httpd_unregister_uri_handler(s_http_server, "/wifi/connect", HTTP_POST);
    s_maintenance_handlers_registered = false;
}

static void wifi_event_handler(void *argument, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)argument;
    (void)event_data;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START &&
        s_sta_configured) {
        (void)esp_wifi_connect();
    } else if (event_base == WIFI_EVENT &&
               event_id == WIFI_EVENT_STA_DISCONNECTED &&
               s_sta_configured) {
        (void)esp_wifi_connect();
    } else if (event_base == IP_EVENT &&
               event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = event_data;
        ESP_LOGI(TAG, "Router Wi-Fi connected, IP=" IPSTR,
                 IP2STR(&event->ip_info.ip));
    }
}

static esp_err_t initialize_wifi(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "NVS erase failed");
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "NVS initialization failed");
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "Network stack init failed");

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (s_sta_netif == NULL || esp_netif_create_default_wifi_ap() == NULL) {
        return ESP_ERR_NO_MEM;
    }

    wifi_init_config_t wifi_init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&wifi_init), TAG, "Wi-Fi init failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_FLASH), TAG,
                        "Wi-Fi storage selection failed");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                   wifi_event_handler, NULL),
        TAG, "Wi-Fi event handler registration failed");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                   wifi_event_handler, NULL),
        TAG, "IP event handler registration failed");

    wifi_config_t station_config = {0};
    ESP_RETURN_ON_ERROR(esp_wifi_get_config(WIFI_IF_STA, &station_config), TAG,
                        "Unable to load Wi-Fi settings");
    s_sta_configured = station_config.sta.ssid[0] != '\0';

    uint8_t station_mac[6];
    ESP_RETURN_ON_ERROR(esp_read_mac(station_mac, ESP_MAC_WIFI_STA), TAG,
                        "Unable to read station Wi-Fi MAC");
    snprintf(s_device_mac, sizeof(s_device_mac),
             "%02X-%02X-%02X-%02X-%02X-%02X", station_mac[0],
             station_mac[1], station_mac[2], station_mac[3], station_mac[4],
             station_mac[5]);

    uint8_t ap_mac[6];
    ESP_RETURN_ON_ERROR(esp_read_mac(ap_mac, ESP_MAC_WIFI_SOFTAP), TAG,
                        "Unable to read AP Wi-Fi MAC");
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%s-%02X%02X",
             BOARD_AP_SSID_PREFIX, ap_mac[4], ap_mac[5]);

    /* Reboot restores only station mode; AP mode requires a button press. */
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG,
                        "Wi-Fi station mode selection failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "Wi-Fi start failed");
    return ESP_OK;
}

static esp_err_t start_access_point(void)
{
    if (s_ap_started) {
        return ESP_OK;
    }

    wifi_config_t ap_config = {0};
    strlcpy((char *)ap_config.ap.ssid, s_ap_ssid,
            sizeof(ap_config.ap.ssid));
    strlcpy((char *)ap_config.ap.password, BOARD_AP_PASSWORD,
            sizeof(ap_config.ap.password));
    ap_config.ap.ssid_len = strlen(s_ap_ssid);
    ap_config.ap.channel = 1;
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG,
                        "Wi-Fi mode selection failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap_config), TAG,
                        "AP configuration failed");
    ESP_RETURN_ON_ERROR(start_status_http_server(), TAG,
                        "Status HTTP server start failed");
    ESP_RETURN_ON_ERROR(register_maintenance_handlers(), TAG,
                        "Maintenance HTTP handlers failed");
    s_ap_started = true;

    ESP_LOGI(TAG, "AP ready: SSID=%s password=%s URL=http://192.168.4.1",
             s_ap_ssid, BOARD_AP_PASSWORD);
    return ESP_OK;
}

static esp_err_t stop_access_point(void)
{
    if (!s_ap_started) {
        return ESP_OK;
    }

    unregister_maintenance_handlers();
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        /* Restore the maintenance endpoints if AP mode could not be stopped. */
        (void)register_maintenance_handlers();
        return err;
    }

    s_ap_started = false;
    ESP_LOGI(TAG, "Maintenance AP stopped; station mode remains active");
    return ESP_OK;
}

static void button_task(void *argument)
{
    (void)argument;

    bool raw_pressed =
        gpio_get_level(BOARD_AP_BUTTON_GPIO) ==
        BOARD_AP_BUTTON_ACTIVE_LEVEL;
    bool stable_pressed = raw_pressed;
    /* A button held or read low during reboot must not start the AP. */
    bool button_armed = !stable_pressed;
    bool long_press_handled = false;
    TickType_t raw_changed_at = xTaskGetTickCount();
    TickType_t pressed_at = 0;
    const TickType_t debounce_ticks = pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS);
    const TickType_t long_press_ticks = pdMS_TO_TICKS(BUTTON_LONG_PRESS_MS);

    while (true) {
        TickType_t now = xTaskGetTickCount();
        bool current_pressed =
            gpio_get_level(BOARD_AP_BUTTON_GPIO) ==
            BOARD_AP_BUTTON_ACTIVE_LEVEL;

        if (current_pressed != raw_pressed) {
            raw_pressed = current_pressed;
            raw_changed_at = now;
        } else if (stable_pressed != raw_pressed &&
                   (now - raw_changed_at) >= debounce_ticks) {
            stable_pressed = raw_pressed;
            if (stable_pressed) {
                if (button_armed) {
                    pressed_at = now;
                    long_press_handled = false;
                }
            } else {
                /* Only a confirmed release arms the next long press. */
                button_armed = true;
                long_press_handled = false;
            }
        }

        if (button_armed && stable_pressed && !long_press_handled &&
            (now - pressed_at) >= long_press_ticks) {
            long_press_handled = true;
            bool stop_requested = s_ap_started;
            ESP_LOGI(TAG, "U4 long press detected; %s maintenance AP",
                     stop_requested ? "stopping" : "starting");
            esp_err_t err = stop_requested ? stop_access_point()
                                           : start_access_point();
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Maintenance AP %s failed: %s",
                         stop_requested ? "stop" : "start",
                         esp_err_to_name(err));
            }
        }

        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
    }
}

esp_err_t maintenance_portal_init(void)
{
    const gpio_config_t button_config = {
        .pin_bit_mask = 1ULL << BOARD_AP_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        /* GPIO34 has no internal pull resistor; U4 provides an external one. */
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&button_config), TAG,
                        "U4 GPIO configuration failed");
    ESP_RETURN_ON_ERROR(initialize_wifi(), TAG, "Wi-Fi initialization failed");
    ESP_RETURN_ON_ERROR(start_status_http_server(), TAG,
                        "Status HTTP server start failed");
    ESP_LOGI(TAG, "Wi-Fi status API ready: /api/status, MAC=%s", s_device_mac);

    if (xTaskCreate(button_task, "ap_button", 4096, NULL, 3, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
