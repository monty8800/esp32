/**
 * @file config_store.h
 *
 * NVS-backed key/value configuration for the firmware.
 *
 * Namespace "fwcfg", string values only. The allowed keys mirror the
 * simulator's environment variables:
 *
 *   wifi_ssid / wifi_psk       WiFi STA credentials (wifi_sta.c)
 *   ha_token                   Home Assistant long-lived token
 *   ha_base_url                HA REST base URL
 *   server_summary_url         server monitor /api/summary URL
 *   weather_url                Open-Meteo forecast URL
 *
 * Values are written with the `cfg` serial console commands (see
 * config_console_start()) and surfaced as environment variables by
 * env_shim_apply() before ha_client_init() reads them.
 */

#ifndef CONFIG_STORE_H
#define CONFIG_STORE_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CONFIG_STORE_NS "fwcfg"

/** True when @p key is one of the known configuration keys. */
bool config_key_valid(const char * key);

/**
 * Read a string value into @p buf (NUL-terminated).
 * Returns ESP_OK, ESP_ERR_NVS_NOT_FOUND when unset, or an NVS error.
 */
esp_err_t config_get_str(const char * key, char * buf, size_t len);

/** Write (or overwrite) a string value and commit it. */
esp_err_t config_set_str(const char * key, const char * value);

/**
 * Start the UART console (esp_console REPL) exposing the `cfg` and
 * `restart` commands. Runs on its own task (UART stdin), safe to start
 * alongside the LVGL task. Never returns a meaningful error - a console
 * failure must not block the UI.
 */
void config_console_start(void);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_STORE_H */
