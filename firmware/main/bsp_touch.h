/**
 * @file bsp_touch.h
 *
 * GT911 capacitive touch on the shared I2C bus, registered with
 * esp_lvgl_port as a pointer input device.
 */

#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the GT911 and attach it to LVGL.
 *
 * Must be called after bsp_display_start(): the GT911 reset is part of
 * the expander reset sequence performed there, and the input device is
 * bound to the display created by that call. The I2C bus handle is
 * shared via bsp_get_i2c_bus().
 *
 * @return LVGL input device handle, or NULL on failure.
 */
lv_indev_t *bsp_touch_init(void);

#ifdef __cplusplus
}
#endif
