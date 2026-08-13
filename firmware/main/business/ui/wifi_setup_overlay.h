/**
 * @file wifi_setup_overlay.h
 *
 * Full-screen WiFi provisioning overlay: scans nearby networks, presents a
 * selectable list, accepts a password via the LVGL keyboard, saves
 * credentials to NVS and restarts.
 *
 * Entry point is called from the status-bar gear button in ui_shell.c.
 */

#ifndef WIFI_SETUP_OVERLAY_H
#define WIFI_SETUP_OVERLAY_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Show the WiFi setup overlay (scans networks, shows list). Must be called
 * on the LVGL thread (under lvgl_port_lock). The overlay is self-contained:
 * it creates itself, handles all interaction, and either restarts the device
 * (on save) or deletes itself (on cancel).
 */
void wifi_setup_overlay_show(void);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_SETUP_OVERLAY_H */
