/**
 * @file net_worker.c
 *
 * Background network worker: one pthread owns every HTTP call so the LVGL
 * UI thread never blocks on the network.
 *
 *   - HA polling: all nine entities per round; 3s base period with
 *     exponential backoff (3->6->...->30s) while every entity fails,
 *     back to 3s as soon as one succeeds (mirrors the old main.c logic).
 *     Skipped entirely in server-only mode (no HA token).
 *   - Server summary polling: 15s base period (the endpoint itself probes
 *     the LAN and takes 1-3s), backoff to 60s on failure. On failure the
 *     previous data is kept and marked stale (valid=false), so the UI can
 *     show a STALE view instead of an empty page.
 *   - Weather polling (Open-Meteo, Shenzhen): first fetch immediately at
 *     start, then every 30min; on failure retry after 5min, back to 30min
 *     on success. Same stale-on-failure semantics as the server round.
 *   - Control commands: ring buffer drained at the top of every loop
 *     iteration, again between the HA and server rounds, and interleaved
 *     between the individual entity fetches inside an HA round, so a
 *     command queued mid-round goes out within one HTTP call. Posting one
 *     signals the condvar so the call goes out immediately.
 *
 * Hard rule: this file never includes lvgl.h and never calls LVGL. Results
 * are published into the three snapshots under one mutex; the UI thread only
 * ever uses the net_worker_get_*_snapshot() copy-out getters.
 */

#include "net_worker.h"
#include "ha_client.h"
#include "server_client.h"
#include "weather_client.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>

/*-----------------------------
 * Timing
 *----------------------------*/
#define HA_POLL_BASE_MS      3000U
#define HA_POLL_MAX_MS       30000U
#define SRV_POLL_BASE_MS     15000U
#define SRV_POLL_MAX_MS      60000U
#define WX_POLL_BASE_MS      (30U * 60U * 1000U)   /* 30 minutes */
#define WX_POLL_RETRY_MS     (5U * 60U * 1000U)    /* 5 minutes on failure */

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
}

/*-----------------------------
 * Shared state
 *----------------------------*/
static pthread_t       worker_thread;
static bool            worker_started;
static volatile bool   stopping;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  wake = PTHREAD_COND_INITIALIZER;

static ha_snapshot_t     ha_snap;      /* guarded by lock */
static server_snapshot_t srv_snap;     /* guarded by lock */
static weather_snapshot_t wx_snap;     /* guarded by lock */

/* Control ring buffer (guarded by lock, condvar 'wake' signals inserts). */
#define CTRL_RING_CAP 16
typedef struct {
    net_control_action_t action;
    int param;
} ctrl_cmd_t;
static ctrl_cmd_t ctrl_ring[CTRL_RING_CAP];
static size_t ctrl_head;               /* next slot to execute */
static size_t ctrl_tail;               /* next slot to fill  */

/* Polling bookkeeping lives on the worker thread only (no lock needed). */
static bool      ha_ready;
static uint32_t  ha_fail_rounds;
static uint32_t  ha_period_ms = HA_POLL_BASE_MS;
static uint32_t  srv_fail_rounds;
static uint32_t  srv_period_ms = SRV_POLL_BASE_MS;
static bool      wx_failed;                          /* last weather round failed */
static uint32_t  wx_period_ms = WX_POLL_BASE_MS;

/*-----------------------------
 * Snapshot publication
 *----------------------------*/

/** Fetch one entity into a snapshot field; empty entity IDs are skipped. */
static bool fetch_field(ha_field_t * field, const char * entity_id)
{
    if(entity_id == NULL || entity_id[0] == '\0') {
        field->valid = false;
        return false;
    }
    char buf[32];
    if(!ha_client_fetch_state(entity_id, buf, sizeof(buf))) {
        field->valid = false;          /* keep the previous value for STALE display */
        return false;
    }
    snprintf(field->value, sizeof(field->value), "%s", buf);
    field->valid = true;
    return true;
}

static void drain_controls(void);     /* defined below, interleaved into rounds */

