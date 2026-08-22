/**
 * @file photos_page.c
 *
 * Page 4: single photo display.
 *
 * Loads one JPEG image (from SPIFFS or HTTP URL) and displays it.
 * Uses LVGL's built-in JPEG decoder with memfs.
 */

#include "photos_page.h"
#include "ui_theme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"

#include "config_store.h"
#include "photo_storage.h"

static const char *TAG = "photos";

/*-----------------------------
 * LVGL widgets
 *----------------------------*/
static lv_obj_t * img_widget;
static lv_obj_t * status_label;

/* JPEG buffer - kept alive for LVGL decoder */
static uint8_t * jpeg_buf = NULL;
static uint32_t  jpeg_buf_size = 0;

/*-----------------------------
 * HTTP download helper
 *----------------------------*/
static esp_err_t download_to_psram(const char * url,
                                   uint8_t ** out_data,
                                   uint32_t * out_size)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 15000,
        .buffer_size = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if(client == NULL) {
        ESP_LOGE(TAG, "http_client_init failed for %s", url);
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if(err != ESP_OK) {
        ESP_LOGW(TAG, "http open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    int content_len = esp_http_client_fetch_headers(client);
    if(content_len <= 0 || content_len > 2 * 1024 * 1024) {
        ESP_LOGW(TAG, "bad content length %d for %s", content_len, url);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    uint8_t * buf = heap_caps_malloc(content_len, MALLOC_CAP_SPIRAM);
    if(buf == NULL) {
        ESP_LOGE(TAG, "PSRAM alloc %d bytes failed", content_len);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    int total = 0;
    while(total < content_len) {
        int n = esp_http_client_read(client, (char *)buf + total,
                                     content_len - total);
        if(n <= 0) break;
        total += n;
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if(total != content_len) {
        ESP_LOGW(TAG, "short read %d / %d for %s", total, content_len, url);
        free(buf);
        return ESP_FAIL;
    }

    *out_data = buf;
    *out_size = (uint32_t)content_len;
    return ESP_OK;
}

/*-----------------------------
 * Build LVGL image descriptor from JPEG data
 *----------------------------*/
static void fill_jpeg_dsc(lv_image_dsc_t * dsc,
                           const uint8_t * data, uint32_t size)
{
    memset(dsc, 0, sizeof(*dsc));
    dsc->header.cf = LV_COLOR_FORMAT_RAW;
    dsc->header.w  = 480;
    dsc->header.h  = 480;
    dsc->header.stride = 0;
    dsc->data      = data;
    dsc->data_size = size;
}

/*-----------------------------
 * Load and display a single photo
 *----------------------------*/
static void load_and_display_photo(const char * source_url)
{
    uint8_t * jpeg = NULL;
    uint32_t jpeg_sz = 0;
    esp_err_t err = ESP_FAIL;

    if (strcmp(source_url, "local://spiffs") == 0) {
        int count = photo_storage_count();
        if (count > 0) {
            err = photo_storage_load(1, &jpeg, &jpeg_sz);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "loaded local photo #%d (%lu bytes)", 1, (unsigned long)jpeg_sz);
            }
        } else {
            ESP_LOGW(TAG, "no local photos in SPIFFS");
        }
    } else {
        err = download_to_psram(source_url, &jpeg, &jpeg_sz);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "downloaded photo (%lu bytes)", (unsigned long)jpeg_sz);
        }
    }

    if (err != ESP_OK || jpeg == NULL) {
        if(status_label) {
            lv_label_set_text(status_label, "加载失败");
            lv_obj_remove_flag(status_label, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    /* Free old JPEG buffer if any */
    if (jpeg_buf != NULL) {
        free(jpeg_buf);
    }
    jpeg_buf = jpeg;
    jpeg_buf_size = jpeg_sz;

    /* Create LVGL image descriptor and set it. */
    static lv_image_dsc_t dsc;
    fill_jpeg_dsc(&dsc, jpeg, jpeg_sz);
    lv_image_set_src(img_widget, &dsc);
    lv_obj_set_size(img_widget, lv_pct(100), lv_pct(100));
    lv_obj_set_style_opa(img_widget, LV_OPA_COVER, 0);
    lv_obj_remove_flag(img_widget, LV_OBJ_FLAG_HIDDEN);

    /* Hide placeholder. */
    if(status_label) lv_obj_add_flag(status_label, LV_OBJ_FLAG_HIDDEN);
}

/*-----------------------------
 * UI construction
 *----------------------------*/
void photos_page_create(lv_obj_t * parent, const lv_font_t * font_sm_in)
{
    const lv_font_t * fs = font_sm_in != NULL ? font_sm_in : &lv_font_montserrat_14;

    lv_obj_t * root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(root, lv_color_hex(0x080c10), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    /* Image widget (fills entire page). */
    img_widget = lv_image_create(root);
    lv_obj_set_size(img_widget, lv_pct(100), lv_pct(100));
    lv_image_set_inner_align(img_widget, LV_IMAGE_ALIGN_STRETCH);
    lv_obj_set_style_opa(img_widget, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(img_widget, LV_OBJ_FLAG_HIDDEN);

    /* Placeholder text. */
    status_label = lv_label_create(root);
    lv_label_set_text(status_label, "未配置照片源");
    lv_obj_set_style_text_color(status_label, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(status_label, fs, 0);
    lv_obj_center(status_label);
}

/*-----------------------------
 * Public API
 *----------------------------*/
void photos_page_set_source_url(const char * url)
{
    if(url == NULL || url[0] == '\0') {
        if(status_label) {
            lv_label_set_text(status_label, "未配置照片源");
            lv_obj_remove_flag(status_label, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_add_flag(img_widget, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    if(status_label) {
        lv_label_set_text(status_label, "正在加载照片…");
        lv_obj_remove_flag(status_label, LV_OBJ_FLAG_HIDDEN);
    }

    load_and_display_photo(url);
}

/* Stubs for removed controls. */
void photos_page_next(void) {}
void photos_page_prev(void) {}
void photos_page_toggle_auto(void) {}
