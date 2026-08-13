/**
 * @file web_config.c
 *
 * Embedded HTTP configuration server (esp_http_server on port 80).
 * Serves a single-page HTML form for advanced settings (HA token, URLs,
 * entity IDs). On POST /save, writes values to NVS and restarts.
 *
 * Design: no filesystem, no dynamic allocation beyond request parsing.
 * HTML is generated via snprintf into a stack buffer (~8KB).
 */

#include "web_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"

#include "config_store.h"

static const char *TAG = "webcfg";

static httpd_handle_t s_server = NULL;

/*-----------------------------
 * HTML helpers
 *----------------------------*/

/** Append a form field row to the buffer. Secret fields show mask text. */
static void append_field(char *buf, size_t buf_size, size_t *off,
                         const char *key, const char *label, bool secret)
{
    char value[512] = "";
    esp_err_t err = config_get_str(key, value, sizeof(value));
    bool has_value = (err == ESP_OK && value[0] != '\0');

    if(secret && has_value) {
        *off += snprintf(buf + *off, buf_size - *off,
            "<div class=\"field\"><label>%s</label>"
            "<span class=\"masked\">已设置 (%d 字符)</span>"
            "<input type=\"password\" name=\"%s\" placeholder=\"留空保持不变\"></div>\n",
            label, (int)strlen(value), key);
    } else {
        *off += snprintf(buf + *off, buf_size - *off,
            "<div class=\"field\"><label>%s</label>"
            "<input type=\"text\" name=\"%s\" value=\"%s\" placeholder=\"留空清除\"></div>\n",
            label, key, has_value ? value : "");
    }
}

/** Get the device's current IP address as a string. */
static void get_ip_str(char *buf, size_t len)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if(netif == NULL) {
        snprintf(buf, len, "N/A");
        return;
    }
    esp_netif_ip_info_t ip;
    if(esp_netif_get_ip_info(netif, &ip) != ESP_OK) {
        snprintf(buf, len, "N/A");
        return;
    }
    snprintf(buf, len, IPSTR, IP2STR(&ip.ip));
}

/*-----------------------------
 * GET / handler
 *----------------------------*/
static esp_err_t root_get_handler(httpd_req_t *req)
{
    /* Build the HTML page. 12KB should be plenty. */
    static char html_buf[12288];
    size_t off = 0;

    char ip_str[32];
    get_ip_str(ip_str, sizeof(ip_str));

    off += snprintf(html_buf + off, sizeof(html_buf) - off,
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>ESP32 配置</title><style>"
        "*{box-sizing:border-box;margin:0;padding:0}"
        "body{font-family:-apple-system,BlinkMacSystemFont,sans-serif;"
        "background:#0f1418;color:#e6edf3;padding:20px}"
        ".container{max-width:480px;margin:0 auto}"
        "h1{font-size:20px;color:#35c4a5;margin-bottom:4px}"
        ".subtitle{font-size:12px;color:#8b98a5;margin-bottom:20px}"
        ".section{background:#1a2128;border:1px solid #2a343f;border-radius:10px;"
        "padding:16px;margin-bottom:16px}"
        ".section h2{font-size:14px;color:#35c4a5;margin-bottom:12px}"
        ".field{margin-bottom:12px}"
        ".field label{display:block;font-size:12px;color:#8b98a5;margin-bottom:4px}"
        ".field input{width:100%%;padding:8px 12px;background:#0f1418;"
        "border:1px solid #2a343f;border-radius:6px;color:#e6edf3;font-size:14px}"
        ".field input:focus{outline:none;border-color:#35c4a5}"
        ".masked{display:block;font-size:13px;color:#8b98a5;padding:6px 0}"
        "button{width:100%%;padding:12px;background:#35c4a5;color:#0f1418;"
        "border:none;border-radius:8px;font-size:15px;font-weight:600;cursor:pointer}"
        "button:hover{background:#2ba389}"
        ".info{font-size:11px;color:#5a6772;margin-top:16px;text-align:center}"
        "</style></head><body><div class=\"container\">"
        "<h1>ESP32 智能面板配置</h1>"
        "<p class=\"subtitle\">设备 IP: %s</p>"
        "<form method=\"POST\" action=\"/save\">", ip_str);

    /* Section: Home Assistant. */
    off += snprintf(html_buf + off, sizeof(html_buf) - off,
        "<div class=\"section\"><h2>Home Assistant</h2>\n");
    append_field(html_buf, sizeof(html_buf), &off, "ha_token", "Access Token (长期令牌)", true);
    append_field(html_buf, sizeof(html_buf), &off, "ha_base_url", "Base URL", false);
    off += snprintf(html_buf + off, sizeof(html_buf) - off, "</div>\n");

    /* Section: Entity IDs. */
    off += snprintf(html_buf + off, sizeof(html_buf) - off,
        "<div class=\"section\"><h2>HA 实体 ID</h2>\n");
    append_field(html_buf, sizeof(html_buf), &off, "ha_entity_temp", "温度传感器", false);
    append_field(html_buf, sizeof(html_buf), &off, "ha_entity_hum", "湿度传感器", false);
    append_field(html_buf, sizeof(html_buf), &off, "ha_entity_pm25", "PM2.5 传感器", false);
    append_field(html_buf, sizeof(html_buf), &off, "ha_entity_mode", "净化器模式", false);
    append_field(html_buf, sizeof(html_buf), &off, "ha_entity_power", "净化器开关", false);
    append_field(html_buf, sizeof(html_buf), &off, "ha_entity_ac", "空调", false);
    append_field(html_buf, sizeof(html_buf), &off, "ha_entity_lamp", "台灯", false);
    append_field(html_buf, sizeof(html_buf), &off, "ha_entity_cam1", "摄像头 4K", false);
    append_field(html_buf, sizeof(html_buf), &off, "ha_entity_cam2", "摄像头 2K", false);
    off += snprintf(html_buf + off, sizeof(html_buf) - off, "</div>\n");

    /* Section: Service URLs. */
    off += snprintf(html_buf + off, sizeof(html_buf) - off,
        "<div class=\"section\"><h2>服务 URL</h2>\n");
    append_field(html_buf, sizeof(html_buf), &off, "server_summary_url", "服务器监控 API", false);
    append_field(html_buf, sizeof(html_buf), &off, "weather_url", "天气 API", false);
    off += snprintf(html_buf + off, sizeof(html_buf) - off, "</div>\n");

    /* Submit button + footer. */
    off += snprintf(html_buf + off, sizeof(html_buf) - off,
        "<button type=\"submit\">保存并重启</button></form>\n"
        "<p class=\"info\">保存后设备将自动重启以应用新配置</p>\n"
        "</div></body></html>");

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, html_buf, off);
    return ESP_OK;
}