static void ha_round(void)
{
    ha_snapshot_t s;

    pthread_mutex_lock(&lock);
    s = ha_snap;                        /* start from the last published state */
    pthread_mutex_unlock(&lock);

    /* fetch_field() calls are interleaved with drain_controls() so a
     * command queued mid-round does not wait for the whole round. */
    int ok = 0;
    ok += fetch_field(&s.temp,  ha_entity_temp())  ? 1 : 0;
    drain_controls();
    ok += fetch_field(&s.hum,   ha_entity_hum())   ? 1 : 0;
    drain_controls();
    ok += fetch_field(&s.pm25,  ha_entity_pm25())  ? 1 : 0;
    drain_controls();
    ok += fetch_field(&s.mode,  ha_entity_mode())  ? 1 : 0;
    drain_controls();
    ok += fetch_field(&s.power, ha_entity_power()) ? 1 : 0;
    drain_controls();
    ok += fetch_field(&s.ac,    ha_entity_ac())    ? 1 : 0;
    drain_controls();
    ok += fetch_field(&s.lamp,  ha_entity_lamp())  ? 1 : 0;
    drain_controls();
    ok += fetch_field(&s.cam1,  ha_entity_cam1())  ? 1 : 0;
    drain_controls();
    ok += fetch_field(&s.cam2,  ha_entity_cam2())  ? 1 : 0;

    s.ha_online = (ok > 0);

    /* Backoff: double the period per all-failed round, cap at 30s;
     * reset immediately when anything succeeds. */
    if(ok == 0) {
        ha_fail_rounds++;
        uint32_t p = HA_POLL_BASE_MS;
        for(uint32_t i = 0; i < ha_fail_rounds && p < HA_POLL_MAX_MS; i++) p *= 2;
        if(p > HA_POLL_MAX_MS) p = HA_POLL_MAX_MS;
        if(p != ha_period_ms) {
            fprintf(stderr, "[net] HA unreachable %u round(s), polling every %u ms\n",
                    ha_fail_rounds, p);
            ha_period_ms = p;
        }
    }
    else if(ha_fail_rounds > 0) {
        fprintf(stderr, "[net] HA back online, polling every %u ms\n", HA_POLL_BASE_MS);
        ha_fail_rounds = 0;
        ha_period_ms = HA_POLL_BASE_MS;
    }

    pthread_mutex_lock(&lock);
    s.seq = ha_snap.seq + 1;
    ha_snap = s;
    pthread_mutex_unlock(&lock);
}

static void srv_round(void)
{
    server_snapshot_t fresh;
    bool ok = server_client_fetch(&fresh);

    if(!ok) {
        srv_fail_rounds++;
        uint32_t p = SRV_POLL_BASE_MS;
        for(uint32_t i = 0; i < srv_fail_rounds && p < SRV_POLL_MAX_MS; i++) p *= 2;
        if(p > SRV_POLL_MAX_MS) p = SRV_POLL_MAX_MS;
        if(p != srv_period_ms) {
            fprintf(stderr, "[net] server monitor unreachable %u round(s), polling every %u ms\n",
                    srv_fail_rounds, p);
            srv_period_ms = p;
        }

        pthread_mutex_lock(&lock);
        srv_snap.valid = false;        /* keep previous data as STALE view */
        srv_snap.seq++;
        pthread_mutex_unlock(&lock);
        return;
    }

    if(srv_fail_rounds > 0) {
        fprintf(stderr, "[net] server monitor back online, polling every %u ms\n",
                SRV_POLL_BASE_MS);
        srv_fail_rounds = 0;
        srv_period_ms = SRV_POLL_BASE_MS;
    }

    pthread_mutex_lock(&lock);
    fresh.seq = srv_snap.seq + 1;
    srv_snap = fresh;
    pthread_mutex_unlock(&lock);
}

