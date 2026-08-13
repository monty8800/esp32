/**
 * @file ha_client.c
 *
 * Implementation of the minimal Home Assistant REST client (libcurl based).
 * All logging goes to stderr with an "ha_client:" prefix. The bearer token
 * is NEVER printed - at most its length.
 */

#include "ha_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>

/*-----------------------------
 * Configuration
 *----------------------------*/
#define HA_DEFAULT_BASE_URL "http://192.168.9.207:8123"
#define HA_RESP_BUF_SIZE    4096    /* fixed response accumulation buffer */
#define HA_TOKEN_MAX        512
#define HA_URL_MAX          160     /* base URL / misc paths only */
#define HA_FULL_URL_MAX     320     /* base_url + "/api/states/" or "/api/services/" + path + NUL */
#define HA_ENTITY_ID_MAX    96
#define HA_SERVICE_BODY_MAX 256     /* upper bound for a service call JSON body */

static char token[HA_TOKEN_MAX];
static char base_url[HA_URL_MAX];
static bool ready = false;

/* Entity IDs after environment-variable overrides. */
static char entity_temp[HA_ENTITY_ID_MAX];
static char entity_hum[HA_ENTITY_ID_MAX];
static char entity_pm25[HA_ENTITY_ID_MAX];
static char entity_mode[HA_ENTITY_ID_MAX];
static char entity_power[HA_ENTITY_ID_MAX];
static char entity_ac[HA_ENTITY_ID_MAX];
static char entity_lamp[HA_ENTITY_ID_MAX];
static char entity_cam1[HA_ENTITY_ID_MAX];
static char entity_cam2[HA_ENTITY_ID_MAX];

/*-----------------------------
 * Helpers
 *----------------------------*/

/** Copy @p src into @p dst, falling back to @p fallback only when src is
 *  NULL. An explicitly empty value is kept as-is: per ha_client.h an empty
 *  entity ID disables the corresponding device card. */
static void resolve_env(char * dst, size_t dst_len, const char * env_name, const char * fallback)
{
    const char * v = getenv(env_name);
    if(v == NULL) v = fallback;
    int n = snprintf(dst, dst_len, "%s", v);
    if(n < 0 || (size_t)n >= dst_len) {
        fprintf(stderr, "ha_client: %s value too long (%d chars), falling back to default %s\n",
                env_name, n, fallback);
        snprintf(dst, dst_len, "%s", fallback);
    }
}

/**
 * Load the bearer token: HA_TOKEN env var first, otherwise the file
 * ~/.ha_esp32_token (path built from $HOME, trailing newline stripped).
 */
static bool load_token(void)
{
    const char * env_token = getenv("HA_TOKEN");
    if(env_token != NULL && env_token[0] != '\0') {
        size_t len = strlen(env_token);
        if(len >= sizeof(token)) {
            fprintf(stderr, "ha_client: HA_TOKEN env too long (%zu chars, max %zu), refusing to use it\n",
                    len, sizeof(token) - 1);
            return false;
        }
        snprintf(token, sizeof(token), "%s", env_token);
        fprintf(stderr, "ha_client: token from HA_TOKEN env (%zu chars)\n", len);
        return true;
    }

    const char * home = getenv("HOME");
    if(home == NULL || home[0] == '\0') {
        fprintf(stderr, "ha_client: no HA_TOKEN env and no HOME, token unavailable\n");
        return false;
    }

    char path[HA_URL_MAX];
    snprintf(path, sizeof(path), "%s/.ha_esp32_token", home);

    FILE * f = fopen(path, "r");
    if(f == NULL) {
        fprintf(stderr, "ha_client: cannot open %s, token unavailable\n", path);
        return false;
    }

    size_t n = fread(token, 1, sizeof(token) - 1, f);
    if(fgetc(f) != EOF) {
        fprintf(stderr, "ha_client: token file %s too long, truncated to %d chars\n",
                path, (int)(sizeof(token) - 1));
    }
    fclose(f);
    token[n] = '\0';

    /* Strip trailing newline / whitespace. */
    while(n > 0 && (token[n - 1] == '\n' || token[n - 1] == '\r' ||
                    token[n - 1] == ' '  || token[n - 1] == '\t')) {
        token[--n] = '\0';
    }

    if(n == 0) {
        fprintf(stderr, "ha_client: token file %s is empty\n", path);
        return false;
    }

    fprintf(stderr, "ha_client: token loaded from %s (%zu chars)\n", path, n);
    return true;
}

