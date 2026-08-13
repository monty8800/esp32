/**
 * @file ui_drain.h
 *
 * Snapshot drain bridge between the net_worker mailbox and the LVGL UI.
 *
 * *** M4 里程碑接入 ***
 * 本文件是受控复制：回调体与 src/main.c 的 ui_drain_cb 保持一致。
 * M3 阶段它尚未加入 CMakeLists SRCS，app_main 也不调用它 —— 因为
 * net_worker 还没有链接进固件，过早接线会导致链接失败。
 * M4（net_worker + WiFi 接入）时：把 ui_drain.c 加入 SRCS，并在
 * net_worker_start() 之后调用 ui_drain_start() 即可。
 */

#ifndef UI_DRAIN_H
#define UI_DRAIN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Drain period: UI copies worker snapshots 10x per second. */
#define UI_DRAIN_PERIOD_MS 100

/**
 * Create the 100ms lv_timer that drains all three net_worker snapshots
 * into the UI (seq de-dup inside the pages, minute-granular clock).
 * Must be called under the LVGL lock, after the UI has been created and
 * net_worker is linked (M4).
 */
void ui_drain_start(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_DRAIN_H */
