/**
 * @file dashboard_page.c
 *
 * Page 1 (read-only), non-vertical layout:
 *   - top: full-width Shenzhen weather hero card - big outdoor temperature
 *     + WMO-code Chinese description on the left, humidity / wind rows on
 *     the right, fetch time HH:MM in the top-right corner
 *   - bottom: one horizontal flex row of three equal-width indoor metric
 *     cards (temperature / humidity / PM2.5)
 * Industrial panel aesthetic shared with the rest of the UI.
 */

#include "dashboard_page.h"
#include "ui_theme.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define METRIC_COUNT 3

typedef struct {
    lv_obj_t * value_label;
    char cache[UI_CACHE_LEN];
} metric_t;

static metric_t metrics[METRIC_COUNT];   /* 0 = temp, 1 = hum, 2 = pm25 */
static lv_obj_t * page_root;

/*--- Weather hero card widgets + redraw caches ---------------------------*/
static lv_obj_t * wx_temp_label;
static lv_obj_t * wx_desc_label;
static lv_obj_t * wx_hum_label;
static lv_obj_t * wx_wind_label;
static lv_obj_t * wx_time_label;

static char wx_temp_cache[UI_CACHE_LEN];
static char wx_desc_cache[UI_CACHE_LEN];
static char wx_hum_cache[UI_CACHE_LEN];
static char wx_wind_cache[UI_CACHE_LEN];
static char wx_time_cache[UI_CACHE_LEN];

/*-----------------------------
 * WMO weather code -> Chinese description
 *----------------------------*/
typedef struct {
    int lo;
    int hi;
    const char * desc;
} wmo_range_t;

static const wmo_range_t wmo_map[] = {
    { 0,   0,  "晴"     },
    { 1,   2,  "多云"   },
    { 3,   3,  "阴"     },
    { 45,  48, "雾"     },
    { 51,  57, "毛毛雨" },
    { 61,  67, "雨"     },
    { 71,  77, "雪"     },
    { 80,  82, "阵雨"   },
    { 95,  99, "雷阵雨" },
};

static const char * wmo_desc(int code)
{
    for(size_t i = 0; i < sizeof(wmo_map) / sizeof(wmo_map[0]); i++) {
        if(code >= wmo_map[i].lo && code <= wmo_map[i].hi) return wmo_map[i].desc;
    }
    return "未知";
}

/*-----------------------------
 * Construction
 *----------------------------*/

