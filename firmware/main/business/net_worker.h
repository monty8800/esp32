/**
 * @file net_worker.h
 *
 * Background network worker + snapshot mailbox (data layer of the three-page
 * UI). The UI thread never touches the network: it only copies locked
 * snapshots via the getters below, and pushes control commands through a
 * queue. The worker thread never touches LVGL.
 *
 * Snapshot structs defined here are the DATA CONTRACT for the UI pages
 * (dashboard_page / devices_page / server_page). Keep them stable.
 *
 * Threading model:
 *   - one pthread runs all network I/O (HA entity polling, server summary
 *     polling, Shenzhen weather polling, control service calls)
 *   - HA polling: 3s base period, exponential backoff 3->30s while every
 *     entity fails, reset to 3s on any success (mirrors old main.c logic)
 *   - server summary polling: 15s base period, backoff to 60s on failure
 *   - weather polling: 30min base period (Open-Meteo, cheap), backoff to
 *     5min on failure, reset to 30min on success
 *   - control commands preempt the wait immediately (condvar wake-up)
 *
 * curl_global_init() must already have run before net_worker_start() when
 * ha_ready is true (ha_client_init() does this in main).
 */

#ifndef NET_WORKER_H
#define NET_WORKER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

/* --------------------------------------------------------------------------
 * HA snapshot
 * -------------------------------------------------------------------------- */

/** One polled HA entity value: short state string + freshness flag. */
typedef struct {
    char value[32];   /**< HA "state" string, e.g. "26.5" / "on" / "cool" */
    bool valid;       /**< true when the last poll of this entity succeeded */
} ha_field_t;

/**
 * Latest Home Assistant poll results, copied out under lock.
 *
 * seq increments on every published update; the UI can use it to skip
 * redundant refreshes. Fields whose entity ID is empty (card disabled) or
 * whose fetch failed keep their previous content but are marked invalid.
 */
typedef struct {
    uint32_t seq;      /**< monotonically increasing snapshot version */

    /* Purifier (existing five entities). */
    ha_field_t temp;
    ha_field_t hum;
    ha_field_t pm25;
    ha_field_t mode;   /**< "自动" / "睡眠" / "最爱" */
    ha_field_t power;  /**< "on" / "off" */

    /* New device entities (state strings, read-only display). */
    ha_field_t ac;     /**< climate.* hvac state, e.g. "cool"/"off" */
    ha_field_t lamp;   /**< light.* "on"/"off" */
    ha_field_t cam1;   /**< camera 4K status entity state */
    ha_field_t cam2;   /**< camera 2K status entity state */

    bool ha_online;    /**< true when at least one entity succeeded last round */
    bool token_ok;     /**< true when ha_client_init() succeeded */
} ha_snapshot_t;

/* --------------------------------------------------------------------------
 * Server monitor snapshot (http://192.168.9.206:8787/api/summary)
 * -------------------------------------------------------------------------- */

#define NET_MAX_HOSTS   24   /**< host array capacity in server_snapshot_t */
#define NET_MAX_PROBES  8    /**< probe array capacity per host */

/** One monitored port/service of a host, e.g. "Web 管理 (8006)". */
typedef struct {
    char name[32];
    bool online;
} server_probe_t;

typedef struct {
    char   id[24];                       /**< e.g. "pve", "vm101" */
    char   name[32];                     /**< e.g. "Proxmox 宿主机" */
    char   ip[20];                       /**< e.g. "192.168.9.202" */
    char   desc[256];                    /**< monitor description text, "" if absent */
    bool   online;
    double latency_ms;
    double mem_used_gb;                  /**< -1 when unknown (stats null) */
    double mem_total_gb;                 /**< -1 when unknown */
    double disk_used_gb;                 /**< -1 when unknown (VMs report 0) */
    double disk_total_gb;                /**< -1 when unknown */
    server_probe_t probes[NET_MAX_PROBES];
    int    probe_count;                  /**< number of valid probes[] entries */
} server_host_t;

/**
 * Latest server-monitor summary, copied out under lock.
 *
 * valid==false means the most recent fetch failed; hosts/hosts_count etc.
 * still carry the previous successful data so the UI can show a STALE view
 * instead of an empty page. seq increments on every publish (success or
 * failure) so the UI can detect transitions.
 */
typedef struct {
    uint32_t seq;
    bool     valid;                      /**< last fetch succeeded */
    char     time_str[32];               /**< server "time" field, e.g. "2026-08-12 10:19:12" */
    int      total;                      /**< total host count reported */
    int      online;                     /**< online host count reported */
    server_host_t hosts[NET_MAX_HOSTS];  /**< truncated to NET_MAX_HOSTS */
    int      host_count;                 /**< number of valid hosts[] entries */
} server_snapshot_t;

/* --------------------------------------------------------------------------
 * Shenzhen outdoor weather (Open-Meteo forecast API)
 * -------------------------------------------------------------------------- */

