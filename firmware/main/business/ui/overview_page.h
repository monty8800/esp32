/**
 * @file overview_page.h
 *
 * 事务总览页 —— tile 4，panel-hub 快照的展示层。
 *
 * 六格布局（2 行 × 3 列）：
 *   今日销量 / 昨日销量 / 缺货
 *   待审草稿 / 定时任务 / 项目
 *
 * 设计纪律（见 local-server/20-LXC-117-panel-hub.md）：
 *   每个数据块看自己的 ok，**不能只看顶层 stale** ——
 *   邮件或 Mac 心跳挂了，不该让销量格跟着变灰。
 *   因此每格都按对应来源的 ok 独立降级为 "--"。
 */

#ifndef OVERVIEW_PAGE_H
#define OVERVIEW_PAGE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"

#include "net_worker.h"

/**
 * Build the page inside @p parent (the tile container from ui_shell).
 * @p font_sm is used for kickers / sub lines, @p font_lg for the big value.
 */
void overview_page_create(lv_obj_t * parent, const lv_font_t * font_sm,
                          const lv_font_t * font_lg);

/**
 * Render a fresh panel-hub snapshot. Call from the drain timer only (i.e.
 * under the LVGL lock). Text is set through a per-label cache so an
 * unchanged value does not trigger a redraw.
 */
void overview_page_update(const panel_snapshot_t * s);

#ifdef __cplusplus
}
#endif

#endif /* OVERVIEW_PAGE_H */
