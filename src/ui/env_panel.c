/**
 * @file env_panel.c
 *
 * Air purifier environment panel: temperature / humidity / PM2.5 readouts,
 * three mode buttons and a power switch. Depends ONLY on the LVGL API.
 *
 * Aesthetic direction mirrors ui_demo.c: industrial instrument panel - deep
 * graphite surface, mint-cyan primary accent, amber secondary accent,
 * uppercase letter-spaced headings with corner tick marks.
 */

#include "env_panel.h"

#include <stdint.h>
#include <string.h>

/*-----------------------------
 * Palette & metrics (identical to ui_demo.c)
 *----------------------------*/
#define COL_BG        lv_color_hex(0x0f1418)
#define COL_PANEL     lv_color_hex(0x1a212a)
#define COL_PANEL_LT  lv_color_hex(0x232d39)
#define COL_ACCENT    lv_color_hex(0x2dd4bf)
#define COL_AMBER     lv_color_hex(0xffb454)
#define COL_TEXT      lv_color_hex(0xe6edf3)
#define COL_TEXT_DIM  lv_color_hex(0x7d8a99)
#define COL_BORDER    lv_color_hex(0x2c3947)

#define VALUE_CACHE_LEN 32
#define MODE_COUNT      3

/*-----------------------------
 * State
 *----------------------------*/
static lv_obj_t * temp_label;
static lv_obj_t * hum_label;
static lv_obj_t * pm25_label;
static lv_obj_t * mode_btns[MODE_COUNT];
static lv_obj_t * mode_labels[MODE_COUNT];
static lv_obj_t * power_switch;
static lv_obj_t * status_label;

/* CJK font injected by the platform layer; NULL falls back to Montserrat. */
static const lv_font_t * cjk_font;

/* Last rendered texts; touched only on change to avoid needless redraws.
 * "-" marks a value that has not been received yet. */
static char temp_cache[VALUE_CACHE_LEN]  = "-";
static char hum_cache[VALUE_CACHE_LEN]   = "-";
static char pm25_cache[VALUE_CACHE_LEN]  = "-";
static char status_cache[VALUE_CACHE_LEN] = "";
static int  ui_mode = -1;   /* currently highlighted mode button, -1 = none */

/* Control callback registered by the platform layer. */
static void (*control_cb)(int action, void * user_data) = NULL;
static void * control_user_data = NULL;

/*-----------------------------
 * Style helpers (static copies of ui_demo.c)
 *----------------------------*/