/**
 * Latest Shenzhen weather, copied out under lock.
 *
 * valid==false means the most recent fetch failed; the numeric fields then
 * carry the previous successful data (or zeros before the first success)
 * and the UI renders the "--" / "离线" degraded view. seq increments on
 * every publish (success or failure) so the UI can detect transitions.
 */
typedef struct {
    uint32_t seq;
    bool valid;
    double temp_c;      /**< Shenzhen outdoor temperature, deg C */
    int hum_pct;        /**< relative humidity, % */
    int weather_code;   /**< WMO weather code */
    double wind_kmh;    /**< wind speed, km/h */
    char fetched_hhmm[8]; /**< fetch moment HH:MM (local wall clock) */
} weather_snapshot_t;

/* --------------------------------------------------------------------------
 * Panel hub snapshot (http://192.168.9.216:8790/api/panel)
 *
 * The hub aggregates three INDEPENDENT sources; each block carries its own
 * `ok`. The UI must consult the per-block flag, never only `stale`:
 *   stale == true  => only the CORE ops data (LixingXing) is untrustworthy
 *   ops_ok / mail_ok / dev_ok  => per-block trust
 * This is deliberate: a mail outage must not grey out the sales tiles.
 * -------------------------------------------------------------------------- */

#define NET_MAX_STOCKOUT   10   /**< stockout detail rows kept */
#define NET_MAX_JOBS        8   /**< cron job rows kept */
#define NET_MAX_PROJECTS    8   /**< project rows kept */
#define NET_MAX_PLATFORMS   8   /**< per-platform rows kept */
#define NET_MAX_COUNTRIES  12   /**< per-country rows kept */

/** last_exit_code sentinel: launchd had no recorded exit code. */
#define NET_EXIT_UNKNOWN  (-999)

/** One stockout-risk SKU from FBA inventory (缺货 / 低库存). */
typedef struct {
    char store[16];        /**< e.g. "XWK-US" */
    char sku[20];
    char asin[16];
    char health[12];       /**< "缺货" / "低库存" */
    char days[12];         /**< days of supply, kept as text ("0.00") */
    int  fulfillable;      /**< afn_fulfillable_quantity */
} panel_stockout_t;

/** One monitored launchd job on the Mac (pushed heartbeat). */
typedef struct {
    char   name[24];       /**< e.g. "每日邮件汇总" */
    bool   ok;
    double age_hours;      /**< wall-clock silence */
    double awake_age_hours;/**< silence counted only while the Mac was awake */
    double max_age_hours;  /**< threshold for awake_age_hours */
    int    last_exit_code; /**< NET_EXIT_UNKNOWN when launchd has no record */
    char   evidence[32];   /**< evidence file basename used for liveness */
} panel_job_t;

/** 一条「平台」或「国家」的今日排行项。 */
typedef struct {
    char name[20];   /**< e.g. "Amazon" / "日本"（国家已跨平台合并） */
    int  orders;     /**< 今日单量 */
    int  units;      /**< 今日销量（件） */
    int  yday_units; /**< 昨日销量（件），用于趋势对比 */
} panel_rank_t;

/** One project from the Mac's PROJECT.md snapshot. */
typedef struct {
    char name[24];
    char progress[8];      /**< e.g. "80%" */
    char updated[12];      /**< e.g. "2026-09-09" */
} panel_project_t;

/**
 * Latest panel-hub payload, copied out under lock.
 *
 * valid==false means the most recent fetch failed; all fields then carry the
 * previous successful data so the UI can show a STALE view instead of an
 * empty screen. seq increments on every publish (success or failure) so the
 * UI can detect transitions.
 */