/** Full-width Shenzhen weather hero card (~165px tall). */
static lv_obj_t * build_weather_card(lv_obj_t * parent,
                                     const lv_font_t * font_sm,
                                     const lv_font_t * font_lg)
{
    lv_obj_t * card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    ui_style_card(card);
    lv_obj_set_width(card, lv_pct(100));
    lv_obj_set_height(card, 165);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    ui_add_corner_tick(card, COL_ACCENT);

    const lv_font_t * fs = font_sm != NULL ? font_sm : &lv_font_montserrat_14;
    const lv_font_t * fl = font_lg != NULL ? font_lg : &lv_font_montserrat_20;

    /* Top-left kicker: what this card is. */
    lv_obj_t * kick = ui_make_kicker(card, "室外 OUTDOOR · 深圳 SHENZHEN",
                                     COL_TEXT_DIM, font_sm);
    lv_obj_align(kick, LV_ALIGN_TOP_LEFT, 20, 14);

    /* Top-right: fetch moment HH:MM. */
    wx_time_label = lv_label_create(card);
    lv_label_set_text(wx_time_label, "--:--");
    lv_obj_align(wx_time_label, LV_ALIGN_TOP_RIGHT, -20, 14);
    lv_obj_set_style_text_color(wx_time_label, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(wx_time_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(wx_time_label, 1, 0);
    lv_snprintf(wx_time_cache, sizeof(wx_time_cache), "%s", "--:--");

    /* Left half: big outdoor temperature + Chinese description below. */
    lv_obj_t * left = lv_obj_create(card);
    lv_obj_remove_style_all(left);
    lv_obj_set_size(left, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(left, LV_ALIGN_LEFT_MID, 20, 12);
    lv_obj_set_layout(left, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(left, 4, 0);

    wx_temp_label = lv_label_create(left);
    lv_label_set_text(wx_temp_label, "--");
    lv_obj_set_style_text_color(wx_temp_label, COL_TEXT, 0);
    lv_obj_set_style_text_font(wx_temp_label, fl, 0);
    lv_obj_set_style_text_letter_space(wx_temp_label, 1, 0);
    lv_snprintf(wx_temp_cache, sizeof(wx_temp_cache), "%s", "--");

    wx_desc_label = lv_label_create(left);
    lv_label_set_text(wx_desc_label, "离线");
    lv_obj_set_style_text_color(wx_desc_label, COL_ACCENT, 0);
    lv_obj_set_style_text_font(wx_desc_label, fs, 0);
    lv_obj_set_style_text_letter_space(wx_desc_label, 3, 0);
    lv_snprintf(wx_desc_cache, sizeof(wx_desc_cache), "%s", "离线");

    /* Right half: humidity / wind rows. */
    lv_obj_t * right = lv_obj_create(card);
    lv_obj_remove_style_all(right);
    lv_obj_set_size(right, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(right, LV_ALIGN_RIGHT_MID, -20, 12);
    lv_obj_set_layout(right, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(right, 10, 0);

    wx_hum_label = lv_label_create(right);
    lv_label_set_text(wx_hum_label, "湿度 --");
    lv_obj_set_style_text_color(wx_hum_label, COL_TEXT, 0);
    lv_obj_set_style_text_font(wx_hum_label, fs, 0);
    lv_snprintf(wx_hum_cache, sizeof(wx_hum_cache), "%s", "湿度 --");

    wx_wind_label = lv_label_create(right);
    lv_label_set_text(wx_wind_label, "风速 --");
    lv_obj_set_style_text_color(wx_wind_label, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(wx_wind_label, fs, 0);
    lv_snprintf(wx_wind_cache, sizeof(wx_wind_cache), "%s", "风速 --");

    return card;
}

/** One small indoor metric card: kicker on top, cached value below. */
static lv_obj_t * build_metric_card(lv_obj_t * parent, const char * kicker,
                                    lv_color_t tick_col,
                                    const lv_font_t * font_sm,
                                    const lv_font_t * font_lg, int idx)
{
    lv_obj_t * card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    ui_style_card(card);
    lv_obj_set_flex_grow(card, 1);       /* equal thirds inside the row */
    lv_obj_set_height(card, 110);
    lv_obj_set_style_pad_left(card, 14, 0);
    lv_obj_set_style_pad_right(card, 14, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    ui_add_corner_tick(card, tick_col);

    lv_obj_t * kick = ui_make_kicker(card, kicker, COL_TEXT_DIM, font_sm);
    lv_obj_align(kick, LV_ALIGN_TOP_LEFT, 14, 14);
    lv_obj_set_style_text_letter_space(kick, 2, 0);

    lv_obj_t * value = lv_label_create(card);
    lv_label_set_text(value, "--");
    lv_obj_align(value, LV_ALIGN_BOTTOM_LEFT, 14, -14);
    lv_obj_set_style_text_color(value, COL_TEXT, 0);
    lv_obj_set_style_text_font(value, font_lg != NULL ? font_lg
                                                      : &lv_font_montserrat_20, 0);

    lv_snprintf(metrics[idx].cache, sizeof(metrics[idx].cache), "%s", "--");
    metrics[idx].value_label = value;
    return card;
}

void dashboard_page_create(lv_obj_t * parent, const lv_font_t * font_sm,
                           const lv_font_t * font_lg)
{
    page_root = lv_obj_create(parent);
    lv_obj_remove_style_all(page_root);
    lv_obj_set_size(page_root, lv_pct(100), lv_pct(100));
    lv_obj_set_layout(page_root, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(page_root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(page_root, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_hor(page_root, 18, 0);
    lv_obj_set_style_pad_row(page_root, 14, 0);
    lv_obj_remove_flag(page_root, LV_OBJ_FLAG_SCROLLABLE);

    ui_make_kicker(page_root, "环境总览 ENVIRONMENT · 深圳", COL_ACCENT, font_sm);

    /* Hero weather card, then one horizontal row of indoor metric cards. */
    build_weather_card(page_root, font_sm, font_lg);

    lv_obj_t * row = lv_obj_create(page_root);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_layout(row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 12, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    build_metric_card(row, "室内温度", COL_ACCENT, font_sm, font_lg, 0);
    build_metric_card(row, "室内湿度", COL_AMBER, font_sm, font_lg, 1);
    build_metric_card(row, "PM2.5", COL_ACCENT, font_sm, font_lg, 2);
}

/*-----------------------------
 * Update
 *----------------------------*/

/** Format a field value into buf; invalid -> "--". */
static void format_field(char * buf, size_t cap, const ha_field_t * f,
                         const char * fmt)
{
    if(f->valid && f->value[0] != '\0') lv_snprintf(buf, cap, fmt, f->value);
    else lv_snprintf(buf, cap, "%s", "--");
}

void dashboard_page_update(const ha_snapshot_t * s)
{
    if(page_root == NULL || s == NULL) return;

    char buf[UI_CACHE_LEN];

    format_field(buf, sizeof(buf), &s->temp, "%s°C");
    ui_set_label_cached(metrics[0].value_label, metrics[0].cache,
                        sizeof(metrics[0].cache), buf);

    format_field(buf, sizeof(buf), &s->hum, "%s%%");
    ui_set_label_cached(metrics[1].value_label, metrics[1].cache,
                        sizeof(metrics[1].cache), buf);

    format_field(buf, sizeof(buf), &s->pm25, "%s");
    ui_set_label_cached(metrics[2].value_label, metrics[2].cache,
                        sizeof(metrics[2].cache), buf);
}

void dashboard_page_update_weather(const weather_snapshot_t * w)
{
    if(page_root == NULL || w == NULL) return;

    char buf[UI_CACHE_LEN];

    if(w->valid) {
        /* Standard snprintf (not lv_snprintf): LVGL's built-in printf has
         * no floating-point support and prints "%.1f" as a literal "f". */
        snprintf(buf, sizeof(buf), "%.1f°C", w->temp_c);
        ui_set_label_cached(wx_temp_label, wx_temp_cache,
                            sizeof(wx_temp_cache), buf);

        ui_set_label_cached(wx_desc_label, wx_desc_cache,
                            sizeof(wx_desc_cache), wmo_desc(w->weather_code));

        lv_snprintf(buf, sizeof(buf), "湿度 %d%%", w->hum_pct);
        ui_set_label_cached(wx_hum_label, wx_hum_cache,
                            sizeof(wx_hum_cache), buf);

        snprintf(buf, sizeof(buf), "风速 %.1f km/h", w->wind_kmh);
        ui_set_label_cached(wx_wind_label, wx_wind_cache,
                            sizeof(wx_wind_cache), buf);

        ui_set_label_cached(wx_time_label, wx_time_cache,
                            sizeof(wx_time_cache),
                            w->fetched_hhmm[0] != '\0' ? w->fetched_hhmm : "--:--");
    }
    else {
        ui_set_label_cached(wx_temp_label, wx_temp_cache,
                            sizeof(wx_temp_cache), "--");
        ui_set_label_cached(wx_desc_label, wx_desc_cache,
                            sizeof(wx_desc_cache), "离线");
        ui_set_label_cached(wx_hum_label, wx_hum_cache,
                            sizeof(wx_hum_cache), "湿度 --");
        ui_set_label_cached(wx_wind_label, wx_wind_cache,
                            sizeof(wx_wind_cache), "风速 --");
        ui_set_label_cached(wx_time_label, wx_time_cache,
                            sizeof(wx_time_cache), "--:--");
    }
}
