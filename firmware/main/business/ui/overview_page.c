/**
 * @file overview_page.c
 *
 * 事务总览页（tile 0，开机默认页）—— 一屏放下全部要点，**不滚动**。
 *
 * 版面（480x480，状态栏占 44px，可用约 436px）：
 *   总览 kicker + 状态行                          约 40px
 *   [今日] [昨日] [本月]  三格销售                 86px
 *   平台 · 今日 | 国家 · 今日   两列并排 8 行       flex 吃掉剩余
 *   [缺货] [待审草稿] [定时任务]  三格事务          72px
 *
 * 每格/每行都按**各自来源的 ok** 独立降级（不看顶层 stale）——
 * 邮件事务挂了不该让销量格变灰，Mac 离线也不该。
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "lvgl.h"

#include "ui_theme.h"
#include "overview_page.h"

#define OV_SALES_CELLS 3
#define OV_TXN_CELLS   3
#define OV_LINES       8      /* 平台/国家每列最多显示行数 */

/* 三格销售：今日 / 昨日 / 本月 */
static const char * OV_SALES_TITLES[OV_SALES_CELLS] = { "今日", "昨日", "本月" };
/* 三格事务：缺货 / 待审草稿 / 定时任务 */
static const char * OV_TXN_TITLES[OV_TXN_CELLS] = { "缺货", "待审草稿", "定时任务" };

typedef struct {
    lv_obj_t * card;
    lv_obj_t * value;
    lv_obj_t * sub;
    char       value_cache[UI_CACHE_LEN];
    char       sub_cache[UI_CACHE_LEN];
} ov_cell_t;

/** 平台/国家列表的一行：左名称 + 右数值。 */
typedef struct {
    lv_obj_t * row;
    lv_obj_t * name;
    lv_obj_t * val;
    lv_obj_t * trend;    /* 对比昨日的涨跌百分比，按方向着色 */
    char       name_cache[UI_CACHE_LEN];
    char       val_cache[UI_CACHE_LEN];
    char       trend_cache[UI_CACHE_LEN];
} ov_line_t;

static lv_obj_t *  page_root;
static lv_obj_t *  status_lbl;
static char        status_cache[UI_CACHE_LEN];
static ov_cell_t   sales_cells[OV_SALES_CELLS];
static ov_cell_t   txn_cells[OV_TXN_CELLS];
static ov_line_t   plat_lines[OV_LINES];
static ov_line_t   ctry_lines[OV_LINES];

/* --------------------------------------------------------------------------
 * Helpers
 * -------------------------------------------------------------------------- */

/** "2026-09-16T10:24:05+08:00" -> "10:24"；格式不对则留空。 */
static void iso_hhmm(const char * iso, char * out, size_t n)
{
    out[0] = '\0';
    if(iso == NULL || strlen(iso) < 16) return;
    if(iso[10] != 'T') return;
    snprintf(out, n, "%c%c:%c%c", iso[11], iso[12], iso[14], iso[15]);
}

/** 人民币金额：大数折算成"万/亿"，小屏才读得下。 */
static void fmt_rmb(double v, char * out, size_t n)
{
    if(v >= 1e8)      snprintf(out, n, "%.2f亿", v / 1e8);
    else if(v >= 1e4) snprintf(out, n, "%.1f万", v / 1e4);
    else              snprintf(out, n, "%.0f", v);
}

static void set_cell(ov_cell_t * c, const char * value, const char * sub, lv_color_t col)
{
    lv_obj_set_style_text_color(c->value, col, 0);
    ui_set_label_cached(c->value, c->value_cache, sizeof(c->value_cache), value);
    ui_set_label_cached(c->sub,   c->sub_cache,   sizeof(c->sub_cache),   sub);
}

