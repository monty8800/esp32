/**
 * @file wifi_setup_overlay.c
 *
 * Full-screen WiFi provisioning overlay with auto-scan.
 *
 * Flow: gear button → overlay opens → WiFi scan → list of networks
 * (sorted by RSSI) → user taps one → password keyboard appears →
 * save to NVS → esp_restart().
 *
 * The overlay is fully self-contained: it creates/deletes its own widgets
 * and never leaks. All LVGL work runs on the LVGL thread.
 */

#include "wifi_setup_overlay.h"

#include <stdbool.h>
#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "ui_theme.h"
#include "config_store.h"
#include "ui_fonts.h"
#include "wifi_sta.h"

static const char *TAG = "wifi-setup";

/* Max APs we display in the list. */
#define MAX_SCAN_APS 20

/*-----------------------------
 * Overlay state
 *----------------------------*/
static lv_obj_t *s_overlay;        /* full-screen container */
static lv_obj_t *s_list_view;      /* scrollable network list */
static lv_obj_t *s_pwd_view;       /* password entry view */
static lv_obj_t *s_ta_pwd;         /* password textarea */
static lv_obj_t *s_keyboard;       /* LVGL keyboard */
static lv_obj_t *s_label_selected; /* shows selected SSID in pwd view */
static lv_obj_t *s_label_scan;     /* "scanning..." indicator */

static char s_selected_ssid[33];   /* SSID chosen by user */

/* Baked CJK bitmap fonts (Chinese text does not exist in Montserrat). */
static const lv_font_t *s_font_sm;
static const lv_font_t *s_font_lg;

/*-----------------------------
 * Forward declarations
 *----------------------------*/
static void show_list_view(void);
static void show_pwd_view(void);
static void do_save_and_restart(void);

/*-----------------------------
 * Helpers
 *----------------------------*/

/** RSSI -> signal strength bar text (unicode blocks). */
static const char *rssi_bars(int rssi)
{
    if(rssi > -50) return "\xe2\x96\x87\xe2\x96\x87\xe2\x96\x87\xe2\x96\x87"; /* 4 bars */
    if(rssi > -60) return "\xe2\x96\x87\xe2\x96\x87\xe2\x96\x87";              /* 3 bars */
    if(rssi > -70) return "\xe2\x96\x87\xe2\x96\x87";                          /* 2 bars */
    return "\xe2\x96\x87";                                                      /* 1 bar  */
}

/*-----------------------------
 * WiFi scan
 *----------------------------*/

/** Ensure WiFi driver is initialised even on a credential-less first boot
 *  (wifi_sta_start() skips esp_wifi_init when no SSID is stored). */
static void ensure_wifi_init(void)
{
    static bool wifi_inited = false;
    if(wifi_inited) return;

    /* Create the default STA netif if wifi_sta_start() didn't (no creds). */
    if(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") == NULL) {
        esp_netif_create_default_wifi_sta();
    }

    esp_err_t err = esp_wifi_init(&(wifi_init_config_t)WIFI_INIT_CONFIG_DEFAULT());
    if(err == ESP_ERR_INVALID_STATE) {
        /* Already initialised by wifi_sta_start(). */
        wifi_inited = true;
        return;
    }
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
        return;
    }
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();
    wifi_inited = true;
    ESP_LOGI(TAG, "WiFi driver initialised for scanning");
}

static int do_scan(wifi_ap_record_t *ap_list, int max_count)
{
    ensure_wifi_init();

    /* Start a blocking scan. */
    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "scan failed: %s", esp_err_to_name(err));
        return 0;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if(ap_count == 0) return 0;

    /* Cap the result list. */
    uint16_t fetch = (ap_count > max_count) ? max_count : ap_count;
    esp_wifi_scan_get_ap_records(&fetch, ap_list);

    /* Simple insertion sort by RSSI (descending). */
    for(int i = 1; i < fetch; i++) {
        wifi_ap_record_t tmp = ap_list[i];
        int j = i - 1;
        while(j >= 0 && ap_list[j].rssi < tmp.rssi) {
            ap_list[j + 1] = ap_list[j];
            j--;
        }
        ap_list[j + 1] = tmp;
    }

    /* Deduplicate by SSID (keep the strongest). */
    int unique = 0;
    for(int i = 0; i < fetch; i++) {
        bool dup = false;
        for(int u = 0; u < unique; u++) {
            if(strcmp((char *)ap_list[u].ssid, (char *)ap_list[i].ssid) == 0) {
                dup = true;
                break;
            }
        }
        if(!dup) ap_list[unique++] = ap_list[i];
    }

    ESP_LOGI(TAG, "scan found %d APs (%d unique)", ap_count, unique);
    return unique;
}

