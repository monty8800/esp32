/**
 * @file server_client.c
 *
 * GET http://192.168.9.206:8787/api/summary and parse it into
 * server_snapshot_t with cJSON. Every failure mode (DNS / connect / timeout,
 * non-200, truncated body, malformed JSON, missing fields) simply returns
 * false - this module never crashes on a hostile or absent server.
 *
 * Response schema (verified live 2026-08-12):
 * {
 *   "time": "2026-08-12 10:19:12",
 *   "total": 10, "online": 10,
 *   "hosts": [ { "id": "pve", "name": "Proxmox 宿主机", "ip": "192.168.9.202",
 *                "desc": "Proxmox VE 8.2.2 虚拟化宿主机 · ...",
 *                "online": true, "latency_ms": 0.1,
 *                "stats": null | { "cpu_pct": 1.6,
 *                                   "mem_used": 5353439232, "mem_total": 8589934592,
 *                                   "disk_used": 0, "disk_total": 107374182400 },
 *                "probes": [ {"name": "Web 管理 (8006)", "online": true} ] } ]
 * }
 * stats byte counters are converted to GB here; missing stats / zero disk
 * usage (VMs without a guest agent) are stored as -1 = unknown.
 */

#include "server_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>

#include "third_party/cjson/cJSON.h"

#define RESP_BUF_INIT   (8 * 1024)     /* initial response buffer size */
#define RESP_BUF_MAX    (256 * 1024)   /* hard cap, abort beyond this */

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

/** Copy a cJSON string item into a fixed-size destination (truncating). */
static void json_str(const cJSON * obj, const char * key, char * dst, size_t dst_len)
{
    dst[0] = '\0';
    const cJSON * it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if(cJSON_IsString(it) && it->valuestring != NULL) {
        snprintf(dst, dst_len, "%s", it->valuestring);
    }
}

/** Truncate a NUL-terminated UTF-8 string in place so it never ends with a
 * partial multi-byte sequence (snprintf truncation can cut a 3-byte CJK
 * character in half, producing mojibake). */
static void utf8_safe_truncate(char * s)
{
    size_t len = strlen(s);
    size_t i = 0, last_ok = 0;
    while(i < len) {
        unsigned char c = (unsigned char)s[i];
        size_t n;
        if(c < 0x80)            n = 1;
        else if((c & 0xE0) == 0xC0) n = 2;
        else if((c & 0xF0) == 0xE0) n = 3;
        else if((c & 0xF8) == 0xF0) n = 4;
        else { i++; last_ok = i; continue; }   /* stray byte: keep as-is */
        if(i + n > len) break;                 /* incomplete sequence at cut */
        i += n;
        last_ok = i;
    }
    s[last_ok] = '\0';
}

