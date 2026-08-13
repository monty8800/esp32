/**
 * @file bsp_pins.h
 *
 * Single source of truth for every pin of the Waveshare
 * ESP32-S3-Touch-LCD-4B board. All BSP modules include this header;
 * no pin number may appear anywhere else.
 *
 * ------------------------------------------------------------------
 * Pin cross-check against the official sources (2026-08-12)
 * ------------------------------------------------------------------
 * Authoritative sources used:
 *   1. https://github.com/waveshareteam/ESP32-S3-Touch-LCD-4B
 *      (branch chore/initial-resource-import, the branch that actually
 *      carries the code; the "main" branch is an empty initial commit)
 *      - examples/esp-idf/02_lvgl_demo_v9/  (official ESP-IDF example)
 *   2. The BSP component the official example pulls in:
 *      waveshare/esp32_s3_touch_lcd_4b v2.0.0 from the ESP Component
 *      Registry (source repo: github.com/waveshareteam/
 *      Waveshare-ESP32-components, path bsp/esp32_s3_touch_lcd_4b).
 *
 * Result of the cross-check:
 *   - I2C: SDA=GPIO47, SCL=GPIO48 ................... MATCH
 *   - TCA9554 I2C address 0x20 (ADDRESS_000) ........ MATCH
 *   - 3-wire SPI over TCA9554: CS=P0, SDA=P1, SCK=P2  MATCH
 *   - RGB565: PCLK=9, DE=17, HSYNC=46, VSYNC=3,
 *     R={40,41,42,2,1}, G={21,8,18,45,38,39},
 *     B={10,11,12,13,14} ............................ MATCH
 *   - Backlight GPIO4, LEDC PWM, active-low .......... MATCH
 *     (official BSP computes duty = 1023 * (100 - pct) / 100)
 *   - GT911 I2C address: official BSP uses
 *     ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS (0x5D) ...... MATCH
 *     (0x14 is the alternate address selected by the INT pin level
 *     at reset time; not used here)
 *
 * DIFFERENCES vs. the original plan (official demo wins):
 *   - The plan assumed an ST7701 RST line on TCA9554 P7. The official
 *     BSP uses NO hardware reset GPIO (panel reset_gpio_num = NC and
 *     GT911 rst_gpio_num = NC). Instead it performs a manual reset
 *     sequence on the expander before creating the panel IO:
 *         P6 (touch reset) driven low, P5 driven low (200 ms),
 *         P5 released high (200 ms), P6 returned to input (200 ms).
 *     That sequence is reproduced in bsp_display.c, see
 *     BSP_EXPANDER_TOUCH_RST_PIN / BSP_EXPANDER_LCD_RST_PIN below.
 *   - GT911 INT is not wired to any GPIO/expander pin: the touch
 *     driver runs in polling mode (int_gpio_num = GPIO_NUM_NC).
 */

#pragma once

#include "driver/gpio.h"
#include "esp_io_expander_tca9554.h"

/* ------------------------------------------------------------------
 * Board geometry
 * ------------------------------------------------------------------ */
#define BSP_LCD_H_RES               480
#define BSP_LCD_V_RES               480
#define BSP_LCD_BITS_PER_PIXEL      16   /* RGB565 on the RGB bus      */
#define BSP_LCD_PANEL_BITS          18   /* ST7701 panel config (RGB666 family) */
#define BSP_RGB_DATA_WIDTH          16

/* ------------------------------------------------------------------
 * I2C bus (shared by TCA9554, GT911 and other peripherals)
 * ------------------------------------------------------------------ */
#define BSP_I2C_NUM                 1
#define BSP_I2C_SDA                 (GPIO_NUM_47)
#define BSP_I2C_SCL                 (GPIO_NUM_48)
#define BSP_I2C_FREQ_HZ             (400 * 1000)

/* TCA9554 sits at address 0x20 (A0=A1=A2=0). */
#define BSP_IO_EXPANDER_I2C_ADDRESS ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000

/* ------------------------------------------------------------------
 * ST7701 3-wire SPI control lines - all behind the TCA9554
 * ------------------------------------------------------------------ */