/** Common panel look: rounded card on the dark background. */
static void style_panel(lv_obj_t * obj)
{
    lv_obj_set_style_bg_color(obj, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, 14, 0);
    lv_obj_set_style_border_color(obj, COL_BORDER, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_shadow_color(obj, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_width(obj, 24, 0);
    lv_obj_set_style_shadow_offset_y(obj, 6, 0);
    lv_obj_set_style_shadow_opa(obj, LV_OPA_30, 0);
    lv_obj_set_style_pad_all(obj, 18, 0);
}

/** Uppercase, letter-spaced kicker text (small heading). */
static lv_obj_t * make_kicker(lv_obj_t * parent, const char * txt, lv_color_t col)
{
    lv_obj_t * lbl = lv_label_create(parent);
    lv_label_set_text(lbl, txt);
    lv_obj_set_style_text_color(lbl, col, 0);
    lv_obj_set_style_text_font(lbl, cjk_font != NULL ? cjk_font
                                                     : &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(lbl, 4, 0);
    return lbl;
}

/** Small corner tick decoration on a panel (top-left). */
static void add_corner_tick(lv_obj_t * panel, lv_color_t col)
{
    lv_obj_t * tick = lv_obj_create(panel);
    lv_obj_remove_style_all(tick);
    lv_obj_set_size(tick, 18, 3);
    lv_obj_align(tick, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(tick, col, 0);
    lv_obj_set_style_bg_opa(tick, LV_OPA_COVER, 0);
    lv_obj_remove_flag(tick, LV_OBJ_FLAG_CLICKABLE);
}

/*-----------------------------
 * UI state helpers
 *----------------------------*/

/** Highlight the selected mode button, dim the others. */
static void set_mode_ui(int idx)
{
    for(int i = 0; i < MODE_COUNT; i++) {
        bool sel = (i == idx);
        lv_obj_set_style_bg_color(mode_btns[i], sel ? COL_ACCENT : COL_PANEL_LT, 0);
        lv_obj_set_style_text_color(mode_labels[i],
                                    sel ? lv_color_hex(0x0f1418) : COL_TEXT_DIM, 0);
    }
    ui_mode = idx;
}

/** Set a label only when the formatted text actually changed. */
static void set_label_cached(lv_obj_t * lbl, char * cache, size_t cache_len,
                             const char * fmt, const char * value)
{
    char buf[VALUE_CACHE_LEN];
    lv_snprintf(buf, sizeof(buf), fmt, value);
    if(strcmp(buf, cache) == 0) return;
    lv_snprintf(cache, cache_len, "%s", buf);
    lv_label_set_text(lbl, buf);
}

/** Footer status word with a meaning-coded color. */
static void set_status(const char * text)
{
    if(text == NULL || strcmp(text, status_cache) == 0) return;
    lv_snprintf(status_cache, sizeof(status_cache), "%s", text);
    lv_label_set_text(status_label, text);

    lv_color_t col = COL_TEXT_DIM;                    /* e.g. "NO TOKEN" */
    if(strcmp(text, "LIVE") == 0) col = COL_ACCENT;
    else if(strcmp(text, "OFFLINE") == 0) col = COL_AMBER;
    lv_obj_set_style_text_color(status_label, col, 0);
}

/*-----------------------------
 * Event callbacks
 *----------------------------*/

static void mode_btn_event_cb(lv_event_t * e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if(idx < 0 || idx >= MODE_COUNT) return;

    /* Optimistic update: highlight immediately, the next poll corrects it. */
    set_mode_ui(idx);

    if(control_cb != NULL) control_cb(ENV_ACT_MODE_AUTO + idx, control_user_data);
}

static void power_switch_event_cb(lv_event_t * e)
{
    lv_obj_t * sw = lv_event_get_target_obj(e);
    /* VALUE_CHANGED: the switch already shows the target state - report it. */
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);

    if(control_cb != NULL) {
        control_cb(on ? ENV_ACT_POWER_ON : ENV_ACT_POWER_OFF, control_user_data);
    }
}

/*-----------------------------
 * Panel construction
 *----------------------------*/

/** One readout column: small kicker + big value. */
static lv_obj_t * build_metric_column(lv_obj_t * parent, const char * kicker,
                                      lv_obj_t ** out_label)
{
    lv_obj_t * col = lv_obj_create(parent);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_layout(col, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(col, 6, 0);

    make_kicker(col, kicker, COL_TEXT_DIM);

    lv_obj_t * value = lv_label_create(col);
    lv_label_set_text(value, "-");
    lv_obj_set_style_text_color(value, COL_TEXT, 0);
    lv_obj_set_style_text_font(value, &lv_font_montserrat_20, 0);

    *out_label = value;
    return col;
}

static lv_obj_t * build_mode_button(lv_obj_t * parent, const char * text, int idx)
{
    lv_obj_t * btn = lv_button_create(parent);
    lv_obj_set_size(btn, 74, 40);
    lv_obj_set_style_bg_color(btn, COL_PANEL_LT, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_border_color(btn, COL_BORDER, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_transform_scale(btn, 248, LV_STATE_PRESSED); /* 256 = 100% */

    lv_obj_t * lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(lbl, 2, 0);
    lv_obj_center(lbl);

    lv_obj_add_event_cb(btn, mode_btn_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)idx);

    mode_btns[idx] = btn;
    mode_labels[idx] = lbl;
    return btn;
}

void env_panel_create(lv_obj_t * scr, const lv_font_t * font)
{
    cjk_font = font;

    lv_obj_t * panel = lv_obj_create(scr);
    style_panel(panel);
    lv_obj_set_width(panel, lv_pct(100));
    /* Fill the whole screen so the card owns the 480x480 canvas, and spread
     * the four rows (kicker / data / controls / status) vertically. */
    lv_obj_set_height(panel, lv_pct(100));
    lv_obj_set_layout(panel, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(panel, 14, 0);
    add_corner_tick(panel, COL_ACCENT);

    /* Kicker (CJK glyphs rendered via the injected font). */
    make_kicker(panel, "空气净化器", COL_ACCENT);

    /* Data row: temperature / humidity / PM2.5 */
    lv_obj_t * data_row = lv_obj_create(panel);
    lv_obj_remove_style_all(data_row);
    lv_obj_set_width(data_row, lv_pct(100));
    lv_obj_set_height(data_row, LV_SIZE_CONTENT);
    lv_obj_set_layout(data_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(data_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(data_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    build_metric_column(data_row, "温度", &temp_label);
    build_metric_column(data_row, "湿度", &hum_label);
    build_metric_column(data_row, "PM2.5", &pm25_label);

    /* Control row: mode buttons left, power switch right */
    lv_obj_t * ctrl_row = lv_obj_create(panel);
    lv_obj_remove_style_all(ctrl_row);
    lv_obj_set_width(ctrl_row, lv_pct(100));
    lv_obj_set_height(ctrl_row, LV_SIZE_CONTENT);
    lv_obj_set_layout(ctrl_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(ctrl_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ctrl_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t * mode_group = lv_obj_create(ctrl_row);
    lv_obj_remove_style_all(mode_group);
    lv_obj_set_size(mode_group, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_layout(mode_group, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(mode_group, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(mode_group, 8, 0);

    build_mode_button(mode_group, "AUTO", 0);
    build_mode_button(mode_group, "SLEEP", 1);
    build_mode_button(mode_group, "FAV", 2);

    /* Power switch (styled like ui_demo.c's switch) */
    power_switch = lv_switch_create(ctrl_row);
    lv_obj_set_size(power_switch, 84, 44);
    lv_obj_set_style_bg_color(power_switch, COL_PANEL_LT, LV_PART_MAIN);
    lv_obj_set_style_border_color(power_switch, COL_BORDER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(power_switch, COL_ACCENT, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(power_switch, COL_TEXT, LV_PART_KNOB);
    lv_obj_add_event_cb(power_switch, power_switch_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* Footer status word */
    status_label = lv_label_create(panel);
    lv_label_set_text(status_label, "OFFLINE");
    lv_obj_set_style_text_color(status_label, COL_AMBER, 0);
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(status_label, 4, 0);
    lv_snprintf(status_cache, sizeof(status_cache), "%s", "OFFLINE");
}

/*-----------------------------
 * Public API
 *----------------------------*/

void env_panel_set_control_cb(void (*cb)(int action, void * user_data), void * user_data)
{
    control_cb = cb;
    control_user_data = user_data;
}

void env_panel_update(const char * temp, bool temp_valid,
                      const char * hum, bool hum_valid,
                      const char * pm25, bool pm25_valid,
                      int mode_idx,
                      bool power_on, bool power_valid,
                      const char * status_text)
{
    if(temp_label == NULL) return;   /* panel not created yet */

    /* Invalid or not-yet-received values collapse to a single "-". */
    if(temp_valid && temp != NULL) {
        set_label_cached(temp_label, temp_cache, sizeof(temp_cache), "%s °C", temp);
    }
    else {
        set_label_cached(temp_label, temp_cache, sizeof(temp_cache), "%s", "-");
    }

    if(hum_valid && hum != NULL) {
        set_label_cached(hum_label, hum_cache, sizeof(hum_cache), "%s %%", hum);
    }
    else {
        set_label_cached(hum_label, hum_cache, sizeof(hum_cache), "%s", "-");
    }

    if(pm25_valid && pm25 != NULL) {
        /* ASCII-only: built-in Montserrat has no glyphs for U+00B5 / U+00B3. */
        set_label_cached(pm25_label, pm25_cache, sizeof(pm25_cache), "%s ug/m3", pm25);
    }
    else {
        set_label_cached(pm25_label, pm25_cache, sizeof(pm25_cache), "%s", "-");
    }

    /* External sync overwrites any optimistic highlight. */
    if(mode_idx >= 0 && mode_idx < MODE_COUNT && mode_idx != ui_mode) {
        set_mode_ui(mode_idx);
    }

    if(power_valid && power_switch != NULL) {
        bool cur = lv_obj_has_state(power_switch, LV_STATE_CHECKED);
        if(cur != power_on) {
            if(power_on) lv_obj_add_state(power_switch, LV_STATE_CHECKED);
            else lv_obj_remove_state(power_switch, LV_STATE_CHECKED);
        }
    }

    if(status_text != NULL) set_status(status_text);
}