typedef struct {
    uint32_t seq;
    bool     valid;        /**< last fetch (HTTP+parse) succeeded */

    /* --- ops: LixingXing sales + FBA stockout (the CORE block) --- */
    bool     stale;        /**< top-level flag: core ops data untrustworthy */
    bool     ops_ok;
    char     ops_fetched_at[24];  /**< when the hub last refreshed ops */
    /** 中枢给出的「为何不可信」的可读说明（如「数据已 3.0 小时未成功刷新」）。
     *
     * 中枢会在**服务时**按数据年龄判过期（STALE_AFTER_SECONDS=90 分钟），
     * 此时 ops.ok 仍为 true，只有这个字段能区分「数据放久了」与「取数失败」。
     * **固件不要自己算年龄** —— 判过期归中枢，固件只忠实展示，避免两份会漂移的事实源。 */
    char     ops_stale_reason[64];
    /* 销售：RMB 由中枢按实时汇率换算（领星只回 USD 且无汇率读取接口） */
    double   today_orders, today_units, today_usd, today_rmb;
    double   yday_orders,  yday_units,  yday_usd,  yday_rmb;
    bool     yday_final;   /**< false = US day not closed yet, may revise up */
    double   month_orders, month_units, month_usd, month_rmb;  /**< 本月累计（1 日起） */
    int      month_days;   /**< 本月已统计天数 */
    double   fx_usd_cny;   /**< 换算用的汇率（便于核对） */
    bool     fx_ok;        /**< 汇率是否为本期真实取到（false = 沿用旧值/兜底） */
    int      stockout_total;
    int      stockout_lack; /**< "缺货" bucket */
    int      stockout_low;  /**< "低库存" bucket */

    /* --- mail: mail-ai-service pending AI drafts --- */
    bool     mail_ok;
    int      mail_drafts_pending;
    double   mail_oldest_hours;  /**< longest wait in the draft queue */
    int      mail_today_received;

    /* --- dev: Mac push heartbeat (cron jobs + projects) --- */
    bool     dev_ok;
    double   dev_age_minutes;    /**< >45 means the Mac is offline */
    int      jobs_total, jobs_ok;
    char     mac_host[24];

    /* --- 按平台 / 按国家的**今日**拆分（来自 statisticsList，无额外 API 调用）--- */
    int platform_count;
    panel_rank_t platforms[NET_MAX_PLATFORMS];
    int country_count;
    panel_rank_t countries[NET_MAX_COUNTRIES];

    int stockout_count;
    panel_stockout_t stockout[NET_MAX_STOCKOUT];
    int job_count;
    panel_job_t jobs[NET_MAX_JOBS];
    int project_count;
    panel_project_t projects[NET_MAX_PROJECTS];
} panel_snapshot_t;

/* --------------------------------------------------------------------------
 * Control commands (UI thread -> worker thread queue)
 * -------------------------------------------------------------------------- */

typedef enum {
    /* Air purifier (existing Xiaomi entities). */
    NET_ACT_PURIFIER_MODE_AUTO,   /**< select/select_option "自动" */
    NET_ACT_PURIFIER_MODE_SLEEP,  /**< select/select_option "睡眠" */
    NET_ACT_PURIFIER_MODE_FAV,    /**< select/select_option "最爱" */
    NET_ACT_PURIFIER_POWER_ON,    /**< switch/turn_on */
    NET_ACT_PURIFIER_POWER_OFF,   /**< switch/turn_off */

    /* Midea AC (climate entity). */
    NET_ACT_AC_POWER_ON,          /**< climate/turn_on */
    NET_ACT_AC_POWER_OFF,         /**< climate/turn_off */

    /* Monitor hanging lamp (light entity). */
    NET_ACT_LAMP_POWER_ON,        /**< light/turn_on */
    NET_ACT_LAMP_POWER_OFF,       /**< light/turn_off */
    NET_ACT_LAMP_BRIGHTNESS,      /**< light/turn_on + brightness param (1-255) */
} net_control_action_t;

/* --------------------------------------------------------------------------
 * API
 * -------------------------------------------------------------------------- */

/**
 * Start the network worker thread. Call once, from the main thread.
 * @p ha_ready tells the worker whether ha_client_init() succeeded (token +
 * curl global state ready); pass false to run server-only mode. Server
 * polling runs in both modes.
 * @return true when the thread was created.
 */
bool net_worker_start(bool ha_ready);

/**
 * Signal the worker to stop and pthread_join it. Safe to call once at exit.
 */
void net_worker_stop(void);

/**
 * Enqueue a control command; the worker wakes immediately and executes it
 * via ha_client_call_service(). Non-blocking; drops the command (with a log
 * line) if the queue is full. @p param is only used by
 * NET_ACT_LAMP_BRIGHTNESS (brightness 1-255, clamped).
 */
void net_worker_post_control(net_control_action_t action, int param);

/**
 * Copy the latest HA snapshot out under lock. Always succeeds.
 */
void net_worker_get_ha_snapshot(ha_snapshot_t * out);

/**
 * Return only the HA snapshot's sequence number (lightweight peek).
 * Cheaper than a full snapshot copy for change detection.
 */
uint32_t net_worker_get_ha_seq(void);

/**
 * Copy the latest server snapshot out under lock. Always succeeds.
 */
void net_worker_get_server_snapshot(server_snapshot_t * out);

/**
 * Return only the server snapshot's sequence number (lightweight peek).
 */
uint32_t net_worker_get_server_seq(void);

/**
 * Copy the latest weather snapshot out under lock. Always succeeds.
 */
void net_worker_get_weather_snapshot(weather_snapshot_t * out);

/**
 * Return only the weather snapshot's sequence number (lightweight peek).
 */
uint32_t net_worker_get_weather_seq(void);

/**
 * Copy the latest panel-hub snapshot out under lock. Always succeeds.
 */
void net_worker_get_panel_snapshot(panel_snapshot_t * out);

/**
 * Return only the panel snapshot's sequence number (lightweight peek).
 */
uint32_t net_worker_get_panel_seq(void);

#ifdef __cplusplus
}
#endif

#endif /* NET_WORKER_H */