#define BSP_EXPANDER_SPI_CS_PIN     IO_EXPANDER_PIN_NUM_0
#define BSP_EXPANDER_SPI_SDA_PIN    IO_EXPANDER_PIN_NUM_1  /* MOSI */
#define BSP_EXPANDER_SPI_SCK_PIN    IO_EXPANDER_PIN_NUM_2

/*
 * Reset sequencing pins (see file header). Not a plain "RST" line:
 * the official BSP toggles them manually before panel IO creation.
 */
#define BSP_EXPANDER_LCD_RST_PIN    IO_EXPANDER_PIN_NUM_5
#define BSP_EXPANDER_TOUCH_RST_PIN  IO_EXPANDER_PIN_NUM_6

/* ------------------------------------------------------------------
 * ST7701 RGB565 parallel interface (direct GPIOs)
 * ------------------------------------------------------------------ */
#define BSP_LCD_PCLK                (GPIO_NUM_9)
#define BSP_LCD_DE                  (GPIO_NUM_17)
#define BSP_LCD_HSYNC               (GPIO_NUM_46)
#define BSP_LCD_VSYNC               (GPIO_NUM_3)
#define BSP_LCD_DISP                (GPIO_NUM_NC)

/* Data lines in RGB bus order (R5, G6, B5). */
#define BSP_LCD_DATA_R0             (GPIO_NUM_40)
#define BSP_LCD_DATA_R1             (GPIO_NUM_41)
#define BSP_LCD_DATA_R2             (GPIO_NUM_42)
#define BSP_LCD_DATA_R3             (GPIO_NUM_2)
#define BSP_LCD_DATA_R4             (GPIO_NUM_1)
#define BSP_LCD_DATA_G0             (GPIO_NUM_21)
#define BSP_LCD_DATA_G1             (GPIO_NUM_8)
#define BSP_LCD_DATA_G2             (GPIO_NUM_18)
#define BSP_LCD_DATA_G3             (GPIO_NUM_45)
#define BSP_LCD_DATA_G4             (GPIO_NUM_38)
#define BSP_LCD_DATA_G5             (GPIO_NUM_39)
#define BSP_LCD_DATA_B0             (GPIO_NUM_10)
#define BSP_LCD_DATA_B1             (GPIO_NUM_11)
#define BSP_LCD_DATA_B2             (GPIO_NUM_12)
#define BSP_LCD_DATA_B3             (GPIO_NUM_13)
#define BSP_LCD_DATA_B4             (GPIO_NUM_14)

/*
 * Conservative pixel clock: the ST7701 macro default targets ~16MHz;
 * 12MHz is used until the wiring is proven on hardware.
 */
#define BSP_LCD_PCLK_HZ             (12 * 1000 * 1000)

/* RGB panel buffers. */
#define BSP_LCD_NUM_FBS             1
/* Bounce buffer: 12 lines (task requirement; official BSP defaults to 20). */
#define BSP_LCD_BOUNCE_BUF_PX       (BSP_LCD_H_RES * 12)

/* ------------------------------------------------------------------
 * Backlight (active-low LEDC PWM)
 * ------------------------------------------------------------------ */
#define BSP_LCD_BACKLIGHT           (GPIO_NUM_4)
#define BSP_BACKLIGHT_LEDC_TIMER    LEDC_TIMER_1
#define BSP_BACKLIGHT_LEDC_CHANNEL  LEDC_CHANNEL_1
#define BSP_BACKLIGHT_LEDC_FREQ_HZ  5000
#define BSP_BACKLIGHT_LEDC_DUTY_RES LEDC_TIMER_10_BIT   /* 0..1023 */

/* ------------------------------------------------------------------
 * GT911 touch
 * ------------------------------------------------------------------ */
/* Official BSP address: ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS (0x5D). */
#define BSP_TOUCH_I2C_ADDR          0x5D
#define BSP_TOUCH_INT               (GPIO_NUM_NC)  /* polling mode  */
#define BSP_TOUCH_RST               (GPIO_NUM_NC)  /* reset done via
                                                      the TCA9554
                                                      sequence above */
