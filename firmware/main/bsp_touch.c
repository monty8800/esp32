/**
 * @file bsp_touch.c
 *
 * GT911 touch initialization for the ESP32-S3-Touch-LCD-4B.
 *
 * Notes (verified against the official BSP):
 *   - I2C address 0x5D (ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS); the INT
 *     pin is not wired, so the driver runs in polling mode.
 *   - The GT911 reset is not a plain GPIO: it shares the expander
 *     reset sequence executed by bsp_display_start() (TCA9554 P6).
 *     Therefore this module must be initialized after the display.
 *   - The I2C bus handle is shared with the display module.
 */

#include "esp_check.h"
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"

#include "bsp_display.h"
#include "bsp_pins.h"
#include "bsp_touch.h"

static const char *TAG = "bsp-touch";

lv_indev_t *bsp_touch_init(void)
{
    i2c_master_bus_handle_t bus = bsp_get_i2c_bus();
    if(!bus) {
        ESP_LOGE(TAG, "i2c bus not available");
        return NULL;
    }

    /* Panel IO on the shared I2C bus, GT911 register layout (16-bit). */
    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    tp_io_cfg.dev_addr = BSP_TOUCH_I2C_ADDR;
    tp_io_cfg.scl_speed_hz = BSP_I2C_FREQ_HZ;
    esp_err_t err = esp_lcd_new_panel_io_i2c(bus, &tp_io_cfg, &tp_io);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "touch panel io failed: %s", esp_err_to_name(err));
        return NULL;
    }

    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = BSP_LCD_H_RES,
        .y_max = BSP_LCD_V_RES,
        .rst_gpio_num = BSP_TOUCH_RST,   /* NC: reset via expander sequence */
        .int_gpio_num = BSP_TOUCH_INT,   /* NC: polling mode */
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };

    esp_lcd_touch_handle_t tp = NULL;
    err = esp_lcd_touch_new_i2c_gt911(tp_io, &tp_cfg, &tp);
    if(err != ESP_OK || !tp) {
        ESP_LOGE(TAG, "GT911 init failed (addr 0x%02X): %s",
                 BSP_TOUCH_I2C_ADDR, esp_err_to_name(err));
        return NULL;
    }
    ESP_LOGI(TAG, "GT911 ready at 0x%02X (polling mode)", BSP_TOUCH_I2C_ADDR);

    const lvgl_port_touch_cfg_t lv_touch_cfg = {
        .disp = lv_display_get_default(),
        .handle = tp,
    };
    lv_indev_t *indev = lvgl_port_add_touch(&lv_touch_cfg);
    if(!indev) {
        ESP_LOGE(TAG, "lvgl_port_add_touch failed");
        return NULL;
    }
    ESP_LOGI(TAG, "touch attached to lvgl");
    return indev;
}
