/**
 * @file bsp_display.c
 *
 * Display bring-up for the Waveshare ESP32-S3-Touch-LCD-4B:
 *   I2C master (GPIO47/48) -> TCA9554 (0x20)
 *     -> ST7701 3-wire SPI, all lines on the expander (IO_TYPE_EXPANDER)
 *     -> ST7701 vendor init sequence (480x480)
 *     -> RGB565 parallel panel (PSRAM frame buffer + bounce buffer)
 *     -> esp_lvgl_port display registration.
 *
 * The init command table and the expander reset sequence below are taken
 * verbatim from the official BSP component waveshare/esp32_s3_touch_lcd_4b
 * v2.0.0 (used by the vendor's ESP-IDF example 02_lvgl_demo_v9).
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_check.h"
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_io_additions.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7701.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"

#include "bsp_display.h"
#include "bsp_pins.h"

/* Log prefix style follows the desktop simulator ("[net]", "[sim]"):
 * lowercase module tags rendered by ESP_LOG as "I (ms) bsp-disp: ...". */
static const char *TAG = "bsp-disp";

/* LVGL render buffer height in lines (official BSP default: 100). */
#define BSP_LVGL_BUF_HEIGHT 100

/* ------------------------------------------------------------------ */
/* Shared handles                                                     */
/* ------------------------------------------------------------------ */
static i2c_master_bus_handle_t s_i2c_bus;
static bool s_i2c_ready;
static esp_io_expander_handle_t s_expander;

/* ------------------------------------------------------------------ */
/* ST7701 vendor init sequence (official BSP, 480x480 panel)          */
/* ------------------------------------------------------------------ */
static const st7701_lcd_init_cmd_t s_lcd_init_cmds[] = {
    /*  cmd   data                                                len  delay_ms */
    {0x11, (uint8_t[]){0x00}, 0, 120},
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x10}, 5, 0},
    {0xC0, (uint8_t[]){0x3B, 0x00}, 2, 0},
    {0xC1, (uint8_t[]){0x0D, 0x02}, 2, 0},
    {0xC2, (uint8_t[]){0x21, 0x08}, 2, 0},
    {0xCD, (uint8_t[]){0x08}, 1, 0},
    /* Positive gamma */
    {0xB0, (uint8_t[]){0x00, 0x11, 0x18, 0x0E, 0x11, 0x06, 0x07, 0x08,
                       0x07, 0x22, 0x04, 0x12, 0x0F, 0xAA, 0x31, 0x18}, 16, 0},
    /* Negative gamma */
    {0xB1, (uint8_t[]){0x00, 0x11, 0x19, 0x0E, 0x12, 0x07, 0x08, 0x08,
                       0x08, 0x22, 0x04, 0x11, 0x11, 0xA9, 0x32, 0x18}, 16, 0},
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x11}, 5, 0},
    {0xB0, (uint8_t[]){0x60}, 1, 0},
    {0xB1, (uint8_t[]){0x30}, 1, 0},
    {0xB2, (uint8_t[]){0x87}, 1, 0},
    {0xB3, (uint8_t[]){0x80}, 1, 0},
    {0xB5, (uint8_t[]){0x49}, 1, 0},
    {0xB7, (uint8_t[]){0x85}, 1, 0},
    {0xB8, (uint8_t[]){0x21}, 1, 0},
    {0xC1, (uint8_t[]){0x78}, 1, 0},
    {0xC2, (uint8_t[]){0x78}, 1, 20},
    {0xE0, (uint8_t[]){0x00, 0x1B, 0x02}, 3, 0},
    {0xE1, (uint8_t[]){0x08, 0xA0, 0x00, 0x00, 0x07, 0xA0, 0x00, 0x00,
                       0x00, 0x44, 0x44}, 11, 0},
    {0xE2, (uint8_t[]){0x11, 0x11, 0x44, 0x44, 0xED, 0xA0, 0x00, 0x00,
                       0xEC, 0xA0, 0x00, 0x00}, 12, 0},
    {0xE3, (uint8_t[]){0x00, 0x00, 0x11, 0x11}, 4, 0},
    {0xE4, (uint8_t[]){0x44, 0x44}, 2, 0},
    {0xE5, (uint8_t[]){0x0A, 0xE9, 0xD8, 0xA0, 0x0C, 0xEB, 0xD8, 0xA0,
                       0x0E, 0xED, 0xD8, 0xA0, 0x10, 0xEF, 0xD8, 0xA0}, 16, 0},
    {0xE6, (uint8_t[]){0x00, 0x00, 0x11, 0x11}, 4, 0},
    {0xE7, (uint8_t[]){0x44, 0x44}, 2, 0},
    {0xE8, (uint8_t[]){0x09, 0xE8, 0xD8, 0xA0, 0x0B, 0xEA, 0xD8, 0xA0,
                       0x0D, 0xEC, 0xD8, 0xA0, 0x0F, 0xEE, 0xD8, 0xA0}, 16, 0},
    {0xEB, (uint8_t[]){0x02, 0x00, 0xE4, 0xE4, 0x88, 0x00, 0x40}, 7, 0},
    {0xEC, (uint8_t[]){0x3C, 0x00}, 2, 0},
    {0xED, (uint8_t[]){0xAB, 0x89, 0x76, 0x54, 0x02, 0xFF, 0xFF, 0xFF,
                       0xFF, 0xFF, 0xFF, 0x20, 0x45, 0x67, 0x98, 0xBA}, 16, 0},
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x00}, 5, 0},
    {0x36, (uint8_t[]){0x00}, 1, 0},   /* MADCTL */
    {0x3A, (uint8_t[]){0x66}, 1, 0},   /* 18-bit/16-bit color mode   */
    {0x21, (uint8_t[]){0x00}, 0, 120}, /* display inversion off      */
    {0x29, (uint8_t[]){0x00}, 0, 0},   /* display on                 */
};