static void weather_round(void)
{
    weather_snapshot_t fresh;
    bool ok = weather_client_fetch(&fresh);

    if(!ok) {
        if(!wx_failed) {
            fprintf(stderr, "[net] weather unreachable, retrying every %u ms\n",
                    WX_POLL_RETRY_MS / 1000U);
        }
        wx_failed = true;
        wx_period_ms = WX_POLL_RETRY_MS;

        pthread_mutex_lock(&lock);
        wx_snap.valid = false;        /* keep previous data as STALE view */
        wx_snap.seq++;
        pthread_mutex_unlock(&lock);
        return;
    }

    if(wx_failed) {
        fprintf(stderr, "[net] weather back online, polling every %u min\n",
                WX_POLL_BASE_MS / 60000U);
    }
    wx_failed = false;
    wx_period_ms = WX_POLL_BASE_MS;

    fprintf(stderr, "[weather] Shenzhen %.1fC code=%d hum=%d%% wind=%.1fkm/h\n",
            fresh.temp_c, fresh.weather_code, fresh.hum_pct, fresh.wind_kmh);

    pthread_mutex_lock(&lock);
    fresh.seq = wx_snap.seq + 1;
    wx_snap = fresh;
    pthread_mutex_unlock(&lock);
}

/*-----------------------------
 * Control command execution
 *----------------------------*/
static void execute_control(const ctrl_cmd_t * cmd)
{
    char body[192];
    bool ok = true;

    switch(cmd->action) {
        case NET_ACT_PURIFIER_MODE_AUTO:
        case NET_ACT_PURIFIER_MODE_SLEEP:
        case NET_ACT_PURIFIER_MODE_FAV: {
            const char * option =
                (cmd->action == NET_ACT_PURIFIER_MODE_AUTO)  ? "自动" :
                (cmd->action == NET_ACT_PURIFIER_MODE_SLEEP) ? "睡眠" : "最爱";
            snprintf(body, sizeof(body), "{\"entity_id\":\"%s\",\"option\":\"%s\"}",
                     ha_entity_mode(), option);
            ok = ha_client_call_service("select", "select_option", body);
            break;
        }
        case NET_ACT_PURIFIER_POWER_ON:
        case NET_ACT_PURIFIER_POWER_OFF:
            snprintf(body, sizeof(body), "{\"entity_id\":\"%s\"}", ha_entity_power());
            ok = ha_client_call_service("switch",
                                        cmd->action == NET_ACT_PURIFIER_POWER_ON
                                            ? "turn_on" : "turn_off", body);
            break;
        case NET_ACT_AC_POWER_ON:
        case NET_ACT_AC_POWER_OFF:
            snprintf(body, sizeof(body), "{\"entity_id\":\"%s\"}", ha_entity_ac());
            ok = ha_client_call_service("climate",
                                        cmd->action == NET_ACT_AC_POWER_ON
                                            ? "turn_on" : "turn_off", body);
            break;
        case NET_ACT_LAMP_POWER_ON:
        case NET_ACT_LAMP_POWER_OFF:
            snprintf(body, sizeof(body), "{\"entity_id\":\"%s\"}", ha_entity_lamp());
            ok = ha_client_call_service("light",
                                        cmd->action == NET_ACT_LAMP_POWER_ON
                                            ? "turn_on" : "turn_off", body);
            break;
        case NET_ACT_LAMP_BRIGHTNESS: {
            int b = cmd->param;
            if(b < 1) b = 1;
            if(b > 255) b = 255;
            snprintf(body, sizeof(body), "{\"entity_id\":\"%s\",\"brightness\":%d}",
                     ha_entity_lamp(), b);
            ok = ha_client_call_service("light", "turn_on", body);
            break;
        }
        default:
            return;
    }

    if(!ok) {
        fprintf(stderr, "[net] control action %d failed, next poll restores UI\n",
                (int)cmd->action);
    }
}

/** Drain the ring buffer; called with the lock NOT held. */
static void drain_controls(void)
{
    for(;;) {
        pthread_mutex_lock(&lock);
        if(ctrl_head == ctrl_tail) {
            pthread_mutex_unlock(&lock);
            return;
        }
        ctrl_cmd_t cmd = ctrl_ring[ctrl_head];
        ctrl_head = (ctrl_head + 1) % CTRL_RING_CAP;
        pthread_mutex_unlock(&lock);

        execute_control(&cmd);         /* network call happens unlocked */
    }
}

/*-----------------------------
 * Worker thread main loop
 *----------------------------*/
