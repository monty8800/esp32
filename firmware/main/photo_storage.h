/**
 * @file photo_storage.h
 *
 * SPIFFS-backed photo storage for the digital photo frame.
 *
 * Photos are stored as JPEG files under /spiffs/p/ with a simple
 * manifest (JSON) tracking the count.  Uploads come from the web
 * config page where the browser compresses the image before sending.
 */

#ifndef PHOTO_STORAGE_H
#define PHOTO_STORAGE_H

#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Mount SPIFFS and ensure the photo directory exists.  Safe to call
 *  multiple times (idempotent). */
esp_err_t photo_storage_init(void);

/** Number of photos currently stored. */
int photo_storage_count(void);

/**
 * Save JPEG @p data as the next photo.  Updates the manifest.
 * Returns the 1-based photo index on success, -1 on failure.
 */
int photo_storage_save(const uint8_t *data, uint32_t size);

/**
 * Load photo @p index (0-based) into a PSRAM buffer.
 * Caller must free(*out_data) when done.
 * Returns ESP_OK on success, ESP_ERR_NOT_FOUND if index is out of range.
 */
esp_err_t photo_storage_load(int index, uint8_t **out_data, uint32_t *out_size);

/** Delete all stored photos and reset the manifest. */
esp_err_t photo_storage_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* PHOTO_STORAGE_H */
