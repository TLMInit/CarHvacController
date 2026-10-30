/**
 * @file ota_image.c
 * @brief App image header check before an OTA write (see ota_image.h). Pure C, host-tested.
 */

#include <stdio.h>
#include <string.h>
#include "ota_image.h"

bool ota_image_check(const uint8_t *hdr, size_t len, const char *project, char *why, size_t why_len)
{
    const char *reason = NULL;
    char name[OTA_IMAGE_PROJECT_LEN + 1] = "";

    if (hdr == NULL || len < OTA_IMAGE_HEADER_LEN) {
        reason = "Image too small";
    } else if (hdr[0] != OTA_IMAGE_MAGIC) {
        reason = "Not an ESP32 app image";
    } else if ((hdr[OTA_IMAGE_CHIP_ID_OFFSET] | (hdr[OTA_IMAGE_CHIP_ID_OFFSET + 1] << 8)) != OTA_IMAGE_CHIP_ESP32S3) {
        reason = "Image is not built for the ESP32-S3";
    } else {
        const uint8_t *d = &hdr[OTA_IMAGE_DESC_OFFSET];
        uint32_t magic = (uint32_t)d[0] | ((uint32_t)d[1] << 8) | ((uint32_t)d[2] << 16) | ((uint32_t)d[3] << 24);
        if (magic != OTA_IMAGE_DESC_MAGIC) {
            reason = "Image has no app description";
        } else {
            memcpy(name, &hdr[OTA_IMAGE_PROJECT_OFFSET], OTA_IMAGE_PROJECT_LEN);
            name[OTA_IMAGE_PROJECT_LEN] = '\0';
            if (strcmp(name, project) == 0) {
                return true;
            }
        }
    }
    if (why && why_len) {
        if (reason) {
            snprintf(why, why_len, "%s", reason);
        } else {
            snprintf(why, why_len, "Wrong firmware: image is %s, this board runs %s", name, project);
        }
    }
    return false;
}
