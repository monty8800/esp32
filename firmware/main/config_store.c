/**
 * @file config_store.c
 *
 * NVS storage ("fwcfg") plus the serial `cfg` console (esp_console REPL on
 * UART0, own task) used to provision credentials:
 *
 *   cfg set <key> <value>     write a value (needs restart to apply)
 *   cfg get <key>             show one value (token masked to its length)
 *   cfg list                  show all values (token masked to its length)
 *   restart                   reboot to apply
 *
 * The REPL shares UART0 with ESP_LOG output, which is the standard IDF
 * console setup; the task stack is 4KB and it never touches LVGL.
 */

#include "config_store.h"

#include <stdio.h>
#include <string.h>

#include "esp_console.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs.h"

static const char *TAG = "cfg";

/* Known keys, kept in one place: storage, console validation and `cfg list`
 * all iterate this table. */
static const char *CONFIG_KEYS[] = {
    "wifi_ssid",
    "wifi_psk",
    "ha_token",
    "ha_base_url",
    "server_summary_url",
    "weather_url",
    /* HA entity-ID overrides (env_shim maps them to HA_ENTITY_* envs) */
    "ha_entity_temp",
    "ha_entity_hum",
    "ha_entity_pm25",
    "ha_entity_mode",
    "ha_entity_power",
    "ha_entity_ac",
    "ha_entity_lamp",
    "ha_entity_cam1",
    "ha_entity_cam2",
    "photo_source_url",
};
#define CONFIG_KEY_COUNT (sizeof(CONFIG_KEYS) / sizeof(CONFIG_KEYS[0]))

/** Values never printed verbatim - only their length. */
static bool key_is_secret(const char * key)
{
    return strcmp(key, "ha_token") == 0 || strcmp(key, "wifi_psk") == 0;
}

bool config_key_valid(const char * key)
{
    if(key == NULL) return false;
    for(size_t i = 0; i < CONFIG_KEY_COUNT; i++) {
        if(strcmp(key, CONFIG_KEYS[i]) == 0) return true;
    }
    return false;
}

esp_err_t config_get_str(const char * key, char * buf, size_t len)
{
    if(key == NULL || buf == NULL || len == 0) return ESP_ERR_INVALID_ARG;
    buf[0] = '\0';

    nvs_handle_t h;
    esp_err_t err = nvs_open(CONFIG_STORE_NS, NVS_READONLY, &h);
    if(err != ESP_OK) return err;    /* namespace may not exist yet */

    err = nvs_get_str(h, key, buf, &len);
    nvs_close(h);
    return err;
}

esp_err_t config_set_str(const char * key, const char * value)
{
    if(key == NULL || value == NULL) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t err = nvs_open(CONFIG_STORE_NS, NVS_READWRITE, &h);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(h, key, value);
    if(err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "nvs write '%s' failed: %s", key, esp_err_to_name(err));
    }
    return err;
}

/*-----------------------------
 * Console commands
 *----------------------------*/

/** Print one key/value; secret keys show their length instead of the value. */
static void print_kv(const char * key)
{
    char buf[512];
    esp_err_t err = config_get_str(key, buf, sizeof(buf));
    if(err == ESP_ERR_NVS_NOT_FOUND) {
        printf("  %-18s = (unset)\n", key);
    } else if(err != ESP_OK) {
        printf("  %-18s = <read error: %s>\n", key, esp_err_to_name(err));
    } else if(key_is_secret(key)) {
        printf("  %-18s = <set, %zu chars>\n", key, strlen(buf));
    } else {
        printf("  %-18s = %s\n", key, buf);
    }
}

static int cmd_cfg(int argc, char ** argv)
{
    if(argc < 2) {
        printf("usage:\n"
               "  cfg set <key> <value>\n"
               "  cfg get <key>\n"
               "  cfg list\n");
        return 0;
    }

    if(strcmp(argv[1], "list") == 0) {
        printf("config (NVS namespace \"%s\"):\n", CONFIG_STORE_NS);
        for(size_t i = 0; i < CONFIG_KEY_COUNT; i++) {
            print_kv(CONFIG_KEYS[i]);
        }
        return 0;
    }

    if(strcmp(argv[1], "get") == 0) {
        if(argc < 3) { printf("usage: cfg get <key>\n"); return 0; }
        if(!config_key_valid(argv[2])) {
            printf("unknown key '%s'\n", argv[2]);
            return 1;
        }
        print_kv(argv[2]);
        return 0;
    }

    if(strcmp(argv[1], "set") == 0) {
        if(argc < 4) { printf("usage: cfg set <key> <value>\n"); return 0; }
        const char * key = argv[2];
        if(!config_key_valid(key)) {
            printf("unknown key '%s'\n", key);
            return 1;
        }

        /* Rejoin the value tokens so SSIDs/passwords with spaces survive.
         * No silent truncation: if the joined value does not fit, refuse
         * the write and report actual length vs. the limit. */
        char value[512];
        size_t total = 0;
        for(int i = 3; i < argc; i++) {
            total += strlen(argv[i]);
            if(i > 3) total++;          /* separator space */
        }
        if(total >= sizeof(value)) {
            printf("value too long: %zu chars, limit %zu - nothing written\n",
                   total, sizeof(value) - 1);
            ESP_LOGE(TAG, "cfg set '%s' refused: value %zu chars > %zu max",
                     key, total, sizeof(value) - 1);
            return 1;
        }
        size_t off = 0;
        for(int i = 3; i < argc; i++) {
            if(i > 3) value[off++] = ' ';
            size_t n = strlen(argv[i]);
            memcpy(value + off, argv[i], n);
            off += n;
        }
        value[off] = '\0';

        esp_err_t err = config_set_str(key, value);
        if(err != ESP_OK) {
            printf("write failed: %s\n", esp_err_to_name(err));
            return 1;
        }
        if(key_is_secret(key)) {
            printf("saved %s (%zu chars). Run 'restart' to apply.\n",
                   key, strlen(value));
        } else {
            printf("saved %s = %s. Run 'restart' to apply.\n", key, value);
        }
        return 0;
    }

    printf("unknown cfg subcommand '%s'\n", argv[1]);
    return 1;
}

static int cmd_restart(int argc, char ** argv)
{
    (void)argc;
    (void)argv;
    printf("restarting...\n");
    fflush(stdout);
    esp_restart();
    return 0;
}

void config_console_start(void)
{
    esp_console_repl_t * repl = NULL;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "fw> ";
    repl_cfg.max_cmdline_length = 600;  /* room for a full HA token */
    repl_cfg.task_stack_size = 4096;    /* independent task, UART stdin */

    const esp_console_cmd_t cfg_cmd = {
        .command = "cfg",
        .help = "config: cfg set <key> <value> | cfg get <key> | cfg list",
        .func = cmd_cfg,
    };
    const esp_console_cmd_t restart_cmd = {
        .command = "restart",
        .help = "reboot the device to apply config changes",
        .func = cmd_restart,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cfg_cmd));
    ESP_ERROR_CHECK(esp_console_cmd_register(&restart_cmd));

    esp_console_dev_uart_config_t uart_cfg = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    esp_err_t err = esp_console_new_repl_uart(&uart_cfg, &repl_cfg, &repl);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "console init failed: %s", esp_err_to_name(err));
        return;
    }
    err = esp_console_start_repl(repl);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "console start failed: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "console ready: type 'cfg list' (keys: wifi_ssid, wifi_psk, "
                  "ha_token, ha_base_url, server_summary_url, weather_url, "
                  "ha_entity_temp/hum/pm25/mode/power/ac/lamp/cam1/cam2, "
                  "photo_source_url)");
}
