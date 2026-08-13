/**
 * @file web_config.h
 *
 * Embedded HTTP configuration server. Once WiFi is connected, the device
 * hosts a web page on port 80 allowing LAN users to configure HA token,
 * base URL, entity IDs, and other advanced settings.
 *
 * Usage: call web_config_start() after WiFi connects. The server runs
 * on its own task (esp_http_server internal) and never touches LVGL.
 */

#ifndef WEB_CONFIG_H
#define WEB_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_err.h"

/**
 * Start the HTTP configuration server on port 80.
 * Must be called after WiFi has obtained an IP address.
 * Idempotent: calling twice is safe (returns ESP_OK without re-starting).
 */
esp_err_t web_config_start(void);

/**
 * Stop the HTTP configuration server (optional; typically not needed
 * since config changes trigger a restart).
 */
void web_config_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* WEB_CONFIG_H */
