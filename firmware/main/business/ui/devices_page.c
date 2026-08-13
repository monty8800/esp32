/**
 * @file devices_page.c
 *
 * Page 2 (controllable): scrollable stack of device cards.
 *
 *   Air conditioner   power switch + hvac state text
 *   Air purifier      power switch + three mode buttons (optimistic UI)
 *   Monitor lamp      power switch + brightness slider (optimistic)
 *   Camera 4K / 2K    read-only status badges
 *
 * Control actions go straight into net_worker_post_control() - non-blocking,
 * the UI never waits for the network. Optimistic state is overwritten by the
 * next snapshot when the device reports back.
 */

#include "devices_page.h"
#include "ui_theme.h"

#include <stdint.h>
#include <string.h>

#define MODE_COUNT 3

/*-----------------------------
 * State
 *----------------------------*/
static lv_obj_t * page_root;

/* AC card */
static lv_obj_t * ac_status_label;
static lv_obj_t * ac_switch;
static char ac_status_cache[UI_CACHE_LEN] = "";

/* Purifier card */
static lv_obj_t * pur_switch;
static lv_obj_t * pur_status_label;
static char pur_status_cache[UI_CACHE_LEN] = "";
static lv_obj_t * mode_btns[MODE_COUNT];
static lv_obj_t * mode_labels[MODE_COUNT];
static int ui_mode = -1;                 /* highlighted mode, -1 = none */

/* Lamp card */
static lv_obj_t * lamp_switch;
static lv_obj_t * lamp_status_label;
static char lamp_status_cache[UI_CACHE_LEN] = "";
static lv_obj_t * lamp_slider;
static lv_obj_t * lamp_slider_label;
static char lamp_slider_cache[UI_CACHE_LEN] = "";
static int lamp_brightness_pct = 60;     /* local value; not in the snapshot */

/* Camera badges */
static lv_obj_t * cam1_badge;
static lv_obj_t * cam2_badge;
static char cam1_cache[UI_CACHE_LEN] = "";
static char cam2_cache[UI_CACHE_LEN] = "";

/* Fonts injected at create time. */
static const lv_font_t * f_sm;
static const lv_font_t * f_lg;

/*-----------------------------
 * Pending (optimistic) grace window
 *
 * User actions apply optimistically at once. A poll already in flight when
 * the action happens still publishes the pre-action state, and its snapshot
 * would snap the control back until a later round catches up. For a grace
 * window after each user action, snapshot values contradicting the pending
 * target are ignored; afterwards the normal write-back resumes (which also
 * corrects actions that failed on the HA side).
 *----------------------------*/
#define PENDING_GRACE_MS 8000U   /* covers 2 poll periods + backoff margin */

typedef struct {
    bool     active;       /* a user action is recent enough to matter */
    uint32_t stamp;        /* lv_tick at the time of the action */
    bool     target_on;    /* requested switch state */
} pending_switch_t;

static pending_switch_t pend_ac;
static pending_switch_t pend_pur;
static pending_switch_t pend_lamp;

static bool     pend_mode_active;
static uint32_t pend_mode_stamp;
static int      pend_mode_target = -1;

static void mark_switch_pending(pending_switch_t * p, bool on)
{
    p->active    = true;
    p->stamp     = lv_tick_get();
    p->target_on = on;
}

/** True while a snapshot value disagreeing with the pending target must be
 *  ignored (user action still within the grace window). */
static bool switch_pending_blocks(const pending_switch_t * p, bool snap_on)
{
    return p->active && snap_on != p->target_on &&
           lv_tick_elaps(p->stamp) < PENDING_GRACE_MS;
}

/*-----------------------------
 * Style / build helpers
 *----------------------------*/