/** Parse the JSON document into @p out. Returns false on structural errors. */
static bool parse_summary(const char * json_text, server_snapshot_t * out)
{
    cJSON * root = cJSON_Parse(json_text);
    if(root == NULL) {
        fprintf(stderr, "server_client: malformed JSON in summary response\n");
        return false;
    }

    const cJSON * hosts = cJSON_GetObjectItemCaseSensitive(root, "hosts");
    if(!cJSON_IsArray(hosts)) {
        fprintf(stderr, "server_client: summary response has no hosts array\n");
        cJSON_Delete(root);
        return false;
    }

    server_snapshot_t tmp;
    memset(&tmp, 0, sizeof(tmp));

    json_str(root, "time", tmp.time_str, sizeof(tmp.time_str));

    const cJSON * total = cJSON_GetObjectItemCaseSensitive(root, "total");
    const cJSON * online = cJSON_GetObjectItemCaseSensitive(root, "online");
    tmp.total  = cJSON_IsNumber(total)  ? (int)total->valuedouble  : 0;
    tmp.online = cJSON_IsNumber(online) ? (int)online->valuedouble : 0;

    int idx = 0;
    const cJSON * h;
    cJSON_ArrayForEach(h, hosts) {
        if(idx >= NET_MAX_HOSTS) {
            fprintf(stderr, "server_client: more than %d hosts, truncating\n", NET_MAX_HOSTS);
            break;
        }
        if(!cJSON_IsObject(h)) continue;

        server_host_t * host = &tmp.hosts[idx];
        json_str(h, "id",   host->id,   sizeof(host->id));
        json_str(h, "name", host->name, sizeof(host->name));
        json_str(h, "ip",   host->ip,   sizeof(host->ip));
        json_str(h, "desc", host->desc, sizeof(host->desc));
        utf8_safe_truncate(host->desc);   /* never cut a CJK character in half */

        const cJSON * on  = cJSON_GetObjectItemCaseSensitive(h, "online");
        const cJSON * lat = cJSON_GetObjectItemCaseSensitive(h, "latency_ms");
        host->online     = cJSON_IsTrue(on);
        host->latency_ms = cJSON_IsNumber(lat) ? lat->valuedouble : 0.0;

        /* Resource stats (bytes -> GB); -1 = unknown. */
        host->mem_used_gb  = -1.0;
        host->mem_total_gb = -1.0;
        host->disk_used_gb  = -1.0;
        host->disk_total_gb = -1.0;
        const cJSON * stats = cJSON_GetObjectItemCaseSensitive(h, "stats");
        if(cJSON_IsObject(stats)) {
            const cJSON * mu = cJSON_GetObjectItemCaseSensitive(stats, "mem_used");
            const cJSON * mt = cJSON_GetObjectItemCaseSensitive(stats, "mem_total");
            const cJSON * du = cJSON_GetObjectItemCaseSensitive(stats, "disk_used");
            const cJSON * dt = cJSON_GetObjectItemCaseSensitive(stats, "disk_total");
            const double GB = 1024.0 * 1024.0 * 1024.0;
            if(cJSON_IsNumber(mu) && cJSON_IsNumber(mt) && mt->valuedouble > 0.0) {
                host->mem_used_gb  = mu->valuedouble / GB;
                host->mem_total_gb = mt->valuedouble / GB;
            }
            /* VMs report disk_used=0 (no guest agent): treat as unknown. */
            if(cJSON_IsNumber(du) && cJSON_IsNumber(dt) && dt->valuedouble > 0.0
               && du->valuedouble > 0.0) {
                host->disk_used_gb  = du->valuedouble / GB;
                host->disk_total_gb = dt->valuedouble / GB;
            }
        }

        const cJSON * probes = cJSON_GetObjectItemCaseSensitive(h, "probes");
        if(cJSON_IsArray(probes)) {
            const cJSON * p;
            cJSON_ArrayForEach(p, probes) {
                if(host->probe_count >= NET_MAX_PROBES) break;
                if(!cJSON_IsObject(p)) continue;
                server_probe_t * probe = &host->probes[host->probe_count];
                json_str(p, "name", probe->name, sizeof(probe->name));
                const cJSON * pon = cJSON_GetObjectItemCaseSensitive(p, "online");
                probe->online = cJSON_IsTrue(pon);
                host->probe_count++;
            }
        }

        idx++;
    }
    tmp.host_count = idx;
    tmp.valid = true;

    cJSON_Delete(root);
    *out = tmp;
    return true;
}

bool server_client_fetch(server_snapshot_t * out)
{
    if(out == NULL) return false;

    if(curl_handle == NULL) {
        curl_handle = curl_easy_init();
        if(curl_handle == NULL) {
            fprintf(stderr, "server_client: curl_easy_init failed\n");
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

    const char * env_url = getenv("SERVER_SUMMARY_URL");
    const char * url = (env_url != NULL && env_url[0] != '\0')
                       ? env_url : SERVER_SUMMARY_URL_DEFAULT;

    resp_state_t st = { .len = 0, .truncated = false };
    resp_buf[0] = '\0';

    curl_easy_setopt(curl_handle, CURLOPT_URL, url);
    curl_easy_setopt(curl_handle, CURLOPT_TIMEOUT_MS, 5000L);
    curl_easy_setopt(curl_handle, CURLOPT_CONNECTTIMEOUT_MS, 2000L);
    curl_easy_setopt(curl_handle, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl_handle, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, &st);

    CURLcode res = curl_easy_perform(curl_handle);
    long code = 0;
    curl_easy_getinfo(curl_handle, CURLINFO_RESPONSE_CODE, &code);

    if(res != CURLE_OK) {
        fprintf(stderr, "server_client: GET %s failed: %s%s\n", url,
                curl_easy_strerror(res),
                st.truncated ? " (response exceeded 256KB cap)" : "");
        return false;
    }
    if(code != 200) {
        fprintf(stderr, "server_client: GET %s -> HTTP %ld\n", url, code);
        return false;
    }

    return parse_summary(resp_buf, out);
}
