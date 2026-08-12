/**
 * @file ui_demo.h
 *
 * Screen scaffold for the ESP32-S3-Touch-LCD-4B (480x480) simulator.
 *
 * IMPORTANT: This module depends ONLY on the LVGL API - no SDL, no platform
 * code - so it can be reused unchanged on the real hardware with ESP-IDF +
 * esp_lvgl_port.
 */

#ifndef UI_DEMO_H
#define UI_DEMO_H

#include "lvgl.h"

/**
 * Prepare the currently active screen: background color, gradient and a
 * flex column layout that content panels (e.g. env_panel.c) append to.
 *
 * @param font_sm  Small font (~16px). Accepted for API stability; unused.
 * @param font_lg  Large font (~20px). Accepted for API stability; unused.
 */
void ui_demo_create(const lv_font_t * font_sm, const lv_font_t * font_lg);

#endif /* UI_DEMO_H */