static void * worker_main(void * arg)
{
    (void)arg;

    uint64_t ha_next  = now_ms();      /* first rounds run immediately */
    uint64_t srv_next = now_ms();
    uint64_t wx_next  = now_ms();

    while(!stopping) {
        drain_controls();

        uint64_t now = now_ms();
        if(ha_ready && now >= ha_next) {
            ha_round();
            ha_next = now_ms() + ha_period_ms;
        }
        drain_controls();            /* between the two rounds as well */
        if(now >= srv_next) {
            srv_round();
            srv_next = now_ms() + srv_period_ms;
        }
        drain_controls();
        if(now >= wx_next) {
            weather_round();
            wx_next = now_ms() + wx_period_ms;
        }

        /* Sleep until the earliest deadline, or until a control arrives. */
        uint64_t wake_at = ha_next < srv_next ? ha_next : srv_next;
        if(!ha_ready && wake_at == ha_next) wake_at = srv_next;
        if(wx_next < wake_at) wake_at = wx_next;

        pthread_mutex_lock(&lock);
        while(!stopping && ctrl_head == ctrl_tail) {
            now = now_ms();
            if(now >= wake_at) break;

            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            uint64_t add_ms = wake_at - now;
            ts.tv_sec  += (time_t)(add_ms / 1000U);
            ts.tv_nsec += (long)(add_ms % 1000U) * 1000000L;
            if(ts.tv_nsec >= 1000000000L) {
                ts.tv_sec  += 1;
                ts.tv_nsec -= 1000000000L;
            }
            int rc = pthread_cond_timedwait(&wake, &lock, &ts);
            if(rc != 0 && rc != ETIMEDOUT) break;   /* defensive */
        }
        pthread_mutex_unlock(&lock);
    }

    return NULL;
}

/*-----------------------------
 * Public API
 *----------------------------*/
bool net_worker_start(bool ha_ready_in)
{
    if(worker_started) return true;

    ha_ready = ha_ready_in;

    pthread_mutex_lock(&lock);
    ha_snap.token_ok = ha_ready_in;
    ha_snap.ha_online = false;
    pthread_mutex_unlock(&lock);

    stopping = false;
    if(pthread_create(&worker_thread, NULL, worker_main, NULL) != 0) {
        fprintf(stderr, "[net] failed to create worker thread\n");
        return false;
    }
    worker_started = true;
    fprintf(stderr, "[net] worker started (ha %s)\n", ha_ready_in ? "on" : "off");
    return true;
}

void net_worker_stop(void)
{
    if(!worker_started) return;

    pthread_mutex_lock(&lock);
    stopping = true;
    pthread_cond_broadcast(&wake);
    pthread_mutex_unlock(&lock);

    pthread_join(worker_thread, NULL);
    worker_started = false;
    fprintf(stderr, "[net] worker stopped\n");
}

void net_worker_post_control(net_control_action_t action, int param)
{
    pthread_mutex_lock(&lock);
    size_t next_tail = (ctrl_tail + 1) % CTRL_RING_CAP;
    if(next_tail == ctrl_head) {
        pthread_mutex_unlock(&lock);
        fprintf(stderr, "[net] control queue full, dropping action %d\n", (int)action);
        return;
    }
    ctrl_ring[ctrl_tail].action = action;
    ctrl_ring[ctrl_tail].param  = param;
    ctrl_tail = next_tail;
    pthread_cond_signal(&wake);
    pthread_mutex_unlock(&lock);
}

void net_worker_get_ha_snapshot(ha_snapshot_t * out)
{
    if(out == NULL) return;
    pthread_mutex_lock(&lock);
    *out = ha_snap;
    pthread_mutex_unlock(&lock);
}

void net_worker_get_server_snapshot(server_snapshot_t * out)
{
    if(out == NULL) return;
    pthread_mutex_lock(&lock);
    *out = srv_snap;
    pthread_mutex_unlock(&lock);
}

void net_worker_get_weather_snapshot(weather_snapshot_t * out)
{
    if(out == NULL) return;
    pthread_mutex_lock(&lock);
    *out = wx_snap;
    pthread_mutex_unlock(&lock);
}