/* Fixed-size response accumulator. */
typedef struct {
    char   buf[HA_RESP_BUF_SIZE];
    size_t len;
} resp_buf_t;

static size_t write_cb(char * ptr, size_t size, size_t nmemb, void * userdata)
{
    resp_buf_t * r = (resp_buf_t *)userdata;
    size_t add = size * nmemb;
    size_t room = (HA_RESP_BUF_SIZE - 1) - r->len;
    if(add > room) add = room;   /* truncate silently, buffer is capped */
    memcpy(r->buf + r->len, ptr, add);
    r->len += add;
    r->buf[r->len] = '\0';
    return size * nmemb;         /* report full size or curl aborts */
}

/**
 * Hand-rolled extraction of the top-level "state" string value:
 * locate `"state"`, skip the colon and whitespace, expect a quote, then copy
 * until the closing quote (backslash escapes are consumed, so `\"` becomes
 * part of the value instead of terminating it). A `"state"` occurrence that
 * is not followed by `: "..."` is skipped and the search continues. Good
 * enough for flat HA state objects.
 */
static bool extract_state(const char * json, char * out, size_t out_len)
{
    const char * key = strstr(json, "\"state\"");
    while(key != NULL) {
        const char * p = key + 7;                 /* past `"state"` */
        while(*p == ' ' || *p == '\t') p++;
        if(*p == ':') {
            p++;
            while(*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
            if(*p == '"') {
                p++;                              /* first char of the value */

                size_t i = 0;
                while(*p != '\0' && *p != '"') {
                    if(*p == '\\' && *(p + 1) != '\0') {
                        p++;                      /* skip escape backslash, keep char */
                    }
                    if(i + 1 < out_len) out[i++] = *p;
                    p++;
                }
                if(*p != '"') return false;      /* unterminated string */
                out[i] = '\0';
                return true;
            }
        }
        /* Not in `"state": "..."` key form: look for the next occurrence. */
        key = strstr(key + 7, "\"state\"");
    }
    return false;
}

/**
 * Strict whitelist for entity IDs: they are spliced into the GET
 * /api/states/<id> URL path (so ?, #, %, '/' and friends are forbidden)
 * and into the hand-built JSON bodies (so quotes, backslashes and control
 * characters are forbidden too). The body is built without an escaper, so
 * every variable part (the entity IDs from the environment) is validated
 * with this check once at init time. The remaining body parts are trusted
 * compile-time constants (on/off, the Chinese option strings), so service
 * bodies never contain unvalidated data.
 */
static bool entity_id_ok(const char * s)
{
    for(const char * c = s; *c != '\0'; c++) {
        if(!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') ||
             *c == '_' || *c == '.')) {
            return false;
        }
    }
    return true;
}

/** Shared curl setup: auth header, timeouts, response sink. URL stays out -
 *  GET /api/states and POST /api/services build different paths.
 *  timeout_ms is passed per call: polling GETs need less slack than service
 *  POSTs, whose HA-side handling (esp. via cloud integrations) can take
 *  longer than a state read. */
static CURL * setup_curl(struct curl_slist ** headers_out,
                         resp_buf_t * resp, const char * url, long timeout_ms)
{
    CURL * curl = curl_easy_init();
    if(curl == NULL) return NULL;

    /* Static scratch buffer for the auth header. Safe under the current
     * usage model: all requests run on a single network worker thread and
     * each call finishes (the header string is copied into the curl_slist
     * by curl_slist_append) before the next one starts. */
    static char auth_hdr[HA_TOKEN_MAX + 32];
    snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: Bearer %s", token);

    struct curl_slist * headers = NULL;
    headers = curl_slist_append(headers, auth_hdr);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 500L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, resp);

    *headers_out = headers;
    return curl;
}

/** Log 401/403 with a hint that the token is invalid. */
static void log_auth_error(long code, const char * what)
{
    if(code == 401 || code == 403) {
        fprintf(stderr, "ha_client: HTTP %ld for %s - token invalid or insufficient rights\n",
                code, what);
    }
}

