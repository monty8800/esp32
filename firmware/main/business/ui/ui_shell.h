/**
 * @file ui_shell.h
 *
 * Four-page navigation shell: a persistent 44px status bar on top (clock +
 * HA connection state + page indicator dots) and an lv_tileview below it
 * holding the four pages. Swipe left/right to switch pages; tapping a dot
 * jumps to that page. Depends ONLY on the LVGL API.
 */

#ifndef UI_SHELL_H
#define UI_SHELL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

#include "lvgl.h"

#define UI_SHELL_TILE_COUNT   4
#define UI_SHELL_BAR_HEIGHT   44

/**
 * Build the shell on the active screen. The screen background, the status
 * bar, the tileview and the four (empty, styled) tiles are created here;
 * the page modules then populate the tiles returned by ui_shell_get_tile().
 */
void ui_shell_create(const lv_font_t * font_sm);

/** Tile container for page @p idx (0 = dashboard, 1 = devices, 2 = server, 3 = photos). */
lv_obj_t * ui_shell_get_tile(int idx);

/** Currently visible tile index (0..UI_SHELL_TILE_COUNT-1). */
int ui_shell_get_active_tile(void);

/** Status bar HA word: "LIVE" / "OFFLINE" / "NO TOKEN" (color-coded). */
void ui_shell_set_ha_status(const char * text);

/**
 * Status bar WiFi icon: accent colour when connected, amber when not.
 * Cached internally - safe to call on every drain tick.
 */
void ui_shell_set_wifi(bool connected);

/** Status bar clock text, e.g. "14:32". Set only when it changes. */
void ui_shell_set_time(const char * hhmm);

/**
 * Status bar indoor environment readout, rendered compactly as
 * "25.3°C · 60%" between the HA word and the page dots.
 * Pass "--" (or NULL) for a value that is unavailable; units and the
 * separator are appended by the shell, strings are used verbatim.
 */
void ui_shell_set_env(const char * temp, const char * hum);

#ifdef __cplusplus
}
#endif

#endif /* UI_SHELL_H */