/** Device card shell: rounded panel, fixed height, full width. */
static lv_obj_t * build_card(lv_obj_t * parent, int height)
{
    lv_obj_t * card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    ui_style_card(card);
    lv_obj_set_width(card, lv_pct(100));
    lv_obj_set_height(card, height);
    lv_obj_set_style_pad_hor(card, 20, 0);
    lv_obj_set_style_pad_ver(card, 14, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

/** Card title row: device name (left). */
static lv_obj_t * build_title(lv_obj_t * card, const char * name, lv_color_t tick_col)
{
    ui_add_corner_tick(card, tick_col);
    lv_obj_t * title = lv_label_create(card);
    lv_label_set_text(title, name);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_text_color(title, COL_TEXT, 0);
    lv_obj_set_style_text_font(title, f_lg != NULL ? f_lg : &lv_font_montserrat_20, 0);
    return title;
}

/** Small state text right of the title. */
static lv_obj_t * build_status_label(lv_obj_t * card)
{
    lv_obj_t * lbl = lv_label_create(card);
    lv_label_set_text(lbl, "--");
    lv_obj_align(lbl, LV_ALIGN_TOP_RIGHT, 0, 2);
    lv_obj_set_style_text_color(lbl, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(lbl, f_sm != NULL ? f_sm : &lv_font_montserrat_14, 0);
    return lbl;
}

/** Power switch at the bottom-right of a card. */
static lv_obj_t * build_switch(lv_obj_t * card, lv_event_cb_t cb, int id)
{
    lv_obj_t * sw = lv_switch_create(card);
    ui_style_switch(sw);
    lv_obj_align(sw, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)id);
    lv_obj_set_style_opa(sw, LV_OPA_40, LV_PART_MAIN | LV_STATE_DISABLED);
    return sw;
}

/*-----------------------------
 * Control events
 *----------------------------*/

#define SW_ID_AC    0
#define SW_ID_PUR   1
#define SW_ID_LAMP  2

static void power_switch_event_cb(lv_event_t * e)
{
    int id = (int)(intptr_t)lv_event_get_user_data(e);
    lv_obj_t * sw = lv_event_get_target_obj(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);

    switch(id) {
        case SW_ID_AC:
            mark_switch_pending(&pend_ac, on);
            net_worker_post_control(on ? NET_ACT_AC_POWER_ON
                                       : NET_ACT_AC_POWER_OFF, 0);
            break;
        case SW_ID_PUR:
            mark_switch_pending(&pend_pur, on);
            net_worker_post_control(on ? NET_ACT_PURIFIER_POWER_ON
                                       : NET_ACT_PURIFIER_POWER_OFF, 0);
            break;
        case SW_ID_LAMP:
            mark_switch_pending(&pend_lamp, on);
            net_worker_post_control(on ? NET_ACT_LAMP_POWER_ON
                                       : NET_ACT_LAMP_POWER_OFF, 0);
            break;
        default:
            break;
    }
}

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

static void mode_btn_event_cb(lv_event_t * e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if(idx < 0 || idx >= MODE_COUNT) return;

    /* Optimistic update: highlight immediately, the next poll corrects it. */
    set_mode_ui(idx);

    pend_mode_active = true;
    pend_mode_stamp  = lv_tick_get();
    pend_mode_target = idx;

    static const net_control_action_t acts[MODE_COUNT] = {
        NET_ACT_PURIFIER_MODE_AUTO, NET_ACT_PURIFIER_MODE_SLEEP,
        NET_ACT_PURIFIER_MODE_FAV,
    };
    net_worker_post_control(acts[idx], 0);
}

#define LAMP_BRI_THROTTLE_MS 250U
static uint32_t lamp_bri_last_post;

/** Slider shows percent; the network layer wants 1-255. */
static int lamp_pct_to_net(int pct)
{
    int b = (pct * 255) / 100;
    if(b < 1) b = 1;
    if(b > 255) b = 255;
    return b;
}

static void lamp_slider_set_pct(int pct)
{
    lamp_brightness_pct = pct;
    lv_slider_set_value(lamp_slider, pct, LV_ANIM_OFF);
    char buf[8];
    lv_snprintf(buf, sizeof(buf), "%d%%", pct);
    ui_set_label_cached(lamp_slider_label, lamp_slider_cache,
                        sizeof(lamp_slider_cache), buf);
}

/** Throttled posting while dragging; the final value goes out on release. */
static void lamp_slider_event_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if(code != LV_EVENT_PRESSING && code != LV_EVENT_RELEASED) return;

    int pct = lv_slider_get_value(lamp_slider);
    if(pct < 1) pct = 1;
    if(pct > 100) pct = 100;

    /* Optimistic local echo on every drag step. */
    if(pct != lamp_brightness_pct) lamp_slider_set_pct(pct);

    /* Brightness implies the light is on; reuse the lamp switch grace
     * window so a lagging snapshot cannot snap the switch back mid-drag. */
    mark_switch_pending(&pend_lamp, true);

    if(code == LV_EVENT_RELEASED ||
       lv_tick_elaps(lamp_bri_last_post) >= LAMP_BRI_THROTTLE_MS) {
        lamp_bri_last_post = lv_tick_get();
        net_worker_post_control(NET_ACT_LAMP_BRIGHTNESS, lamp_pct_to_net(pct));
    }
}

/*-----------------------------
 * Card builders
 *----------------------------*/

static void build_ac_card(lv_obj_t * parent)
{
    lv_obj_t * card = build_card(parent, 96);
    build_title(card, "空调", COL_ACCENT);
    ac_status_label = build_status_label(card);
    lv_snprintf(ac_status_cache, sizeof(ac_status_cache), "%s", "--");
    ac_switch = build_switch(card, power_switch_event_cb, SW_ID_AC);
}

static void build_purifier_card(lv_obj_t * parent)
{
    lv_obj_t * card = build_card(parent, 152);
    build_title(card, "空气净化器", COL_ACCENT);
    pur_status_label = build_status_label(card);
    lv_snprintf(pur_status_cache, sizeof(pur_status_cache), "%s", "--");
    pur_switch = build_switch(card, power_switch_event_cb, SW_ID_PUR);

    /* Mode button row, bottom-left. */
    static const char * names[MODE_COUNT] = { "自动", "睡眠", "最爱" };
    int x = 0;
    for(int i = 0; i < MODE_COUNT; i++) {
        lv_obj_t * btn = lv_button_create(card);
        lv_obj_set_size(btn, 64, 38);
        lv_obj_align(btn, LV_ALIGN_BOTTOM_LEFT, x, 0);
        x += 64 + 8;
        lv_obj_set_style_bg_color(btn, COL_PANEL_LT, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(btn, 10, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_border_color(btn, COL_BORDER, 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_transform_scale(btn, 248, LV_STATE_PRESSED); /* 256=100% */
        lv_obj_set_style_opa(btn, LV_OPA_40, LV_STATE_DISABLED);

        lv_obj_t * lbl = lv_label_create(btn);
        lv_label_set_text(lbl, names[i]);
        lv_obj_set_style_text_color(lbl, COL_TEXT_DIM, 0);
        lv_obj_set_style_text_font(lbl, f_sm != NULL ? f_sm : &lv_font_montserrat_14, 0);
        lv_obj_center(lbl);

        lv_obj_add_event_cb(btn, mode_btn_event_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        mode_btns[i] = btn;
        mode_labels[i] = lbl;
    }
    ui_mode = -1;
}

static void build_lamp_card(lv_obj_t * parent)
{
    lv_obj_t * card = build_card(parent, 140);
    build_title(card, "显示器挂灯", COL_AMBER);
    lamp_status_label = build_status_label(card);
    lv_snprintf(lamp_status_cache, sizeof(lamp_status_cache), "%s", "--");
    lamp_switch = build_switch(card, power_switch_event_cb, SW_ID_LAMP);

    /* Brightness slider row along the bottom-left, beside the switch. */
    lamp_slider = lv_slider_create(card);
    lv_obj_set_width(lamp_slider, lv_pct(62));
    lv_obj_set_height(lamp_slider, 12);
    lv_obj_align(lamp_slider, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(lamp_slider, COL_PANEL_LT, LV_PART_MAIN);
    lv_obj_set_style_border_color(lamp_slider, COL_BORDER, LV_PART_MAIN);
    lv_obj_set_style_border_width(lamp_slider, 1, LV_PART_MAIN);
    lv_obj_set_style_bg_color(lamp_slider, COL_AMBER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(lamp_slider, COL_TEXT, LV_PART_KNOB);
    lv_obj_set_style_pad_all(lamp_slider, 7, LV_PART_KNOB);
    lv_obj_set_style_opa(lamp_slider, LV_OPA_40,
                         LV_PART_MAIN | LV_STATE_DISABLED);
    lv_obj_set_style_opa(lamp_slider, LV_OPA_40,
                         LV_PART_INDICATOR | LV_STATE_DISABLED);
    lv_obj_set_style_opa(lamp_slider, LV_OPA_40,
                         LV_PART_KNOB | LV_STATE_DISABLED);
    lv_slider_set_range(lamp_slider, 1, 100);
    lv_obj_add_event_cb(lamp_slider, lamp_slider_event_cb, LV_EVENT_ALL, NULL);

    lamp_slider_label = lv_label_create(card);
    lv_obj_set_style_text_color(lamp_slider_label, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(lamp_slider_label,
                               f_sm != NULL ? f_sm : &lv_font_montserrat_14, 0);
    lv_obj_align_to(lamp_slider_label, lamp_slider, LV_ALIGN_OUT_RIGHT_MID,
                    12, 0);
    lamp_slider_cache[0] = '\0';
    lamp_slider_set_pct(lamp_brightness_pct);
}

/** Camera badge: "on" -> 已开启 (accent); off/unknown/unavailable/invalid ->
 *  已关闭 (dim). Cameras are read-only: no controls. */
static void build_camera_card(lv_obj_t * parent, const char * name,
                              lv_obj_t ** out_badge, char * cache, size_t cache_len)
{
    lv_obj_t * card = build_card(parent, 80);
    build_title(card, name, COL_BORDER);

    lv_obj_t * badge = lv_label_create(card);
    lv_label_set_text(badge, "--");
    lv_obj_align(badge, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_text_color(badge, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(badge, f_sm != NULL ? f_sm : &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(badge, 2, 0);
    lv_snprintf(cache, cache_len, "%s", "--");
    *out_badge = badge;
}

void devices_page_create(lv_obj_t * parent, const lv_font_t * font_sm,
                         const lv_font_t * font_lg)
{
    f_sm = font_sm;
    f_lg = font_lg;

    page_root = lv_obj_create(parent);
    lv_obj_remove_style_all(page_root);
    lv_obj_set_size(page_root, lv_pct(100), lv_pct(100));
    lv_obj_set_layout(page_root, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(page_root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(page_root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_hor(page_root, 18, 0);
    lv_obj_set_style_pad_top(page_root, 12, 0);
    lv_obj_set_style_pad_bottom(page_root, 16, 0);
    lv_obj_set_style_pad_row(page_root, 12, 0);
    lv_obj_add_flag(page_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(page_root, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_set_scroll_snap_y(page_root, LV_SCROLL_SNAP_NONE);

    build_ac_card(page_root);
    build_purifier_card(page_root);
    build_lamp_card(page_root);
    build_camera_card(page_root, "摄像机 4K", &cam1_badge, cam1_cache,
                      sizeof(cam1_cache));
    build_camera_card(page_root, "摄像机 2K", &cam2_badge, cam2_cache,
                      sizeof(cam2_cache));
}

/*-----------------------------
 * Update helpers
 *----------------------------*/

/** Sync a switch with polled state and enable/disable it. */
static void sync_switch(lv_obj_t * sw, bool on, bool enabled)
{
    if(on) lv_obj_add_state(sw, LV_STATE_CHECKED);
    else lv_obj_remove_state(sw, LV_STATE_CHECKED);

    if(enabled) lv_obj_remove_state(sw, LV_STATE_DISABLED);
    else lv_obj_add_state(sw, LV_STATE_DISABLED);
}

/** Set a status label with caching; dim color when invalid. */
static void set_status(lv_obj_t * lbl, char * cache, size_t cache_len,
                       const ha_field_t * f, bool on_word_accent)
{
    char buf[UI_CACHE_LEN];
    if(f->valid && f->value[0] != '\0') {
        lv_snprintf(buf, sizeof(buf), "%s", f->value);
    }
    else {
        lv_snprintf(buf, sizeof(buf), "%s", "--");
    }
    ui_set_label_cached(lbl, cache, cache_len, buf);

    lv_color_t col = COL_TEXT_DIM;
    if(f->valid && on_word_accent && strcmp(f->value, "off") != 0) col = COL_ACCENT;
    lv_obj_set_style_text_color(lbl, col, 0);
}

/** Camera badge text/color from a state field. */
static void set_camera_badge(lv_obj_t * badge, char * cache, size_t cache_len,
                             const ha_field_t * f)
{
    bool on = f->valid && strcmp(f->value, "on") == 0;
    /* "off", "unknown", "unavailable" and invalid all read as offline. */
    ui_set_label_cached(badge, cache, cache_len,
                        f->valid ? (on ? "已开启" : "已关闭") : "--");
    lv_obj_set_style_text_color(badge, on ? COL_ACCENT : COL_TEXT_DIM, 0);
}

void devices_page_update(const ha_snapshot_t * s)
{
    if(page_root == NULL || s == NULL) return;

    const bool tok = s->token_ok;

    /*--- AC: hvac state, on when state is valid and not "off" ---*/
    set_status(ac_status_label, ac_status_cache, sizeof(ac_status_cache),
               &s->ac, true);
    bool ac_on = s->ac.valid && strcmp(s->ac.value, "off") != 0;
    /* Keep the optimistic state while a pending action is within grace;
     * enable/disable still follows the snapshot (token_ok etc.). */
    if(switch_pending_blocks(&pend_ac, ac_on)) ac_on = pend_ac.target_on;
    sync_switch(ac_switch, ac_on, tok && s->ac.valid);

    /*--- Purifier: power + mode ---*/
    bool pur_on = s->power.valid && strcmp(s->power.value, "on") == 0;
    set_status(pur_status_label, pur_status_cache, sizeof(pur_status_cache),
               &s->power, false);
    if(switch_pending_blocks(&pend_pur, pur_on)) pur_on = pend_pur.target_on;
    sync_switch(pur_switch, pur_on, tok && s->power.valid);

    int mode_idx = -1;
    if(s->mode.valid) {
        if(strcmp(s->mode.value, "自动") == 0) mode_idx = 0;
        else if(strcmp(s->mode.value, "睡眠") == 0) mode_idx = 1;
        else if(strcmp(s->mode.value, "最爱") == 0) mode_idx = 2;
    }
    /* External sync overwrites any optimistic highlight - unless the mode
     * was just changed locally and the snapshot still shows the old value. */
    bool mode_blocked = pend_mode_active && mode_idx != pend_mode_target &&
                        lv_tick_elaps(pend_mode_stamp) < PENDING_GRACE_MS;
    if(!mode_blocked) {
        if(mode_idx >= 0 && mode_idx != ui_mode) set_mode_ui(mode_idx);
        if(mode_idx < 0 && s->mode.valid == false && ui_mode >= 0) {
            /* Lost mode data: clear the highlight but keep buttons usable
             * only with a valid token. */
            set_mode_ui(-1);
        }
    }
    for(int i = 0; i < MODE_COUNT; i++) {
        if(tok && s->mode.valid) lv_obj_remove_state(mode_btns[i], LV_STATE_DISABLED);
        else lv_obj_add_state(mode_btns[i], LV_STATE_DISABLED);
    }

    /*--- Lamp: power + brightness slider. The snapshot carries no brightness,
     *  so the slider keeps its last local value; enable/disable follows the
     *  same chain as the switch (entity valid + token ok). ---*/
    bool lamp_on = s->lamp.valid && strcmp(s->lamp.value, "on") == 0;
    set_status(lamp_status_label, lamp_status_cache, sizeof(lamp_status_cache),
               &s->lamp, false);
    if(switch_pending_blocks(&pend_lamp, lamp_on)) lamp_on = pend_lamp.target_on;
    bool lamp_enabled = tok && s->lamp.valid;
    sync_switch(lamp_switch, lamp_on, lamp_enabled);
    if(lamp_enabled) lv_obj_remove_state(lamp_slider, LV_STATE_DISABLED);
    else lv_obj_add_state(lamp_slider, LV_STATE_DISABLED);
    lv_obj_set_style_text_color(lamp_slider_label,
                                lamp_enabled ? COL_TEXT_DIM : COL_BORDER, 0);

    /*--- Cameras: read-only badges ---*/
    set_camera_badge(cam1_badge, cam1_cache, sizeof(cam1_cache), &s->cam1);
    set_camera_badge(cam2_badge, cam2_cache, sizeof(cam2_cache), &s->cam2);
}
