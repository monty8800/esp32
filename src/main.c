/**
 * @file main.c
 *
 * Platform layer of the ESP32-S3-Touch-LCD-4B simulator.
 *
 * Responsibilities:
 *   - SDL2 init
 *   - LVGL init + tick source (SDL_GetTicks)
 *   - SDL window display driver (480x480)
 *   - SDL mouse as pointer input device (simulates the capacitive touch
 *     panel) with a bound LVGL cursor object
 *   - main loop driving lv_timer_handler()
 *
 * The demo UI (src/ui/) contains no platform code and can be moved to the
 * real ESP32 hardware (ESP-IDF + esp_lvgl_port) unchanged.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <SDL2/SDL.h>
#include "lvgl.h"
#include "ha_client.h"
#include "ui/ui_demo.h"
#include "ui/env_panel.h"

#if defined(__APPLE__)
#include <objc/runtime.h>
#include <objc/message.h>

/**
 * macOS: when the simulator is started from a background shell (nohup /
 * daemon) the WindowServer never activates the app, so the SDL window's
 * Metal presents are discarded and the window composites as pure black
 * until something activates the app. Activate it programmatically so the
 * rendered frames are actually shown.
 */
static void macos_activate_app(void)
{
    Class cls = objc_getClass("NSApplication");
    if(cls == NULL) return;
    id app = ((id (*)(Class, SEL))objc_msgSend)(cls, sel_registerName("sharedApplication"));
    if(app == NULL) return;
    /* NSApplicationActivationPolicyRegular */
    ((void (*)(id, SEL, long))objc_msgSend)(app, sel_registerName("setActivationPolicy:"), 0L);
    ((void (*)(id, SEL, char))objc_msgSend)(app, sel_registerName("activateIgnoringOtherApps:"), (char)1);
}

/** One-shot: re-assert activation once the loop is live and force a redraw. */
static void macos_activate_once_cb(lv_timer_t * t)
{
    macos_activate_app();
    lv_obj_invalidate(lv_screen_active());
    lv_timer_delete(t);
}
#endif /* __APPLE__ */

#define SIM_HOR_RES 480
#define SIM_VER_RES 480

/* Font sizes for the injected CJK fonts (small = body, large = headings). */
#define FONT_SIZE_SM 16
#define FONT_SIZE_LG 20

/**
 * Read an entire file into a heap buffer. The caller keeps the buffer alive
 * for the whole program (tiny_ttf renders from it on demand).
 */
