/**
 * @file ha_client.h
 *
 * Minimal Home Assistant REST client for the desktop simulator.
 *
 * Responsibilities:
 *   - resolve the bearer token (HA_TOKEN env var, fallback ~/.ha_esp32_token)
 *   - resolve the base URL (HA_BASE_URL env var, default below)
 *   - GET /api/states/<entity_id>             -> extract the top-level "state"
 *   - POST /api/services/<domain>/<service>   -> real HA service call that
 *     actually drives the device (writing /api/states only fakes the state
 *     machine and never reaches the hardware)
 *
 * Implemented with libcurl + hand-rolled JSON extraction on purpose: no JSON
 * library dependency, tiny footprint, good enough for flat HA state objects.
 */

#ifndef HA_CLIENT_H
#define HA_CLIENT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>

/* --------------------------------------------------------------------------
 * Entity IDs of the Xiaomi air purifier (confirmed against the live HA).
 * Each one can be overridden via an environment variable of the same name.
 * -------------------------------------------------------------------------- */
#define HA_ENTITY_TEMP_DEFAULT   "sensor.zhimi_cn_56424062_m1_temperature_p_3_3"
#define HA_ENTITY_HUM_DEFAULT    "sensor.zhimi_cn_56424062_m1_relative_humidity_p_3_1"
#define HA_ENTITY_PM25_DEFAULT   "sensor.zhimi_cn_56424062_m1_pm2_5_density_p_3_2"
#define HA_ENTITY_MODE_DEFAULT   "select.zhimi_cn_56424062_m1_mode_p_2_2"
#define HA_ENTITY_POWER_DEFAULT  "switch.zhimi_cn_56424062_m1_on_p_2_1"

/* Device screen entities, discovered against the live HA /api/states
 * (2026-08-12). Each one can be overridden via an environment variable of
 * the same name; an empty value disables the corresponding device card.
 *   AC   : Midea AC "513里屋" (the 外屋 climate.air_conditioner is unavailable)
 *   lamp : 米家显示器挂灯2 (xiaomi.light.bar2), the MIoT light entity
 *   cam1 : 小米智能摄像机 4 4K (chuangmi.camera.079ac1), camera power switch
 *   cam2 : 小米智能摄像机 云台版2K2 (chuangmi.camera.029a02), camera power switch */
#define HA_ENTITY_AC_DEFAULT     "climate.air_conditioner_2"
#define HA_ENTITY_LAMP_DEFAULT   "light.xiaomi_cn_2055582114_bar2_s_2_light"
#define HA_ENTITY_CAM1_DEFAULT   "switch.chuangmi_cn_1184358788_079ac1_on_p_2_1"
#define HA_ENTITY_CAM2_DEFAULT   "switch.chuangmi_cn_451026136_029a02_on_p_2_1"

/**
 * Initialise curl and load configuration (token, base URL, entity IDs).
 *
 * @return true when a token was found and the client is usable,
 *         false otherwise (a missing token only logs its *length*, never
 *         its content).
 */
bool ha_client_init(void);

/**
 * GET the state of an entity.
 *
 * @param entity_id  HA entity ID, e.g. "sensor.xxx"
 * @param out        buffer receiving the "state" string (NUL-terminated)
 * @param out_len    size of @p out
 * @return true on HTTP 200 + successful "state" extraction
 */
bool ha_client_fetch_state(const char * entity_id, char * out, size_t out_len);

/**
 * Call a Home Assistant service, e.g. select/select_option or
 * switch/turn_on. The request is POST /api/services/<domain>/<service>
 * with @p json_body as the raw JSON payload - this is what actually drives
 * the real device, unlike writing /api/states.
 *
 * @param domain    service domain, e.g. "select", "switch"
 * @param service   service name, e.g. "select_option", "turn_on"
 * @param json_body prebuilt JSON body, e.g.
 *                  {"entity_id":"select.xxx","option":"自动"}
 * @return true on HTTP 200
 */
bool ha_client_call_service(const char * domain, const char * service,
                            const char * json_body);

/* Resolved entity IDs (after environment-variable overrides). */
const char * ha_entity_temp(void);
const char * ha_entity_hum(void);
const char * ha_entity_pm25(void);
const char * ha_entity_mode(void);
const char * ha_entity_power(void);
const char * ha_entity_ac(void);
const char * ha_entity_lamp(void);
const char * ha_entity_cam1(void);
const char * ha_entity_cam2(void);

#ifdef __cplusplus
}
#endif

#endif /* HA_CLIENT_H */