/* ------------------------------------------------------------------ */
/* I2C + IO expander                                                  */
/* ------------------------------------------------------------------ */
i2c_master_bus_handle_t bsp_get_i2c_bus(void)
{
    if(s_i2c_ready) return s_i2c_bus;

    const i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = BSP_I2C_NUM,
        .sda_io_num = BSP_I2C_SDA,
        .scl_io_num = BSP_I2C_SCL,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    if(i2c_new_master_bus(&bus_cfg, &s_i2c_bus) != ESP_OK) {
        ESP_LOGE(TAG, "i2c master bus init failed (SDA=%d SCL=%d)",
                 BSP_I2C_SDA, BSP_I2C_SCL);
        return NULL;
    }
    s_i2c_ready = true;
    ESP_LOGI(TAG, "i2c%d up: SDA=%d SCL=%d internal pull-ups on",
             BSP_I2C_NUM, BSP_I2C_SDA, BSP_I2C_SCL);
    return s_i2c_bus;
}

esp_io_expander_handle_t bsp_get_io_expander(void)
{
    if(s_expander) return s_expander;

    i2c_master_bus_handle_t bus = bsp_get_i2c_bus();
    if(!bus) return NULL;

    if(esp_io_expander_new_i2c_tca9554(bus, BSP_IO_EXPANDER_I2C_ADDRESS,
                                       &s_expander) != ESP_OK) {
        ESP_LOGE(TAG, "TCA9554 not found at 0x20");
        return NULL;
    }
    ESP_LOGI(TAG, "TCA9554 ready at 0x20");
    return s_expander;
}

/**
 * Panel + touch reset sequence. Reproduced from the official BSP: there
 * is no dedicated RST GPIO; the TCA9554 pins P5/P6 are toggled manually
 * (see bsp_pins.h header for the verified details).
 */
