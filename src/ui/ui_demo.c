/**
 * @file ui_demo.c
 *
 * Screen scaffold for the ESP32-S3-Touch-LCD-4B simulator: background color,
 * vertical vignette gradient and a flex column layout that hosts the content
 * panels (currently the air purifier panel from env_panel.c).
 *
 * Aesthetic direction: industrial instrument panel - deep graphite surface
 * with a slate-to-near-black gradient.
 */

#include "ui_demo.h"

/*-----------------------------
 * Palette & metrics
 *----------------------------*/
#define COL_BG        lv_color_hex(0x0f1418)

#define PAD_GAP       14

void ui_demo_create(const lv_font_t * font_sm_in, const lv_font_t * font_lg_in)
{
    /* Fonts are injected by the platform layer; the panels bring their own
     * typefaces, so they are accepted for API stability only. */
    LV_UNUSED(font_sm_in);
    LV_UNUSED(font_lg_in);

    lv_obj_t * scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* Vertical vignette gradient from slate to near-black. */
    static lv_grad_dsc_t grad;
    lv_color_t grad_colors[2] = {
        lv_color_hex(0x16202b),
        lv_color_hex(0x0b0f13),
    };
    lv_grad_init_stops(&grad, grad_colors, NULL, NULL, 2);
    lv_grad_vertical_init(&grad);
    lv_obj_set_style_bg_grad(scr, &grad, 0);

    /* Flex column that env_panel.c appends its panel to. The panel is the
     * only content, so it sits flush at the top at full width. */
    lv_obj_set_layout(scr, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(scr, 28, 0);
    lv_obj_set_style_pad_row(scr, PAD_GAP, 0);
}
