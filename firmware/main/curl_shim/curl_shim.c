/**
 * @file curl_shim.c
 *
 * Implementation of the libcurl subset declared in curl/curl.h, backed by
 * esp_http_client. Each curl_easy_perform() builds a fresh
 * esp_http_client handle, runs the request (following redirects manually
 * when CURLOPT_FOLLOWLOCATION is set) and tears it down again; the shim's
 * CURL handle is only an option container, which matches how the clients
 * use it (persistent handle + curl_easy_reset() between calls).
 *
 * HTTPS verification uses the built-in mbedTLS certificate bundle
 * (esp_crt_bundle_attach), needed for the Open-Meteo weather endpoint.
 *
 * Sensitive values (Authorization header, tokens) are NEVER logged; only
 * method/URL/status/errors are.
 */

#include <curl/curl.h>

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>                 /* strncasecmp */

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

static const char *TAG = "curl_shim";

#define SHIM_MAX_REDIRECTS  5
#define SHIM_BUF_SIZE       4096     /* esp_http_client receive buffer */

/*-----------------------------
 * Easy handle
 *----------------------------*/
struct curl_shim_easy {
    /* options */
    char * url;                       /* strdup'd copy (libcurl copies too) */
    char * useragent;                 /* strdup'd copy */
    const char * postfields;          /* borrowed, like libcurl default */
    struct curl_slist * headers;      /* borrowed, owned by the caller */
    curl_write_callback write_cb;
    void * write_data;
    long timeout_ms;                  /* 0 = esp_http_client default */
    long connect_timeout_ms;
    long post;
    long follow_location;

    /* results */
    long response_code;
    bool write_error;                 /* write_cb refused data */
    size_t bytes_received;            /* body bytes delivered to write_cb */
};

/*-----------------------------
 * esp_http_client event handler
 *----------------------------*/

/** Forward every body chunk to the curl WRITEFUNCTION. esp_http_client
 *  delivers chunked bodies as a stream of ON_DATA events, so simply
 *  relaying each chunk reproduces libcurl's repeated write_cb calls.
 *  Returning non-ESP_OK aborts the transfer (write_cb reported short). */
