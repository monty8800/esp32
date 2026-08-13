/**
 * @file env_shim.c
 *
 * NVS -> environment variable bridge; see env_shim.h. Secret values are
 * never logged - only the env var name and value length.
 */

#include "env_shim.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"

#include "config_store.h"

static const char *TAG = "env_shim";

void env_shim_apply(void)
{
    static const struct {
        const char * nvs_key;
        const char * env_name;
    } map[] = {
        { "ha_token",           "HA_TOKEN"           },
        { "ha_base_url",        "HA_BASE_URL"        },
        { "server_summary_url", "SERVER_SUMMARY_URL" },
        { "weather_url",        "WEATHER_URL"        },
        /* HA entity-ID overrides consumed by ha_client.c */
        { "ha_entity_temp",     "HA_ENTITY_TEMP"     },
        { "ha_entity_hum",      "HA_ENTITY_HUM"      },
        { "ha_entity_pm25",     "HA_ENTITY_PM25"     },
        { "ha_entity_mode",     "HA_ENTITY_MODE"     },
        { "ha_entity_power",    "HA_ENTITY_POWER"    },
        { "ha_entity_ac",       "HA_ENTITY_AC"       },
        { "ha_entity_lamp",     "HA_ENTITY_LAMP"     },
        { "ha_entity_cam1",     "HA_ENTITY_CAM1"     },
        { "ha_entity_cam2",     "HA_ENTITY_CAM2"     },
    };

    char buf[512];
    for(size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        esp_err_t err = config_get_str(map[i].nvs_key, buf, sizeof(buf));
        if(err == ESP_ERR_NVS_NOT_FOUND || buf[0] == '\0') {
            continue;                 /* client uses its compiled-in default */
        }
        if(err != ESP_OK) {
            ESP_LOGW(TAG, "reading '%s' failed: %s", map[i].nvs_key,
                     esp_err_to_name(err));
            continue;
        }
        if(setenv(map[i].env_name, buf, 1) != 0) {
            ESP_LOGE(TAG, "setenv %s failed", map[i].env_name);
            continue;
        }
        ESP_LOGI(TAG, "%s set from NVS (%zu chars)", map[i].env_name,
                 strlen(buf));
    }
}
