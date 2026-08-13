/**
 * @file server_page.c
 *
 * Page 3 (read-only): server monitor. Summary card (online/total + last
 * snapshot time + STALE mark) above a scrollable list of host rows with
 * latency and probe status dots. Rebuilds only when the snapshot seq
 * changes.
 */

#include "server_page.h"
#include "ui_theme.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*-----------------------------
 * State
 *----------------------------*/
static lv_obj_t * page_root;
static lv_obj_t * summary_card;
static lv_obj_t * summary_count_label;   /* "3/4"            */
static lv_obj_t * summary_time_label;    /* last snapshot ts */
static lv_obj_t * stale_label;           /* STALE badge      */
static lv_obj_t * placeholder_label;     /* probing text     */
static lv_obj_t * host_list;             /* scrollable rows  */

static const lv_font_t * f_sm;

static uint32_t last_seq = 0;
static bool rendered = false;

/*-----------------------------
 * Small builders
 *----------------------------*/

static lv_obj_t * make_dot(lv_obj_t * parent, int size, lv_color_t col)
{
    lv_obj_t * dot = lv_obj_create(parent);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, size, size);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, col, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    return dot;
}

static void build_summary(void)
{
    summary_card = lv_obj_create(page_root);
    lv_obj_remove_style_all(summary_card);
    ui_style_card(summary_card);
    lv_obj_set_width(summary_card, lv_pct(100));
    lv_obj_set_height(summary_card, 92);
    lv_obj_set_style_pad_hor(summary_card, 20, 0);
    lv_obj_remove_flag(summary_card, LV_OBJ_FLAG_SCROLLABLE);
    ui_add_corner_tick(summary_card, COL_ACCENT);

    /* Left: big online/total count + ONLINE kicker. */
    summary_count_label = lv_label_create(summary_card);
    lv_label_set_text(summary_count_label, "-/-");
    lv_obj_align(summary_count_label, LV_ALIGN_LEFT_MID, 0, -10);
    lv_obj_set_style_text_color(summary_count_label, COL_TEXT, 0);
    lv_obj_set_style_text_font(summary_count_label, &lv_font_montserrat_20, 0);

    lv_obj_t * kicker = lv_label_create(summary_card);
    lv_label_set_text(kicker, "ONLINE");
    lv_obj_align_to(kicker, summary_count_label, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 4);
    lv_obj_set_style_text_color(kicker, COL_ACCENT, 0);
    lv_obj_set_style_text_font(kicker, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(kicker, 3, 0);

    /* Right: last snapshot time + STALE mark. */
    summary_time_label = lv_label_create(summary_card);
    lv_label_set_text(summary_time_label, "--");
    lv_obj_align(summary_time_label, LV_ALIGN_RIGHT_MID, 0, -10);
    lv_obj_set_style_text_color(summary_time_label, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(summary_time_label,
                               f_sm != NULL ? f_sm : &lv_font_montserrat_14, 0);

    stale_label = lv_label_create(summary_card);
    lv_label_set_text(stale_label, "STALE");
    lv_obj_align_to(stale_label, summary_time_label, LV_ALIGN_OUT_BOTTOM_RIGHT, 0, 4);
    lv_obj_set_style_text_color(stale_label, COL_AMBER, 0);
    lv_obj_set_style_text_font(stale_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(stale_label, 3, 0);
    lv_obj_add_flag(stale_label, LV_OBJ_FLAG_HIDDEN);
}

/** One host row card: name + latency on top, desc, mem/disk usage, then
 * ip + probe dots below. */
static void build_host_row(const server_host_t * h)
{
    lv_obj_t * row = lv_obj_create(host_list);
    lv_obj_remove_style_all(row);
    ui_style_card(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(row, 16, 0);
    lv_obj_set_style_pad_ver(row, 10, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(row, 6, 0);

    const lv_font_t * f14 = f_sm != NULL ? f_sm : &lv_font_montserrat_14;

    /*--- Top line: dot + name .... latency ---*/
    lv_obj_t * top = lv_obj_create(row);
    lv_obj_remove_style_all(top);
    lv_obj_set_width(top, lv_pct(100));
    lv_obj_set_height(top, LV_SIZE_CONTENT);
    lv_obj_set_layout(top, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t * left = lv_obj_create(top);
    lv_obj_remove_style_all(left);
    lv_obj_set_size(left, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_layout(left, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(left, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(left, 8, 0);

    make_dot(left, 10, h->online ? COL_ACCENT : COL_BORDER);

    lv_obj_t * name = lv_label_create(left);
    lv_label_set_text(name, h->name[0] != '\0' ? h->name : h->id);
    lv_obj_set_style_text_color(name, COL_TEXT, 0);
    lv_obj_set_style_text_font(name, f14, 0);

    char lat[24];
    if(h->online && h->latency_ms > 0.0) {
        /* Standard snprintf (not lv_snprintf): LVGL's built-in printf has
         * no floating-point support and prints "%.1f" as a literal "f". */
        snprintf(lat, sizeof(lat), "%.1f ms", h->latency_ms);
    }
    else {
        lv_snprintf(lat, sizeof(lat), "%s", "--");
    }
    lv_obj_t * lat_lbl = lv_label_create(top);
    lv_label_set_text(lat_lbl, lat);
    lv_obj_set_style_text_color(lat_lbl, h->online ? COL_ACCENT : COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(lat_lbl, f14, 0);

    /*--- Description line (dim, wrapped) ---*/
    if(h->desc[0] != '\0') {
        lv_obj_t * desc = lv_label_create(row);
        lv_label_set_text(desc, h->desc);
        lv_obj_set_width(desc, lv_pct(100));
        lv_label_set_long_mode(desc, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_color(desc, COL_TEXT_DIM, 0);
        lv_obj_set_style_text_font(desc, f14, 0);
    }

    /*--- Resource usage line: MEM / DISK with >85% warning colour ---*/
    char part[48];
    char stats_line[112];
    char tot[16];
    int mpct = -1, dpct = -1;

    if(h->mem_used_gb >= 0.0 && h->mem_total_gb > 0.0) {
        mpct = (int)(h->mem_used_gb / h->mem_total_gb * 100.0 + 0.5);
        /* Standard snprintf: LVGL's built-in printf has no %f support. */
        snprintf(tot, sizeof(tot), h->mem_total_gb < 9.95 ? "%.1f" : "%.0f",
                 h->mem_total_gb);
        snprintf(part, sizeof(part), "MEM %d%% %.1f/%sG",
                 mpct, h->mem_used_gb, tot);
    }
    else {
        snprintf(part, sizeof(part), "MEM --");
    }
    snprintf(stats_line, sizeof(stats_line), "%s", part);

    if(h->disk_used_gb >= 0.0 && h->disk_total_gb > 0.0) {
        dpct = (int)(h->disk_used_gb / h->disk_total_gb * 100.0 + 0.5);
        snprintf(tot, sizeof(tot), h->disk_total_gb < 9.95 ? "%.1f" : "%.0f",
                 h->disk_total_gb);
        snprintf(part, sizeof(part), " · DISK %d%% %.1f/%sG",
                 dpct, h->disk_used_gb, tot);
    }
    else {
        snprintf(part, sizeof(part), " · DISK --");
    }
    /* strncat-style append with explicit bound. */
    size_t cur = strlen(stats_line);
    snprintf(stats_line + cur, sizeof(stats_line) - cur, "%s", part);

    lv_obj_t * stats_lbl = lv_label_create(row);
    lv_label_set_text(stats_lbl, stats_line);
    lv_obj_set_style_text_color(stats_lbl,
                                (mpct > 85 || dpct > 85) ? COL_AMBER : COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(stats_lbl, f14, 0);
    lv_obj_set_style_text_letter_space(stats_lbl, 1, 0);

    /*--- Bottom line: ip + probe dots ---*/
    lv_obj_t * bottom = lv_obj_create(row);
    lv_obj_remove_style_all(bottom);
    lv_obj_set_width(bottom, lv_pct(100));
    lv_obj_set_height(bottom, LV_SIZE_CONTENT);
    lv_obj_set_layout(bottom, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(bottom, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bottom, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t * ip = lv_label_create(bottom);
    lv_label_set_text(ip, h->ip[0] != '\0' ? h->ip : "--");
    lv_obj_set_style_text_color(ip, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(ip, f14, 0);

    if(h->probe_count > 0) {
        lv_obj_t * probes = lv_obj_create(bottom);
        lv_obj_remove_style_all(probes);
        lv_obj_set_size(probes, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_layout(probes, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(probes, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(probes, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(probes, 5, 0);

        int online_count = 0;
        for(int i = 0; i < h->probe_count; i++) {
            if(h->probes[i].online) online_count++;
            make_dot(probes, 7, h->probes[i].online ? COL_ACCENT : COL_BORDER);
        }

        char cnt[16];
        lv_snprintf(cnt, sizeof(cnt), "%d/%d", online_count, h->probe_count);
        lv_obj_t * cnt_lbl = lv_label_create(probes);
        lv_label_set_text(cnt_lbl, cnt);
        lv_obj_set_style_text_color(cnt_lbl, COL_TEXT_DIM, 0);
        lv_obj_set_style_text_font(cnt_lbl, &lv_font_montserrat_14, 0);
    }
}

/*-----------------------------
 * Public API
 *----------------------------*/

void server_page_create(lv_obj_t * parent, const lv_font_t * font_sm)
{
    f_sm = font_sm;

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
    lv_obj_remove_flag(page_root, LV_OBJ_FLAG_SCROLLABLE);

    ui_make_kicker(page_root, "服务器监控 SERVERS", COL_ACCENT, font_sm);

    build_summary();

    /* Placeholder shown until the first snapshot arrives. */
    placeholder_label = lv_label_create(page_root);
    lv_label_set_text(placeholder_label, "正在探测…");
    lv_obj_set_style_text_color(placeholder_label, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(placeholder_label,
                               f_sm != NULL ? f_sm : &lv_font_montserrat_14, 0);

    /* Scrollable host list fills the remainder. */
    host_list = lv_obj_create(page_root);
    lv_obj_remove_style_all(host_list);
    lv_obj_set_width(host_list, lv_pct(100));
    lv_obj_set_flex_grow(host_list, 1);
    lv_obj_set_layout(host_list, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(host_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(host_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(host_list, 10, 0);
    lv_obj_add_flag(host_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(host_list, LV_OBJ_FLAG_SCROLL_MOMENTUM);

    lv_obj_add_flag(summary_card, LV_OBJ_FLAG_HIDDEN);
}

void server_page_update(const server_snapshot_t * s)
{
    if(page_root == NULL || s == NULL) return;

    /* seq de-dup: the 100ms drain sees the same snapshot most ticks. */
    if(rendered && s->seq == last_seq) return;
    last_seq = s->seq;
    rendered = true;

    /* Never published: probing placeholder, nothing else. */
    if(s->seq == 0) {
        lv_obj_add_flag(summary_card, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(placeholder_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clean(host_list);
        return;
    }

    lv_obj_remove_flag(summary_card, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(placeholder_label, LV_OBJ_FLAG_HIDDEN);

    char buf[48];
    lv_snprintf(buf, sizeof(buf), "%d/%d", s->online, s->total);
    lv_label_set_text(summary_count_label, buf);
    lv_label_set_text(summary_time_label,
                      s->time_str[0] != '\0' ? s->time_str : "--");

    if(s->valid) lv_obj_add_flag(stale_label, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(stale_label, LV_OBJ_FLAG_HIDDEN);

    /* Rebuild host rows (max 24 rows, refreshed at most every 15s). */
    lv_obj_clean(host_list);
    for(int i = 0; i < s->host_count && i < NET_MAX_HOSTS; i++) {
        build_host_row(&s->hosts[i]);
    }
    lv_obj_scroll_to_y(host_list, 0, LV_ANIM_OFF);
}
