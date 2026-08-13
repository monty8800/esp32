/**
 * @file wifi_sta.c
 *
 * WiFi STA implementation: NVS credentials -> esp_wifi, event-driven
 * connect/reconnect. See wifi_sta.h for the public contract.
 */

#include "wifi_sta.h"

#include <stdio.h>
#include <string.h>

#include "esp_bit_defs.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs.h"

#include "config_store.h"

static const char *TAG = "wifi";

#define WIFI_CONNECTED_BIT BIT0

static EventGroupHandle_t conn_events;
static bool started;

static void on_wifi_event(void * arg, esp_event_base_t base,
                          int32_t event_id, void * data)
{
    (void)arg;
    (void)data;

    if(base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if(base == WIFI_EVENT &&
              event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(conn_events, WIFI_CONNECTED_BIT);
        ESP_LOGW(TAG, "disconnected, reconnecting...");
        esp_wifi_connect();                       /* retry forever */
    } else if(base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t * got = (const ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&got->ip_info.ip));
        xEventGroupSetBits(conn_events, WIFI_CONNECTED_BIT);
    }
}

esp_err_t wifi_sta_start(void)
{
    /* Network stack (lwIP + default event loop) is already initialised in
     * app_main() before this call.  esp_netif_init() is truly idempotent;
     * esp_event_loop_create_default() returns ESP_ERR_INVALID_STATE when
     * the loop already exists, which we tolerate. */
    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t ev_err = esp_event_loop_create_default();
    if(ev_err != ESP_OK && ev_err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(ev_err);
    }

    char ssid[64] = { 0 };
    char psk[64] = { 0 };

    esp_err_t err = config_get_str("wifi_ssid", ssid, sizeof(ssid));
    if(err != ESP_OK || ssid[0] == '\0') {
        ESP_LOGW(TAG, "no wifi_ssid in NVS - WiFi disabled, UI runs "
                      "degraded. Use the 'cfg set' console to provision.");
        return ESP_ERR_NVS_NOT_FOUND;
    }
    /* IEEE 802.11 caps SSIDs at 32 bytes; wifi_cfg.sta.ssid is char[32].
     * Refuse to start instead of silently truncating a stored SSID. */
    if(strlen(ssid) > 31) {
        ESP_LOGE(TAG, "wifi_ssid is %zu chars (max 31) - WiFi disabled. "
                      "Re-provision with 'cfg set wifi_ssid'.", strlen(ssid));
        return ESP_ERR_INVALID_ARG;
    }
    err = config_get_str("wifi_psk", psk, sizeof(psk));
    if(err != ESP_OK) psk[0] = '\0';   /* allow open networks */

    conn_events = xEventGroupCreate();
    if(conn_events == NULL) return ESP_ERR_NO_MEM;

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL, NULL));

    wifi_config_t wifi_cfg;
    memset(&wifi_cfg, 0, sizeof(wifi_cfg));
    snprintf((char *)wifi_cfg.sta.ssid, sizeof(wifi_cfg.sta.ssid), "%s", ssid);
    snprintf((char *)wifi_cfg.sta.password, sizeof(wifi_cfg.sta.password),
             "%s", psk);
    wifi_cfg.sta.threshold.authmode = psk[0] == '\0' ? WIFI_AUTH_OPEN
                                                     : WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    started = true;
    ESP_LOGI(TAG, "STA started, connecting to \"%s\"", ssid);
    return ESP_OK;
}

bool wifi_sta_is_connected(void)
{
    return conn_events != NULL &&
           (xEventGroupGetBits(conn_events) & WIFI_CONNECTED_BIT);
}

bool wifi_sta_wait_connected(uint32_t timeout_ms)
{
    if(!started || conn_events == NULL) return false;

    EventBits_t bits = xEventGroupWaitBits(conn_events, WIFI_CONNECTED_BIT,
                                           pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(timeout_ms));
    if(bits & WIFI_CONNECTED_BIT) return true;

    ESP_LOGW(TAG, "no IP after %lu ms, continuing degraded "
                  "(WiFi keeps retrying)", (unsigned long)timeout_ms);
    return false;
}
