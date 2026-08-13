/**
 * @file env_shim.h
 *
 * Bridges NVS configuration into the POSIX environment so the UNMODIFIED
 * simulator network code (ha_client / server_client / weather_client, all
 * getenv()-based) picks it up on the device. Call env_shim_apply() after
 * nvs_flash_init() and BEFORE ha_client_init().
 */

#ifndef ENV_SHIM_H
#define ENV_SHIM_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Read the NVS "fwcfg" keys and setenv() the corresponding variables:
 *
 *   ha_token           -> HA_TOKEN
 *   ha_base_url        -> HA_BASE_URL
 *   server_summary_url -> SERVER_SUMMARY_URL
 *   weather_url        -> WEATHER_URL
 *
 * Unset keys are skipped (the clients then fall back to their compiled-in
 * defaults). ESP-IDF newlib provides setenv/getenv.
 */
void env_shim_apply(void);

#ifdef __cplusplus
}
#endif

#endif /* ENV_SHIM_H */