static esp_err_t http_event_handler(esp_http_client_event_t * evt)
{
    struct curl_shim_easy * c = (struct curl_shim_easy *)evt->user_data;

    switch(evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if(c->write_cb != NULL && evt->data_len > 0) {
                size_t got = c->write_cb((char *)evt->data, 1,
                                         (size_t)evt->data_len, c->write_data);
                if(got != (size_t)evt->data_len) {
                    c->write_error = true;
                    ESP_LOGW(TAG, "write callback aborted the transfer");
                    return ESP_FAIL;
                }
                c->bytes_received += got;
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

/*-----------------------------
 * Helpers
 *----------------------------*/
static char * dup_str(const char * s)
{
    if(s == NULL) return NULL;
    size_t n = strlen(s) + 1;
    char * d = (char *)malloc(n);
    if(d != NULL) memcpy(d, s, n);
    return d;
}

static void replace_str(char ** slot, const char * s)
{
    free(*slot);
    *slot = dup_str(s);
}

/** Does the header list already carry a Content-Type? (case-insensitive) */
static bool headers_have_content_type(const struct curl_slist * list)
{
    for(const struct curl_slist * it = list; it != NULL; it = it->next) {
        if(strncasecmp(it->data, "Content-Type:", 13) == 0) return true;
    }
    return false;
}

/** Apply the slist to the esp_http_client handle. Entries look like
 *  "Name: value" (that is all the callers ever append). */
static void apply_headers(esp_http_client_handle_t client,
                          const struct curl_slist * list)
{
    for(const struct curl_slist * it = list; it != NULL; it = it->next) {
        const char * colon = strchr(it->data, ':');
        if(colon == NULL) {
            ESP_LOGW(TAG, "skipping malformed header entry (no ':')");
            continue;
        }
        size_t name_len = (size_t)(colon - it->data);
        char name[64];
        if(name_len >= sizeof(name)) {
            ESP_LOGW(TAG, "header name too long, skipping");
            continue;
        }
        memcpy(name, it->data, name_len);
        name[name_len] = '\0';

        const char * value = colon + 1;
        while(*value == ' ') value++;
        /* value itself is never logged (may be a bearer token) */
        esp_http_client_set_header(client, name, value);
    }
}

static CURLcode map_esp_error(esp_err_t err, bool write_error)
{
    if(write_error)                    return CURLE_WRITE_ERROR;
    switch(err) {
        case ESP_OK:                          return CURLE_OK;
        case ESP_ERR_HTTP_CONNECT:            return CURLE_COULDNT_CONNECT;
        case ESP_ERR_HTTP_CONNECTING:         return CURLE_COULDNT_CONNECT; /* connect phase timed out (IDF v6) */
        case ESP_ERR_HTTP_READ_TIMEOUT:       return CURLE_OPERATION_TIMEDOUT;
        case ESP_ERR_HTTP_CONNECTION_CLOSED:  return CURLE_PARTIAL_FILE;
        case ESP_ERR_NO_MEM:                  return CURLE_OUT_OF_MEMORY;
        case ESP_ERR_HTTP_FETCH_HEADER:       return CURLE_RECV_ERROR;
        default:                              return CURLE_RECV_ERROR;
    }
}

/** True for HTTP statuses that carry a Location header. */
static bool is_redirect_code(long code)
{
    return code == 301 || code == 302 || code == 303 ||
           code == 307 || code == 308;
}

/**
 * Execute a single request against @p url with the current options.
 * Returns the mapped CURLcode, stores the final status code, and copies
 * the Location response header (if any) into @p location_out for the
 * redirect-following loop in curl_easy_perform().
 */
static CURLcode perform_once(struct curl_shim_easy * c, const char * url,
                             char * location_out, size_t location_len)
{
    if(location_len > 0) location_out[0] = '\0';
    esp_http_client_config_t cfg = {
        .url               = url,
        .crt_bundle_attach = esp_crt_bundle_attach,   /* HTTPS verification */
        .event_handler     = http_event_handler,
        .user_data         = c,
        .buffer_size       = SHIM_BUF_SIZE,
        .timeout_ms        = (int)c->timeout_ms,        /* 0 = default */
        /* Follow redirects ourselves (curl semantics; esp_http_client's
         * built-in follower is disabled so the hop cap/log stays ours).
         * CURLOPT_CONNECTTIMEOUT_MS is accepted but not applied: this
         * esp_http_client build only exposes a single timeout_ms. */
        .disable_auto_redirect = true,
    };

    c->bytes_received = 0;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if(client == NULL) {
        ESP_LOGE(TAG, "esp_http_client_init failed");
        return CURLE_OUT_OF_MEMORY;
    }

    const bool is_post = (c->post != 0) || (c->postfields != NULL);
    esp_http_client_set_method(client, is_post ? HTTP_METHOD_POST
                                               : HTTP_METHOD_GET);

    if(c->useragent != NULL) {
        esp_http_client_set_header(client, "User-Agent", c->useragent);
    }
    apply_headers(client, c->headers);

    if(is_post && c->postfields != NULL) {
        /* libcurl default when the caller did not set one themselves */
        if(!headers_have_content_type(c->headers)) {
            esp_http_client_set_header(client, "Content-Type",
                                       "application/x-www-form-urlencoded");
        }
        esp_http_client_set_post_field(client, c->postfields,
                                       strlen(c->postfields));
    }

    esp_err_t err = esp_http_client_perform(client);
    c->response_code = esp_http_client_get_status_code(client);

    /* esp_http_client misreads any response WITHOUT a Content-Length
     * (e.g. HTTP/1.0 Python BaseHTTP servers that close after the body)
     * as chunked, then reports ESP_ERR_HTTP_INCOMPLETE_DATA when the
     * connection closes. The body bytes were still delivered through
     * ON_DATA/write_cb, so treat a close-terminated transfer with data
     * as success; the caller's JSON parse validates completeness. */
    if(err == ESP_ERR_HTTP_INCOMPLETE_DATA && c->bytes_received > 0
       && !c->write_error) {
        ESP_LOGD(TAG, "connection-close termination (%zu bytes received), "
                      "treating response as complete", c->bytes_received);
        err = ESP_OK;
    }

    /* Capture Location before cleanup for the redirect loop. A truncated
     * copy would silently follow a mangled URL - fail loudly instead. */
    if(is_redirect_code(c->response_code) && location_len > 0) {
        char * loc = NULL;
        if(esp_http_client_get_header(client, "Location", &loc) == ESP_OK
           && loc != NULL) {
            int n = snprintf(location_out, location_len, "%s", loc);
            free(loc);
            if(n < 0 || (size_t)n >= location_len) {
                ESP_LOGE(TAG, "redirect Location exceeds %zu bytes, refusing "
                              "to follow a truncated URL", location_len);
                esp_http_client_cleanup(client);
                return CURLE_URL_MALFORMAT;
            }
        }
    }

    CURLcode rc = map_esp_error(err, c->write_error);
    if(rc != CURLE_OK) {
        ESP_LOGW(TAG, "%s %s failed: %s (curl=%d, esp=%s)",
                 is_post ? "POST" : "GET", url,
                 curl_easy_strerror(rc), (int)rc, esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    return rc;
}

/*-----------------------------
 * Public API
 *----------------------------*/
CURLcode curl_global_init(long flags)
{
    (void)flags;     /* mbedtls/lwip are initialised by IDF startup */
    return CURLE_OK;
}

CURL * curl_easy_init(void)
{
    struct curl_shim_easy * c = (struct curl_shim_easy *)calloc(1, sizeof(*c));
    if(c == NULL) ESP_LOGE(TAG, "curl_easy_init: out of memory");
    return c;
}

void curl_easy_reset(CURL * curl)
{
    if(curl == NULL) return;
    free(curl->url);
    free(curl->useragent);
    memset(curl, 0, sizeof(*curl));
}

CURLcode curl_easy_setopt(CURL * curl, CURLoption option, ...)
{
    if(curl == NULL) return CURLE_FAILED_INIT;

    va_list ap;
    va_start(ap, option);
    CURLcode rc = CURLE_OK;

    switch(option) {
        case CURLOPT_URL: {
            const char * s = va_arg(ap, const char *);
            if(s == NULL) { rc = CURLE_URL_MALFORMAT; break; }
            replace_str(&curl->url, s);
            if(curl->url == NULL) rc = CURLE_OUT_OF_MEMORY;
            break;
        }
        case CURLOPT_USERAGENT:
            replace_str(&curl->useragent, va_arg(ap, const char *));
            break;
        case CURLOPT_HTTPHEADER:
            curl->headers = va_arg(ap, struct curl_slist *);
            break;
        case CURLOPT_WRITEFUNCTION:
            curl->write_cb = va_arg(ap, curl_write_callback);
            break;
        case CURLOPT_WRITEDATA:
            curl->write_data = va_arg(ap, void *);
            break;
        case CURLOPT_POSTFIELDS:
            curl->postfields = va_arg(ap, const char *);
            break;
        case CURLOPT_TIMEOUT_MS:
            curl->timeout_ms = va_arg(ap, long);
            break;
        case CURLOPT_CONNECTTIMEOUT_MS:
            curl->connect_timeout_ms = va_arg(ap, long);
            break;
        case CURLOPT_POST:
            curl->post = va_arg(ap, long);
            break;
        case CURLOPT_FOLLOWLOCATION:
            curl->follow_location = va_arg(ap, long);
            break;
        case CURLOPT_NOSIGNAL:
            (void)va_arg(ap, long);   /* accepted: shim is always threaded */
            break;
        default:
            ESP_LOGE(TAG, "curl_easy_setopt: unsupported option %d", (int)option);
            rc = CURLE_UNKNOWN_OPTION;
            break;
    }

    va_end(ap);
    return rc;
}

CURLcode curl_easy_perform(CURL * curl)
{
    if(curl == NULL) return CURLE_FAILED_INIT;
    if(curl->url == NULL || curl->url[0] == '\0') {
        ESP_LOGE(TAG, "perform without CURLOPT_URL");
        return CURLE_URL_MALFORMAT;
    }

    curl->response_code = 0;
    curl->write_error = false;

    /* Manual redirect following (esp_http_client does not auto-follow).
     * The callers only ever set FOLLOWLOCATION on idempotent GETs, so
     * re-issuing a GET at the new location is correct. */
    char current_url[512];
    char location[512];
    if(snprintf(current_url, sizeof(current_url), "%s", curl->url)
       >= (int)sizeof(current_url)) {
        ESP_LOGE(TAG, "CURLOPT_URL exceeds %zu bytes, refusing to request "
                      "a truncated URL", sizeof(current_url));
        return CURLE_URL_MALFORMAT;
    }

    for(int hop = 0; hop < SHIM_MAX_REDIRECTS; hop++) {
        CURLcode rc = perform_once(curl, current_url, location, sizeof(location));
        if(rc != CURLE_OK) return rc;

        if(!is_redirect_code(curl->response_code) || !curl->follow_location) {
            return CURLE_OK;
        }
        if(location[0] == '\0') {
            /* Redirect status without a Location header: nothing to follow;
             * let the caller see the 3xx code as the final response. */
            ESP_LOGW(TAG, "redirect without Location header, stopping");
            return CURLE_OK;
        }
        ESP_LOGI(TAG, "following redirect -> %s", location);
        if(snprintf(current_url, sizeof(current_url), "%s", location)
           >= (int)sizeof(current_url)) {
            ESP_LOGE(TAG, "redirect Location exceeds %zu bytes, refusing to "
                          "follow a truncated URL", sizeof(current_url));
            return CURLE_URL_MALFORMAT;
        }
    }

    /* Fallback: redirects beyond the hop cap count as too many. */
    ESP_LOGW(TAG, "redirect limit reached for %s", current_url);
    return CURLE_TOO_MANY_REDIRECTS;
}

CURLcode curl_easy_getinfo(CURL * curl, CURLINFO info, ...)
{
    if(curl == NULL) return CURLE_FAILED_INIT;

    va_list ap;
    va_start(ap, info);
    CURLcode rc;

    switch(info) {
        case CURLINFO_RESPONSE_CODE: {
            long * out = va_arg(ap, long *);
            if(out == NULL) { rc = CURLE_FAILED_INIT; break; }
            *out = curl->response_code;
            rc = CURLE_OK;
            break;
        }
        default:
            ESP_LOGE(TAG, "curl_easy_getinfo: unsupported info %d", (int)info);
            rc = CURLE_UNKNOWN_OPTION;
            break;
    }

    va_end(ap);
    return rc;
}

void curl_easy_cleanup(CURL * curl)
{
    if(curl == NULL) return;
    curl_easy_reset(curl);
    free(curl);
}

const char * curl_easy_strerror(CURLcode code)
{
    switch(code) {
        case CURLE_OK:                         return "No error";
        case CURLE_UNSUPPORTED_PROTOCOL:       return "Unsupported protocol";
        case CURLE_FAILED_INIT:                return "Failed initialization";
        case CURLE_URL_MALFORMAT:              return "URL using bad/illegal format";
        case CURLE_COULDNT_RESOLVE_HOST:       return "Couldn't resolve host name";
        case CURLE_COULDNT_CONNECT:            return "Couldn't connect to server";
        case CURLE_WEIRD_SERVER_REPLY:         return "Weird server reply";
        case CURLE_PARTIAL_FILE:               return "Transfer closed with data remaining";
        case CURLE_WRITE_ERROR:                return "Failed writing received data";
        case CURLE_OUT_OF_MEMORY:              return "Out of memory";
        case CURLE_OPERATION_TIMEDOUT:         return "Timeout was reached";
        case CURLE_SSL_CONNECT_ERROR:          return "SSL connect error";
        case CURLE_ABORTED_BY_CALLBACK:        return "Operation was aborted by an application callback";
        case CURLE_TOO_MANY_REDIRECTS:         return "Number of redirects hit maximum amount";
        case CURLE_UNKNOWN_OPTION:             return "Unknown option";
        case CURLE_GOT_NOTHING:                return "Server returned nothing";
        case CURLE_SEND_ERROR:                 return "Send failure";
        case CURLE_RECV_ERROR:                 return "Failure when receiving data from the peer";
        case CURLE_PEER_FAILED_VERIFICATION:   return "SSL peer certificate was not ok";
        default:                               return "Unknown error";
    }
}

struct curl_slist * curl_slist_append(struct curl_slist * list, const char * str)
{
    if(str == NULL) return list;

    struct curl_slist * node = (struct curl_slist *)malloc(sizeof(*node));
    if(node == NULL) {
        ESP_LOGE(TAG, "curl_slist_append: out of memory");
        return list;
    }
    node->data = dup_str(str);
    node->next = NULL;
    if(node->data == NULL) {
        free(node);
        ESP_LOGE(TAG, "curl_slist_append: out of memory");
        return list;
    }

    if(list == NULL) return node;

    struct curl_slist * tail = list;
    while(tail->next != NULL) tail = tail->next;
    tail->next = node;
    return list;
}

void curl_slist_free_all(struct curl_slist * list)
{
    while(list != NULL) {
        struct curl_slist * next = list->next;
        free(list->data);
        free(list);
        list = next;
    }
}
