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

/**
 * 100ms drain: copy all three locked snapshots out of the worker and push
 * them into the UI. Seq comparison avoids re-rendering unchanged data; the
 * clock is refreshed at most once per minute.
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

    /* Snapshots live in static storage: server_snapshot_t alone is ~15.6KB,
     * far beyond the LVGL task stack even after it was raised. The timer
     * callback only ever runs on the LVGL thread, so static is safe here. */
    static ha_snapshot_t ha;
    static server_snapshot_t srv;
    static weather_snapshot_t wx;
    net_worker_get_ha_snapshot(&ha);
    net_worker_get_server_snapshot(&srv);
    net_worker_get_weather_snapshot(&wx);

    /*--- Status bar: HA status word ---*/
    ui_shell_set_ha_status(ha.token_ok ? (ha.ha_online ? "LIVE" : "OFFLINE")
                                       : "NO TOKEN");

    /*--- Status bar: clock, refreshed only on minute change ---*/
    time_t now = time(NULL);
    struct tm lt;
    if(localtime_r(&now, &lt) != NULL && lt.tm_min != last_minute) {
        last_minute = lt.tm_min;
        char hhmm[8];
        snprintf(hhmm, sizeof(hhmm), "%02d:%02d", lt.tm_hour, lt.tm_min);
        ui_shell_set_time(hhmm);
    }

    /*--- Pages: refresh only when the worker published a new snapshot ---*/
    if(!ha_seen || ha.seq != last_ha_seq) {
        ha_seen = true;
        last_ha_seq = ha.seq;
        dashboard_page_update(&ha);
        devices_page_update(&ha);

        /* Status bar indoor temp/humidity: state strings verbatim, no float
         * parsing. Invalid/empty fields (incl. NO TOKEN / offline degraded
         * mode) simply render as "--". */
        const char * t = (ha.temp.valid && ha.temp.value[0] != '\0') ? ha.temp.value : "--";
        const char * h = (ha.hum.valid && ha.hum.value[0] != '\0') ? ha.hum.value : "--";
        ui_shell_set_env(t, h);
    }

    if(!srv_seen || srv.seq != last_srv_seq) {
        srv_seen = true;
        last_srv_seq = srv.seq;
        server_page_update(&srv);
    }

    if(!wx_seen || wx.seq != last_wx_seq) {
        wx_seen = true;
        last_wx_seq = wx.seq;
        dashboard_page_update_weather(&wx);
    }
}

void ui_drain_start(void)
{
    lv_timer_create(ui_drain_cb, UI_DRAIN_PERIOD_MS, NULL);
}
