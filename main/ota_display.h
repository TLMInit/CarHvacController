/**
 * @file ota_display.h
 * @brief Display firmware update into the inactive slot, with rollback.
 *
 * INTERFACE (lead). Implemented by WP-G in ota_display.c; partitions.csv gets
 * ota_0/ota_1/otadata and CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE.
 * A new image is marked valid only after it has run the UI for 30 s with the
 * IO-node link up; otherwise the next reset rolls back.
 */

#ifndef OTA_DISPLAY_H
#define OTA_DISPLAY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Updates are refused unless mode is OFF and the blower is off. Returns true if
 * allowed; otherwise writes the reason ("Switch the system OFF first", ...) to why.
 */
bool ota_allowed(char *why, size_t len);

/** Streamed display update (HTTP handler): begin/write/end. end verifies and sets the boot slot. */
esp_err_t ota_display_begin(size_t image_size);
esp_err_t ota_display_write(const void *data, size_t len);
esp_err_t ota_display_end(void);        /* then the caller schedules a restart */
void ota_display_abort(void);

/** Start the 30 s "UI + link up" watch that marks a pending image valid (from app_main). */
void ota_display_start_validation(void);

/** True while running an image that is not yet marked valid. */
bool ota_display_pending_validation(void);

#ifdef __cplusplus
}
#endif

#endif /* OTA_DISPLAY_H */
