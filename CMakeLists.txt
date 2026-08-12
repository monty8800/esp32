cmake_minimum_required(VERSION 3.15)
project(esp32_4b_sim C)

set(CMAKE_C_STANDARD 99)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ---------------------------------------------------------------------------
# SDL2 (via pkg-config, with find_package fallback)
# Resolved BEFORE lvgl: the SDL driver sources are compiled inside the lvgl
# library target, so lvgl itself needs the SDL2 include/link dependencies.
# ---------------------------------------------------------------------------
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(SDL2 IMPORTED_TARGET sdl2)
endif()

if(SDL2_FOUND)
    set(SDL2_TARGET PkgConfig::SDL2)
else()
    find_package(SDL2 REQUIRED)
    if(TARGET SDL2::SDL2)
        set(SDL2_TARGET SDL2::SDL2)
    else()
        message(FATAL_ERROR "SDL2 found but no usable CMake target")
    endif()
endif()

# ---------------------------------------------------------------------------
# libcurl (HA REST client; macOS SDK ships a usable libcurl)
# ---------------------------------------------------------------------------
find_package(CURL REQUIRED)

# ---------------------------------------------------------------------------
# LVGL: use our lv_conf.h located at the project root
# ---------------------------------------------------------------------------
set(LV_BUILD_CONF_PATH "${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h" CACHE PATH "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(lvgl)

# PUBLIC: SDL2 include paths and link deps are needed by lvgl's own SDL
# driver sources (lv_sdl_window.c, lv_sdl_mouse.c, ...) and must also
# propagate to every target linking lvgl (e.g. sim).
target_link_libraries(lvgl PUBLIC ${SDL2_TARGET})

# ---------------------------------------------------------------------------
# Application
# ---------------------------------------------------------------------------
add_executable(sim
    src/main.c
    src/ha_client.c
    src/ui/ui_demo.c
    src/ui/env_panel.c
)

target_include_directories(sim PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(sim PRIVATE lvgl CURL::libcurl)

# macOS self-activation helper (objc runtime) used to fix black window when
# the simulator is launched from a background shell (nohup).
if(APPLE)
    target_link_libraries(sim PRIVATE objc)
endif()
