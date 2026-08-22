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

#include "config_store.h"
#include "photo_storage.h"

static const char *TAG = "webcfg";

static httpd_handle_t s_server = NULL;

static esp_err_t upload_post_handler(httpd_req_t *req);
static esp_err_t clear_photos_handler(httpd_req_t *req);

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
        ".tpg{display:grid;grid-template-columns:repeat(4,1fr);gap:6px;margin-top:8px}"
        ".tpt{aspect-ratio:1;border-radius:6px;overflow:hidden;position:relative;"
        "border:1px solid #2c3947;cursor:pointer}"
        ".tpt img{width:100%%;height:100%%;object-fit:cover;display:block}"
        ".tpt .tx{position:absolute;top:2px;right:2px;width:22px;height:22px;"
        "background:rgba(0,0,0,.6);color:#fff;border:none;border-radius:50%%;"
        "font-size:12px;cursor:pointer;display:flex;align-items:center;justify-content:center}"
        ".crm{display:none;position:fixed;top:0;left:0;width:100%%;height:100%%;"
        "background:rgba(0,0,0,.88);z-index:999;flex-direction:column;align-items:center;"
        "justify-content:center;padding:16px;box-sizing:border-box}"
        ".crm.on{display:flex}"
        ".crv{width:min(80vw,360px);aspect-ratio:1;position:relative;touch-action:none}"
        ".crv canvas{width:100%%;height:100%%;border-radius:8px;display:block}"
        ".crz{display:flex;gap:12px;margin-top:14px;align-items:center}"
        ".crz button{min-height:40px;padding:0 16px;background:#232d39;color:#e6edf3;"
        "border:1px solid #2c3947;border-radius:8px;font-size:14px;cursor:pointer}"
        ".crz .ok{background:#2dd4bf;color:#0b1116;font-weight:600;border:none}"
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

    /* Section: Photo frame. */
    int local_photos = 0;
    {
        FILE *mf = fopen("/spiffs/p/manifest.json", "r");
        if (mf) {
            char mb[64];
            if (fgets(mb, sizeof(mb), mf)) {
                char *p = strstr(mb, "\"count\"");
                if (p) { p = strchr(p, ':'); if (p) local_photos = atoi(p + 1); }
            }
            fclose(mf);
        }
    }
    off += snprintf(html_buf + off, sizeof(html_buf) - off,
        "<div class=\"section\"><h2>电子相册</h2>\n"
        "<div class=\"field\"><label>本地照片（%d 张）</label>"
        "<div style=\"display:flex;gap:8px;margin-top:6px\">"
        "<label style=\"flex:1;cursor:pointer;min-height:44px;display:flex;align-items:center;"
        "justify-content:center;background:#232d39;border:1px solid #2c3947;border-radius:8px;"
        "color:#e6edf3;font-size:14px\">"
        "📷 选择照片"
        "<input type=\"file\" id=\"pf\" multiple accept=\"image/jpeg,image/png,image/webp\" "
        "style=\"display:none\"></label>"
        "<button type=\"button\" id=\"ub\" onclick=\"upl()\" "
        "style=\"flex:1;min-height:44px;background:#2dd4bf;color:#0b1116;border:none;"
        "border-radius:8px;font-size:14px;font-weight:600;cursor:pointer\">"
        "上传</button>"
        "<button type=\"button\" onclick=\"clr()\" "
        "style=\"flex:0 0 auto;min-height:44px;padding:0 12px;background:#dc2626;color:#fff;"
        "border:none;border-radius:8px;font-size:14px;cursor:pointer\">"
        "清除</button></div>"
        "<div id=\"pr\" style=\"font-size:13px;color:#94a3b3;margin-top:8px\"></div></div>\n",
        local_photos);
    append_field(html_buf, sizeof(html_buf), &off, "photo_source_url",
                 "照片列表 URL (JSON)", false, true);
    off += snprintf(html_buf + off, sizeof(html_buf) - off,
        "<p style=\"font-size:12px;color:#64748a;margin-top:8px\">"
        "浏览器自动压缩，无需公网。"
        "JSON 格式: {&quot;photos&quot;: [&quot;http://host/img1.jpg&quot;, ...]}"
        "</p>\n");
    off += snprintf(html_buf + off, sizeof(html_buf) - off, "</div>\n");

    /* Crop modal + thumbnail preview grid (outside form). */
    off += snprintf(html_buf + off, sizeof(html_buf) - off,
        "<div id=\"tp\" class=\"tpg\"></div>"
        "<div id=\"cm\" class=\"crm\">"
        "<div class=\"crv\"><canvas id=\"cv\" width=\"480\" height=\"480\"></canvas></div>"
        "<div class=\"crz\">"
        "<button type=\"button\" onclick=\"cz(.8)\">&#8722;</button>"
        "<button type=\"button\" class=\"ok\" onclick=\"cc()\">OK</button>"
        "<button type=\"button\" onclick=\"cz(1.25)\">&#43;</button>"
        "<button type=\"button\" onclick=\"cx()\">&#10005;</button>"
        "</div></div>\n");

    /* Submit button + footer + complete JS for photo workflow. */
    off += snprintf(html_buf + off, sizeof(html_buf) - off,
        "<button type=\"submit\" id=\"sb\">保存并重启</button></form>\n"
        "<p class=\"info\">保存后设备将自动重启以应用新配置</p>\n"
        "<style>"
        ".tpg{display:grid;grid-template-columns:repeat(4,1fr);gap:6px;margin-top:8px}"
        ".tpt{aspect-ratio:1;border-radius:6px;overflow:hidden;position:relative;"
        "border:1px solid #2c3947;cursor:pointer}"
        ".tpt img{width:100%%;height:100%%;object-fit:cover;display:block}"
        ".tpt .tx{position:absolute;top:2px;right:2px;width:22px;height:22px;"
        "background:rgba(0,0,0,.6);color:#fff;border:none;border-radius:50%%;"
        "font-size:12px;cursor:pointer;display:flex;align-items:center;justify-content:center}"
        ".crm{display:none;position:fixed;top:0;left:0;width:100%%;height:100%%;"
        "background:rgba(0,0,0,.88);z-index:999;flex-direction:column;align-items:center;"
        "justify-content:center;padding:16px;box-sizing:border-box}"
        ".crm.on{display:flex}"
        ".crv{width:min(85vw,400px);aspect-ratio:1;position:relative;touch-action:none}"
        ".crv canvas{width:100%%;height:100%%;border-radius:8px;display:block}"
        ".crz{display:flex;gap:12px;margin-top:14px;align-items:center}"
        ".crz button{min-height:40px;padding:0 16px;background:#232d39;color:#e6edf3;"
        "border:1px solid #2c3947;border-radius:8px;font-size:14px;cursor:pointer}"
        ".crz .ok{background:#2dd4bf;color:#0b1116;font-weight:600;border:none}"
        "</style>\n"
        "<script>\n"
        "function t(b){var i=b.previousElementSibling;"
        "var s=i.type==='password';i.type=s?'text':'password';"
        "b.textContent=s?'隐藏':'显示';}"
        "function s(f){var b=document.getElementById('sb');"
        "b.disabled=true;b.textContent='保存中…';return true;}"
        "var FQ=[],CQ=[],CI=0,PW=480;"
        "var cr={im:null,s:1,ix:0,iy:0,iw:0,ih:0,pw:PW,dr:false,dx:0,dy:0};"
        "document.getElementById('pf').addEventListener('change',function(e){"
        "FQ=Array.from(e.target.files);CQ=[];"
        "var g=document.getElementById('tp');g.innerHTML='';"
        "FQ.forEach(function(f,i){var d=document.createElement('div');d.className='tpt';"
        "var im=document.createElement('img');im.src=URL.createObjectURL(f);"
        "d.appendChild(im);g.appendChild(d);"
        "var r=new FileReader();r.onload=function(ev){FQ[i]={name:f.name,src:ev.target.result,ready:false}};"
        "r.readAsDataURL(f)});});"
        "function upl(){if(!FQ.length){alert('请先选择照片');return;}"
        "CI=0;CQ=new Array(FQ.length);nx();}"
        "function nx(){if(CI>=FQ.length){au();return;}"
        "var f=FQ[CI];if(f.ready){cp(f.blob,function(b){CQ[CI]=b;CI++;nx();});return;}"
        "co(f.src,function(){});}"
        "function au(){var pr=document.getElementById('pr');"
        "(async function(){for(var i=0;i<CQ.length;i++){"
        "if(!CQ[i])continue;pr.textContent='上传 '+(i+1)+'/'+CQ.length;"
        "var b=CQ[i];try{var r=await fetch('/upload_photo',{method:'POST',"
        "headers:{'Content-Type':'image/jpeg'},body:b});"
        "var j=await r.json();console.log('saved',j.index)}"
        "catch(e){pr.textContent='上传失败: '+(i+1);return;}}}"
        "()).then(function(){"
        "pr.textContent='全部上传完成！';"
        "setTimeout(function(){location.reload()},1200)});}\n"
        "function clr(){if(!confirm('确定清除？'))return;"
        "fetch('/clear_photos',{method:'POST'}).then(function(){"
        "document.getElementById('tp').innerHTML='';"
        "document.getElementById('pr').textContent='';"
        "FQ=[];CQ=[];setTimeout(function(){location.reload()},500)});}\n"
        "function co(src,done){var m=document.getElementById('cm');m.classList.add('on');"
        "var im=new Image();im.onload=function(){cr.im=im;var p=cr.pw;"
        "var s=Math.min(p/im.width,p/im.height);"
        "cr.s=s;cr.iw=im.width*s;cr.ih=im.height*s;"
        "cr.ix=(p-cr.iw)/2;cr.iy=(p-cr.ih)/2;cr._d=done;cd()};im.src=src;}"
        "function cd(){var c=document.getElementById('cv'),x=c.getContext('2d');"
        "x.clearRect(0,0,PW,PW);x.drawImage(cr.im,cr.ix,cr.iy,cr.iw,cr.ih);"
        "x.fillStyle='rgba(0,0,0,.4)';x.fillRect(0,0,PW,PW);"
        "x.save();x.beginPath();x.rect(0,0,PW,PW);x.clip();"
        "x.drawImage(cr.im,cr.ix,cr.iy,cr.iw,cr.ih);x.restore();"
        "x.strokeStyle='#2dd4bf';x.lineWidth=2;x.strokeRect(1,1,PW-2,PW-2);}"
        "function cp(blob,done){var im=new Image();im.onload=function(){"
        "var c=document.createElement('canvas');c.width=PW;c.height=PW;"
        "var x=c.getContext('2d');var p=PW;"
        "var s=Math.min(p/im.width,p/im.height);"
        "var w=im.width*s,h=im.height*s;"
        "x.drawImage(im,(p-w)/2,(p-h)/2,w,h);"
        "c.toBlob(function(b){done(b)},  'image/jpeg',0.85);};im.src=URL.createObjectURL(blob);}"
        "function cc(){var c=document.createElement('canvas');c.width=PW;c.height=PW;"
        "var x=c.getContext('2d');x.drawImage(cr.im,cr.ix,cr.iy,cr.iw,cr.ih);"
        "c.toBlob(function(b){FQ[CI]={name:'c.jpg',ready:true,blob:b};"
        "var g=document.getElementById('tp');if(g.children[CI]){"
        "var im=g.children[CI].querySelector('img');if(im)im.src=URL.createObjectURL(b)}"
        "CQ[CI]=b;document.getElementById('cm').classList.remove('on');"
        "CI++;nx()},  'image/jpeg',0.85);}\n"
        "function cx(){document.getElementById('cm').classList.remove('on');CI++;nx();}"
        "function cz(f){var os=cr.s,ns=os*f;if(ns<.1||ns>10)return;"
        "var cx2=cr.ix+cr.iw/2,cy2=cr.iy+cr.ih/2;cr.s=ns;"
        "cr.iw=cr.im.width*ns;cr.ih=cr.im.height*ns;"
        "cr.ix=cx2-cr.iw/2;cr.iy=cy2-cr.ih/2;cl();cd();}"
        "function cl(){var p=cr.pw;"
        "if(cr.iw<=p)cr.ix=(p-cr.iw)/2;"
        "else cr.ix=Math.min(0,Math.max(p-cr.iw,cr.ix));"
        "if(cr.ih<=p)cr.iy=(p-cr.ih)/2;"
        "else cr.iy=Math.min(0,Math.max(p-cr.ih,cr.iy));}"
        "(function(){var cv=document.getElementById('cv');"
        "cv.addEventListener('mousedown',function(e){if(!cr.im)return;"
        "cr.dr=true;cr.dx=e.clientX;cr.dy=e.clientY;e.preventDefault();});"
        "document.addEventListener('mousemove',function(e){if(!cr.dr)return;"
        "cr.ix+=e.clientX-cr.dx;cr.iy+=e.clientY-cr.dy;"
        "cr.dx=e.clientX;cr.dy=e.clientY;cl();cd();});"
        "document.addEventListener('mouseup',function(){cr.dr=false;});"
        "cv.addEventListener('touchstart',function(e){if(!cr.im)return;"
        "var t=e.touches[0];cr.dr=true;cr.dx=t.clientX;cr.dy=t.clientY;"
        "e.preventDefault();},{passive:false});"
        "cv.addEventListener('touchmove',function(e){if(!cr.dr)return;"
        "var t=e.touches[0];cr.ix+=t.clientX-cr.dx;cr.iy+=t.clientY-cr.dy;"
        "cr.dx=t.clientX;cr.dy=t.clientY;cl();cd();e.preventDefault();}"
        ",{passive:false});"
        "cv.addEventListener('touchend',function(){cr.dr=false;});"
        "})();\n"
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
 * POST /upload_photo handler
 *
 * Receives a browser-compressed JPEG as the raw POST body and saves it
 * to SPIFFS via photo_storage_save().
 *----------------------------*/
static esp_err_t upload_post_handler(httpd_req_t *req)
{
    int content_len = req->content_len;
    if (content_len <= 0 || content_len > 512 * 1024) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid content length");
        return ESP_FAIL;
    }

    uint8_t *buf = heap_caps_malloc(content_len, MALLOC_CAP_SPIRAM);
    if (buf == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    int received = 0;
    while (received < content_len) {
        int n = httpd_req_recv(req, (char *)(buf + received), content_len - received);
        if (n <= 0) {
            free(buf);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Receive failed");
            return ESP_FAIL;
        }
        received += n;
    }

    int idx = photo_storage_save(buf, (uint32_t)content_len);
    free(buf);

    if (idx < 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Save failed");
        return ESP_FAIL;
    }

    char resp[128];
    int len = snprintf(resp, sizeof(resp),
        "{\"ok\":true,\"index\":%d,\"size\":%d}", idx, content_len);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, len);
    return ESP_OK;
}

/*-----------------------------
 * POST /clear_photos handler
 *
 * Deletes all stored photos from SPIFFS.
 *----------------------------*/
static esp_err_t clear_photos_handler(httpd_req_t *req)
{
    esp_err_t err = photo_storage_clear();
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Clear failed");
        return ESP_FAIL;
    }
    const char *resp = "{\"ok\":true}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, strlen(resp));
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
        .handler = upload_post_handler,
    };
    httpd_register_uri_handler(s_server, &upload_uri);

    const httpd_uri_t clear_uri = {
        .uri = "/clear_photos",
        .method = HTTP_POST,
        .handler = clear_photos_handler,
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
