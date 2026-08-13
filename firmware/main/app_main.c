/**
 * @file app_main.c
 *
 * M4 (network integration) build for the ESP32-S3-Touch-LCD-4B.
 *
 * Boot order mirrors the simulator's assembly sequence in src/main.c
 * L325-341, adapted for the device:
 *
 *   nvs_flash_init -> env_shim_apply (NVS -> getenv) -> cfg console ->
 *   wifi_sta_start -> display/touch/backlight -> UI assembly (LVGL lock)
 *   -> wait for WiFi (bounded) -> curl_global_init -> ha_client_init ->
 *   net_worker_start -> ui_drain_start.
 *
 * Every stage tolerates failure: without NVS credentials or a token the
 * UI keeps running in the "--" / NO TOKEN degraded state, and the cfg
 * console stays usable for provisioning.
 */

#include <curl/curl.h>

#include <stdlib.h>
#include <time.h>

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_netif_sntp.h"
#include "lvgl.h"
#include "nvs_flash.h"

#include "bsp_backlight.h"
#include "bsp_display.h"
#include "bsp_pins.h"
#include "bsp_touch.h"
#include "config_store.h"
#include "env_shim.h"
#include "ui_drain.h"
#include "ui_fonts.h"
#include "web_config.h"
#include "wifi_sta.h"

#include "esp_event.h"
#include "esp_netif.h"

/* Unmodified simulator network layer (compiled via ../../src C files). */
#include "ha_client.h"
#include "net_worker.h"
#include "ui/ui_shell.h"
#include "ui/dashboard_page.h"
#include "ui/devices_page.h"
#include "ui/server_page.h"

static const char *TAG = "fw";

/** Start the web config server whenever WiFi obtains an IP - also covers
 *  connections that complete after the bounded 5s wait below. */
static void on_sta_got_ip(void * arg, esp_event_base_t base,
                          int32_t event_id, void * data)
{
    (void)arg;
    (void)base;
    (void)event_id;
    (void)data;
    web_config_start();
}

/* RAM copies of the baked CJK fonts: the originals are `const` and live in
 * flash (.rodata), so their `fallback` field cannot be patched in place.
 * LVGL only reads the descriptor, so a patched copy is safe. */
static lv_font_t cjk_sm_rt;
static lv_font_t cjk_lg_rt;

/** Assemble shell + the three pages. Caller must hold the LVGL lock. */
static void ui_assemble(void)
{
    /* CJK bitmap fonts baked at build time (fonts/gen_fonts.sh). The page
     * modules tolerate NULL and fall back to Montserrat; in the firmware
     * build ui_fonts_get() always yields valid fonts. */
    const lv_font_t *font_sm;
    const lv_font_t *font_lg;
    ui_fonts_get(&font_sm, &font_lg);

    cjk_sm_rt = *font_sm;
    cjk_lg_rt = *font_lg;
    /* Runtime text the network layer can surface (HA state strings, the
     * weather description, ...) is not fully covered by the baked CJK
     * glyph set; Montserrat catches whatever is missing (digits/latin)
     * instead of rendering an empty box. */
    cjk_sm_rt.fallback = &lv_font_montserrat_16;
    cjk_lg_rt.fallback = &lv_font_montserrat_20;
    font_sm = &cjk_sm_rt;
    font_lg = &cjk_lg_rt;

    ESP_LOGI(TAG, "[fw] fonts: cjk16=%p cjk20=%p (fallback=Montserrat)",
             (const void *)font_sm, (const void *)font_lg);

    /* Same order as src/main.c: shell first, then pages into the tiles. */
    ui_shell_create(font_sm);
    dashboard_page_create(ui_shell_get_tile(0), font_sm, font_lg);
    devices_page_create(ui_shell_get_tile(1), font_sm, font_lg);
    server_page_create(ui_shell_get_tile(2), font_sm);
    ESP_LOGI(TAG, "[fw] ui assembled: shell + dashboard/devices/server");
}