static esp_err_t bsp_reset_panel_via_expander(esp_io_expander_handle_t exp)
{
    esp_err_t err;

    err = esp_io_expander_set_dir(exp,
                                  BSP_EXPANDER_LCD_RST_PIN | BSP_EXPANDER_TOUCH_RST_PIN,
                                  IO_EXPANDER_OUTPUT);
    ESP_RETURN_ON_ERROR(err, TAG, "expander set_dir failed");

    /* Hold touch reset low while the LCD reset pulses. */
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(exp, BSP_EXPANDER_TOUCH_RST_PIN, 0),
                        TAG, "expander set_level failed");
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(exp, BSP_EXPANDER_LCD_RST_PIN, 0),
                        TAG, "expander set_level failed");
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(exp, BSP_EXPANDER_LCD_RST_PIN, 1),
                        TAG, "expander set_level failed");
    vTaskDelay(pdMS_TO_TICKS(200));

    /* Release the touch reset line: GT911 boots and latches its I2C
     * address (0x5D) from the INT pin level. */
    ESP_RETURN_ON_ERROR(esp_io_expander_set_dir(exp, BSP_EXPANDER_TOUCH_RST_PIN,
                                                IO_EXPANDER_INPUT),
                        TAG, "expander set_dir failed");

    ESP_LOGI(TAG, "panel reset sequence done (expander P5/P6)");
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Panel creation                                                     */
/* ------------------------------------------------------------------ */
static esp_err_t bsp_panel_new(esp_io_expander_handle_t exp,
                               esp_lcd_panel_handle_t *ret_panel,
                               esp_lcd_panel_io_handle_t *ret_io)
{
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_handle_t panel_handle = NULL;
    esp_err_t err;

    ESP_LOGI(TAG, "install 3-wire SPI panel IO on TCA9554 (CS=P0 SDA=P1 SCK=P2)");
    const spi_line_config_t line_cfg = {
        .cs_io_type = IO_TYPE_EXPANDER,
        .cs_expander_pin = BSP_EXPANDER_SPI_CS_PIN,
        .scl_io_type = IO_TYPE_EXPANDER,
        .scl_expander_pin = BSP_EXPANDER_SPI_SCK_PIN,
        .sda_io_type = IO_TYPE_EXPANDER,
        .sda_expander_pin = BSP_EXPANDER_SPI_SDA_PIN,
        .io_expander = exp,
    };
    const esp_lcd_panel_io_3wire_spi_config_t io_cfg =
        ST7701_PANEL_IO_3WIRE_SPI_CONFIG(line_cfg, 0);
    err = esp_lcd_new_panel_io_3wire_spi(&io_cfg, &io_handle);
    ESP_RETURN_ON_ERROR(err, TAG, "3-wire SPI panel IO failed");

    ESP_LOGI(TAG, "create RGB panel %dx%d pclk=%dHz fbs=%d bounce=%dpx",
             BSP_LCD_H_RES, BSP_LCD_V_RES, BSP_LCD_PCLK_HZ,
             BSP_LCD_NUM_FBS, BSP_LCD_BOUNCE_BUF_PX);
    esp_lcd_rgb_panel_config_t rgb_cfg = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        /* IDF v6: psram_trans_align / bits_per_pixel were removed from
         * esp_lcd_rgb_panel_config_t (color format now drives the layout). */
        .data_width = BSP_RGB_DATA_WIDTH,
        .de_gpio_num = BSP_LCD_DE,
        .pclk_gpio_num = BSP_LCD_PCLK,
        .vsync_gpio_num = BSP_LCD_VSYNC,
        .hsync_gpio_num = BSP_LCD_HSYNC,
        .disp_gpio_num = BSP_LCD_DISP,
        .data_gpio_nums = {
            BSP_LCD_DATA_R0, BSP_LCD_DATA_R1, BSP_LCD_DATA_R2,
            BSP_LCD_DATA_R3, BSP_LCD_DATA_R4,
            BSP_LCD_DATA_G0, BSP_LCD_DATA_G1, BSP_LCD_DATA_G2,
            BSP_LCD_DATA_G3, BSP_LCD_DATA_G4, BSP_LCD_DATA_G5,
            BSP_LCD_DATA_B0, BSP_LCD_DATA_B1, BSP_LCD_DATA_B2,
            BSP_LCD_DATA_B3, BSP_LCD_DATA_B4,
        },
        .timings = ST7701_480_480_PANEL_60HZ_RGB_TIMING(),
        .num_fbs = BSP_LCD_NUM_FBS,
        .bounce_buffer_size_px = BSP_LCD_BOUNCE_BUF_PX,
        .flags.fb_in_psram = 1,
    };
    /* Keep the macro's porches, but run a conservative pixel clock. */
    rgb_cfg.timings.pclk_hz = BSP_LCD_PCLK_HZ;
    rgb_cfg.timings.h_res = BSP_LCD_H_RES;
    rgb_cfg.timings.v_res = BSP_LCD_V_RES;

    const st7701_vendor_config_t vendor_cfg = {
        .init_cmds = s_lcd_init_cmds,
        .init_cmds_size = sizeof(s_lcd_init_cmds) / sizeof(s_lcd_init_cmds[0]),
        .rgb_config = &rgb_cfg,
        .flags = {
            /* The 3-wire SPI IO is only needed for the init sequence;
             * let the ST7701 driver release it afterwards. */
            .auto_del_panel_io = 1,
            .mirror_by_cmd = 0,
        },
    };
    const esp_lcd_panel_dev_config_t panel_cfg = {
        /* Reset is handled through the expander, see header. */
        .reset_gpio_num = GPIO_NUM_NC,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = BSP_LCD_PANEL_BITS,
        .vendor_config = (void *)&vendor_cfg,
    };
    err = esp_lcd_new_panel_st7701(io_handle, &panel_cfg, &panel_handle);
    ESP_RETURN_ON_ERROR(err, TAG, "esp_lcd_new_panel_st7701 failed");

    err = esp_lcd_panel_init(panel_handle);
    ESP_RETURN_ON_ERROR(err, TAG, "esp_lcd_panel_init failed");

    ESP_LOGI(TAG, "ST7701 initialized");
    *ret_panel = panel_handle;
    *ret_io = io_handle;
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* LVGL port                                                          */
/* ------------------------------------------------------------------ */
static lv_display_t *bsp_lvgl_attach(esp_lcd_panel_handle_t panel,
                                     esp_lcd_panel_io_handle_t io)
{
    /* LVGL task: generous stack (UI pages are heavy; ui_drain_cb's
     * snapshot handling adds call-depth slack on top), pinned to core 1
     * so it does not fight the wifi/lwIP stack on core 0 later. */
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_stack = 16384;
    port_cfg.task_affinity = 1;
    if(lvgl_port_init(&port_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "lvgl_port_init failed");
        return NULL;
    }

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io,
        .panel_handle = panel,
        .hres = BSP_LCD_H_RES,
        .vres = BSP_LCD_V_RES,
        .buffer_size = BSP_LCD_H_RES * BSP_LVGL_BUF_HEIGHT,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            .sw_rotate = true,
            .buff_dma = false,
            .buff_spiram = false,
            .swap_bytes = false,
        },
    };
    /* RGB panel specifics: use the RGB bounce buffer path. With a single
     * frame buffer there is no tearing avoidance mode to enable. */
    const lvgl_port_display_rgb_cfg_t rgb_cfg = {
        .flags = {
            .bb_mode = 1,
            .avoid_tearing = false,
        },
    };

    lv_display_t *disp = lvgl_port_add_disp_rgb(&disp_cfg, &rgb_cfg);
    if(!disp) {
        ESP_LOGE(TAG, "lvgl_port_add_disp_rgb failed");
        return NULL;
    }
    ESP_LOGI(TAG, "lvgl display attached (%dx%d RGB565)",
             BSP_LCD_H_RES, BSP_LCD_V_RES);
    return disp;
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */
lv_display_t *bsp_display_start(void)
{
    esp_io_expander_handle_t exp = bsp_get_io_expander();
    if(!exp) return NULL;

    if(bsp_reset_panel_via_expander(exp) != ESP_OK) return NULL;

    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_io_handle_t io = NULL;
    if(bsp_panel_new(exp, &panel, &io) != ESP_OK) return NULL;

    return bsp_lvgl_attach(panel, io);
}
