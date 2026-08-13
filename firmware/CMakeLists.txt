# ESP-IDF project for the Waveshare ESP32-S3-Touch-LCD-4B board.
#
# ------------------------------------------------------------------
# lv_conf.h integration strategy
# ------------------------------------------------------------------
# The official Waveshare ESP-IDF example (examples/esp-idf/02_lvgl_demo_v9
# in https://github.com/waveshareteam/ESP32-S3-Touch-LCD-4B) does NOT ship a
# custom lv_conf.h; it configures LVGL entirely through LVGL's own Kconfig
# (CONFIG_LV_*) with CONFIG_LV_CONF_SKIP=y.
#
# This project deliberately keeps a custom firmware/lv_conf.h instead,
# because it was derived from the simulator's root lv_conf.h so that the UI
# code (now vendored in main/business/ui/) builds with identical feature
# flags.
#
# Mechanism (verified against LVGL v9 lv_conf_internal.h):
#   #if !defined(LV_CONF_SKIP) || defined(LV_CONF_PATH)
#       #ifdef LV_CONF_PATH
#           #include LV_CONF_PATH          <- this wins over LV_CONF_SKIP
#
# So with CONFIG_LV_CONF_SKIP=y (prevents any accidental ../../lv_conf.h
# pickup) plus -DLV_CONF_PATH="<abs path>" the managed lvgl/lvgl component
# still includes our file. We inject the macro globally via
# add_compile_options() at project scope so it reaches the lvgl component
# itself (compile definitions added to the "main" component would NOT
# propagate to lvgl, which is compiled independently and before main).
#
# main/CMakeLists.txt additionally defines LV_CONF_INCLUDE_SIMPLE and adds
# firmware/ to its include dirs, so app code can #include "lv_conf.h".
# ------------------------------------------------------------------

cmake_minimum_required(VERSION 3.16)

# Absolute, quoted path so "#include LV_CONF_PATH" works as-is.
set(LV_CONF_PATH "\"${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h\"")

# Applied to every component, including managed_components/lvgl.
add_compile_options("-DLV_CONF_PATH=${LV_CONF_PATH}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(fw)
