/**
 * @file wifi_sta.h
 *
 * WiFi STA bring-up for the firmware. Credentials come from the NVS
 * "fwcfg" keys wifi_ssid / wifi_psk (written via the `cfg` console).
 * Without credentials the firmware still boots - the UI simply runs in
 * the degraded "--" state until WiFi + config are provisioned.
 */

#ifndef WIFI_STA_H
#define WIFI_STA_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialise and start the STA interface.
 *
 *   ESP_OK                  WiFi started (connection still in progress)
 *   ESP_ERR_NVS_NOT_FOUND   no credentials stored: logs a warning and
 *                           returns without touching the WiFi driver, so
 *                           the caller can continue in degraded mode
 *   other                   driver/init failure
 *
 * Disconnects trigger automatic reconnect (esp_wifi_connect from the
 * WIFI_EVENT_STA_DISCONNECTED handler); got-ip sets an internal event bit.
 */
esp_err_t wifi_sta_start(void);

/**
 * Non-blocking check: true once the STA has obtained an IP.
 * Safe to call from any thread; returns false before wifi_sta_start().
 */
bool wifi_sta_is_connected(void);

/**
 * Block up to @p timeout_ms waiting for the STA to obtain an IP.
 * Returns true once connected; false on timeout (caller continues in
 * degraded mode - the WiFi task keeps retrying in the background and the
 * network worker tolerates being started before WiFi is up).
 */
bool wifi_sta_wait_connected(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_STA_H */
