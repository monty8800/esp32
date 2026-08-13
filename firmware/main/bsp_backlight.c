/**
 * @file bsp_backlight.c
 *
 * Backlight PWM on GPIO4. The hardware is active-low: the official BSP
 * drives duty = 1023 * (100 - percent) / 100, i.e. 100% brightness is
 * duty 0. The same mapping is reproduced here so behaviour matches the
 * vendor firmware exactly.
 */

#include <stdbool.h>

#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_log.h"

#include "bsp_backlight.h"
#include "bsp_pins.h"

static const char *TAG = "bsp-bl";

static bool s_ready;

esp_err_t bsp_backlight_init(void)
{
    if(s_ready) return ESP_OK;

    const ledc_timer_config_t timer_cfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = BSP_BACKLIGHT_LEDC_DUTY_RES,
        .timer_num = BSP_BACKLIGHT_LEDC_TIMER,
        .freq_hz = BSP_BACKLIGHT_LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer_cfg), TAG, "ledc timer failed");

    const ledc_channel_config_t ch_cfg = {
        .gpio_num = BSP_LCD_BACKLIGHT,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = BSP_BACKLIGHT_LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = BSP_BACKLIGHT_LEDC_TIMER,
        /* Start dark; app_main sets the target level explicitly. */
        .duty = 0,
        .hpoint = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&ch_cfg), TAG, "ledc channel failed");

    s_ready = true;
    ESP_LOGI(TAG, "backlight PWM ready on GPIO%d (active-low)", BSP_LCD_BACKLIGHT);
    return ESP_OK;
}

esp_err_t bsp_backlight_set_percent(int percent)
{
    if(!s_ready) {
        ESP_RETURN_ON_ERROR(bsp_backlight_init(), TAG, "lazy init failed");
    }

    if(percent < 0) percent = 0;
    if(percent > 100) percent = 100;

    /* Active-low: full brightness = duty 0, off = duty 1023. */
    const uint32_t max_duty = (1U << 10) - 1;   /* 10-bit resolution */
    const uint32_t duty = (max_duty * (uint32_t)(100 - percent)) / 100;

    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE,
                                      BSP_BACKLIGHT_LEDC_CHANNEL, duty),
                        TAG, "ledc_set_duty failed");
    ESP_RETURN_ON_ERROR(ledc_update_duty(LEDC_LOW_SPEED_MODE,
                                         BSP_BACKLIGHT_LEDC_CHANNEL),
                        TAG, "ledc_update_duty failed");

    ESP_LOGI(TAG, "backlight %d%%", percent);
    return ESP_OK;
}
