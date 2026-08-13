/**
 * @file devices_page.h
 *
 * Page 2 (controllable): device cards for the Midea AC, the Xiaomi air
 * purifier (power + 3 modes), the Mijia monitor light bar and the two
 * Xiaomi cameras (read-only status). Controls are posted to the network
 * worker queue (non-blocking); the next poll corrects the UI.
 */

#ifndef DEVICES_PAGE_H
#define DEVICES_PAGE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"
#include "../net_worker.h"

/**
 * Build the devices page inside @p parent (a tileview tile). The page is a
 * vertically scrollable column of cards.
 */
void devices_page_create(lv_obj_t * parent, const lv_font_t * font_sm,
                         const lv_font_t * font_lg);

/**
 * Refresh the cards from a snapshot. Invalid fields render as "--";
 * camera states "off"/"unknown"/"unavailable" all count as offline.
 * Safe to call before create (no-op).
 */
void devices_page_update(const ha_snapshot_t * s);

#ifdef __cplusplus
}
#endif

#endif /* DEVICES_PAGE_H */
