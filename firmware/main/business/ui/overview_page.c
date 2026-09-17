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
#include "ui_fonts.h"
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
/* 状态行最长文案约 49 字节（「中枢离线 · 上次数据 · 2026年9月17日」），
 * 恰好超过 UI_CACHE_LEN(48) —— 这是原有代码就存在的隐患，2026-09-17 一并修掉。
 * 与生成该文案的 status[] 保持同尺寸。 */
#define OV_STATUS_LEN 96
static char        status_cache[OV_STATUS_LEN];
static lv_obj_t *  alert_bar;                 /* 数据过期 / 中枢离线的醒目横幅 */
static lv_obj_t *  alert_lbl;
/* 横幅文案最长约 56 字节（「运营数据未更新 · 最后 00:55（6.1 小时前）」），
 * 比 UI_CACHE_LEN(48) 长，故单独给足。**与生成该文案的 alert[] 必须同尺寸**。
 * ⚠️ 编译器**抓不到**这一处：文案是 snprintf 运行时生成的，不触发
 * -Werror=string-compare（那条只对字面量生效）。缓存短于文案时
 * ui_set_label_cached 的 strcmp 去重会把不同文案误判成「没变」而漏刷新。 */
#define OV_ALERT_LEN 96
static char        alert_cache[OV_ALERT_LEN];
static void refresh_click_cb(lv_event_t * e);  /* 前向声明：定义在 create 之后 */

static lv_obj_t *  refresh_btn;               /* 顶部「刷新」按钮 */
static lv_obj_t *  refresh_lbl;
static char        refresh_cache[16];
static bool        refresh_busy;              /* 刷新进行中：防连点 + 改按钮文案 */
static lv_obj_t *  lists_row;                 /* 平台 / 国家 两列 */
static lv_obj_t *  lists_fallback;            /* 上面那块的替代说明（二者互斥显示） */
static lv_obj_t *  list_hint_lbl;
/* 提示文案含换行，比 UI_CACHE_LEN 长，故单独给足缓冲。
 * ⚠️ 缓存**绝不能短于文案**：ui_set_label_cached 用 strcmp 去重，
 * 截断后两个前缀相同的长文案会被误判为「没变」，从而漏掉应有的刷新。
 * （这条是 -Werror=string-compare 抓出来的，不要为了过编译而关掉该警告。） */
#define OV_HINT_CACHE_LEN 96
static char        list_hint_cache[OV_HINT_CACHE_LEN];
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

/** 工业读数盒：标题骑在边框上 + 大读数 + 副行。
 *
 * 与旧版（kicker 在盒内）的区别：
 *   - 标题**骑在盒子上边框上**、底色与屏底一致以切断边框（工业 HMI 惯用法）
 *   - ⚠️ 标题占盒子上方约 13px。**调用方的行高与行间距必须留出这个余量**，
 *     否则标题会压到上一个带的内容（效果图第一版就踩过，三格底部被切）。
 *   - 大读数色改为 COL_ACCENT（琥珀磷光）；字体由调用方给：
 *     销售格用 34px 纯数字字体（font_num_34），事务格用 20px 中文字体。 */