/*-----------------------------
 * Public API
 *----------------------------*/

bool ha_client_init(void)
{
    if(curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        fprintf(stderr, "ha_client: curl_global_init failed\n");
        return false;
    }

    if(!load_token()) {
        fprintf(stderr, "ha_client: init failed - no token (length 0)\n");
        return false;
    }

    const char * url = getenv("HA_BASE_URL");
    snprintf(base_url, sizeof(base_url), "%s",
             (url != NULL && url[0] != '\0') ? url : HA_DEFAULT_BASE_URL);

    resolve_env(entity_temp,  sizeof(entity_temp),  "HA_ENTITY_TEMP",  HA_ENTITY_TEMP_DEFAULT);
    resolve_env(entity_hum,   sizeof(entity_hum),   "HA_ENTITY_HUM",   HA_ENTITY_HUM_DEFAULT);
    resolve_env(entity_pm25,  sizeof(entity_pm25),  "HA_ENTITY_PM25",  HA_ENTITY_PM25_DEFAULT);
    resolve_env(entity_mode,  sizeof(entity_mode),  "HA_ENTITY_MODE",  HA_ENTITY_MODE_DEFAULT);
    resolve_env(entity_power, sizeof(entity_power), "HA_ENTITY_POWER", HA_ENTITY_POWER_DEFAULT);
    resolve_env(entity_ac,    sizeof(entity_ac),    "HA_ENTITY_AC",    HA_ENTITY_AC_DEFAULT);
    resolve_env(entity_lamp,  sizeof(entity_lamp),  "HA_ENTITY_LAMP",  HA_ENTITY_LAMP_DEFAULT);
    resolve_env(entity_cam1,  sizeof(entity_cam1),  "HA_ENTITY_CAM1",  HA_ENTITY_CAM1_DEFAULT);
    resolve_env(entity_cam2,  sizeof(entity_cam2),  "HA_ENTITY_CAM2",  HA_ENTITY_CAM2_DEFAULT);

    /* Safety net for the hand-built JSON bodies: the entity IDs are the
     * only variable strings ever spliced into a request body, so validate
     * them here once. An unsafe ID is blanked out; requests built from an
     * empty entity ID are refused below (empty body check / HA 404), so
     * no unvalidated string can reach the wire. */
    struct {
        char * id;
        const char * name;
    } ids[] = {
        { entity_temp,  "HA_ENTITY_TEMP"  },
        { entity_hum,   "HA_ENTITY_HUM"   },
        { entity_pm25,  "HA_ENTITY_PM25"  },
        { entity_mode,  "HA_ENTITY_MODE"  },
        { entity_power, "HA_ENTITY_POWER" },
        { entity_ac,    "HA_ENTITY_AC"    },
        { entity_lamp,  "HA_ENTITY_LAMP"  },
        { entity_cam1,  "HA_ENTITY_CAM1"  },
        { entity_cam2,  "HA_ENTITY_CAM2"  },
    };
    for(size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        if(ids[i].id[0] == '\0') continue;   /* explicitly disabled, fine */
        if(!entity_id_ok(ids[i].id)) {
            fprintf(stderr, "ha_client: %s contains invalid char, entity disabled\n",
                    ids[i].name);
            ids[i].id[0] = '\0';
        }
    }

    fprintf(stderr, "ha_client: ready, base url %s\n", base_url);
    ready = true;
    return true;
}

