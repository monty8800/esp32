/**
 * @file net_worker.h
 *
 * Background network worker + snapshot mailbox (data layer of the three-page
 * UI). The UI thread never touches the network: it only copies locked
 * snapshots via the getters below, and pushes control commands through a
 * queue. The worker thread never touches LVGL (LV_USE_OS=NONE, LVGL is not
 * thread-safe).
 *
 * Snapshot structs defined here are the DATA CONTRACT for the UI pages
 * (dashboard_page / devices_page / server_page). Keep them stable.
 *
 * Threading model:
 *   - one pthread runs all network I/O (HA entity polling, server summary
 *     polling, control service calls)
 *   - HA polling: 3s base period, exponential backoff 3->30s while every
 *     entity fails, reset to 3s on any success (mirrors old main.c logic)
 *   - server summary polling: 15s base period, backoff to 60s on failure
 *   - control commands preempt the wait immediately (condvar wake-up)
 *
 * curl_global_init() must already have run before net_worker_start() when
 * ha_ready is true (ha_client_init() does this in main).
 */

#ifndef NET_WORKER_H
#define NET_WORKER_H

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
    bool   online;
    double latency_ms;
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
 * Copy the latest server snapshot out under lock. Always succeeds.
 */
void net_worker_get_server_snapshot(server_snapshot_t * out);

#endif /* NET_WORKER_H */
