/**
 * @file ui_shell.c
 *
 * Navigation shell implementation: status bar (clock + HA word + page dots)
 * + horizontal lv_tileview with three tiles. Page switching is handled by
 * the tileview's built-in swipe; the dots mirror and drive the active page.
 */

#include "ui_shell.h"
#include "ui_theme.h"

#include <string.h>

/*-----------------------------
 * State
 *----------------------------*/
static lv_obj_t * tiles[UI_SHELL_TILE_COUNT];
static lv_obj_t * tileview;
static lv_obj_t * dots[UI_SHELL_TILE_COUNT];
static lv_obj_t * time_label;
static lv_obj_t * ha_status_label;
static lv_obj_t * env_temp_label;
static lv_obj_t * env_hum_label;

static char time_cache[UI_CACHE_LEN] = "";
static char status_cache[UI_CACHE_LEN] = "";
static char env_temp_cache[UI_CACHE_LEN] = "";
static char env_hum_cache[UI_CACHE_LEN] = "";

static const lv_font_t * shell_font;

/*-----------------------------
 * Active page tracking
 *----------------------------*/

/** Derive the visible tile index from the tileview scroll position. */
static int calc_active_tile(void)
{
    if(tileview == NULL) return 0;
    int32_t w = lv_obj_get_content_width(tileview);
    if(w <= 0) return 0;
    int32_t x = lv_obj_get_scroll_x(tileview);
    int idx = (int)((x + w / 2) / w);
    if(idx < 0) idx = 0;
    if(idx >= UI_SHELL_TILE_COUNT) idx = UI_SHELL_TILE_COUNT - 1;
    return idx;
}

static void set_dots(int active)
{
    for(int i = 0; i < UI_SHELL_TILE_COUNT; i++) {
        bool on = (i == active);
        lv_obj_set_style_bg_color(dots[i], on ? COL_ACCENT : COL_BORDER, 0);
        lv_obj_set_style_bg_opa(dots[i], LV_OPA_COVER, 0);
        lv_obj_set_size(dots[i], on ? 10 : 8, on ? 10 : 8);
    }
}

static void tileview_changed_cb(lv_event_t * e)
{
    LV_UNUSED(e);
    set_dots(calc_active_tile());
}

static void dot_click_cb(lv_event_t * e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if(idx < 0 || idx >= UI_SHELL_TILE_COUNT) return;
    lv_tileview_set_tile_by_index(tileview, (uint32_t)idx, 0, LV_ANIM_ON);
    set_dots(idx);   /* scroll anim reports VALUE_CHANGED too; keep in sync */
}

/*-----------------------------
 * Construction
 *----------------------------*/