bool ha_client_fetch_state(const char * entity_id, char * out, size_t out_len)
{
    if(!ready || entity_id == NULL || out == NULL || out_len == 0) return false;
    if(entity_id[0] == '\0') return false;   /* entity disabled at init */
    out[0] = '\0';

    char url[HA_FULL_URL_MAX];
    int n = snprintf(url, sizeof(url), "%s/api/states/%s", base_url, entity_id);
    if(n < 0 || (size_t)n >= sizeof(url)) {
        fprintf(stderr, "ha_client: URL for %s exceeds %zu bytes, refusing request\n",
                entity_id, sizeof(url));
        return false;
    }

    resp_buf_t resp = { .len = 0 };
    resp.buf[0] = '\0';

    struct curl_slist * headers = NULL;
    CURL * curl = setup_curl(&headers, &resp, url, 1200L);
    if(curl == NULL) return false;

    CURLcode res = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if(res != CURLE_OK) {
        fprintf(stderr, "ha_client: GET %s failed: %s\n", entity_id, curl_easy_strerror(res));
        return false;
    }
    if(code != 200) {
        fprintf(stderr, "ha_client: GET %s -> HTTP %ld\n", entity_id, code);
        log_auth_error(code, entity_id);
        return false;
    }
    if(!extract_state(resp.buf, out, out_len)) {
        fprintf(stderr, "ha_client: GET %s -> no \"state\" key in response\n", entity_id);
        return false;
    }
    fprintf(stderr, "ha_client: GET %s = %s\n", entity_id, out);
    return true;
}

bool ha_client_call_service(const char * domain, const char * service,
                            const char * json_body)
{
    if(!ready || domain == NULL || service == NULL || json_body == NULL) return false;

    /* Skip calls whose entity ID was blanked at init (unsafe env value):
     * every body this client builds starts with the entity_id field. */
    if(strstr(json_body, "\"entity_id\":\"\"") != NULL) {
        fprintf(stderr, "ha_client: POST %s/%s skipped, entity disabled at init\n",
                domain, service);
        return false;
    }

    /* Domain / service become URL path segments: allow only [a-z0-9_]. */
    for(const char * c = domain; *c != '\0'; c++) {
        if(!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '_')) {
            fprintf(stderr, "ha_client: service domain contains invalid char, refusing request\n");
            return false;
        }
    }
    for(const char * c = service; *c != '\0'; c++) {
        if(!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '_')) {
            fprintf(stderr, "ha_client: service name contains invalid char, refusing request\n");
            return false;
        }
    }

    char url[HA_FULL_URL_MAX];
    int n = snprintf(url, sizeof(url), "%s/api/services/%s/%s", base_url, domain, service);
    if(n < 0 || (size_t)n >= sizeof(url)) {
        fprintf(stderr, "ha_client: URL for %s/%s exceeds %zu bytes, refusing request\n",
                domain, service, sizeof(url));
        return false;
    }

    /* Body safety is guaranteed upstream: it is assembled only from
     * trusted compile-time constants (on/off, the Chinese option strings)
     * and entity IDs that passed entity_id_ok() in ha_client_init(), so no
     * whole-body character scan is needed here - a legitimate body always
     * contains the JSON string quotes. Only the size bound is enforced. */
    size_t body_len = strlen(json_body);
    if(body_len >= HA_SERVICE_BODY_MAX) {
        fprintf(stderr, "ha_client: POST %s/%s rejected, body exceeds %d bytes\n",
                domain, service, HA_SERVICE_BODY_MAX);
        return false;
    }

    resp_buf_t resp = { .len = 0 };
    resp.buf[0] = '\0';

    struct curl_slist * headers = NULL;
    CURL * curl = setup_curl(&headers, &resp, url, 2000L);
    if(curl == NULL) return false;

    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body);

    CURLcode res = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if(res != CURLE_OK) {
        fprintf(stderr, "ha_client: POST %s/%s failed: %s\n",
                domain, service, curl_easy_strerror(res));
        return false;
    }
    if(code != 200) {
        fprintf(stderr, "ha_client: POST %s/%s failed, HTTP %ld\n",
                domain, service, code);
        log_auth_error(code, url + strlen(base_url));
        return false;
    }

    fprintf(stderr, "ha_client: POST %s/%s ok\n", domain, service);
    return true;
}

const char * ha_entity_temp(void)  { return entity_temp; }
const char * ha_entity_hum(void)   { return entity_hum; }
const char * ha_entity_pm25(void)  { return entity_pm25; }
const char * ha_entity_mode(void)  { return entity_mode; }
const char * ha_entity_power(void) { return entity_power; }
const char * ha_entity_ac(void)    { return entity_ac; }
const char * ha_entity_lamp(void)  { return entity_lamp; }
const char * ha_entity_cam1(void)  { return entity_cam1; }
const char * ha_entity_cam2(void)  { return entity_cam2; }
