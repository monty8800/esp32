/**
 * @file ui_theme.h
 *
 * Shared palette + small style helpers for the three-page UI
 * (ui_shell / dashboard_page / devices_page / server_page).
 * Header-only: static inline functions, no extra compilation unit.
 *
 * Aesthetic: industrial instrument panel - deep graphite surfaces, mint-cyan
 * primary accent, amber secondary accent, uppercase letter-spaced kickers
 * with corner tick marks.
 *
 * Include lvgl.h BEFORE this header.
 */

#ifndef UI_THEME_H
#define UI_THEME_H

#ifdef __cplusplus
extern "C" {
#endif

#include <string.h>

/*-----------------------------
 * Palette
 *----------------------------*/
#define COL_BG        lv_color_hex(0x0f1418)   /* screen background          */
#define COL_BAR       lv_color_hex(0x0b1116)   /* status bar, darker         */
#define COL_PANEL     lv_color_hex(0x1a212a)   /* card surface               */
#define COL_PANEL_LT  lv_color_hex(0x232d39)   /* raised surface / controls  */
#define COL_ACCENT    lv_color_hex(0x2dd4bf)   /* mint-cyan primary          */
#define COL_AMBER     lv_color_hex(0xffb454)   /* warning / secondary        */
#define COL_TEXT      lv_color_hex(0xe6edf3)
#define COL_TEXT_DIM  lv_color_hex(0x7d8a99)
#define COL_BORDER    lv_color_hex(0x2c3947)

#define UI_CACHE_LEN  48   /* label cache buffer size used by the pages */

/*-----------------------------
 * Helpers
 *----------------------------*/

/** Common card look: rounded panel on the dark background. */
static inline void ui_style_card(lv_obj_t * obj)
{
    lv_obj_set_style_bg_color(obj, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, 14, 0);
    lv_obj_set_style_border_color(obj, COL_BORDER, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    /* Shadows (shadow_width > 0) are the most expensive LVGL draw
     * operation: a Gaussian blur pass per card per frame.  On a 480x480
     * RGB panel with a SW renderer this tanks scrolling FPS.  Replace
     * the shadow with a slightly heavier border for visual separation. */
    lv_obj_set_style_border_width(obj, 2, 0);
}

/** Uppercase, letter-spaced kicker text (small heading). */
static inline lv_obj_t * ui_make_kicker(lv_obj_t * parent, const char * txt,
                                        lv_color_t col, const lv_font_t * cjk_font)
{
    lv_obj_t * lbl = lv_label_create(parent);
    lv_label_set_text(lbl, txt);
    lv_obj_set_style_text_color(lbl, col, 0);
    lv_obj_set_style_text_font(lbl, cjk_font != NULL ? cjk_font
                                                     : &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(lbl, 3, 0);
    return lbl;
}

/** Small corner tick decoration on a card (top-left). */
static inline void ui_add_corner_tick(lv_obj_t * card, lv_color_t col)
{
    lv_obj_t * tick = lv_obj_create(card);
    lv_obj_remove_style_all(tick);
    lv_obj_set_size(tick, 18, 3);
    lv_obj_align(tick, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(tick, col, 0);
    lv_obj_set_style_bg_opa(tick, LV_OPA_COVER, 0);
    lv_obj_remove_flag(tick, LV_OBJ_FLAG_CLICKABLE);
}

/** Set a label only when the text actually changed (redraw de-dup). */
static inline void ui_set_label_cached(lv_obj_t * lbl, char * cache,
                                       size_t cache_len, const char * text)
{
    if(strcmp(text, cache) == 0) return;
    lv_snprintf(cache, cache_len, "%s", text);
    lv_label_set_text(lbl, text);
}

/** Shared switch styling: dark track, mint indicator, light knob. */
static inline void ui_style_switch(lv_obj_t * sw)
{
    lv_obj_set_size(sw, 74, 38);
    lv_obj_set_style_bg_color(sw, COL_PANEL_LT, LV_PART_MAIN);
    lv_obj_set_style_border_color(sw, COL_BORDER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, COL_ACCENT, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, COL_TEXT, LV_PART_KNOB);
}

#ifdef __cplusplus
}
#endif

#endif /* UI_THEME_H */