/** 一张紧凑卡片：kicker 标题 + 大字 + 副行。 */
static void build_cell(lv_obj_t * parent, ov_cell_t * c, const char * title,
                       const lv_font_t * font_sm, const lv_font_t * font_lg)
{
    lv_obj_t * card = lv_obj_create(parent);
    ui_style_card(card);
    lv_obj_set_flex_grow(card, 1);
    lv_obj_set_height(card, lv_pct(100));
    lv_obj_set_layout(card, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(card, 8, 0);
    lv_obj_set_style_pad_row(card, 0, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    ui_make_kicker(card, title, COL_TEXT_DIM, font_sm);

    c->value = lv_label_create(card);
    lv_obj_set_style_text_font(c->value, font_lg, 0);
    lv_obj_set_style_text_color(c->value, COL_TEXT_DIM, 0);
    lv_label_set_text(c->value, "--");
    c->value_cache[0] = '\0';

    c->sub = lv_label_create(card);
    lv_obj_set_width(c->sub, lv_pct(100));   /* 限宽：长文案折行而不是溢出卡片 */
    lv_obj_set_style_text_font(c->sub, font_sm, 0);
    lv_obj_set_style_text_color(c->sub, COL_TEXT_DIM, 0);
    lv_label_set_text(c->sub, "");
    c->sub_cache[0] = '\0';

    c->card = card;
}

/** 列表一行：左名称 + 右侧「数值 + 趋势」；行高固定 20px 以求紧凑。 */
static void build_line(lv_obj_t * parent, ov_line_t * L, const lv_font_t * font_sm)
{
    lv_obj_t * row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, 20);
    lv_obj_set_layout(row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    L->name = lv_label_create(row);
    lv_obj_set_style_text_font(L->name, font_sm, 0);
    lv_obj_set_style_text_color(L->name, COL_TEXT, 0);
    lv_label_set_text(L->name, "");
    L->name_cache[0] = '\0';

    /* 右侧容器：让「数值」与「趋势」成组右对齐 */
    lv_obj_t * right = lv_obj_create(row);
    lv_obj_remove_style_all(right);
    lv_obj_set_size(right, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_layout(right, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(right, 6, 0);
    lv_obj_remove_flag(right, LV_OBJ_FLAG_SCROLLABLE);

    L->val = lv_label_create(right);
    lv_obj_set_style_text_font(L->val, font_sm, 0);
    lv_obj_set_style_text_color(L->val, COL_ACCENT, 0);
    lv_label_set_text(L->val, "");
    L->val_cache[0] = '\0';

    L->trend = lv_label_create(right);
    lv_obj_set_style_text_font(L->trend, font_sm, 0);
    lv_obj_set_style_text_color(L->trend, COL_TEXT_DIM, 0);
    lv_label_set_text(L->trend, "");
    L->trend_cache[0] = '\0';

    L->row = row;
}

/** 趋势文案 + 颜色：涨=薄荷，跌=琥珀，昨日无基数=「新」，两天都无=「—」。 */
static void fmt_trend(int today, int yday, char * out, size_t n, lv_color_t * col)
{
    if(yday <= 0) {
        if(today > 0) { snprintf(out, n, "新");  *col = COL_ACCENT; }
        else          { snprintf(out, n, "—");  *col = COL_TEXT_DIM; }
        return;
    }
    /* 四舍五入到整数百分比 */
    double pct = (double)(today - yday) * 100.0 / (double)yday;
    int    ip  = (int)(pct >= 0 ? pct + 0.5 : pct - 0.5);
    snprintf(out, n, "%+d%%", ip);
    *col = (ip >= 0) ? COL_ACCENT : COL_AMBER;
}

/** 填充一列列表（平台或国家）；超出 @p item_count 的行隐藏。 */
static void fill_lines(ov_line_t * lines, int count,
                       const panel_rank_t * items, int item_count)
{
    char val[UI_CACHE_LEN];
    char trend[UI_CACHE_LEN];
    for(int i = 0; i < count; i++) {
        if(i >= item_count) {
            lv_obj_add_flag(lines[i].row, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(lines[i].row, LV_OBJ_FLAG_HIDDEN);
        ui_set_label_cached(lines[i].name, lines[i].name_cache,
                            sizeof(lines[i].name_cache), items[i].name);
        snprintf(val, sizeof(val), "%d单 %d件", items[i].orders, items[i].units);
        ui_set_label_cached(lines[i].val, lines[i].val_cache,
                            sizeof(lines[i].val_cache), val);

        lv_color_t col;
        fmt_trend(items[i].units, items[i].yday_units, trend, sizeof(trend), &col);
        lv_obj_set_style_text_color(lines[i].trend, col, 0);
        ui_set_label_cached(lines[i].trend, lines[i].trend_cache,
                            sizeof(lines[i].trend_cache), trend);
    }
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

void overview_page_create(lv_obj_t * parent, const lv_font_t * font_sm,
                          const lv_font_t * font_lg)
{
    memset(sales_cells, 0, sizeof(sales_cells));
    memset(txn_cells, 0, sizeof(txn_cells));
    memset(plat_lines, 0, sizeof(plat_lines));
    memset(ctry_lines, 0, sizeof(ctry_lines));
    status_cache[0] = '\0';

    page_root = lv_obj_create(parent);
    lv_obj_remove_style_all(page_root);
    lv_obj_set_size(page_root, lv_pct(100), lv_pct(100));
    lv_obj_set_layout(page_root, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(page_root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(page_root, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_hor(page_root, 12, 0);
    lv_obj_set_style_pad_top(page_root, 6, 0);
    lv_obj_set_style_pad_row(page_root, 6, 0);
    lv_obj_remove_flag(page_root, LV_OBJ_FLAG_SCROLLABLE);   /* 一屏放下，不滚动 */

    ui_make_kicker(page_root, "事务总览 OVERVIEW", COL_ACCENT, font_sm);

    status_lbl = lv_label_create(page_root);
    lv_obj_set_style_text_font(status_lbl, font_sm, 0);
    lv_obj_set_style_text_color(status_lbl, COL_TEXT_DIM, 0);
    lv_label_set_text(status_lbl, "正在获取数据…");

    /* ---- 第一行：销售三格 ---- */
    lv_obj_t * sales_row = lv_obj_create(page_root);
    lv_obj_remove_style_all(sales_row);
    lv_obj_set_width(sales_row, lv_pct(100));
    lv_obj_set_height(sales_row, 86);
    lv_obj_set_layout(sales_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(sales_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(sales_row, 8, 0);
    lv_obj_remove_flag(sales_row, LV_OBJ_FLAG_SCROLLABLE);
    for(int i = 0; i < OV_SALES_CELLS; i++)
        build_cell(sales_row, &sales_cells[i], OV_SALES_TITLES[i], font_sm, font_lg);

    /* ---- 第二段：平台 / 国家 两列并排（吃掉剩余高度）---- */
    lv_obj_t * lists_row = lv_obj_create(page_root);
    lv_obj_remove_style_all(lists_row);
    lv_obj_set_width(lists_row, lv_pct(100));
    lv_obj_set_flex_grow(lists_row, 1);
    lv_obj_set_layout(lists_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(lists_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(lists_row, 10, 0);
    lv_obj_remove_flag(lists_row, LV_OBJ_FLAG_SCROLLABLE);

    for(int col = 0; col < 2; col++) {
        lv_obj_t * box = lv_obj_create(lists_row);
        lv_obj_remove_style_all(box);
        lv_obj_set_flex_grow(box, 1);
        lv_obj_set_height(box, lv_pct(100));
        lv_obj_set_layout(box, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(box, 1, 0);
        lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);

        ui_make_kicker(box, col == 0 ? "平台 · 今日" : "国家 · 今日",
                       COL_ACCENT, font_sm);
        for(int i = 0; i < OV_LINES; i++) {
            build_line(box, col == 0 ? &plat_lines[i] : &ctry_lines[i], font_sm);
        }
    }

    /* ---- 第三行：事务三格 ---- */
    lv_obj_t * txn_row = lv_obj_create(page_root);
    lv_obj_remove_style_all(txn_row);
    lv_obj_set_width(txn_row, lv_pct(100));
    lv_obj_set_height(txn_row, 72);
    lv_obj_set_layout(txn_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(txn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(txn_row, 8, 0);
    lv_obj_remove_flag(txn_row, LV_OBJ_FLAG_SCROLLABLE);
    for(int i = 0; i < OV_TXN_CELLS; i++)
        build_cell(txn_row, &txn_cells[i], OV_TXN_TITLES[i], font_sm, font_lg);
}

void overview_page_update(const panel_snapshot_t * s)
{
    if(page_root == NULL || s == NULL) return;

    char value[UI_CACHE_LEN];
    char sub[UI_CACHE_LEN];
    char rmb[16];
    char hhmm[8];
    const bool ops_live = s->ops_ok && !s->stale;

    /* ============ 销售三格：大字 = 件数，副行 = 单量 + RMB ============ */
    if(ops_live) {
        snprintf(value, sizeof(value), "%.0f 件", s->today_units);
        fmt_rmb(s->today_rmb, rmb, sizeof(rmb));
        snprintf(sub, sizeof(sub), "%.0f单 RMB%s", s->today_orders, rmb);
        set_cell(&sales_cells[0], value, sub, COL_ACCENT);
    }
    else {
        set_cell(&sales_cells[0], "--", s->ops_ok ? "数据已过期" : "取数失败", COL_AMBER);
    }

    if(ops_live) {
        snprintf(value, sizeof(value), "%.0f 件", s->yday_units);
        fmt_rmb(s->yday_rmb, rmb, sizeof(rmb));
        /* 美国站自然日北京时间 15:00 才收口，此前标 * 提示可能上修 */
        snprintf(sub, sizeof(sub), "%.0f单 RMB%s%s", s->yday_orders, rmb,
                 s->yday_final ? "" : "*");
        set_cell(&sales_cells[1], value, sub, COL_ACCENT);
    }
    else {
        set_cell(&sales_cells[1], "--", s->ops_ok ? "数据已过期" : "取数失败", COL_AMBER);
    }

    if(ops_live && s->month_days > 0) {
        snprintf(value, sizeof(value), "%.0f 件", s->month_units);
        fmt_rmb(s->month_rmb, rmb, sizeof(rmb));
        snprintf(sub, sizeof(sub), "%.0f单 RMB%s · %d天",
                 s->month_orders, rmb, s->month_days);
        set_cell(&sales_cells[2], value, sub, COL_ACCENT);
    }
    else {
        set_cell(&sales_cells[2], "--", s->ops_ok ? "数据已过期" : "取数失败", COL_AMBER);
    }

    /* ============ 平台 / 国家（今日；国家已跨平台合并）============ */
    if(ops_live) {
        fill_lines(plat_lines, OV_LINES, s->platforms, s->platform_count);
        fill_lines(ctry_lines, OV_LINES, s->countries, s->country_count);
    }
    else {
        for(int i = 0; i < OV_LINES; i++) {
            lv_obj_add_flag(plat_lines[i].row, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(ctry_lines[i].row, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* ============ 事务三格 ============ */
    if(ops_live) {
        snprintf(value, sizeof(value), "%d", s->stockout_total);
        snprintf(sub, sizeof(sub), "缺%d 低%d", s->stockout_lack, s->stockout_low);
        set_cell(&txn_cells[0], value, sub,
                 (s->stockout_total > 0) ? COL_AMBER : COL_ACCENT);
    }
    else {
        set_cell(&txn_cells[0], "--", "取数失败", COL_AMBER);
    }

    if(s->mail_ok && s->mail_drafts_pending > 0) {
        snprintf(value, sizeof(value), "%d", s->mail_drafts_pending);
        snprintf(sub, sizeof(sub), "最久 %.0fh", s->mail_oldest_hours);
        set_cell(&txn_cells[1], value, sub, COL_AMBER);
    }
    else if(s->mail_ok) {
        set_cell(&txn_cells[1], "0", "全部已处理", COL_ACCENT);
    }
    else {
        set_cell(&txn_cells[1], "--", "邮件服务离线", COL_TEXT_DIM);
    }

    /* 心跳断了 → 任务状态不可信，别当成"任务停摆" */
    if(s->dev_ok) {
        snprintf(value, sizeof(value), "%d/%d", s->jobs_ok, s->jobs_total);
        if(s->jobs_ok >= s->jobs_total) {
            snprintf(sub, sizeof(sub), "项目 %d", s->project_count);
            set_cell(&txn_cells[2], value, sub, COL_ACCENT);
        }
        else {
            snprintf(sub, sizeof(sub), "%d项异常 · 项目%d",
                     s->jobs_total - s->jobs_ok, s->project_count);
            set_cell(&txn_cells[2], value, sub, COL_AMBER);
        }
    }
    else {
        snprintf(sub, sizeof(sub), "已 %.0f 分钟", s->dev_age_minutes);
        set_cell(&txn_cells[2], "--", sub, COL_TEXT_DIM);
    }

    /* ============ 状态行：运营时间 · 汇率 · 今天的年月日 ============ */
    char status[96];
    char date_txt[48] = "";
    time_t nowt = time(NULL);
    struct tm lt;
    if(localtime_r(&nowt, &lt) != NULL) {
        snprintf(date_txt, sizeof(date_txt), "%d年%d月%d日",
                 lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday);
    }

    char fx_txt[20] = "";
    if(s->fx_usd_cny > 0) {
        snprintf(fx_txt, sizeof(fx_txt), " · 汇率%.2f%s",
                 s->fx_usd_cny, s->fx_ok ? "" : "(旧)");
    }

    /* 心跳正常时不再占位；只有离线才提示。
     * 不用警告三角符号 —— 它不在 GB2312 里，16px 字库可能缺字；颜色已表达警示。 */
    char mac_warn[20] = "";
    if(!s->dev_ok) snprintf(mac_warn, sizeof(mac_warn), " · Mac离线");

    if(!s->valid) {
        snprintf(status, sizeof(status), "中枢离线 · 上次数据 · %s", date_txt);
    }
    else if(s->stale) {
        snprintf(status, sizeof(status), "运营数据已过期 · %s", date_txt);
    }
    else {
        iso_hhmm(s->ops_fetched_at, hhmm, sizeof(hhmm));
        snprintf(status, sizeof(status), "运营 %s%s · %s%s",
                 hhmm[0] != '\0' ? hhmm : "ok", fx_txt, date_txt, mac_warn);
    }

    lv_obj_set_style_text_color(status_lbl,
                                (s->valid && !s->stale) ? COL_TEXT_DIM : COL_AMBER, 0);
    ui_set_label_cached(status_lbl, status_cache, sizeof(status_cache), status);
}
