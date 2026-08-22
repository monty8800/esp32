# Main component: app entry point + board support code + shared UI.
#
# The UI and the network layer live in business/ (vendored from the former
# macOS simulator tree; the simulator has been retired).
# PRIV_INCLUDE_DIRS exposes business/ so "ui/ui_shell.h" / "net_worker.h" /
# "third_party/cjson/cJSON.h" resolve, plus curl_shim/ so
# `#include <curl/curl.h>` resolves to the esp_http_client-backed shim.

idf_component_register(
    SRCS
        "app_main.c"
        "bsp_display.c"
        "bsp_touch.c"
        "bsp_backlight.c"
        "curl_shim/curl_shim.c"
        "config_store.c"
        "env_shim.c"
        "wifi_sta.c"
        "web_config.c"
        "ui_drain.c"
        "business/ui/ui_shell.c"
        "business/ui/dashboard_page.c"
        "business/ui/devices_page.c"
        "business/ui/server_page.c"
        "business/ui/photos_page.c"
        "photo_storage.c"
        "business/ui/wifi_setup_overlay.c"
        "business/net_worker.c"
        "business/ha_client.c"
        "business/server_client.c"
        "business/weather_client.c"
        "business/third_party/cjson/cJSON.c"
        "fonts/font_cjk_16.c"
        "fonts/font_cjk_20.c"
    INCLUDE_DIRS
        "."                 # bsp_*.h, ui_fonts.h, ui_drain.h live here
        "${CMAKE_CURRENT_SOURCE_DIR}/.."   # firmware/lv_conf.h
    PRIV_INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}/business"    # ui/*.h, net_worker.h
        "${CMAKE_CURRENT_SOURCE_DIR}/curl_shim"   # curl/curl.h shim
    REQUIRES
        nvs_flash
        esp_lcd
        driver
        esp_driver_ledc
        esp_timer
        esp_http_client
        spiffs
        app_update
    PRIV_REQUIRES
        esp_lvgl_port
        lvgl
        esp_lcd_st7701
        esp_lcd_touch
        esp_lcd_touch_gt911
        esp_lcd_panel_io_additions
        esp_io_expander_tca9554
        esp_io_expander
        esp_http_server
        esp_wifi
        esp_netif
        esp_event
        console
        mbedtls
)

# lv_conf wiring (see firmware/CMakeLists.txt header for the full story):
# the managed lvgl component gets -DLV_CONF_PATH globally; this definition
# keeps app-side includes of lv_conf.h working via "lv_conf.h" as well.
target_compile_definitions(${COMPONENT_LIB} PUBLIC LV_CONF_INCLUDE_SIMPLE)

# The business/ sources originated from the macOS simulator build.
# On xtensa uint32_t is `unsigned long` while on
# macOS it is `unsigned int`, so %u printf formats trip
# -Werror=format here. Demote format warnings to non-fatal for these files
# only (values are real uint32_t, so the output stays correct).
set(_shared_srcs
    "${CMAKE_CURRENT_SOURCE_DIR}/business/ui/ui_shell.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/business/ui/dashboard_page.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/business/ui/devices_page.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/business/ui/server_page.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/business/ui/photos_page.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/photo_storage.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/business/ui/wifi_setup_overlay.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/business/net_worker.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/business/ha_client.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/business/server_client.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/business/weather_client.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/business/third_party/cjson/cJSON.c"
)
set_source_files_properties(${_shared_srcs} PROPERTIES
    COMPILE_OPTIONS "-Wno-error=format"
)
