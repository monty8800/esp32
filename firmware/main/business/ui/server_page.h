/**
 * @file server_page.h
 *
 * Page 3 (read-only): server monitor summary - online/total counts, last
 * snapshot time, and one row per host with latency + probe status dots.
 */

#ifndef SERVER_PAGE_H
#define SERVER_PAGE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"
#include "../net_worker.h"

/**
 * Build the server page inside @p parent (a tileview tile).
 * @param font_sm CJK 16px font (NULL -> Montserrat 14)
 */
void server_page_create(lv_obj_t * parent, const lv_font_t * font_sm);

/**
 * Refresh from a snapshot. Uses seq to skip redundant rebuilds; a failed
 * latest fetch (seq>0, !valid) keeps previous data under a STALE mark,
 * seq==0 shows a probing placeholder. Safe before create (no-op).
 */
void server_page_update(const server_snapshot_t * s);

#ifdef __cplusplus
}
#endif

#endif /* SERVER_PAGE_H */
