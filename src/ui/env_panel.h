/**
 * @file env_panel.h
 *
 * Air purifier environment panel (temperature / humidity / PM2.5 readouts,
 * mode buttons, power switch) for the ESP32-S3-Touch-LCD-4B simulator.
 *
 * IMPORTANT: This module depends ONLY on the LVGL API - no networking, no
 * platform code - so it can be reused unchanged on the real hardware with
 * ESP-IDF + esp_lvgl_port.
 */

#ifndef ENV_PANEL_H
#define ENV_PANEL_H

#include <stdbool.h>
#include "lvgl.h"

/* Control actions reported by the panel's control callback. */
enum {
    ENV_ACT_MODE_AUTO = 0,
    ENV_ACT_MODE_SLEEP,
    ENV_ACT_MODE_FAV,
    ENV_ACT_POWER_ON,
    ENV_ACT_POWER_OFF,
};

/**
 * Build the environment panel as a child of @p scr.
 * Call once, after ui_demo_create().
 *
 * @param cjk_font  Font covering CJK glyphs for the metric kickers
 *                  (温度/湿度). Injected by the platform layer; when NULL the
 *                  panel falls back to the built-in Montserrat fonts.
 */
void env_panel_create(lv_obj_t * scr, const lv_font_t * cjk_font);

/**
 * Register the control callback. Invoked (from the LVGL event context) after
 * the panel has optimistically updated its own UI.
 */
void env_panel_set_control_cb(void (*cb)(int action, void * user_data), void * user_data);

/**
 * Push externally fetched state into the panel.
 *
 * @param temp/hum/pm25       plain value strings ("25.8", "51", "106");
 *                            formatted with units by the panel. Ignored when
 *                            the matching *_valid flag is false.
 * @param mode_idx            0=AUTO 1=SLEEP 2=FAV, -1 keeps the current UI
 * @param power_on/power_valid power switch state; ignored when invalid
 * @param status_text         footer status word, e.g. "LIVE", "OFFLINE",
 *                            "NO TOKEN" (color picked from the text)
 *
 * Only changed widgets are touched, so this is safe to call on every poll.
 */
void env_panel_update(const char * temp, bool temp_valid,
                      const char * hum, bool hum_valid,
                      const char * pm25, bool pm25_valid,
                      int mode_idx,
                      bool power_on, bool power_valid,
                      const char * status_text);

#endif /* ENV_PANEL_H */