static void on_cancel_overlay(lv_event_t *e)
{
    LV_UNUSED(e);
    if(s_overlay) {
        lv_obj_del(s_overlay);
        s_overlay = NULL;
        s_list_view = NULL;
        s_pwd_view = NULL;
        s_ta_pwd = NULL;
        s_keyboard = NULL;
    }
}

/*-----------------------------
 * Network list item callback
 *----------------------------*/
static void on_network_tap(lv_event_t *e)
{
    const char *ssid = (const char *)lv_event_get_user_data(e);
    if(ssid == NULL || ssid[0] == '\0') return;

    strncpy(s_selected_ssid, ssid, sizeof(s_selected_ssid) - 1);
    s_selected_ssid[sizeof(s_selected_ssid) - 1] = '\0';
    ESP_LOGI(TAG, "selected SSID: %s", s_selected_ssid);

    show_pwd_view();
}

/*-----------------------------
 * List view construction
 *----------------------------*/
static void build_list_view(void)
{
    if(s_list_view) {
        lv_obj_del(s_list_view);
        s_list_view = NULL;
    }

    s_list_view = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(s_list_view);
    lv_obj_set_size(s_list_view, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_list_view, COL_BG, 0);
    lv_obj_set_style_bg_opa(s_list_view, LV_OPA_COVER, 0);
    lv_obj_set_layout(s_list_view, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_list_view, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(s_list_view, 16, 0);
    lv_obj_set_style_pad_row(s_list_view, 8, 0);

    /* Title. */
    lv_obj_t *title = lv_label_create(s_list_view);
    lv_label_set_text(title, "WiFi 设置");
    lv_obj_set_style_text_color(title, COL_TEXT, 0);
    lv_obj_set_style_text_font(title, s_font_lg, 0);

    /* Current connection info + web config hint (when online). */
    if(wifi_sta_is_connected()) {
        char ssid[64] = "";
        char ip_str[20] = "---";
        config_get_str("wifi_ssid", ssid, sizeof(ssid));
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if(netif != NULL) {
            esp_netif_ip_info_t ip;
            if(esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
                snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip.ip));
            }
        }
        lv_obj_t *st = lv_label_create(s_list_view);
        lv_label_set_text_fmt(st, "已连接: %s  IP: %s", ssid, ip_str);
        lv_obj_set_style_text_color(st, COL_ACCENT, 0);
        lv_obj_set_style_text_font(st, s_font_sm, 0);

        lv_obj_t *hint = lv_label_create(s_list_view);
        lv_label_set_text_fmt(hint, "电脑浏览器访问 http://%s 配置HA",
                              ip_str);
        lv_obj_set_style_text_color(hint, COL_TEXT_DIM, 0);
        lv_obj_set_style_text_font(hint, s_font_sm, 0);
    }

    /* Scanning indicator. */
    s_label_scan = lv_label_create(s_list_view);
    lv_label_set_text(s_label_scan, "正在扫描附近网络...");
    lv_obj_set_style_text_color(s_label_scan, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(s_label_scan, s_font_sm, 0);

    /* Scrollable AP list container. */
    lv_obj_t *list_container = lv_obj_create(s_list_view);
    lv_obj_remove_style_all(list_container);
    lv_obj_set_width(list_container, lv_pct(100));
    lv_obj_set_flex_grow(list_container, 1);
    lv_obj_set_layout(list_container, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(list_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list_container, 6, 0);
    lv_obj_set_scroll_dir(list_container, LV_DIR_VER);

    /* Perform scan (blocking — acceptable here since overlay just opened). */
    static wifi_ap_record_t aps[MAX_SCAN_APS];
    int count = do_scan(aps, MAX_SCAN_APS);

    if(count == 0) {
        lv_label_set_text(s_label_scan, "未找到网络，请检查路由器");
    } else {
        lv_label_set_text_fmt(s_label_scan, "找到 %d 个网络", count);
    }

    /* Create a button for each AP. */
    for(int i = 0; i < count; i++) {
        lv_obj_t *btn = lv_obj_create(list_container);
        lv_obj_remove_style_all(btn);
        lv_obj_set_width(btn, lv_pct(100));
        lv_obj_set_height(btn, 44);
        lv_obj_set_style_bg_color(btn, COL_PANEL, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(btn, 10, 0);
        lv_obj_set_style_border_color(btn, COL_BORDER, 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_pad_left(btn, 12, 0);
        lv_obj_set_style_pad_right(btn, 12, 0);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);

        /* Store SSID pointer as event user_data (APs are in static buffer). */
        lv_obj_add_event_cb(btn, on_network_tap, LV_EVENT_CLICKED, (void *)aps[i].ssid);

        /* Layout: SSID on left, signal bars on right. */
        lv_obj_t *ssid_lbl = lv_label_create(btn);
        lv_label_set_text(ssid_lbl, (char *)aps[i].ssid);
        lv_obj_set_style_text_color(ssid_lbl, COL_TEXT, 0);
        lv_obj_set_style_text_font(ssid_lbl, s_font_sm, 0);
        lv_obj_align(ssid_lbl, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_set_width(ssid_lbl, lv_pct(70));
        lv_label_set_long_mode(ssid_lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);

        lv_obj_t *rssi_lbl = lv_label_create(btn);
        lv_label_set_text(rssi_lbl, rssi_bars(aps[i].rssi));
        lv_obj_set_style_text_color(rssi_lbl, COL_ACCENT, 0);
        lv_obj_set_style_text_font(rssi_lbl, s_font_sm, 0);
        lv_obj_align(rssi_lbl, LV_ALIGN_RIGHT_MID, 0, 0);
    }

    /* Cancel button at the bottom. */
    lv_obj_t *cancel_btn = lv_obj_create(s_list_view);
    lv_obj_remove_style_all(cancel_btn);
    lv_obj_set_width(cancel_btn, lv_pct(100));
    lv_obj_set_height(cancel_btn, 40);
    lv_obj_set_style_bg_color(cancel_btn, COL_PANEL_LT, 0);
    lv_obj_set_style_bg_opa(cancel_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(cancel_btn, 10, 0);
    lv_obj_add_flag(cancel_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(cancel_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(cancel_btn, on_cancel_overlay, LV_EVENT_CLICKED, NULL);

    lv_obj_t *cancel_lbl = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_lbl, "取消");
    lv_obj_set_style_text_color(cancel_lbl, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(cancel_lbl, s_font_sm, 0);
    lv_obj_center(cancel_lbl);
}

/*-----------------------------
 * Password view
 *----------------------------*/
static void on_save_btn(lv_event_t *e)
{
    LV_UNUSED(e);
    do_save_and_restart();
}

static void on_cancel_pwd(lv_event_t *e)
{
    LV_UNUSED(e);
    /* Go back to list view. */
    show_list_view();
}

static void on_ta_focus(lv_event_t *e)
{
    LV_UNUSED(e);
    if(s_keyboard) {
        lv_keyboard_set_textarea(s_keyboard, s_ta_pwd);
        lv_obj_remove_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    }
}

static void build_pwd_view(void)
{
    if(s_pwd_view) {
        lv_obj_del(s_pwd_view);
        s_pwd_view = NULL;
    }

    s_pwd_view = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(s_pwd_view);
    lv_obj_set_size(s_pwd_view, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_pwd_view, COL_BG, 0);
    lv_obj_set_style_bg_opa(s_pwd_view, LV_OPA_COVER, 0);
    lv_obj_set_layout(s_pwd_view, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_pwd_view, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(s_pwd_view, 16, 0);
    lv_obj_set_style_pad_row(s_pwd_view, 10, 0);

    /* Title: selected SSID. */
    s_label_selected = lv_label_create(s_pwd_view);
    lv_label_set_text_fmt(s_label_selected, "连接: %s", s_selected_ssid);
    lv_obj_set_style_text_color(s_label_selected, COL_ACCENT, 0);
    lv_obj_set_style_text_font(s_label_selected, s_font_sm, 0);

    /* Password textarea. */
    s_ta_pwd = lv_textarea_create(s_pwd_view);
    lv_obj_set_width(s_ta_pwd, lv_pct(100));
    lv_obj_set_height(s_ta_pwd, 44);
    lv_textarea_set_one_line(s_ta_pwd, true);
    lv_textarea_set_password_mode(s_ta_pwd, true);
    lv_textarea_set_placeholder_text(s_ta_pwd, "输入 WiFi 密码");
    lv_obj_set_style_bg_color(s_ta_pwd, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(s_ta_pwd, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_ta_pwd, COL_BORDER, 0);
    lv_obj_set_style_border_width(s_ta_pwd, 1, 0);
    lv_obj_set_style_radius(s_ta_pwd, 10, 0);
    lv_obj_set_style_text_color(s_ta_pwd, COL_TEXT, 0);
    lv_obj_set_style_text_font(s_ta_pwd, s_font_sm, 0);
    lv_obj_add_event_cb(s_ta_pwd, on_ta_focus, LV_EVENT_FOCUSED, NULL);

    /* Button row: Save + Cancel. */
    lv_obj_t *btn_row = lv_obj_create(s_pwd_view);
    lv_obj_remove_style_all(btn_row);
    lv_obj_set_width(btn_row, lv_pct(100));
    lv_obj_set_height(btn_row, 44);
    lv_obj_set_layout(btn_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(btn_row, 12, 0);
    lv_obj_remove_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

    /* Save button. */
    lv_obj_t *save_btn = lv_obj_create(btn_row);
    lv_obj_remove_style_all(save_btn);
    lv_obj_set_flex_grow(save_btn, 1);
    lv_obj_set_height(save_btn, 44);
    lv_obj_set_style_bg_color(save_btn, COL_ACCENT, 0);
    lv_obj_set_style_bg_opa(save_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(save_btn, 10, 0);
    lv_obj_add_flag(save_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(save_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(save_btn, on_save_btn, LV_EVENT_CLICKED, NULL);

    lv_obj_t *save_lbl = lv_label_create(save_btn);
    lv_label_set_text(save_lbl, "保存并重启");
    lv_obj_set_style_text_color(save_lbl, lv_color_hex(0x0f1418), 0);
    lv_obj_set_style_text_font(save_lbl, s_font_sm, 0);
    lv_obj_center(save_lbl);

    /* Cancel button. */
    lv_obj_t *cancel_btn = lv_obj_create(btn_row);
    lv_obj_remove_style_all(cancel_btn);
    lv_obj_set_flex_grow(cancel_btn, 1);
    lv_obj_set_height(cancel_btn, 44);
    lv_obj_set_style_bg_color(cancel_btn, COL_PANEL_LT, 0);
    lv_obj_set_style_bg_opa(cancel_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(cancel_btn, 10, 0);
    lv_obj_add_flag(cancel_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(cancel_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(cancel_btn, on_cancel_pwd, LV_EVENT_CLICKED, NULL);

    lv_obj_t *cancel_lbl = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_lbl, "返回");
    lv_obj_set_style_text_color(cancel_lbl, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(cancel_lbl, s_font_sm, 0);
    lv_obj_center(cancel_lbl);

    /* Keyboard (fills remaining space). */
    s_keyboard = lv_keyboard_create(s_pwd_view);
    lv_obj_set_width(s_keyboard, lv_pct(100));
    lv_obj_set_flex_grow(s_keyboard, 1);
    lv_keyboard_set_textarea(s_keyboard, s_ta_pwd);
    lv_obj_set_style_bg_color(s_keyboard, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(s_keyboard, LV_OPA_COVER, 0);
}

/*-----------------------------
 * Save & restart
 *----------------------------*/
static void do_save_and_restart(void)
{
    const char *pwd = lv_textarea_get_text(s_ta_pwd);

    ESP_LOGI(TAG, "saving WiFi: ssid=%s pwd_len=%zu",
             s_selected_ssid, strlen(pwd));

    config_set_str("wifi_ssid", s_selected_ssid);
    config_set_str("wifi_psk", pwd);

    /* Brief delay so the user sees the action was registered. */
    lv_obj_t *msg = lv_label_create(s_overlay);
    lv_label_set_text(msg, "已保存，正在重启...");
    lv_obj_set_style_text_color(msg, COL_ACCENT, 0);
    lv_obj_set_style_text_font(msg, s_font_sm, 0);
    lv_obj_center(msg);

    /* Give LVGL a moment to render the message before restarting. */
    lv_refr_now(NULL);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

/*-----------------------------
 * View switching
 *----------------------------*/
static void show_list_view(void)
{
    if(s_pwd_view) {
        lv_obj_del(s_pwd_view);
        s_pwd_view = NULL;
        s_ta_pwd = NULL;
        s_keyboard = NULL;
    }
    build_list_view();
}

static void show_pwd_view(void)
{
    if(s_list_view) {
        lv_obj_del(s_list_view);
        s_list_view = NULL;
    }
    build_pwd_view();
}

/*-----------------------------
 * Public entry
 *----------------------------*/
void wifi_setup_overlay_show(void)
{
    /* If overlay is already open, just bring it to front. */
    if(s_overlay != NULL) {
        lv_obj_move_foreground(s_overlay);
        return;
    }

    ui_fonts_get(&s_font_sm, &s_font_lg);

    /* Create full-screen overlay on the active screen. */
    lv_obj_t *scr = lv_screen_active();
    s_overlay = lv_obj_create(scr);
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, lv_pct(100), lv_pct(100));
    lv_obj_set_pos(s_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_overlay, COL_BG, 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(s_overlay);

    /* Start with the network list (which triggers a scan). */
    show_list_view();
}
