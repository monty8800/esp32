/**
 * @file bsp_backlight.h
 *
 * LCD backlight control: GPIO4, LEDC PWM, active-low (brighter = lower
 * duty, identical behaviour to the official BSP).
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Configure the LEDC timer/channel for the backlight.
 *
 * Called implicitly by bsp_backlight_set_percent() on first use; may
 * also be called explicitly during board bring-up.
 *
 * @return ESP_OK on success.
 */
esp_err_t bsp_backlight_init(void);

/**
 * @brief Set backlight brightness.
 *
 * @param percent Brightness in percent, clamped to 0..100.
 *                0 = backlight off, 100 = full brightness.
 * @return ESP_OK on success.
 */
esp_err_t bsp_backlight_set_percent(int percent);

#ifdef __cplusplus
}
#endif
