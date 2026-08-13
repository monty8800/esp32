/**
 * @file bsp_display.h
 *
 * Display pipeline for the ESP32-S3-Touch-LCD-4B:
 *   I2C master -> TCA9554 -> ST7701 3-wire SPI (IO_TYPE_EXPANDER)
 *   -> ST7701 vendor init -> RGB565 parallel panel -> esp_lvgl_port.
 */

#pragma once

#include "driver/i2c_master.h"
#include "esp_io_expander.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Bring up the whole display path and start the LVGL task.
 *
 * Performs: I2C bus init, TCA9554 init, panel reset sequence on the
 * expander, 3-wire SPI panel IO, ST7701 init, RGB panel creation and
 * lvgl_port_add_disp_rgb().
 *
 * @return LVGL display handle, or NULL on failure.
 */
lv_display_t *bsp_display_start(void);

/**
 * @brief Get the shared I2C master bus handle (lazy-init).
 *
 * The GT911 touch driver shares this bus. bsp_display_start() must be
 * called first in practice, but this function initializes the bus on
 * demand as well.
 *
 * @return I2C master bus handle, or NULL on failure.
 */
i2c_master_bus_handle_t bsp_get_i2c_bus(void);

/**
 * @brief Get the TCA9554 IO expander handle (lazy-init).
 *
 * @return Expander handle, or NULL on failure.
 */
esp_io_expander_handle_t bsp_get_io_expander(void);

#ifdef __cplusplus
}
#endif
