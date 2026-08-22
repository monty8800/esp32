/**
 * @file photo_storage.c
 *
 * SPIFFS photo storage implementation.
 *
 * Layout on the "storage" SPIFFS partition:
 *   /p/manifest.json   →  {"count":N}
 *   /p/photo_001.jpg   →  compressed JPEG (browser-resized)
 *   /p/photo_002.jpg
 *   ...
 *
 * The partition is 2 MB; after SPIFFS metadata ~1.9 MB is usable.
 * At ~80-150 KB per browser-compressed JPEG this holds roughly 13-23
 * photos - plenty for a small digital frame.
 */

#include "photo_storage.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <dirent.h>

#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_heap_caps.h"

static const char *TAG = "photo_stor";

#define MOUNT_POINT  "/spiffs"
#define PART_LABEL   "storage"       /* matches partitions.csv          */
#define PHOTO_DIR    MOUNT_POINT "/p"
#define MANIFEST_PATH PHOTO_DIR "/manifest.json"

static bool s_mounted;

/*-----------------------------
 * Init / mount
 *----------------------------*/
esp_err_t photo_storage_init(void)
{
    if (s_mounted) return ESP_OK;

    esp_vfs_spiffs_conf_t conf = {
        .base_path = MOUNT_POINT,
        .partition_label = PART_LABEL,
        .max_files = 5,
        .format_if_mount_failed = true,
    };

    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(err));
        return err;
    }

    size_t total = 0, used = 0;
    esp_spiffs_info(PART_LABEL, &total, &used);
    ESP_LOGI(TAG, "SPIFFS: total=%u, used=%u (free=%u)",
             (unsigned)total, (unsigned)used, (unsigned)(total - used));

    /* Ensure photo directory exists. */
    mkdir(PHOTO_DIR, 0755);

    /* Create manifest if missing. */
    FILE *f = fopen(MANIFEST_PATH, "r");
    if (f == NULL) {
        f = fopen(MANIFEST_PATH, "w");
        if (f) {
            fprintf(f, "{\"count\":0}");
            fclose(f);
        }
    } else {
        fclose(f);
    }

    s_mounted = true;
    return ESP_OK;
}

/*-----------------------------
 * Count
 *----------------------------*/
int photo_storage_count(void)
{
    FILE *f = fopen(MANIFEST_PATH, "r");
    if (f == NULL) return 0;

    char buf[64];
    int n = 0;
    if (fgets(buf, sizeof(buf), f) != NULL) {
        char *p = strstr(buf, "\"count\"");
        if (p) {
            p = strchr(p, ':');
            if (p) n = atoi(p + 1);
        }
    }
    fclose(f);
    return n;
}

/*-----------------------------
 * Write helper (used by web_config upload handler)
 *
 * Saves @p data as the next photo and increments the manifest count.
 * Returns the photo index (1-based) on success, -1 on failure.
 *----------------------------*/
int photo_storage_save(const uint8_t *data, uint32_t size)
{
    int count = photo_storage_count();
    int idx = count + 1;

    char path[64];
    snprintf(path, sizeof(path), PHOTO_DIR "/photo_%03d.jpg", idx);

    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "failed to create %s", path);
        return -1;
    }
    size_t written = fwrite(data, 1, size, f);
    fclose(f);

    if (written != size) {
        ESP_LOGE(TAG, "short write %u / %lu", (unsigned)written, (unsigned long)size);
        remove(path);
        return -1;
    }

    /* Update manifest. */
    f = fopen(MANIFEST_PATH, "w");
    if (f) {
        fprintf(f, "{\"count\":%d}", idx);
        fclose(f);
    }

    ESP_LOGI(TAG, "saved photo #%d (%lu bytes) -> %s",
             idx, (unsigned long)size, path);
    return idx;
}

/*-----------------------------
 * Load
 *----------------------------*/
esp_err_t photo_storage_load(int index, uint8_t **out_data, uint32_t *out_size)
{
    if (index < 1 || index > photo_storage_count()) {
        return ESP_ERR_NOT_FOUND;
    }

    char path[64];
    snprintf(path, sizeof(path), PHOTO_DIR "/photo_%03d.jpg", index);

    struct stat st;
    if (stat(path, &st) != 0) {
        ESP_LOGW(TAG, "stat failed for %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    uint32_t fsize = (uint32_t)st.st_size;
    uint8_t *buf = heap_caps_malloc(fsize, MALLOC_CAP_SPIRAM);
    if (buf == NULL) {
        ESP_LOGE(TAG, "PSRAM alloc %lu bytes failed", (unsigned long)fsize);
        return ESP_ERR_NO_MEM;
    }

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        free(buf);
        return ESP_FAIL;
    }

    size_t nread = fread(buf, 1, fsize, f);
    fclose(f);

    if (nread != fsize) {
        ESP_LOGW(TAG, "short read %u / %lu", (unsigned)nread, (unsigned long)fsize);
        free(buf);
        return ESP_FAIL;
    }

    *out_data = buf;
    *out_size = fsize;
    return ESP_OK;
}

/*-----------------------------
 * Clear
 *----------------------------*/
esp_err_t photo_storage_clear(void)
{
    DIR *d = opendir(PHOTO_DIR);
    if (d == NULL) return ESP_FAIL;

    struct dirent *ent;
    int deleted = 0;
    while ((ent = readdir(d)) != NULL) {
        if (strstr(ent->d_name, "photo_") != NULL) {
            char path[sizeof(PHOTO_DIR) + 256];
            snprintf(path, sizeof(path), PHOTO_DIR "/%s", ent->d_name);
            if (remove(path) == 0) deleted++;
        }
    }
    closedir(d);

    /* Reset manifest. */
    FILE *f = fopen(MANIFEST_PATH, "w");
    if (f) {
        fprintf(f, "{\"count\":0}");
        fclose(f);
    }

    ESP_LOGI(TAG, "cleared %d photo(s)", deleted);
    return ESP_OK;
}