/*-----------------------------
 * POST /save handler
 *----------------------------*/

/** Parse URL-encoded form body and write non-empty values to NVS. */
static esp_err_t save_post_handler(httpd_req_t *req)
{
    int content_len = req->content_len;
    if(content_len <= 0 || content_len > 8192) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid content length");
        return ESP_FAIL;
    }

    /* Read the entire body. */
    char *body = malloc(content_len + 1);
    if(body == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    int received = 0;
    while(received < content_len) {
        int ret = httpd_req_recv(req, body + received, content_len - received);
        if(ret <= 0) {
            free(body);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body receive failed");
            return ESP_FAIL;
        }
        received += ret;
    }
    body[received] = '\0';

    /* Parse key=value pairs (URL-encoded). */
    int saved_count = 0;
    char *saveptr = NULL;
    char *pair = strtok_r(body, "&", &saveptr);
    while(pair != NULL) {
        char *eq = strchr(pair, '=');
        if(eq != NULL) {
            *eq = '\0';
            char *key = pair;
            char *value = eq + 1;

            /* URL-decode the value. */
            char decoded[512];
            size_t di = 0;
            for(size_t si = 0; value[si] != '\0' && di < sizeof(decoded) - 1; si++) {
                if(value[si] == '%' && value[si+1] && value[si+2]) {
                    char hex[3] = { value[si+1], value[si+2], '\0' };
                    decoded[di++] = (char)strtol(hex, NULL, 16);
                    si += 2;
                } else if(value[si] == '+') {
                    decoded[di++] = ' ';
                } else {
                    decoded[di++] = value[si];
                }
            }
            decoded[di] = '\0';

            /* Only write known keys with non-empty values. */
            if(config_key_valid(key) && decoded[0] != '\0') {
                config_set_str(key, decoded);
                saved_count++;
                ESP_LOGI(TAG, "saved '%s' (%zu chars)", key, strlen(decoded));
            }
        }
        pair = strtok_r(NULL, "&", &saveptr);
    }

    free(body);

    ESP_LOGI(TAG, "config saved: %d keys written, restarting in 2s", saved_count);

    /* Send success response with auto-restart page. */
    static const char success_html[] =
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>已保存</title><style>"
        "body{font-family:sans-serif;background:#0f1418;color:#e6edf3;"
        "display:flex;align-items:center;justify-content:center;height:100vh}"
        ".box{text-align:center}"
        ".icon{font-size:48px;color:#35c4a5}"
        "h1{font-size:20px;margin:12px 0}"
        "p{color:#8b98a5;font-size:14px}"
        "</style></head><body><div class=\"box\">"
        "<div class=\"icon\">&#10003;</div>"
        "<h1>配置已保存</h1>"
        "<p>设备将在 <span id=\"cnt\">3</span> 秒后重启...</p>"
        "<script>let c=3;setInterval(()=>{c--;document.getElementById('cnt').textContent=c;"
        "if(c<=0)location.href='/';},1000);</script>"
        "</div></body></html>";

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, success_html, HTTPD_RESP_USE_STRLEN);

    /* Schedule restart after a short delay so the response is sent. */
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();

    return ESP_OK;  /* never reached */
}

/*-----------------------------
 * Public API
 *----------------------------*/
esp_err_t web_config_start(void)
{
    if(s_server != NULL) {
        ESP_LOGW(TAG, "already running");
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 4;
    config.stack_size = 8192;

    ESP_LOGI(TAG, "starting HTTP config server on port %d", config.server_port);

    esp_err_t err = httpd_start(&s_server, &config);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        s_server = NULL;
        return err;
    }

    /* Register URI handlers. */
    const httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_server, &root_uri);

    const httpd_uri_t save_uri = {
        .uri = "/save",
        .method = HTTP_POST,
        .handler = save_post_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_server, &save_uri);

    char ip_str[32];
    get_ip_str(ip_str, sizeof(ip_str));
    ESP_LOGI(TAG, "config page: http://%s/", ip_str);

    return ESP_OK;
}

void web_config_stop(void)
{
    if(s_server != NULL) {
        httpd_stop(s_server);
        s_server = NULL;
        ESP_LOGI(TAG, "HTTP config server stopped");
    }
}
