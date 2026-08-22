/**
 * @file photos_page.h
 *
 * Page 4: digital photo frame.  A dedicated FreeRTOS task downloads JPEG
 * images from a configurable HTTP source into PSRAM double-buffers; an
 * LVGL timer on the UI thread swaps the displayed slot with a crossfade.
 *
 * Photo list source (JSON):
 *   { "photos": [ "http://host/img1.jpg", "http://host/img2.jpg", ... ] }
 *
 * The page is fully self-contained: it owns its HTTP client and download
 * task, and does NOT go through net_worker.  This keeps the existing
 * polling architecture untouched.
 */

#ifndef PHOTOS_PAGE_H
#define PHOTOS_PAGE_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Build the photo-frame UI inside @p parent (a tileview tile).
 * Widgets are created but no download starts until
 * photos_page_set_source_url() provides a source.
 */
void photos_page_create(lv_obj_t * parent, const lv_font_t * font_sm);

/**
 * Set the photo-list URL and (re)start the download task.
 * Pass NULL or "" to stop.  The string is copied internally.
 */
void photos_page_set_source_url(const char * url);

/**
 * Manual navigation: advance or go back one photo.
 * Also resets the auto-advance timer.
 */
void photos_page_next(void);
void photos_page_prev(void);

/**
 * Toggle auto-advance on / off.
 */
void photos_page_toggle_auto(void);

#ifdef __cplusplus
}
#endif

#endif /* PHOTOS_PAGE_H */
