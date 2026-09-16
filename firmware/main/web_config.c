/**
 * @file web_config.c
 *
 * Embedded HTTP configuration server (esp_http_server on port 80).
 * Serves a single-page HTML form for advanced settings (HA token, URLs,
 * entity IDs). On POST /save, writes values to NVS and restarts.
 *
 * Design: no filesystem, no dynamic allocation beyond request parsing.
 * HTML is generated via snprintf into a static buffer (~16KB).
 */

#include "web_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_app_desc.h"

#include "config_store.h"
#include "nvs.h"

static const char *TAG = "webcfg";

static httpd_handle_t s_server = NULL;

static esp_err_t photo_gone_handler(httpd_req_t *req);

/*-----------------------------
 * HTML helpers
 *----------------------------*/

/** Append a form field row to the buffer. Secret fields show mask text.
 * Label is associated via for/id for accessibility; URL fields use
 * type="url" for the right mobile keyboard. */
static void append_field(char *buf, size_t buf_size, size_t *off,
                         const char *key, const char *label, bool secret,
                         bool is_url)
{
    char value[512] = "";
    esp_err_t err = config_get_str(key, value, sizeof(value));
    bool has_value = (err == ESP_OK && value[0] != '\0');
    const char *itype = secret ? "password" : (is_url ? "url" : "text");

    if(secret && has_value) {
        *off += snprintf(buf + *off, buf_size - *off,
            "<div class=\"field\"><label for=\"%s\">%s</label>"
            "<span class=\"masked\">已设置 (%d 字符)</span>"
            "<div class=\"pw\"><input type=\"password\" id=\"%s\" name=\"%s\" "
            "placeholder=\"留空保持不变\" autocomplete=\"off\">"
            "<button type=\"button\" class=\"eye\" aria-label=\"显示或隐藏\" "
            "onclick=\"t(this)\">显示</button></div></div>\n",
            key, label, (int)strlen(value), key, key);
    } else {
        *off += snprintf(buf + *off, buf_size - *off,
            "<div class=\"field\"><label for=\"%s\">%s</label>"
            "<input type=\"%s\" id=\"%s\" name=\"%s\" value=\"%s\" "
            "placeholder=\"留空清除\"></div>\n",
            key, label, itype, key, key, has_value ? value : "");
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
    /* Build the HTML page. Tokens mirror the on-device LVGL theme
     * (ui_theme.h) for visual consistency across surfaces. */
    static char html_buf[16384];
    size_t off = 0;

    char ip_str[32];
    get_ip_str(ip_str, sizeof(ip_str));

    off += snprintf(html_buf + off, sizeof(html_buf) - off,
        "<!DOCTYPE html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<meta name=\"theme-color\" content=\"#0f1418\">"
        "<title>ESP32 配置</title><style>"
        "*{box-sizing:border-box;margin:0;padding:0}"
        "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;"
        "background:#0f1418;color:#e6edf3;padding:16px 16px 40px;line-height:1.5;"
        "-webkit-text-size-adjust:100%%}"
        ".container{max-width:480px;margin:0 auto}"
        "h1{font-size:22px;font-weight:700;color:#2dd4bf}"
        ".subtitle{font-size:14px;color:#94a3b3;margin:4px 0 20px}"
        ".section{background:#1a212a;border:1px solid #2c3947;border-radius:12px;"
        "padding:16px;margin-bottom:16px}"
        "h2{font-size:13px;font-weight:600;color:#2dd4bf;letter-spacing:.06em;margin-bottom:14px}"
        ".field{margin-bottom:14px}"
        "label{display:block;font-size:13px;font-weight:500;color:#94a3b3;margin-bottom:6px}"
        "input{width:100%%;min-height:44px;padding:10px 12px;background:#0b1116;"
        "border:1px solid #2c3947;border-radius:8px;color:#e6edf3;font-size:16px;"
        "touch-action:manipulation}"
        "input::placeholder{color:#64748a}"
        "input:focus{outline:none;border-color:#2dd4bf;"
        "box-shadow:0 0 0 3px rgba(45,212,191,.28)}"
        ".pw{display:flex;gap:8px}"
        ".pw input{flex:1;min-width:0}"
        ".eye{flex:0 0 auto;min-height:44px;padding:0 12px;background:#232d39;"
        "color:#e6edf3;border:1px solid #2c3947;border-radius:8px;font-size:14px;cursor:pointer}"
        ".masked{display:block;font-size:13px;color:#94a3b3;padding:4px 0 6px}"
        "button[type=submit]{width:100%%;min-height:48px;margin-top:4px;background:#2dd4bf;"
        "color:#0b1116;border:none;border-radius:10px;font-size:16px;font-weight:600;"
        "cursor:pointer;touch-action:manipulation;transition:background .15s ease,opacity .15s ease}"
        "button[type=submit]:hover{background:#25b8a5}"
        "button[type=submit]:active{background:#1fa392}"
        "button[type=submit]:disabled{opacity:.55;cursor:wait}"
        ":focus-visible{outline:2px solid #2dd4bf;outline-offset:2px}"
        ".info{font-size:13px;color:#94a3b3;margin-top:16px;text-align:center}"
        "</style></head><body><div class=\"container\">"
        "<h1>ESP32 智能面板配置</h1>"
        "<p class=\"subtitle\">设备 IP：%s</p>"
        "<form method=\"POST\" action=\"/save\" onsubmit=\"return s(this)\">", ip_str);

    /* Section: Home Assistant. */
    off += snprintf(html_buf + off, sizeof(html_buf) - off,
        "<div class=\"section\"><h2>Home Assistant</h2>\n");
    append_field(html_buf, sizeof(html_buf), &off, "ha_token", "Access Token (长期令牌)", true, false);
    append_field(html_buf, sizeof(html_buf), &off, "ha_base_url", "Base URL", false, true);
    off += snprintf(html_buf + off, sizeof(html_buf) - off, "</div>\n");

    /* Section: Entity IDs. */
    off += snprintf(html_buf + off, sizeof(html_buf) - off,
        "<div class=\"section\"><h2>HA 实体 ID</h2>\n");
    append_field(html_buf, sizeof(html_buf), &off, "ha_entity_temp", "温度传感器", false, false);
    append_field(html_buf, sizeof(html_buf), &off, "ha_entity_hum", "湿度传感器", false, false);
    append_field(html_buf, sizeof(html_buf), &off, "ha_entity_pm25", "PM2.5 传感器", false, false);
    append_field(html_buf, sizeof(html_buf), &off, "ha_entity_mode", "净化器模式", false, false);
    append_field(html_buf, sizeof(html_buf), &off, "ha_entity_power", "净化器开关", false, false);
    append_field(html_buf, sizeof(html_buf), &off, "ha_entity_ac", "空调", false, false);
    append_field(html_buf, sizeof(html_buf), &off, "ha_entity_lamp", "台灯", false, false);
    append_field(html_buf, sizeof(html_buf), &off, "ha_entity_cam1", "摄像头 4K", false, false);
    append_field(html_buf, sizeof(html_buf), &off, "ha_entity_cam2", "摄像头 2K", false, false);
    off += snprintf(html_buf + off, sizeof(html_buf) - off, "</div>\n");

    /* Section: Service URLs. */
    off += snprintf(html_buf + off, sizeof(html_buf) - off,
        "<div class=\"section\"><h2>服务 URL</h2>\n");
    append_field(html_buf, sizeof(html_buf), &off, "server_summary_url", "服务器监控 API", false, true);
    append_field(html_buf, sizeof(html_buf), &off, "weather_url", "天气 API", false, true);
    off += snprintf(html_buf + off, sizeof(html_buf) - off, "</div>\n");

    off += snprintf(html_buf + off, sizeof(html_buf) - off, "</div>\n");
    
    /* Section: System. */
    const esp_app_desc_t *app_desc = esp_app_get_description();
    off += snprintf(html_buf + off, sizeof(html_buf) - off,
        "<div class=\"section\"><h2>系统</h2>\n"
        "<div class=\"field\"><label>当前版本</label>"
        "<div style=\"font-size:14px;color:#e6edf3;padding:8px 0\">%s</div></div>\n"
        "</div>\n", app_desc->version);


    /* Submit button + footer + page JS. */
    off += snprintf(html_buf + off, sizeof(html_buf) - off,
        "<button type=\"submit\" id=\"sb\">保存并重启</button></form>\n"
        "<p class=\"info\">保存后设备将自动重启以应用新配置</p>\n"
        "<style>"
        "</style>\n"
        "<script>\n"
        "function t(b){var i=b.previousElementSibling;"
        "var s=i.type==='password';i.type=s?'text':'password';"
        "b.textContent=s?'隐藏':'显示';}"
        "function s(f){var b=document.getElementById('sb');"
        "b.disabled=true;b.textContent='保存中…';return true;}"
        "</script>\n"
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

    /* Send success response with auto-restart page (SVG check icon,
     * tabular countdown digits to avoid layout shift). */
    static const char success_html[] =
        "<!DOCTYPE html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<meta name=\"theme-color\" content=\"#0f1418\">"
        "<title>已保存</title><style>"
        "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;"
        "background:#0f1418;color:#e6edf3;display:flex;align-items:center;"
        "justify-content:center;min-height:100dvh;padding:16px}"
        ".box{text-align:center}"
        "h1{font-size:20px;margin:16px 0 8px}"
        "p{color:#94a3b3;font-size:14px}"
        "#cnt{font-variant-numeric:tabular-nums}"
        "</style></head><body><div class=\"box\">"
        "<svg width=\"56\" height=\"56\" viewBox=\"0 0 24 24\" fill=\"none\" "
        "style=\"margin:0 auto;display:block\" aria-hidden=\"true\">"
        "<circle cx=\"12\" cy=\"12\" r=\"10\" stroke=\"#2dd4bf\" stroke-width=\"2\"/>"
        "<path d=\"M8 12.5l2.5 2.5L16 9.5\" stroke=\"#2dd4bf\" stroke-width=\"2\" "
        "stroke-linecap=\"round\" stroke-linejoin=\"round\"/></svg>"
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
 * 照片相关端点：已停用
 *
 * 电子相册功能 2026-09-16 按用户指令移除 —— 显示端每帧重绘约 1.3 秒
 * （根因：LVGL 对 RAW JPEG 变量源每帧整幅重解码），修复代价过高。
 * 这里保留两个 URI 只为让旧版配置页的残留 JS 拿到明确的 410 而非 404，
 * 避免控制台报错混淆；功能本身已不存在。
 *----------------------------*/
static esp_err_t photo_gone_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "410 Gone");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":false,\"error\":\"photo feature removed\"}",
                    HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
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
    config.max_uri_handlers = 6;
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

    const httpd_uri_t upload_uri = {
        .uri = "/upload_photo",
        .method = HTTP_POST,
        .handler = photo_gone_handler,
    };
    httpd_register_uri_handler(s_server, &upload_uri);

    const httpd_uri_t clear_uri = {
        .uri = "/clear_photos",
        .method = HTTP_POST,
        .handler = photo_gone_handler,
    };
    httpd_register_uri_handler(s_server, &clear_uri);

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
