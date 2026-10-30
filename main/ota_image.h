/**
 * @file ota_image.h
 * @brief Checks the start of an uploaded app image before anything is written to flash.
 *
 * Pure C (no ESP-IDF), identical in the display and the node repo, host-tested.
 * The offsets are those of esp_image_header_t, esp_image_segment_header_t and
 * esp_app_desc_t; the firmware asserts them at compile time (ota_display.c, io_ota.c).
 */

#ifndef OTA_IMAGE_H
#define OTA_IMAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OTA_IMAGE_MAGIC             0xE9u           /* ESP_IMAGE_HEADER_MAGIC */
#define OTA_IMAGE_CHIP_ID_OFFSET    12              /* esp_image_header_t.chip_id, u16 LE */
#define OTA_IMAGE_CHIP_ESP32S3      9u              /* ESP_CHIP_ID_ESP32S3 */
#define OTA_IMAGE_DESC_OFFSET       32              /* image header (24) + first segment header (8) */
#define OTA_IMAGE_DESC_MAGIC        0xABCD5432u     /* ESP_APP_DESC_MAGIC_WORD, u32 LE */
#define OTA_IMAGE_PROJECT_OFFSET    (OTA_IMAGE_DESC_OFFSET + 48)  /* esp_app_desc_t.project_name */
#define OTA_IMAGE_PROJECT_LEN       32
#define OTA_IMAGE_HEADER_LEN        (OTA_IMAGE_DESC_OFFSET + 256) /* through esp_app_desc_t */

/**
 * True if @p hdr (at least OTA_IMAGE_HEADER_LEN bytes of the image) is an ESP32-S3 app
 * whose project name equals @p project. Otherwise writes a reason for the user to @p why.
 */
bool ota_image_check(const uint8_t *hdr, size_t len, const char *project, char *why, size_t why_len);

#ifdef __cplusplus
}
#endif

#endif /* OTA_IMAGE_H */
