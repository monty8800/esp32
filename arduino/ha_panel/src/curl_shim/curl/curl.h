/**
 * @file curl/curl.h
 *
 * libcurl subset shim for the ESP-IDF firmware build.
 *
 * The firmware links the UNMODIFIED simulator network code (src/ha_client.c,
 * src/server_client.c, src/weather_client.c) which uses libcurl. On the
 * device there is no libcurl; this header + curl_shim.c provide the exact
 * subset actually used, backed by esp_http_client:
 *
 *   functions:  curl_global_init, curl_easy_init, curl_easy_reset,
 *               curl_easy_setopt, curl_easy_perform, curl_easy_getinfo,
 *               curl_easy_cleanup, curl_easy_strerror,
 *               curl_slist_append, curl_slist_free_all
 *   options:    CURLOPT_URL, CURLOPT_HTTPHEADER, CURLOPT_TIMEOUT_MS,
 *               CURLOPT_CONNECTTIMEOUT_MS, CURLOPT_NOSIGNAL,
 *               CURLOPT_WRITEFUNCTION, CURLOPT_WRITEDATA,
 *               CURLOPT_POST, CURLOPT_POSTFIELDS,
 *               CURLOPT_FOLLOWLOCATION, CURLOPT_USERAGENT
 *   info:       CURLINFO_RESPONSE_CODE
 *
 * Any option NOT in the list above makes curl_easy_setopt() log an error
 * and return CURLE_UNKNOWN_OPTION - nothing is silently ignored.
 *
 * Option/info enum values mirror real libcurl so this header stays source
 * compatible if the code ever moves to a real libcurl build.
 */

#ifndef CURL_SHIM_CURL_H
#define CURL_SHIM_CURL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*-----------------------------
 * Error codes (libcurl numbering)
 *----------------------------*/
typedef enum {
    CURLE_OK                     = 0,
    CURLE_UNSUPPORTED_PROTOCOL   = 1,
    CURLE_FAILED_INIT            = 2,
    CURLE_URL_MALFORMAT          = 3,
    CURLE_COULDNT_RESOLVE_HOST   = 6,
    CURLE_COULDNT_CONNECT        = 7,
    CURLE_WEIRD_SERVER_REPLY     = 8,
    CURLE_PARTIAL_FILE           = 18,
    CURLE_WRITE_ERROR            = 23,
    CURLE_OUT_OF_MEMORY          = 27,
    CURLE_OPERATION_TIMEDOUT     = 28,
    CURLE_SSL_CONNECT_ERROR      = 35,
    CURLE_ABORTED_BY_CALLBACK    = 42,
    CURLE_TOO_MANY_REDIRECTS     = 47,
    CURLE_UNKNOWN_OPTION         = 48,
    CURLE_GOT_NOTHING            = 52,
    CURLE_SEND_ERROR             = 55,
    CURLE_RECV_ERROR             = 56,
    CURLE_PEER_FAILED_VERIFICATION = 60,
} CURLcode;

/*-----------------------------
 * Global init flags
 *----------------------------*/
#define CURL_GLOBAL_SSL      (1 << 0)
#define CURL_GLOBAL_DEFAULT  (CURL_GLOBAL_SSL | (1 << 1))

/*-----------------------------
 * Options (libcurl numbering)
 *----------------------------*/
typedef enum {
    /* long options (type base 0) */
    CURLOPT_POST              = 47,
    CURLOPT_FOLLOWLOCATION    = 52,
    CURLOPT_NOSIGNAL          = 99,
    CURLOPT_TIMEOUT_MS        = 155,
    CURLOPT_CONNECTTIMEOUT_MS = 148,

    /* string / pointer options (type base 10000) */
    CURLOPT_WRITEDATA         = 10001,
    CURLOPT_URL               = 10002,
    CURLOPT_POSTFIELDS        = 10015,
    CURLOPT_USERAGENT         = 10018,
    CURLOPT_HTTPHEADER        = 10023,

    /* function pointer options (type base 20000) */
    CURLOPT_WRITEFUNCTION     = 20011,
} CURLoption;

/*-----------------------------
 * Info (libcurl numbering)
 *----------------------------*/
typedef enum {
    CURLINFO_RESPONSE_CODE = 0x200002,
} CURLINFO;

/*-----------------------------
 * Types
 *----------------------------*/
typedef struct curl_shim_easy CURL;          /* opaque, defined in curl_shim.c */

struct curl_slist {
    char * data;
    struct curl_slist * next;
};

typedef size_t (* curl_write_callback)(char * data, size_t size, size_t nmemb,
                                       void * userdata);

/*-----------------------------
 * API
 *----------------------------*/
CURLcode curl_global_init(long flags);

CURL * curl_easy_init(void);
void   curl_easy_reset(CURL * curl);
CURLcode curl_easy_setopt(CURL * curl, CURLoption option, ...);
CURLcode curl_easy_perform(CURL * curl);
CURLcode curl_easy_getinfo(CURL * curl, CURLINFO info, ...);
void   curl_easy_cleanup(CURL * curl);
const char * curl_easy_strerror(CURLcode code);

struct curl_slist * curl_slist_append(struct curl_slist * list, const char * str);
void curl_slist_free_all(struct curl_slist * list);

#ifdef __cplusplus
}
#endif

#endif /* CURL_SHIM_CURL_H */
