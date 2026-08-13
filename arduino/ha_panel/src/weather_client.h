/**
 * @file weather_client.h
 *
 * Client for the Open-Meteo forecast API (Shenzhen outdoor weather).
 *
 * Endpoint: GET https://api.open-meteo.com/v1/forecast
 *   - no authentication, no API key
 *   - response schema (verified live 2026-08-12):
 *     {"latitude":...,"timezone":"Asia/Shanghai",...,
 *      "current":{"time":"2026-08-12T11:30","temperature_2m":34.4,
 *                 "relative_humidity_2m":53,"weather_code":51,
 *                 "wind_speed_10m":11.8}}
 *
 * The URL can be overridden with the WEATHER_URL environment variable.
 * Implemented with libcurl + cJSON, mirroring server_client.c (growable
 * response buffer, 5s timeout, every failure returns false - never crashes
 * on a hostile or absent server).
 *
 * Only ever called from the net_worker thread - never from the UI thread.
 */

#ifndef WEATHER_CLIENT_H
#define WEATHER_CLIENT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

#include "net_worker.h"

#define WEATHER_URL_DEFAULT \
    "https://api.open-meteo.com/v1/forecast" \
    "?latitude=22.5431&longitude=114.0579" \
    "&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m" \
    "&timezone=auto"

/**
 * Fetch and parse the current Shenzhen weather into @p out. On success
 * fills temp_c/hum_pct/weather_code/wind_kmh/fetched_hhmm (valid=true) and
 * returns true. On any failure (connect, timeout, HTTP error, malformed
 * JSON, missing fields) returns false and leaves @p out untouched, so the
 * caller can keep the previous data and mark it stale. The seq field is
 * never touched here.
 */
bool weather_client_fetch(weather_snapshot_t * out);

#ifdef __cplusplus
}
#endif

#endif /* WEATHER_CLIENT_H */
