/**
 * @file dashboard_page.h
 *
 * Page 1 (read-only): Shenzhen outdoor weather hero card on top (fed from
 * the weather snapshot), a row of three indoor metric cards below
 * (temperature / humidity / PM2.5, fed from the HA snapshot).
 */

#ifndef DASHBOARD_PAGE_H
#define DASHBOARD_PAGE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"
#include "../net_worker.h"

/**
 * Build the dashboard page inside @p parent (a tileview tile).
 * @param font_sm CJK 16px font (NULL -> Montserrat 14)
 * @param font_lg CJK 20px font (NULL -> Montserrat 20)
 */
void dashboard_page_create(lv_obj_t * parent, const lv_font_t * font_sm,
                           const lv_font_t * font_lg);

/**
 * Refresh the indoor metric cards from a snapshot; invalid fields render
 * as "--". Safe to call before create (no-op).
 */
void dashboard_page_update(const ha_snapshot_t * s);

/**
 * Refresh the Shenzhen weather hero card from a snapshot; valid==false
 * renders "--" for the temperature and "离线" for the description.
 * Safe to call before create (no-op).
 */
void dashboard_page_update_weather(const weather_snapshot_t * w);

#ifdef __cplusplus
}
#endif

#endif /* DASHBOARD_PAGE_H */
