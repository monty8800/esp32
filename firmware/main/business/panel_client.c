/**
 * @file panel_client.c
 *
 * Fetches the pre-aggregated panel-hub payload
 * (GET http://192.168.9.216:8790/api/panel) and parses it into
 * panel_snapshot_t. Structure mirrors server_client.c on purpose: same
 * persistent curl easy handle, same growable response buffer, same cJSON
 * helpers, same "leave *out untouched on failure" contract.
 *
 * Payload shape (see local-server/20-LXC-117-panel-hub.md for the contract):
 *
 * {
 *   "generated_at": "...", "stale": false,
 *   "sources": {...},
 *   "ops": { "ok": true, "fetched_at": "...",
 *            "today":     {"orders":170,"units":184,"sales_usd":4465.75},
 *            "yesterday": {"orders":495,"units":541,"sales_usd":14757.33,"final":true},
 *            "currency":"USD", "stores":18,
 *            "stockout": {"total":24, "by_health":{"缺货":13,"低库存":11},
 *                         "by_store":{...}, "detail":[{...}]} },
 *   "mail": { "ok": true, "drafts_pending":24, "oldest_draft_hours":319.9,
 *             "oldest_draft_subject":"", "today_received":11 },
 *   "dev":  { "ok": true, "mac_host":"MacbookPro", "age_minutes":0.2,
 *             "jobs_total":6, "jobs_ok":5, "jobs_failing":[...],
 *             "jobs":[{name,ok,age_hours,awake_age_hours,max_age_hours,
 *                      last_exit_code,evidence,reason}],
 *             "projects":[{name,progress,updated}] }
 * }
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>

#include "third_party/cjson/cJSON.h"
#include "panel_client.h"

#define RESP_BUF_INIT   (16 * 1024)    /* initial response buffer size */
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

    size_t need = st->len + add + 1;      /* +1 for the NUL terminator */
    if(need > RESP_BUF_MAX) {
        st->truncated = true;
        return 0;                         /* != add -> curl aborts */
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

/* --------------------------------------------------------------------------
 * Small cJSON helpers (all NULL-safe: any missing key yields the default)
 * -------------------------------------------------------------------------- */

static void json_str(const cJSON * obj, const char * key, char * dst, size_t dst_len)
{
    dst[0] = '\0';
    if(obj == NULL) return;
    const cJSON * it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if(cJSON_IsString(it) && it->valuestring != NULL) {
        snprintf(dst, dst_len, "%s", it->valuestring);
    }
}

static double json_num(const cJSON * obj, const char * key, double dflt)
{
    if(obj == NULL) return dflt;
    const cJSON * it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if(it == NULL) return dflt;
    if(cJSON_IsNumber(it)) return it->valuedouble;
    /* the hub emits some numbers as strings (e.g. "days":"0.00") */
    if(cJSON_IsString(it) && it->valuestring != NULL) return atof(it->valuestring);
    if(cJSON_IsBool(it)) return cJSON_IsTrue(it) ? 1.0 : 0.0;
    return dflt;
}

static bool json_bool(const cJSON * obj, const char * key, bool dflt)
{
    if(obj == NULL) return dflt;
    const cJSON * it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if(it == NULL) return dflt;
    if(cJSON_IsBool(it)) return cJSON_IsTrue(it);
    if(cJSON_IsNumber(it)) return it->valuedouble != 0.0;
    return dflt;
}

static const cJSON * json_arr(const cJSON * obj, const char * key)
{
    if(obj == NULL) return NULL;
    const cJSON * it = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsArray(it) ? it : NULL;
}

static const cJSON * json_obj(const cJSON * obj, const char * key)
{
    if(obj == NULL) return NULL;
    const cJSON * it = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsObject(it) ? it : NULL;
}

/* --------------------------------------------------------------------------
 * Parsing
 * -------------------------------------------------------------------------- */

/** 取某日块的 sales_rmb（人民币）。 */
static double json_day_rmb(const cJSON * parent, const char * key)
{
    const cJSON * day = json_obj(parent, key);
    return json_num(day, "sales_rmb", 0.0);
}

/** 解析 platforms / countries 排行数组。 */
static void parse_rank(const cJSON * arr, panel_rank_t * out, int cap, int * count)
{
    *count = 0;
    if(!cJSON_IsArray(arr)) return;
    int idx = 0;
    const cJSON * row;
    cJSON_ArrayForEach(row, arr) {
        if(idx >= cap) break;
        if(!cJSON_IsObject(row)) continue;
        memset(&out[idx], 0, sizeof(out[idx]));
        json_str(row, "name", out[idx].name, sizeof(out[idx].name));
        out[idx].orders = (int)json_num(row, "orders", 0.0);
        out[idx].units  = (int)json_num(row, "units", 0.0);
        out[idx].yday_units = (int)json_num(row, "yday_units", 0.0);
        idx++;
    }
    *count = idx;
}

/** Parse the day block {"orders":..,"units":..,"sales_usd":..}. */
static void json_day(const cJSON * parent, const char * key,
                     double * orders, double * units, double * usd, bool * final_flag)
{
    const cJSON * day = json_obj(parent, key);
    *orders = json_num(day, "orders", 0.0);
    *units  = json_num(day, "units", 0.0);
    *usd    = json_num(day, "sales_usd", 0.0);
    if(final_flag != NULL) *final_flag = json_bool(day, "final", false);
}

static void parse_stockout(const cJSON * ops, panel_snapshot_t * tmp)
{
    const cJSON * so = json_obj(ops, "stockout");
    tmp->stockout_total = (int)json_num(so, "total", 0.0);

    const cJSON * by_health = json_obj(so, "by_health");
    tmp->stockout_lack = (int)json_num(by_health, "缺货", 0.0);
    tmp->stockout_low  = (int)json_num(by_health, "低库存", 0.0);

    const cJSON * detail = NULL;
    if(so != NULL) {
        const cJSON * it = cJSON_GetObjectItemCaseSensitive(so, "detail");
        if(cJSON_IsArray(it)) detail = it;
    }
    if(detail == NULL) return;

    int idx = 0;
    const cJSON * row;
    cJSON_ArrayForEach(row, detail) {
        if(idx >= NET_MAX_STOCKOUT) break;
        if(!cJSON_IsObject(row)) continue;
        panel_stockout_t * s = &tmp->stockout[idx];
        json_str(row, "store",      s->store,   sizeof(s->store));
        json_str(row, "sku",        s->sku,     sizeof(s->sku));
        json_str(row, "asin",       s->asin,    sizeof(s->asin));
        json_str(row, "health",     s->health,  sizeof(s->health));
        json_str(row, "days",       s->days,    sizeof(s->days));
        s->fulfillable = (int)json_num(row, "fulfillable", 0.0);
        idx++;
    }
    tmp->stockout_count = idx;
}

static void parse_jobs(const cJSON * dev, panel_snapshot_t * tmp)
{
    const cJSON * jobs = NULL;
    if(dev != NULL) {
        const cJSON * it = cJSON_GetObjectItemCaseSensitive(dev, "jobs");
        if(cJSON_IsArray(it)) jobs = it;
    }
    if(jobs == NULL) return;

    int idx = 0;
    const cJSON * row;
    cJSON_ArrayForEach(row, jobs) {
        if(idx >= NET_MAX_JOBS) break;
        if(!cJSON_IsObject(row)) continue;
        panel_job_t * j = &tmp->jobs[idx];
        json_str(row, "name",     j->name,     sizeof(j->name));
        json_str(row, "evidence", j->evidence, sizeof(j->evidence));
        j->ok              = json_bool(row, "ok", false);
        j->age_hours       = json_num(row, "age_hours", 0.0);
        j->awake_age_hours = json_num(row, "awake_age_hours", 0.0);
        j->max_age_hours   = json_num(row, "max_age_hours", 0.0);
        /* last_exit_code may be JSON null -> unknown */
        const cJSON * ec = cJSON_GetObjectItemCaseSensitive(row, "last_exit_code");
        j->last_exit_code = (ec == NULL || cJSON_IsNull(ec))
                            ? NET_EXIT_UNKNOWN
                            : (int)json_num(row, "last_exit_code", 0.0);
        idx++;
    }
    tmp->job_count = idx;
}

static void parse_projects(const cJSON * dev, panel_snapshot_t * tmp)
{
    const cJSON * projects = NULL;
    if(dev != NULL) {
        const cJSON * it = cJSON_GetObjectItemCaseSensitive(dev, "projects");
        if(cJSON_IsArray(it)) projects = it;
    }
    if(projects == NULL) return;

    int idx = 0;
    const cJSON * row;
    cJSON_ArrayForEach(row, projects) {
        if(idx >= NET_MAX_PROJECTS) break;
        if(!cJSON_IsObject(row)) continue;
        panel_project_t * p = &tmp->projects[idx];
        json_str(row, "name",     p->name,     sizeof(p->name));
        json_str(row, "progress", p->progress, sizeof(p->progress));
        json_str(row, "updated",  p->updated,  sizeof(p->updated));
        idx++;
    }
    tmp->project_count = idx;
}

/**
 * Parse the payload. Returns true on structural success (a JSON object with
 * an "ops" block). Per-source failures are NOT errors here: the hub encodes
 * them as ok:false + reason, and the UI renders them as such.
 */
static bool parse_panel(const char * text, panel_snapshot_t * out)
{
    if(text == NULL || out == NULL) return false;

    cJSON * root = cJSON_Parse(text);
    if(root == NULL) {
        fprintf(stderr, "panel_client: malformed JSON\n");
        return false;
    }

    const cJSON * ops = json_obj(root, "ops");
    if(ops == NULL) {
        fprintf(stderr, "panel_client: payload has no ops block\n");
        cJSON_Delete(root);
        return false;
    }

    /* Build into a scratch copy so a parse failure never half-writes *out. */
    panel_snapshot_t tmp;
    memset(&tmp, 0, sizeof(tmp));

    tmp.stale = json_bool(root, "stale", true);

    /* --- ops (core: drives the top-level stale flag) --- */
    tmp.ops_ok = json_bool(ops, "ok", false);
    json_str(ops, "fetched_at", tmp.ops_fetched_at, sizeof(tmp.ops_fetched_at));
    json_day(ops, "today",     &tmp.today_orders, &tmp.today_units, &tmp.today_usd, NULL);
    json_day(ops, "yesterday", &tmp.yday_orders,  &tmp.yday_units,  &tmp.yday_usd,
             &tmp.yday_final);
    /* RMB 金额与本月累计（P2：面板显示人民币） */
    tmp.today_rmb = json_day_rmb(ops, "today");
    tmp.yday_rmb  = json_day_rmb(ops, "yesterday");
    {
        const cJSON * mo = json_obj(ops, "month");
        tmp.month_orders = json_num(mo, "orders", 0.0);
        tmp.month_units = json_num(mo, "units", 0.0);
        tmp.month_usd   = json_num(mo, "sales_usd", 0.0);
        tmp.month_rmb   = json_num(mo, "sales_rmb", 0.0);
        tmp.month_days  = (int)json_num(mo, "days", 0.0);
    }
    {
        const cJSON * fx = json_obj(ops, "fx");
        tmp.fx_usd_cny = json_num(fx, "usd_cny", 0.0);
        tmp.fx_ok      = json_bool(fx, "ok", false);
    }
    parse_rank(json_arr(ops, "platforms"), tmp.platforms, NET_MAX_PLATFORMS,
               &tmp.platform_count);
    parse_rank(json_arr(ops, "countries"), tmp.countries, NET_MAX_COUNTRIES,
               &tmp.country_count);
    parse_stockout(ops, &tmp);

    /* --- mail --- */
    const cJSON * mail = json_obj(root, "mail");
    tmp.mail_ok               = json_bool(mail, "ok", false);
    tmp.mail_drafts_pending   = (int)json_num(mail, "drafts_pending", 0.0);
    tmp.mail_oldest_hours     = json_num(mail, "oldest_draft_hours", 0.0);
    tmp.mail_today_received   = (int)json_num(mail, "today_received", 0.0);

    /* --- dev (Mac push heartbeat) --- */
    const cJSON * dev = json_obj(root, "dev");
    tmp.dev_ok           = json_bool(dev, "ok", false);
    tmp.dev_age_minutes  = json_num(dev, "age_minutes", 0.0);
    tmp.jobs_total       = (int)json_num(dev, "jobs_total", 0.0);
    tmp.jobs_ok          = (int)json_num(dev, "jobs_ok", 0.0);
    json_str(dev, "mac_host", tmp.mac_host, sizeof(tmp.mac_host));
    parse_jobs(dev, &tmp);
    parse_projects(dev, &tmp);

    tmp.valid = true;

    cJSON_Delete(root);
    *out = tmp;
    return true;
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

bool panel_client_fetch(panel_snapshot_t * out)
{
    if(out == NULL) return false;

    if(curl_handle == NULL) {
        curl_handle = curl_easy_init();
        if(curl_handle == NULL) {
            fprintf(stderr, "panel_client: curl_easy_init failed\n");
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

    const char * env_url = getenv("PANEL_URL");
    const char * url = (env_url != NULL && env_url[0] != '\0')
                       ? env_url : PANEL_API_URL_DEFAULT;

    resp_state_t st = { .len = 0, .truncated = false };
    resp_buf[0] = '\0';

    curl_easy_setopt(curl_handle, CURLOPT_URL, url);
    /* The hub answers from a local state file (~10ms); generous margins only
     * guard against a busy LAN. */
    curl_easy_setopt(curl_handle, CURLOPT_TIMEOUT_MS, 8000L);
    curl_easy_setopt(curl_handle, CURLOPT_CONNECTTIMEOUT_MS, 3000L);
    curl_easy_setopt(curl_handle, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl_handle, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, &st);

    CURLcode res = curl_easy_perform(curl_handle);
    long code = 0;
    curl_easy_getinfo(curl_handle, CURLINFO_RESPONSE_CODE, &code);

    if(res != CURLE_OK) {
        fprintf(stderr, "panel_client: GET %s failed: %s%s\n", url,
                curl_easy_strerror(res),
                st.truncated ? " (response exceeded 256KB cap)" : "");
        return false;
    }
    if(code != 200) {
        fprintf(stderr, "panel_client: GET %s -> HTTP %ld\n", url, code);
        return false;
    }

    return parse_panel(resp_buf, out);
}