void ui_shell_create(const lv_font_t * font_sm)
{
    shell_font = font_sm;

    lv_obj_t * scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /*--- Status bar -------------------------------------------------------*/
    lv_obj_t * bar = lv_obj_create(scr);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, lv_pct(100), UI_SHELL_BAR_HEIGHT);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(bar, COL_BAR, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(bar, COL_BORDER, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_pad_left(bar, 16, 0);
    lv_obj_set_style_pad_right(bar, 16, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    /* Left cluster: clock + temp + humidity + HA word in one flex row.
     * Flex layout guarantees a shared horizontal centre line and
     * gap-free horizontal flow at any text width - no align chains. */
    lv_obj_t * left_cluster = lv_obj_create(bar);
    lv_obj_remove_style_all(left_cluster);
    lv_obj_set_size(left_cluster, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(left_cluster, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_opa(left_cluster, LV_OPA_TRANSP, 0);
    lv_obj_set_layout(left_cluster, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(left_cluster, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(left_cluster, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(left_cluster, 12, 0);
    lv_obj_remove_flag(left_cluster, LV_OBJ_FLAG_SCROLLABLE);

    const lv_font_t * env_font = shell_font != NULL ? shell_font : &lv_font_montserrat_14;

    /* Clock. */
    time_label = lv_label_create(left_cluster);
    lv_label_set_text(time_label, "--:--");
    lv_obj_set_style_text_color(time_label, COL_TEXT, 0);
    lv_obj_set_style_text_font(time_label, &lv_font_montserrat_16, 0);

    /* Indoor temperature (accent) + humidity (dim). */
    env_temp_label = lv_label_create(left_cluster);
    lv_label_set_text(env_temp_label, "--°C");
    lv_obj_set_style_text_color(env_temp_label, COL_ACCENT, 0);
    lv_obj_set_style_text_font(env_temp_label, env_font, 0);

    env_hum_label = lv_label_create(left_cluster);
    lv_label_set_text(env_hum_label, "· --%");
    lv_obj_set_style_text_color(env_hum_label, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(env_hum_label, env_font, 0);

    /* HA connection word. */
    ha_status_label = lv_label_create(left_cluster);
    lv_label_set_text(ha_status_label, "OFFLINE");
    lv_obj_set_style_text_color(ha_status_label, COL_AMBER, 0);
    lv_obj_set_style_text_font(ha_status_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(ha_status_label, 2, 0);
    lv_snprintf(status_cache, sizeof(status_cache), "%s", "OFFLINE");

    /* Page indicator dots (right, tappable). */
    for(int i = 0; i < UI_SHELL_TILE_COUNT; i++) {
        lv_obj_t * dot = lv_obj_create(bar);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 8, 8);
        lv_obj_align(dot, LV_ALIGN_RIGHT_MID, -i * 20, 0);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, COL_BORDER, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        /* Enlarge the hit area a bit for fingers. */
        lv_obj_set_style_pad_all(dot, 6, 0);
        lv_obj_add_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(dot, dot_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        dots[i] = dot;
    }

    /*--- Tileview ----------------------------------------------------------*/
    tileview = lv_tileview_create(scr);
    lv_obj_set_pos(tileview, 0, UI_SHELL_BAR_HEIGHT);
    /* Height in pixels: lv_pct() values are encoded coordinates, so doing
     * arithmetic on them (lv_pct(100) - BAR_HEIGHT) decodes to garbage. */
    lv_obj_set_size(tileview, lv_pct(100),
                    lv_display_get_vertical_resolution(NULL) - UI_SHELL_BAR_HEIGHT);
    lv_obj_set_style_bg_color(tileview, COL_BG, 0);
    lv_obj_set_style_bg_opa(tileview, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(tileview, 0, 0);
    lv_obj_set_style_pad_all(tileview, 0, 0);
    lv_obj_remove_flag(tileview, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_add_event_cb(tileview, tileview_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    for(int i = 0; i < UI_SHELL_TILE_COUNT; i++) {
        lv_obj_t * tile = lv_tileview_add_tile(tileview, (uint8_t)i, 0, LV_DIR_HOR);
        lv_obj_set_style_bg_color(tile, COL_BG, 0);
        lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(tile, 0, 0);
        lv_obj_set_style_pad_all(tile, 0, 0);
        lv_obj_set_style_pad_row(tile, 10, 0);
        /* Pages scroll vertically inside their tile when content overflows. */
        lv_obj_add_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLL_MOMENTUM);
        lv_obj_set_scroll_dir(tile, LV_DIR_VER);
        tiles[i] = tile;
    }

    set_dots(0);
}

/*-----------------------------
 * Public API
 *----------------------------*/
lv_obj_t * ui_shell_get_tile(int idx)
{
    if(idx < 0 || idx >= UI_SHELL_TILE_COUNT) return NULL;
    return tiles[idx];
}

int ui_shell_get_active_tile(void)
{
    return calc_active_tile();
}

void ui_shell_set_ha_status(const char * text)
{
    if(text == NULL || ha_status_label == NULL) return;
    if(strcmp(text, status_cache) == 0) return;
    lv_snprintf(status_cache, sizeof(status_cache), "%s", text);
    lv_label_set_text(ha_status_label, text);

    lv_color_t col = COL_TEXT_DIM;               /* e.g. "NO TOKEN" */
    if(strcmp(text, "LIVE") == 0) col = COL_ACCENT;
    else if(strcmp(text, "OFFLINE") == 0) col = COL_AMBER;
    lv_obj_set_style_text_color(ha_status_label, col, 0);
}

void ui_shell_set_time(const char * hhmm)
{
    if(hhmm == NULL || time_label == NULL) return;
    ui_set_label_cached(time_label, time_cache, sizeof(time_cache), hhmm);
}

void ui_shell_set_env(const char * temp, const char * hum)
{
    if(env_temp_label == NULL || env_hum_label == NULL) return;

    const char * t = (temp != NULL && temp[0] != '\0') ? temp : "--";
    const char * h = (hum != NULL && hum[0] != '\0') ? hum : "--";

    char buf[UI_CACHE_LEN];
    lv_snprintf(buf, sizeof(buf), "%s°C", t);
    ui_set_label_cached(env_temp_label, env_temp_cache, sizeof(env_temp_cache), buf);

    lv_snprintf(buf, sizeof(buf), "· %s%%", h);
    ui_set_label_cached(env_hum_label, env_hum_cache, sizeof(env_hum_cache), buf);
}
