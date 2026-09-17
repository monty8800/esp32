/**
 * @file ui_theme.h
 *
 * 全站配色与样式 helper（ui_shell / overview_page / dashboard_page /
 * devices_page / server_page 共用）。Header-only：全部 static inline，
 * 不新增编译单元。
 *
 * 风格：**工业设备 HMI**（2026-09-18 改版，见 docs/ui-redesign-hmi.md）
 *   - 炭灰金属面板 + 明显的盒子边框（工业面板的边框从不含蓄）
 *   - 带标题的边框盒：标题压在边框线上、底色与屏底相同以「切断」边框
 *   - LED 状态灯、可下压按键、琥珀磷光读数
 *   - 方角为主（radius 2–3），不用消费级 App 的大圆角
 *
 * ⚠️ 配色名保持不变、只换色值 —— 这样四页无需逐处改颜色引用即可整体换肤。
 *    语义变化最大的一项是 COL_ACCENT：原本是薄荷青（消费级强调色），
 *    现在是**琥珀磷光**（工业读数色）。
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
 * Palette —— 工业 HMI
 *----------------------------*/
#define COL_BG        lv_color_hex(0x1a1e21)   /* 屏内底色（炭灰，非蓝黑）  */
#define COL_BAR       lv_color_hex(0x14171a)   /* 顶栏 / 状态栏，更深        */
#define COL_PANEL     lv_color_hex(0x21262a)   /* 盒子填充                   */
#define COL_PANEL_LT  lv_color_hex(0x2a3035)   /* 凸起面 / 控件              */
#define COL_ACCENT    lv_color_hex(0xffb03c)   /* 琥珀磷光：主读数           */
#define COL_AMBER     lv_color_hex(0xff8a2b)   /* 告警：偏橙红，与读数区分   */
#define COL_TEXT      lv_color_hex(0xced6dd)
#define COL_TEXT_DIM  lv_color_hex(0x808b95)
/* 工业风的边框必须看得见 —— 原值 0x2c3947 在炭灰底上几乎融掉 */
#define COL_BORDER    lv_color_hex(0x5c656e)

/* 新增：工业 HMI 专有 */
#define COL_BEZEL     lv_color_hex(0x3a4046)   /* 外层金属框                 */
#define COL_BEZEL_LT  lv_color_hex(0x4e565d)   /* 框体高光边（上/左）        */
#define COL_LABEL     lv_color_hex(0x96a2ad)   /* 盒子标题文字               */
#define COL_GREEN     lv_color_hex(0x7ae27a)   /* 磷光绿：正常 / 正向        */
#define COL_RED       lv_color_hex(0xff5c52)   /* 故障                       */
#define COL_LEADER    lv_color_hex(0x60696f)   /* 引导线（原「点线」降级）   */

#define UI_CACHE_LEN  48   /* label cache buffer size used by the pages */

/*-----------------------------
 * Helpers
 *----------------------------*/

/** 工业盒子：方角、1px 明显边框。
 *
 * 原实现的注释记录了「不用 shadow，改用较粗边框」——那个结论仍然成立
 * （LVGL 的 shadow 是每帧一次高斯模糊，SW 渲染下会拖垮帧率），此处沿用。 */
