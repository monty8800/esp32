/**
 * @file panel_client.h
 *
 * Client for the LAN "panel hub" (LXC 117 panel-hub on Proxmox) — the single
 * backend of the desk panel.
 *
 * Endpoint: GET http://192.168.9.216:8790/api/panel
 *   - no authentication (LAN-only service; optional X-Panel-Token if the
 *     operator sets PANEL_API_TOKEN on the hub)
 *   - the hub serves a PRE-AGGREGATED json from a state file, so the answer
 *     is fast (~10ms) and does NOT depend on upstream health: the hub keeps
 *     polling LixingXing / mail-ai-service on its own schedule and flags
 *     stale data in the payload.
 *   - payload carries three independent blocks, each with its own "ok":
 *       ops  -> LixingXing sales + FBA stockout   (core, drives top-level stale)
 *       mail -> mail-ai-service pending drafts
 *       dev  -> Mac push heartbeat (cron jobs + projects)
 *     The UI must consult the PER-BLOCK ok, never only the top-level stale.
 *
 * The URL can be overridden with the PANEL_URL environment variable (fed from
 * the "panel_url" NVS key by env_shim.c).
 *
 * Only ever called from the net_worker thread - never from the UI thread.
 */

#ifndef PANEL_CLIENT_H
#define PANEL_CLIENT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

#include "net_worker.h"

#define PANEL_API_URL_DEFAULT "http://192.168.9.216:8790/api/panel"

/**
 * Fetch and parse /api/panel into @p out. On success fills the whole
 * snapshot (valid=true) and returns true. On any failure (connect, timeout,
 * HTTP error, malformed JSON) returns false and leaves @p out untouched, so
 * the caller can keep the previous data and mark it stale. The seq field is
 * never touched here.
 */
bool panel_client_fetch(panel_snapshot_t * out);

/**
 * 请中枢**立即采集一轮**（总览页「刷新」按钮的后端）。
 *
 * 用途：中枢 01:00–08:00 停止采集，早上进办公室时想立刻看到最新数字，
 * 而不必等到 08:00 那一轮。**有意绕过夜间排程** —— 这正是手动刷新存在的意义。
 *
 * ⚠️ 这是**阻塞**调用：中枢会同步等待采集完成（实测 2–5 秒）再响应。
 * 必须在工作线程里调用，绝不能在 LVGL/UI 线程里调用（会卡住界面）。
 *
 * 返回 true 仅表示「请求被受理且采集已完成」，**不代表数据一定更新了**
 * （可能命中中枢的冷却保护）。调用方应紧接着调用 panel_client_refresh_fetch()，
 * 并以拿到的 generated_at 为准。
 *
 * 目标 URL 由 PANEL_URL 自动推出（把最后一段路径换成 refresh），
 * 这样配置项仍然只有一个 host，不会出现两处填不一致的问题。
 */
bool panel_client_refresh(void);

/**
 * panel_client_refresh() + panel_client_fetch() 的组合：
 * 先请中枢采集，再拉一次快照。返回 panel_client_fetch() 的结果。
 */
bool panel_client_refresh_fetch(panel_snapshot_t * out);

#ifdef __cplusplus
}
#endif

#endif /* PANEL_CLIENT_H */