static uint8_t * read_file(const char * path, size_t * out_size)
{
    FILE * f = fopen(path, "rb");
    if(f == NULL) return NULL;

    if(fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long len = ftell(f);
    if(len <= 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }

    uint8_t * buf = (uint8_t *)malloc((size_t)len);
    if(buf == NULL) {
        fclose(f);
        return NULL;
    }

    if(fread(buf, 1, (size_t)len, f) != (size_t)len) {
        free(buf);
        fclose(f);
        return NULL;
    }

    fclose(f);
    *out_size = (size_t)len;
    return buf;
}

/**
 * Probe the system for a .ttf carrying CJK glyphs.
 * stb_truetype (tiny_ttf) cannot parse .ttc collections, so only plain .ttf
 * files are usable. Never touches the network, only reads local files.
 */
static bool find_cjk_ttf(char * out, size_t cap)
{
    /* Preferred candidate: ships with macOS and covers CJK. */
    static const char * preferred = "/System/Library/Fonts/Supplemental/Arial Unicode.ttf";
    FILE * probe = fopen(preferred, "rb");
    if(probe != NULL) {
        fclose(probe);
        snprintf(out, cap, "%s", preferred);
        return true;
    }

    /* Fallback: scan the system font tree for any plain .ttf. */
    FILE * p = popen("find /System/Library/Fonts -name '*.ttf' 2>/dev/null | head -n 1", "r");
    if(p == NULL) return false;
    if(fgets(out, (int)cap, p) == NULL) out[0] = '\0';
    pclose(p);

    size_t l = strlen(out);
    while(l > 0 && (out[l - 1] == '\n' || out[l - 1] == '\r')) out[--l] = '\0';
    return l > 0;
}

/**
 * Create the two UI fonts via tiny_ttf. On any failure the outputs stay NULL
 * and ui_demo falls back to the built-in Montserrat fonts (no crash).
 */
static void create_cjk_fonts(const lv_font_t ** out_sm, const lv_font_t ** out_lg)
{
    *out_sm = NULL;
    *out_lg = NULL;

    char path[512];
    if(!find_cjk_ttf(path, sizeof(path))) {
        fprintf(stderr, "[sim] WARNING: no CJK .ttf found, using built-in fonts\n");
        return;
    }

    size_t data_size = 0;
    static uint8_t * font_data = NULL; /* intentionally kept for program lifetime */
    font_data = read_file(path, &data_size);
    if(font_data == NULL) {
        fprintf(stderr, "[sim] WARNING: cannot read font '%s', using built-in fonts\n", path);
        return;
    }

    *out_sm = lv_tiny_ttf_create_data(font_data, data_size, FONT_SIZE_SM);
    *out_lg = lv_tiny_ttf_create_data(font_data, data_size, FONT_SIZE_LG);
    if(*out_sm == NULL || *out_lg == NULL) {
        fprintf(stderr, "[sim] WARNING: tiny_ttf failed on '%s', using built-in fonts where needed\n", path);
        return;
    }

    printf("[sim] CJK font loaded: %s (%zu bytes)\n", path, data_size);
}

/*-----------------------------
 * Home Assistant wiring
 *----------------------------*/

#define HA_POLL_PERIOD_MS     3000     /* normal polling interval */
#define HA_POLL_PERIOD_MAX_MS 30000    /* backoff cap while HA is down */

static lv_timer_t * ha_timer;          /* polling timer handle */
static uint32_t fail_rounds;           /* consecutive all-failed poll rounds */
static uint32_t current_period = HA_POLL_PERIOD_MS;  /* mirrors timer period */

/* Values pushed into the panel on the previous round, used only for the
 * "panel refresh" diagnostics log. */
static char last_temp[32] = "";
static char last_hum[32] = "";
static char last_pm25[32] = "";
static int last_mode_idx = -2;          /* -2 = never seen */
static int last_power = -1;             /* -1 = never seen */

/** Poll all five purifier entities and push the results into the panel. */
static void ha_refresh_cb(lv_timer_t * t)
{
    LV_UNUSED(t);

    char temp[32] = "";
    char hum[32] = "";
    char pm25[32] = "";
    char mode[32] = "";
    char power[32] = "";

    bool ok_temp = ha_client_fetch_state(ha_entity_temp(), temp, sizeof(temp));
    bool ok_hum  = ha_client_fetch_state(ha_entity_hum(), hum, sizeof(hum));
    bool ok_pm25 = ha_client_fetch_state(ha_entity_pm25(), pm25, sizeof(pm25));
    bool ok_mode = ha_client_fetch_state(ha_entity_mode(), mode, sizeof(mode));
    bool ok_pwr  = ha_client_fetch_state(ha_entity_power(), power, sizeof(power));

    /* Chinese mode strings -> panel button index. */
    int mode_idx = -1;
    if(ok_mode) {
        if(strcmp(mode, "自动") == 0) mode_idx = 0;
        else if(strcmp(mode, "睡眠") == 0) mode_idx = 1;
        else if(strcmp(mode, "最爱") == 0) mode_idx = 2;
    }

    int ok_count = (ok_temp ? 1 : 0) + (ok_hum ? 1 : 0) + (ok_pm25 ? 1 : 0) +
                   (ok_mode ? 1 : 0) + (ok_pwr ? 1 : 0);
    const char * status = (ok_count > 0) ? "LIVE" : "OFFLINE";

    /* Backoff: while every entity fails, double the poll period per round
     * (3s -> 6s -> ... capped at 30s); back to 3s as soon as one succeeds. */
    if(ha_timer != NULL) {
        if(ok_count == 0) {
            fail_rounds++;
            uint32_t period = HA_POLL_PERIOD_MS;
            for(uint32_t i = 0; i < fail_rounds && period < HA_POLL_PERIOD_MAX_MS; i++) {
                period *= 2;
            }
            if(period > HA_POLL_PERIOD_MAX_MS) period = HA_POLL_PERIOD_MAX_MS;
            if(period != current_period) {
                fprintf(stderr, "[sim] HA unreachable %u round(s), polling every %u ms\n",
                        fail_rounds, period);
                current_period = period;
                lv_timer_set_period(ha_timer, period);
            }
        }
        else if(fail_rounds > 0) {
            fprintf(stderr, "[sim] HA back online, polling every %d ms\n", HA_POLL_PERIOD_MS);
            fail_rounds = 0;
            current_period = HA_POLL_PERIOD_MS;
            lv_timer_set_period(ha_timer, HA_POLL_PERIOD_MS);
        }
    }

    env_panel_update(temp, ok_temp,
                     hum, ok_hum,
                     pm25, ok_pm25,
                     mode_idx,
                     ok_pwr && strcmp(power, "on") == 0, ok_pwr,
                     status);

    /* Diagnostics: log which values actually changed this round so UI
     * refresh behaviour can be verified from the run log. */
    bool changed = false;
    if(ok_temp && strcmp(temp, last_temp) != 0) {
        fprintf(stderr, "[sim] panel refresh: temp %s -> %s\n", last_temp[0] ? last_temp : "--", temp);
        changed = true;
    }
    if(ok_hum && strcmp(hum, last_hum) != 0) {
        fprintf(stderr, "[sim] panel refresh: hum %s -> %s\n", last_hum[0] ? last_hum : "--", hum);
        changed = true;
    }
    if(ok_pm25 && strcmp(pm25, last_pm25) != 0) {
        fprintf(stderr, "[sim] panel refresh: pm25 %s -> %s\n", last_pm25[0] ? last_pm25 : "--", pm25);
        changed = true;
    }
    if(mode_idx >= 0 && mode_idx != last_mode_idx) {
        fprintf(stderr, "[sim] panel refresh: mode -> %d\n", mode_idx);
        changed = true;
    }
    if(ok_pwr) {
        int pwr = strcmp(power, "on") == 0;
        if(pwr != last_power) {
            fprintf(stderr, "[sim] panel refresh: power -> %s\n", pwr ? "on" : "off");
            changed = true;
        }
        last_power = pwr;
    }
    (void)changed;

    if(ok_temp) snprintf(last_temp, sizeof(last_temp), "%s", temp);
    if(ok_hum) snprintf(last_hum, sizeof(last_hum), "%s", hum);
    if(ok_pm25) snprintf(last_pm25, sizeof(last_pm25), "%s", pm25);
    if(mode_idx >= 0) last_mode_idx = mode_idx;
}

/**
 * Optimistic UI already moved; drive the real device through HA service
 * calls. Writing /api/states would only fake the state machine without
 * reaching the purifier, so select/select_option and switch/turn_on|off
 * are used instead.
 */
static void env_control_cb(int action, void * user_data)
{
    LV_UNUSED(user_data);

    bool ok = true;
    char body[192];

    switch(action) {
        case ENV_ACT_MODE_AUTO:
        case ENV_ACT_MODE_SLEEP:
        case ENV_ACT_MODE_FAV: {
            const char * option = (action == ENV_ACT_MODE_AUTO) ? "自动" :
                                  (action == ENV_ACT_MODE_SLEEP) ? "睡眠" : "最爱";
            snprintf(body, sizeof(body), "{\"entity_id\":\"%s\",\"option\":\"%s\"}",
                     ha_entity_mode(), option);
            ok = ha_client_call_service("select", "select_option", body);
            break;
        }
        case ENV_ACT_POWER_ON:
            snprintf(body, sizeof(body), "{\"entity_id\":\"%s\"}", ha_entity_power());
            ok = ha_client_call_service("switch", "turn_on", body);
            break;
        case ENV_ACT_POWER_OFF:
            snprintf(body, sizeof(body), "{\"entity_id\":\"%s\"}", ha_entity_power());
            ok = ha_client_call_service("switch", "turn_off", body);
            break;
        default:
            return;
    }
    if(!ok) {
        fprintf(stderr, "[sim] HA control action %d failed, next poll restores UI\n", action);
    }
}

/**
 * Create a small LVGL object used as the mouse/touch cursor.
 * Rendered on the system layer, follows the pointer like a finger marker.
 */
static lv_obj_t * create_cursor_object(lv_display_t * disp)
{
    lv_obj_t * cur = lv_obj_create(lv_display_get_layer_sys(disp));
    lv_obj_remove_style_all(cur);
    lv_obj_set_size(cur, 26, 26);
    lv_obj_set_style_radius(cur, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(cur, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_bg_opa(cur, LV_OPA_30, 0);
    lv_obj_set_style_border_color(cur, lv_color_hex(0x2dd4bf), 0);
    lv_obj_set_style_border_width(cur, 2, 0);
    lv_obj_remove_flag(cur, LV_OBJ_FLAG_CLICKABLE);
    return cur;
}

int main(void)
{
    /*--- SDL ---*/
    if(SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    /*--- LVGL ---*/
    lv_init();

    /* Tick source: milliseconds since SDL_Init, exactly what LVGL wants. */
    lv_tick_set_cb(SDL_GetTicks);

    /* Display: 480x480 SDL window (mirrors the board's square LCD). */
    lv_display_t * disp = lv_sdl_window_create(SIM_HOR_RES, SIM_VER_RES);
    if(disp == NULL) {
        fprintf(stderr, "Failed to create SDL window\n");
        SDL_Quit();
        return 1;
    }
    lv_sdl_window_set_title(disp, "ESP32-S3-Touch-LCD-4B · 480x480 touch simulator");

    /* Pointer input: mouse == capacitive touch.
     * Pressed state = finger down, released = finger up,
     * drag while pressed behaves like touch-move. */
    lv_indev_t * mouse = lv_sdl_mouse_create();
    if(mouse != NULL) {
        lv_obj_t * cursor = create_cursor_object(disp);
        lv_indev_set_cursor(mouse, cursor);
    }

    /* Hide the OS cursor so only the LVGL cursor ring is visible. */
    SDL_ShowCursor(SDL_DISABLE);

    /*--- Fonts: system CJK TTF via tiny_ttf, injected into the UI ---*/
    const lv_font_t * font_sm;
    const lv_font_t * font_lg;
    create_cjk_fonts(&font_sm, &font_lg);

    /*--- Demo UI (pure LVGL code) ---*/
    ui_demo_create(font_sm, font_lg);

    /*--- Air purifier panel + Home Assistant polling ---*/
    env_panel_create(lv_screen_active(), font_sm);
    env_panel_set_control_cb(env_control_cb, NULL);

    /* Keep the screen scrollable but kill inertia: momentum scrolling would
     * interfere with slider drags on the rest of the UI. */
    lv_obj_remove_flag(lv_screen_active(), LV_OBJ_FLAG_SCROLL_MOMENTUM);

    if(ha_client_init()) {
        ha_timer = lv_timer_create(ha_refresh_cb, HA_POLL_PERIOD_MS, NULL);
        /* No blocking call here: the first poll runs inside the first
         * lv_timer_handler(), so the UI is rendered before any fetch. */
        lv_timer_ready(ha_timer);
    }
    else {
        env_panel_update(NULL, false, NULL, false, NULL, false, -1, false, false,
                         "NO TOKEN");
    }

    /*--- Main loop ---*/
#if defined(__APPLE__)
    /* Background launches (nohup) are never activated by macOS, which leaves
     * the window black. Pump Cocoa events so the app finishes launching,
     * activate now, and re-assert once from the running loop. */
    for(int i = 0; i < 20; i++) {
        SDL_PumpEvents();
        SDL_Delay(10);
    }
    macos_activate_app();
    lv_timer_create(macos_activate_once_cb, 800, NULL);
#endif

    bool quit = false;
    while(!quit) {
        lv_timer_handler();
        SDL_Delay(5);
    }

    SDL_Quit();
    return 0;
}
