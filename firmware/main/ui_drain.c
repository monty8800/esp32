/**
 * @file ui_drain.c
 *
 * *** M4 里程碑接入 ***
 * 受控复制自 src/main.c 的 ui_drain_cb（回调体保持对照演进）。与仿真器
 * 的唯一差异：三个快照局部变量改为 static —— server_snapshot_t 约 15.6KB，
 * 放在 LVGL 任务栈上必然溢出；回调只在 LVGL 线程执行，static 安全。
 *
 * 职责：100ms 周期把 net_worker 的三份锁内快照拷贝到 UI 线程，seq
 * 比较避免重复渲染；时钟每分钟最多刷新一次。
 */

#include <stdio.h>
#include <time.h>

#include "lvgl.h"
#include "net_worker.h"
#include "ui/ui_shell.h"
#include "ui/dashboard_page.h"
#include "ui/devices_page.h"
#include "ui/server_page.h"
#include "ui_drain.h"
#include "wifi_sta.h"

/**
 * 100ms drain: check sequence numbers first (cheap, ~12 bytes of mutex-
 * protected reads), then copy only the snapshots whose seq actually
 * changed.  This avoids ~21KB of memcpy per tick when data is idle —
 * important because the LVGL thread owns both the timer and rendering.
 * The clock is refreshed at most once per minute.
 */
static void ui_drain_cb(lv_timer_t * t)
{
    LV_UNUSED(t);

    static uint32_t last_ha_seq = 0;
    static bool ha_seen = false;
    static uint32_t last_srv_seq = 0;
    static bool srv_seen = false;
    static uint32_t last_wx_seq = 0;
    static bool wx_seen = false;
    static int last_minute = -1;

    /*--- Cheap seq peek (12 bytes total, 6 lock/unlock cycles) ---*/
    uint32_t ha_seq  = net_worker_get_ha_seq();
    uint32_t srv_seq = net_worker_get_server_seq();
    uint32_t wx_seq  = net_worker_get_weather_seq();

    bool ha_new = !ha_seen || ha_seq != last_ha_seq;
    bool srv_new = !srv_seen || srv_seq != last_srv_seq;
    bool wx_new = !wx_seen || wx_seq != last_wx_seq;

    /*--- HA snapshot: always copy (~330 bytes, cheap) for status bar ---*/
    static ha_snapshot_t ha;   /* static for stack safety */
    net_worker_get_ha_snapshot(&ha);

    ui_shell_set_ha_status(ha.token_ok ? (ha.ha_online ? "LIVE" : "OFFLINE")
                                       : "NO TOKEN");

    /* Status bar WiFi icon (cached inside the shell, cheap). */
    ui_shell_set_wifi(wifi_sta_is_connected());

    if(ha_new) {
        ha_seen = true;
        last_ha_seq = ha.seq;
        dashboard_page_update(&ha);
        devices_page_update(&ha);

        const char * t = (ha.temp.valid && ha.temp.value[0] != '\0') ? ha.temp.value : "--";
        const char * h = (ha.hum.valid && ha.hum.value[0] != '\0') ? ha.hum.value : "--";
        ui_shell_set_env(t, h);
    }

    /*--- Server snapshot: ~20KB, only copy when changed ---*/
    if(srv_new) {
        static server_snapshot_t srv;   /* ~20KB, static for stack safety */
        net_worker_get_server_snapshot(&srv);
        srv_seen = true;
        last_srv_seq = srv.seq;
        server_page_update(&srv);
    }

    /*--- Weather snapshot: tiny, copy when changed ---*/
    if(wx_new) {
        static weather_snapshot_t wx;
        net_worker_get_weather_snapshot(&wx);
        wx_seen = true;
        last_wx_seq = wx.seq;
        dashboard_page_update_weather(&wx);
    }

    /*--- Status bar: clock, refreshed only on minute change ---*/
    time_t now = time(NULL);
    struct tm lt;
    if(localtime_r(&now, &lt) != NULL && lt.tm_min != last_minute) {
        last_minute = lt.tm_min;
        char hhmm[8];
        snprintf(hhmm, sizeof(hhmm), "%02d:%02d", lt.tm_hour, lt.tm_min);
        ui_shell_set_time(hhmm);
    }
}

void ui_drain_start(void)
{
    lv_timer_create(ui_drain_cb, UI_DRAIN_PERIOD_MS, NULL);
}