void app_main(void)
{
    ESP_LOGI(TAG, "[fw] firmware starting (M4 network build)");

    /* NVS is required by several IDF subsystems and by the config store;
     * tolerate a first-boot erase and continue either way. */
    esp_err_t err = nvs_flash_init();
    if(err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "[fw] nvs needs erase (%s), retrying", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_LOGI(TAG, "[fw] nvs ready");

    /* Bring up the TCP stack (lwIP tcpip thread + default event loop)
     * unconditionally, BEFORE WiFi or the network worker.  Without this,
     * a no-credentials boot skips esp_wifi / esp_netif_init entirely and
     * the worker thread's first HTTP call hits lwIP's "Invalid mbox"
     * assert because the tcpip thread was never created.  Both calls are
     * idempotent; wifi_sta_start() repeats them harmlessly. */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Surface NVS config as environment variables BEFORE ha_client_init()
     * reads getenv("HA_TOKEN") & co. */
    env_shim_apply();

    /* Serial provisioning console (own UART task, coexists with LVGL). */
    config_console_start();

    /* WiFi: no credentials -> warning + degraded UI, boot continues. */
    wifi_sta_start();

    /* Web config server follows the connection, not the 5s wait below. */
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                               on_sta_got_ip, NULL);

    /* M1: display pipeline + LVGL task. */
    lv_display_t *disp = bsp_display_start();
    if(!disp) {
        ESP_LOGE(TAG, "[fw] display bring-up failed, halting");
        return;
    }
    ESP_LOGI(TAG, "[fw] display ready");

    /* M2: touch (after the display reset sequence) and backlight. */
    if(!bsp_touch_init()) {
        ESP_LOGE(TAG, "[fw] touch init failed, continuing without touch");
    } else {
        ESP_LOGI(TAG, "[fw] touch ready");
    }
    if(bsp_backlight_set_percent(80) != ESP_OK) {
        ESP_LOGE(TAG, "[fw] backlight init failed");
    } else {
        ESP_LOGI(TAG, "[fw] backlight at 80%%");
    }

    /* Build the three-page UI under the LVGL lock. */
    if(lvgl_port_lock(0)) {
        ui_assemble();
        lvgl_port_unlock();
    } else {
        ESP_LOGE(TAG, "[fw] lvgl_port_lock failed, UI not created");
        return;
    }

    /* Bounded wait: if WiFi is not up within 5s the network stack still
     * starts (requests fail + retry via the net_worker backoff), and the
     * UI shows the degraded state instead of a black boot. */
    bool wifi_connected = wifi_sta_wait_connected(5000);

    /* Fallback in case GOT_IP fired before the handler was registered
     * (web_config_start() is idempotent). */
    if(wifi_connected) {
        web_config_start();
    } else {
        ESP_LOGW(TAG, "[fw] WiFi not connected yet, web config will start "
                      "on connect (or use touchscreen WiFi setup)");
    }

    /*--- Clock: the board has no RTC, so time() is 1970 until SNTP syncs.
     * Set the timezone first so the shell clock renders local time the
     * moment sync completes; SNTP itself retries internally until the
     * network answers, so it needs no WiFi wait here. Single server only
     * (CONFIG_LWIP_SNTP_MAX_SERVERS = 1). */
    setenv("TZ", "CST-8", 1);   /* Asia/Shanghai without DST */
    tzset();
    esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
    err = esp_netif_sntp_init(&sntp_cfg);
    if(err != ESP_OK) {
        ESP_LOGW(TAG, "[fw] sntp init failed (%s), clock stays unsynced",
                 esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "[fw] sntp started (ntp.aliyun.com)");
    }

    /*--- Network: HA token + background worker (mirrors src/main.c) ---
     * curl_global_init must run before any curl use; ha_client_init() also
     * calls it, but the no-token degraded path still needs the shim for
     * server/weather polling, so initialise it unconditionally (repeat
     * calls are safe). */
    curl_global_init(CURL_GLOBAL_DEFAULT);

    bool ha_ok = ha_client_init();
    if(!ha_ok) {
        ESP_LOGW(TAG, "[fw] HA token missing/invalid: degraded mode "
                      "(server + weather polling still active)");
        /* Match the simulator: show NO TOKEN immediately; the drain timer
         * re-asserts it from the snapshot on every tick. */
        if(lvgl_port_lock(0)) {
            ui_shell_set_ha_status("NO TOKEN");
            lvgl_port_unlock();
        }
    }
    net_worker_start(ha_ok);

    /* Drain timer: the only bridge between the worker snapshots and LVGL.
     * lv_timer_create belongs on the LVGL thread, so take the lock. */
    if(lvgl_port_lock(0)) {
        ui_drain_start();
        lvgl_port_unlock();
    } else {
        ESP_LOGE(TAG, "[fw] lvgl_port_lock failed, ui_drain not started");
    }

    ESP_LOGI(TAG, "[fw] M4 running: net_worker active (ha %s)",
             ha_ok ? "on" : "off");
}
