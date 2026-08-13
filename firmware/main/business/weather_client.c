/**
 * @file weather_client.c
 *
 * GET the Open-Meteo current-weather endpoint for Shenzhen and parse it
 * into weather_snapshot_t with cJSON. Every failure mode (DNS / connect /
 * timeout, non-200, truncated body, malformed JSON, missing fields) simply
 * returns false - this module never crashes on a hostile or absent server.
 *
 * Response schema (verified live 2026-08-12):
 * {
 *   "latitude": 22.530754, "timezone": "Asia/Shanghai", ...,
 *   "current_units": {...},
 *   "current": {
 *     "time": "2026-08-12T11:30",
 *     "temperature_2m": 34.4,
 *     "relative_humidity_2m": 53,
 *     "weather_code": 51,
 *     "wind_speed_10m": 11.8
 *   }
 * }
 */

#include "weather_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <curl/curl.h>

#include "third_party/cjson/cJSON.h"

#define RESP_BUF_INIT   (4 * 1024)     /* initial response buffer size */
#define RESP_BUF_MAX    (64 * 1024)    /* hard cap, abort beyond this */

/* Persistent easy handle + growable response buffer, reused across calls
 * (this module is only ever called from the single worker thread). */
static CURL * curl_handle;
static char * resp_buf;
static size_t resp_cap;

typedef struct {
    size_t len;
    bool   truncated;   /* body exceeded RESP_BUF_MAX */
} resp_state_t;

static size_t write_cb(char * ptr, size_t size, size_t nmemb, void * userdata)
{
    resp_state_t * st = (resp_state_t *)userdata;
    size_t add = size * nmemb;

    /* Grow (doubling) while respecting the cap. */
    size_t need = st->len + add + 1;      /* +1 for the NUL terminator */
    if(need > RESP_BUF_MAX) {
        st->truncated = true;
        return 0;                         /* != add -> curl aborts with CURLE_WRITE_ERROR */
    }
    if(need > resp_cap) {
        size_t new_cap = resp_cap;
        while(new_cap < need) new_cap *= 2;
        if(new_cap > RESP_BUF_MAX) new_cap = RESP_BUF_MAX;
        char * nb = (char *)realloc(resp_buf, new_cap);
        if(nb == NULL) {
            st->truncated = true;
            return 0;
        }
        resp_buf = nb;
        resp_cap = new_cap;
    }

    memcpy(resp_buf + st->len, ptr, add);
    st->len += add;
    resp_buf[st->len] = '\0';
    return add;
}

/** Parse the JSON document into @p out. Returns false on structural errors. */
static bool parse_weather(const char * json_text, weather_snapshot_t * out)
{
    cJSON * root = cJSON_Parse(json_text);
    if(root == NULL) {
        fprintf(stderr, "weather_client: malformed JSON in forecast response\n");
        return false;
    }

    const cJSON * cur = cJSON_GetObjectItemCaseSensitive(root, "current");
    if(!cJSON_IsObject(cur)) {
        fprintf(stderr, "weather_client: response has no current object\n");
        cJSON_Delete(root);
        return false;
    }

    const cJSON * temp = cJSON_GetObjectItemCaseSensitive(cur, "temperature_2m");
    const cJSON * hum  = cJSON_GetObjectItemCaseSensitive(cur, "relative_humidity_2m");
    const cJSON * code = cJSON_GetObjectItemCaseSensitive(cur, "weather_code");
    const cJSON * wind = cJSON_GetObjectItemCaseSensitive(cur, "wind_speed_10m");

    if(!cJSON_IsNumber(temp) || !cJSON_IsNumber(hum) || !cJSON_IsNumber(code)) {
        fprintf(stderr, "weather_client: current object missing numeric fields\n");
        cJSON_Delete(root);
        return false;
    }

    weather_snapshot_t tmp;
    memset(&tmp, 0, sizeof(tmp));

    tmp.temp_c       = temp->valuedouble;
    tmp.hum_pct      = (int)hum->valuedouble;
    tmp.weather_code = (int)code->valuedouble;
    tmp.wind_kmh     = cJSON_IsNumber(wind) ? wind->valuedouble : 0.0;

    /* Fetch moment (local wall clock), HH:MM. */
    time_t now = time(NULL);
    struct tm lt;
    if(localtime_r(&now, &lt) != NULL) {
        snprintf(tmp.fetched_hhmm, sizeof(tmp.fetched_hhmm),
                 "%02d:%02d", lt.tm_hour, lt.tm_min);
    }
    else {
        snprintf(tmp.fetched_hhmm, sizeof(tmp.fetched_hhmm), "--:--");
    }

    tmp.valid = true;

    cJSON_Delete(root);
    *out = tmp;
    return true;
}

bool weather_client_fetch(weather_snapshot_t * out)
{
    if(out == NULL) return false;

    if(curl_handle == NULL) {
        curl_handle = curl_easy_init();
        if(curl_handle == NULL) {
            fprintf(stderr, "weather_client: curl_easy_init failed\n");
            return false;
        }
    }
    else {
        curl_easy_reset(curl_handle);
    }

    if(resp_buf == NULL) {
        resp_buf = (char *)malloc(RESP_BUF_INIT);
        if(resp_buf == NULL) return false;
        resp_cap = RESP_BUF_INIT;
    }

    const char * env_url = getenv("WEATHER_URL");
    const char * url = (env_url != NULL && env_url[0] != '\0')
                       ? env_url : WEATHER_URL_DEFAULT;

    resp_state_t st = { .len = 0, .truncated = false };
    resp_buf[0] = '\0';

    curl_easy_setopt(curl_handle, CURLOPT_URL, url);
    curl_easy_setopt(curl_handle, CURLOPT_TIMEOUT_MS, 5000L);
    curl_easy_setopt(curl_handle, CURLOPT_CONNECTTIMEOUT_MS, 2000L);
    curl_easy_setopt(curl_handle, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl_handle, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "esp32-4b-sim/1.0");
    curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, &st);

    CURLcode res = curl_easy_perform(curl_handle);
    long code = 0;
    curl_easy_getinfo(curl_handle, CURLINFO_RESPONSE_CODE, &code);

    if(res != CURLE_OK) {
        fprintf(stderr, "weather_client: GET failed: %s%s\n",
                curl_easy_strerror(res),
                st.truncated ? " (response exceeded 64KB cap)" : "");
        return false;
    }
    if(code != 200) {
        fprintf(stderr, "weather_client: GET -> HTTP %ld\n", code);
        return false;
    }

    return parse_weather(resp_buf, out);
}