static void build_cell(lv_obj_t * parent, ov_cell_t * c, const char * title,
                       const lv_font_t * font_sm, const lv_font_t * font_val)
{
    lv_obj_t * card = ui_titled_box(parent, title, font_sm);
    lv_obj_set_flex_grow(card, 1);
    lv_obj_set_height(card, lv_pct(100));
    lv_obj_set_layout(card, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(card, 7, 0);
    lv_obj_set_style_pad_row(card, 0, 0);

    c->value = lv_label_create(card);
    lv_obj_set_style_text_font(c->value, font_val, 0);
    lv_obj_set_style_text_color(c->value, COL_ACCENT, 0);
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
    lv_obj_set_style_pad_column(row, 5, 0);   /* 引导线别贴住文字 */
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    L->name = lv_label_create(row);
    lv_obj_set_style_text_font(L->name, font_sm, 0);
    lv_obj_set_style_text_color(L->name, COL_TEXT, 0);
    lv_label_set_text(L->name, "");
    L->name_cache[0] = '\0';

    /* 引导线：名称与数值之间的细线（效果图里的点线引导符）。
     * 用 flex_grow 吃掉中间剩余宽度 —— 名称自然左对齐、数值仍右对齐，
     * 线自动铺满中间，不需要任何绝对定位。
     *
     * ⚠️ 这是**降级实现**：LVGL 没有虚线/点线，ui_leader_line() 画的是
     * 1px 暗实线。真点线要每个点一个对象（一行十来个点）—— 16 行就是上百个
     * 对象，RAM 与重绘都不划算。 */
    lv_obj_t * lead = ui_leader_line(row);
    lv_obj_set_flex_grow(lead, 1);

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

/** 填充一列列表（平台或国家）；超出 @p item_count 的行隐藏。
 *  @param dim 数据陈旧时压暗名称，让「这不是实时的」有视觉区分。 */
static void fill_lines(ov_line_t * lines, int count,
                       const panel_rank_t * items, int item_count, bool dim)
{
    char val[UI_CACHE_LEN];
    char trend[UI_CACHE_LEN];
    for(int i = 0; i < count; i++) {
        if(i >= item_count) {
            lv_obj_add_flag(lines[i].row, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(lines[i].row, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_color(lines[i].name,
                                    dim ? COL_TEXT_DIM : COL_TEXT, 0);
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
    /* 纵向预算（可用 436px）：pad_top 16 + 状态行 24 + 销售 90 + 事务 76
     * + 行间距 10x3 = 236，剩 200 给平台/国家两列（含其 13px 标题位）。 */
    lv_obj_set_style_pad_top(page_root, 16, 0);
    lv_obj_set_style_pad_row(page_root, 10, 0);
    lv_obj_remove_flag(page_root, LV_OBJ_FLAG_SCROLLABLE);   /* 一屏放下，不滚动 */

    /* ---- 顶部状态行：运营状态（左）+ 刷新按键（右）----
     *
     * 工业风下**不再有页面级 kicker**（原「事务总览 OVERVIEW」）。
     * 理由：480px 上纵向空间是本改版最紧张的资源 —— 工业风的盒子标题要骑在
     * 边框上、每个带多吃 13px。设备标识由外壳顶栏承担（step 5），
     * 页面归属由底部导航标签承担，页面内再重复一遍标题是纯浪费。
     * 省下的 26px + 间距正好补上各带标题所需的空间。 */
    lv_obj_t * status_row = lv_obj_create(page_root);
    lv_obj_remove_style_all(status_row);
    lv_obj_set_width(status_row, lv_pct(100));
    lv_obj_set_height(status_row, 24);
    lv_obj_set_layout(status_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(status_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(status_row, LV_OBJ_FLAG_SCROLLABLE);

    status_lbl = lv_label_create(status_row);
    lv_obj_set_style_text_font(status_lbl, font_sm, 0);
    lv_obj_set_style_text_color(status_lbl, COL_TEXT_DIM, 0);
    lv_label_set_text(status_lbl, "正在获取数据…");
    refresh_btn = ui_bevel_button(status_row, "刷新", font_sm);
    lv_obj_set_size(refresh_btn, 76, 24);
    refresh_lbl = lv_obj_get_child(refresh_btn, 0);
    refresh_cache[0] = '\0';
    lv_obj_add_event_cb(refresh_btn, refresh_click_cb, LV_EVENT_CLICKED, NULL);

    /* ---- 过期/离线横幅：默认隐藏，出问题时才占位 ----
     *
     * 2026-09-16 新增。起因：中枢取数失败时会用上一次的好数据兜底（served_from
     * = last_good），但本页原先把这份兜底数据全扔掉 —— 三个格子显示 "--"，
     * 平台/国家区 16 行整片留白 —— 结果整个页面看起来像「坏了、没数据」，
     * 用户实际就报了「怎么没数据了」。
     *
     * 因此把「数据是旧的」这件事做成**一眼可见**：深色字压琥珀实底，
     * 在暗色 UI 上对比最强，且放在状态行正下方、销售格之上，视线必经之处。
     *
     * 取值策略（同日按用户决定调整）：**照常显示上一次的可用数据**，
     * 不再退化成 "--"（旧数据也比没有数据有用）。仅在确实没有数据可用时
     * （fetched_at 为空）才显示 "--"。
     *
     * 布局注意：本条 26px 会挤压下方列表行数，故横幅可见时列表**仍在显示**
     * （有旧数据的情况），需要确认 OV_LINES 行仍放得下 —— 见 build 后的实测。
     * 只有「完全无数据」时列表才被 lists_fallback 取代，那种情况下空间反而更宽裕。 */
    alert_bar = lv_obj_create(page_root);
    lv_obj_remove_style_all(alert_bar);
    lv_obj_set_width(alert_bar, lv_pct(100));
    lv_obj_set_height(alert_bar, 26);
    lv_obj_set_style_bg_color(alert_bar, COL_AMBER, 0);
    lv_obj_set_style_bg_opa(alert_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(alert_bar, 5, 0);
    lv_obj_set_style_pad_hor(alert_bar, 8, 0);
    lv_obj_set_layout(alert_bar, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(alert_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(alert_bar, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(alert_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(alert_bar, LV_OBJ_FLAG_HIDDEN);

    alert_lbl = lv_label_create(alert_bar);
    lv_obj_set_style_text_font(alert_lbl, font_sm, 0);
    lv_obj_set_style_text_color(alert_lbl, COL_BAR, 0);   /* 深色字压琥珀底 */
    lv_label_set_text(alert_lbl, "");
    alert_cache[0] = '\0';

    /* ---- 第一行：销售三格 ---- */
    lv_obj_t * sales_row = lv_obj_create(page_root);
    lv_obj_remove_style_all(sales_row);
    lv_obj_set_width(sales_row, lv_pct(100));
    lv_obj_set_height(sales_row, 90);
    lv_obj_set_style_pad_top(sales_row, 13, 0);   /* 给骑在边框上的标题留位 */
    lv_obj_add_flag(sales_row, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_set_layout(sales_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(sales_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(sales_row, 8, 0);
    lv_obj_remove_flag(sales_row, LV_OBJ_FLAG_SCROLLABLE);
    for(int i = 0; i < OV_SALES_CELLS; i++)
        /* 销售：34px 纯数字磷光读数 */
        build_cell(sales_row, &sales_cells[i], OV_SALES_TITLES[i], font_sm,
                   ui_fonts_num());

    /* ---- 第二段：平台 / 国家 两列并排（吃掉剩余高度）---- */
    lists_row = lv_obj_create(page_root);
    lv_obj_remove_style_all(lists_row);
    lv_obj_set_width(lists_row, lv_pct(100));
    lv_obj_set_flex_grow(lists_row, 1);
    lv_obj_set_layout(lists_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(lists_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(lists_row, 10, 0);
    lv_obj_set_style_pad_top(lists_row, 13, 0);   /* 同上：标题位 */
    lv_obj_add_flag(lists_row, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_remove_flag(lists_row, LV_OBJ_FLAG_SCROLLABLE);

    for(int col = 0; col < 2; col++) {
        /* 列表面板也用**带标题边框盒**（step 3 只改了读数盒，这里补上）——
         * 边框 + 骑在边框上的标题，与读数盒同一套语言。
         *
         * ⚠️ 顺带修掉一处会切行的浪费：lists_row 已按 13px 预留了标题位，
         *    但面板仍在**盒内**画 kicker，等于 13px 白占 + 盒内又占 20px。
         *    按实际常量核算：列表区可用 187px，而「盒内 kicker 20 + 8 行×20 +
         *    行间距 7」正好 = 187 —— **零余量**，最后一行随时可能被切。
         *    改用骑边框标题后不再需要盒内 kicker，正好把这 20px 还给行：
         *    需要 167 vs 可用 187，留出 20px 余量。
         *
         * 这类「预留了资源却没用上」的浪费光看代码看不出来，
         * 是**把常量逐个加起来核算**才发现的。 */
        lv_obj_t * box = ui_titled_box(lists_row,
                                       col == 0 ? "平台 · 今日" : "国家 · 今日",
                                       font_sm);
        lv_obj_set_flex_grow(box, 1);
        lv_obj_set_height(box, lv_pct(100));
        lv_obj_set_layout(box, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_all(box, 4, 0);
        lv_obj_set_style_pad_row(box, 1, 0);
        for(int i = 0; i < OV_LINES; i++) {
            build_line(box, col == 0 ? &plat_lines[i] : &ctry_lines[i], font_sm);
        }
    }

    /* ---- 列表不可用时的替代说明（与 lists_row 互斥显示）----
     *
     * 原先的情况：取数失败时把 16 行列表全部隐藏，该区域变成一整片空白，
     * 没有任何文字说明 —— 这是「看起来像坏了」的主要来源。
     * 现在改为显示居中的两行说明，并与 lists_row 交替出现。
     * 二者都设 flex_grow(1)，而 LVGL 的 flex 布局会跳过隐藏对象，故切换时不需手动调尺寸。 */
    lists_fallback = lv_obj_create(page_root);
    lv_obj_remove_style_all(lists_fallback);
    lv_obj_set_width(lists_fallback, lv_pct(100));
    lv_obj_set_flex_grow(lists_fallback, 1);
    lv_obj_set_layout(lists_fallback, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(lists_fallback, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(lists_fallback, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(lists_fallback, 4, 0);
    lv_obj_remove_flag(lists_fallback, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(lists_fallback, LV_OBJ_FLAG_HIDDEN);

    list_hint_lbl = lv_label_create(lists_fallback);
    lv_obj_set_style_text_font(list_hint_lbl, font_sm, 0);
    lv_obj_set_style_text_color(list_hint_lbl, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_align(list_hint_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(list_hint_lbl, lv_pct(100));
    lv_label_set_text(list_hint_lbl, "");
    list_hint_cache[0] = '\0';

    /* ---- 第三行：事务三格 ---- */
    lv_obj_t * txn_row = lv_obj_create(page_root);
    lv_obj_remove_style_all(txn_row);
    lv_obj_set_width(txn_row, lv_pct(100));
    lv_obj_set_height(txn_row, 76);
    lv_obj_set_style_pad_top(txn_row, 13, 0);
    lv_obj_add_flag(txn_row, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_set_layout(txn_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(txn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(txn_row, 8, 0);
    lv_obj_remove_flag(txn_row, LV_OBJ_FLAG_SCROLLABLE);
    for(int i = 0; i < OV_TXN_CELLS; i++)
        /* 事务盒小得多，用 20px 中文字体（34px 会撑破盒高） */
        build_cell(txn_row, &txn_cells[i], OV_TXN_TITLES[i], font_sm, font_lg);
}

/* --------------------------------------------------------------------------
 * 手动刷新
 * -------------------------------------------------------------------------- */

/** 「刷新」按钮：请中枢立刻采集一轮，然后重新拉取。
 *
 * 关键：**本回调里绝不能做网络请求**。panel_client_refresh() 会阻塞 2–5 秒
 * （中枢在服务端同步等采集完成），若在 LVGL 线程里做，整个界面会僵住。
 * 所以这里只投递一条控制命令；真正的网络动作由 net_worker 的工作线程执行。 */
static void refresh_click_cb(lv_event_t * e)
{
    (void)e;
    if(refresh_busy) return;                 /* 防连点：一次刷新要几秒，别排队 */

    refresh_busy = true;
    /* 立刻给反馈：否则用户不知道按钮是否生效，会反复戳 */
    ui_set_label_cached(refresh_lbl, refresh_cache, sizeof(refresh_cache), "刷新中");
    lv_obj_add_state(refresh_btn, LV_STATE_DISABLED);
    net_worker_request_panel_refresh();
}

void overview_page_update(const panel_snapshot_t * s)
{
    if(page_root == NULL || s == NULL) return;

    char value[UI_CACHE_LEN];
    char sub[UI_CACHE_LEN];
    char rmb[16];
    char hhmm[8];
    time_t nowt = time(NULL);

    /* ⚠️ 不要在固件里自己判「数据放了多久」—— 中枢已经判了，而且它才是权威。
     *
     * 2026-09-17 的教训：为支持「凌晨 1–8 点不采集」，我先在固件里按 fetched_at
     * 算了一次数据年龄（阈值 15 分钟）。写完才发现中枢的 load_panel()
     * **早在服务时就会按年龄判过期**（STALE_AFTER_SECONDS = 90 分钟），
     * 并给出 `stale_reason` 字段（"数据已 N 分钟未成功刷新"）。
     * 我等于造了第二份会漂移的事实源，而且两处阈值还不一致。
     *
     * 现在职责划分明确：**判过期（数据问题）归中枢，怎么显示（展示问题）归这里。**
     * 固件只忠实采用中枢的 stale / stale_reason，不自行推断。 */
    const bool ops_live = s->ops_ok && !s->stale;

    /* 「有可用的上次数据」判据 = ops.fetched_at 非空。
     *
     * 中枢在取数失败时会沿用上次成功的数据，并且**绝不改写 fetched_at**
     * （成功才写当前时间；失败且无兜底时为 null）。因此：
     *   fetched_at 非空 ⟺ 这组数字确实来自某次真实成功采集（可能已经旧了）
     *   fetched_at 为空 ⟺ 从来没有取到过数，此时才真的该显示 "--"
     *
     * 2026-09-16 用户决定：**宁可显示上一次的可用数据，也不要 "--"**。
     * 原先本页把中枢辛苦留下的兜底数据全扔掉（连列表 16 行都隐藏留白），
     * 结果一次瞬时抖动就让整块看板看起来像坏了 —— 与中枢
     * 「0 和不知道是两件完全不同的事」的设计原则相违背。
     * 现在改为：照常显示旧数值，但用**三重视觉区分**表明它不是实时的：
     *   1) 顶部醒目的琥珀横幅（写明最后更新时刻）
     *   2) 数值改用琥珀色（实时为薄荷青）
     *   3) 列表名称压暗
     * 只有真的无数据可用时才回到 "--"。 */
    const bool ops_usable = ops_live || (s->ops_fetched_at[0] != '\0');
    const lv_color_t ops_col = ops_live ? COL_ACCENT : COL_AMBER;

    /* ============ 过期/离线横幅 ============ */
    /* 措辞刻意区分**四种**情况，因为它们的处置完全不同：
     *   1) 联系不上中枢        → 数据已停止更新
     *   2) 采集失败但有兜底    → 运营取数失败
     *   3) 采集成功但数据放久了 → 运营数据未更新（典型场景：凌晨 1–8 点不采集）
     *   4) 完全无数据可显示     → 暂无可显示数据
     * 2 与 3 **必须措辞不同**：夜间暂停每天都会发生，若都写「取数失败」，
     * 用户每天早上都会把它误读成故障，久之就对该提示脱敏了。
     * 且必须带「最后 HH:MM」—— 用户最需要知道的是「数据停在什么时候」，
     * 只说「已过期」他无法判断是刚断的还是断了一上午。 */
    char alert[OV_ALERT_LEN] = "";
    if(!s->valid) {
        snprintf(alert, sizeof(alert), "中枢离线 · 数据已停止更新");
    }
    else if(!ops_live) {
        iso_hhmm(s->ops_fetched_at, hhmm, sizeof(hhmm));

        if(!ops_usable) {
            /* 从来没有取到过数：这才是真的没东西可显示 */
            snprintf(alert, sizeof(alert), "运营取数失败 · 暂无可显示数据");
        }
        else if(s->ops_stale_reason[0] != '\0') {
            /* 中枢说「数据放久了」（它按年龄判的过期，典型场景是夜间不采集）——
             * 直接采用中枢给的措辞。这**不是取数失败**，措辞必须区分：
             * 夜间暂停每天都会发生，若也写「取数失败」，用户每天早上都会误读成
             * 故障，久之就对该提示脱敏了。 */
            if(hhmm[0] != '\0')
                snprintf(alert, sizeof(alert), "%s · 最后 %s",
                         s->ops_stale_reason, hhmm);
            else
                snprintf(alert, sizeof(alert), "%s", s->ops_stale_reason);
        }
        else if(hhmm[0] != '\0') {
            /* 中枢没给 stale_reason ⇒ 是这一轮采集真的失败了 */
            snprintf(alert, sizeof(alert), "运营取数失败 · 显示 %s 的数据", hhmm);
        }
        else {
            snprintf(alert, sizeof(alert), "运营取数失败 · 显示上次数据");
        }
    }
    if(alert[0] != '\0') {
        ui_set_label_cached(alert_lbl, alert_cache, sizeof(alert_cache), alert);
        lv_obj_remove_flag(alert_bar, LV_OBJ_FLAG_HIDDEN);
    }
    else {
        lv_obj_add_flag(alert_bar, LV_OBJ_FLAG_HIDDEN);
    }

    /* ============ 销售三格：大字 = 件数，副行 = 单量 + RMB ============ */
    if(ops_usable) {
        snprintf(value, sizeof(value), "%.0f 件", s->today_units);
        fmt_rmb(s->today_rmb, rmb, sizeof(rmb));
        snprintf(sub, sizeof(sub), "%.0f单 RMB%s", s->today_orders, rmb);
        set_cell(&sales_cells[0], value, sub, ops_col);
    }
    else {
        set_cell(&sales_cells[0], "--", "取数失败", COL_AMBER);
    }

    if(ops_usable) {
        snprintf(value, sizeof(value), "%.0f 件", s->yday_units);
        fmt_rmb(s->yday_rmb, rmb, sizeof(rmb));
        /* 美国站自然日北京时间 15:00 才收口，此前标 * 提示可能上修 */
        snprintf(sub, sizeof(sub), "%.0f单 RMB%s%s", s->yday_orders, rmb,
                 s->yday_final ? "" : "*");
        set_cell(&sales_cells[1], value, sub, ops_col);
    }
    else {
        set_cell(&sales_cells[1], "--", "取数失败", COL_AMBER);
    }

    if(ops_usable && s->month_days > 0) {
        snprintf(value, sizeof(value), "%.0f 件", s->month_units);
        fmt_rmb(s->month_rmb, rmb, sizeof(rmb));
        snprintf(sub, sizeof(sub), "%.0f单 RMB%s · %d天",
                 s->month_orders, rmb, s->month_days);
        set_cell(&sales_cells[2], value, sub, ops_col);
    }
    else {
        set_cell(&sales_cells[2], "--", "取数失败", COL_AMBER);
    }

    /* ============ 平台 / 国家（今日；国家已跨平台合并）============ */
    /* 横幅占 26px，与列表**同时出现**时会把高度从列表区挤掉。
     * 不靠估算像素，直接在有横幅时少显示一行 —— 陈旧状态下少一行明细，
     * 好过把最后一行裁掉一半（那也是「看起来坏了」的一种）。 */
    const int list_lines = (alert[0] != '\0') ? (OV_LINES - 1) : OV_LINES;

    if(ops_usable) {
        /* dim = 数据陈旧：名称压暗，数值照常显示（旧数据也比没有好） */
        fill_lines(plat_lines, list_lines, s->platforms, s->platform_count, !ops_live);
        fill_lines(ctry_lines, list_lines, s->countries, s->country_count, !ops_live);
        /* 被降到不用的行必须显式隐藏，否则会残留上一次的内容 */
        for(int i = list_lines; i < OV_LINES; i++) {
            lv_obj_add_flag(plat_lines[i].row, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(ctry_lines[i].row, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_remove_flag(lists_row, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lists_fallback, LV_OBJ_FLAG_HIDDEN);
    }
    else {
        for(int i = 0; i < OV_LINES; i++) {
            lv_obj_add_flag(plat_lines[i].row, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(ctry_lines[i].row, LV_OBJ_FLAG_HIDDEN);
        }
        /* 真的一条数据都没有时才走这里：给一句明确交代，并说明会自愈 */
        ui_set_label_cached(
            list_hint_lbl, list_hint_cache, sizeof(list_hint_cache),
            "平台 / 国家明细暂无数据\n下一轮采集成功后自动显示");
        lv_obj_add_flag(lists_row, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(lists_fallback, LV_OBJ_FLAG_HIDDEN);
    }

    /* ============ 事务三格 ============ */
    /* 缺货数同样来自 ops 块，故与销售格共用 ops_usable 判据 */
    if(ops_usable) {
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
    char status[OV_STATUS_LEN];
    char date_txt[48] = "";
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
    else if(!ops_live) {
        /* 采集失败与「放久了」合并成一句：只说「已过期」，具体原因由顶部横幅给 */
        snprintf(status, sizeof(status), "运营数据非实时 · %s", date_txt);
    }
    else {
        iso_hhmm(s->ops_fetched_at, hhmm, sizeof(hhmm));
        snprintf(status, sizeof(status), "运营 %s%s · %s%s",
                 hhmm[0] != '\0' ? hhmm : "ok", fx_txt, date_txt, mac_warn);
    }

    lv_obj_set_style_text_color(status_lbl,
                                (s->valid && ops_live) ? COL_TEXT_DIM : COL_AMBER, 0);
    ui_set_label_cached(status_lbl, status_cache, sizeof(status_cache), status);

    /* 刷新流程走完 → 复位按钮文案与可点状态。
     * 本函数由 ui_drain 在 panel_seq 变化时调用，而手动刷新**无论成败**都会
     * 自增 seq（见 net_worker.c），所以按钮绝不会卡在「刷新中」出不来。 */
    if(refresh_busy) {
        refresh_busy = false;
        ui_set_label_cached(refresh_lbl, refresh_cache, sizeof(refresh_cache), "刷新");
        lv_obj_remove_state(refresh_btn, LV_STATE_DISABLED);
    }
}