static inline void ui_style_card(lv_obj_t * obj)
{
    lv_obj_set_style_bg_color(obj, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, 2, 0);          /* 工业风：近乎方角 */
    lv_obj_set_style_border_color(obj, COL_BORDER, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
}

/** 外层金属 bezel（屏框）。用在整屏容器上。 */
static inline void ui_style_bezel(lv_obj_t * obj)
{
    lv_obj_set_style_bg_color(obj, COL_BEZEL, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_border_color(obj, COL_BEZEL_LT, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
}

/** 带标题的边框盒 —— 工业 HMI 最标志性的惯用法。
 *
 * 标题文字压在盒子上边框线上，其**不透明底色与屏底一致**，从而「切断」边框。
 *
 * ⚠️ 关键：标题要占用盒子上方约 6px 的空间。**调用方必须为这 6px 留出余量**，
 *    否则标题会压到上一个盒子的内容（效果图第一版就踩过这个，三格底部的
 *    「金额」被切掉）。本函数把标题挂在盒子上、用负 y 偏移，盒子本身尺寸不变，
 *    故**布局计算时请把盒子之间留 ≥14px 间距**。
 *
 * @return 盒子对象（后续往里放内容）
 */
static inline lv_obj_t * ui_titled_box(lv_obj_t * parent, const char * title,
                                       const lv_font_t * font)
{
    lv_obj_t * box = lv_obj_create(parent);
    ui_style_card(box);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * lbl = lv_label_create(box);
    lv_label_set_text(lbl, title);
    lv_obj_set_style_text_color(lbl, COL_LABEL, 0);
    lv_obj_set_style_text_font(lbl, font != NULL ? font : &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(lbl, 1, 0);
    /* 不透明底色 = 屏底，压在盒子上边框上把它「切断」 */
    lv_obj_set_style_bg_color(lbl, COL_BG, 0);
    lv_obj_set_style_bg_opa(lbl, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(lbl, 3, 0);
    lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 5, -13);   /* 负偏移 = 骑在边框上 */
    lv_obj_remove_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

    return box;
}

/** LED 状态灯：彩色圆点 + 暗灰外环。 */
static inline lv_obj_t * ui_led(lv_obj_t * parent, lv_color_t col, int d)
{
    lv_obj_t * led = lv_obj_create(parent);
    lv_obj_remove_style_all(led);
    lv_obj_set_size(led, d, d);
    lv_obj_set_style_radius(led, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(led, col, 0);
    lv_obj_set_style_bg_opa(led, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(led, lv_color_hex(0x232a2f), 0);
    lv_obj_set_style_border_width(led, 2, 0);
    lv_obj_remove_flag(led, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(led, LV_OBJ_FLAG_SCROLLABLE);
    return led;
}

/** 可下压的实体按键。
 *
 * LVGL 的边框四边同色，做不出效果图里「上缘高光 / 下缘阴影」的真立体；
 * 用**垂直渐变**（上亮下暗）近似，观感接近且零额外对象。
 */
static inline lv_obj_t * ui_bevel_button(lv_obj_t * parent, const char * label,
                                         const lv_font_t * font)
{
    lv_obj_t * btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x464e55), 0);
    lv_obj_set_style_bg_grad_color(btn, lv_color_hex(0x2f353a), 0);
    lv_obj_set_style_bg_grad_dir(btn, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 3, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(0x181b1e), 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(btn, LV_LAYOUT_FLEX);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t * lbl = lv_label_create(btn);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_color(lbl, COL_TEXT, 0);
    lv_obj_set_style_text_font(lbl, font != NULL ? font : &lv_font_montserrat_14, 0);
    /* 按下时整体下移 1px —— 物理按键的位移反馈 */
    lv_obj_set_style_translate_y(lbl, 0, 0);
    lv_obj_set_style_translate_y(lbl, 1, LV_STATE_PRESSED);
    return btn;
}

/** Uppercase, letter-spaced kicker text (small heading).
 *  工业风下保留，但页面改版后应优先用 ui_titled_box 的标题。 */
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

/** 引导线（原「点线引导符」的降级实现）。
 *
 * ⚠️ LVGL **没有虚线/点线**，效果图里的 `Amazon ......... 265` 做不出来。
 *    这里用 1px 实线 + 低亮度替代 —— 观感接近、成本为零。
 *    若日后要真点线，只能手画一串小方块对象（每个点一个对象，128 点 = 128 对象），
 *    RAM 与重绘成本都不划算，**不建议**。 */
static inline lv_obj_t * ui_leader_line(lv_obj_t * parent)
{
    lv_obj_t * ln = lv_obj_create(parent);
    lv_obj_remove_style_all(ln);
    lv_obj_set_height(ln, 1);
    lv_obj_set_style_bg_color(ln, COL_LEADER, 0);
    lv_obj_set_style_bg_opa(ln, LV_OPA_COVER, 0);
    lv_obj_remove_flag(ln, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(ln, LV_OBJ_FLAG_SCROLLABLE);
    return ln;
}

/** Set a label only when the text actually changed (redraw de-dup).
 *
 * ⚠️⚠️ cache_len **必须 ≥ 可能的最长文案**。本函数用 strcmp 去重，缓存被截断后
 * 两个前缀相同的长文案会被误判为「没变」，从而**漏刷新**。
 * 编译器只对**字面量**报 -Werror=string-compare，运行时 snprintf 生成的文案
 * 抓不到 —— 本项目已被这个坑咬过三次（alert_cache / status_cache / hint）。
 * 新增长文案时请一并确认对应 cache 的尺寸。 */
static inline void ui_set_label_cached(lv_obj_t * lbl, char * cache,
                                       size_t cache_len, const char * text)
{
    if(strcmp(text, cache) == 0) return;
    lv_snprintf(cache, cache_len, "%s", text);
    lv_label_set_text(lbl, text);
}

/** 工业风开关：方角轨道、琥珀指示、浅色滑块。
 *  效果图里的「实体拨杆」观感靠方角 + 明显的边框与滑块对比来近似。 */
static inline void ui_style_switch(lv_obj_t * sw)
{
    lv_obj_set_size(sw, 74, 34);
    lv_obj_set_style_radius(sw, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, COL_PANEL_LT, LV_PART_MAIN);
    lv_obj_set_style_border_color(sw, COL_BORDER, LV_PART_MAIN);
    lv_obj_set_style_border_width(sw, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(sw, 2, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sw, COL_ACCENT, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_radius(sw, 2, LV_PART_KNOB);
    lv_obj_set_style_bg_color(sw, COL_TEXT, LV_PART_KNOB);
    lv_obj_set_style_border_color(sw, lv_color_hex(0x181b1e), LV_PART_KNOB);
    lv_obj_set_style_border_width(sw, 1, LV_PART_KNOB);
}

#ifdef __cplusplus
}
#endif

#endif /* UI_THEME_H */
