/**
 * @file ota_display.c
 * @brief Display firmware update into the inactive slot, with rollback (WP-G).
 */

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "esp_app_desc.h"
#include "ota_image.h"
#include "esp_system.h"
#include "ota_display.h"
#include "hvac_controller.h"
#include "bts7960_driver.h"
#include "remote_io.h"
#include "event_log.h"

static const char *TAG = "OTA_DISP";

#define VALIDATE_LINK_UP_S      30
#define VALIDATE_GIVE_UP_S      (5 * 60)

_Static_assert(offsetof(esp_image_header_t, chip_id) == OTA_IMAGE_CHIP_ID_OFFSET, "image header layout");
_Static_assert(sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) == OTA_IMAGE_DESC_OFFSET,
               "app description offset");
_Static_assert(OTA_IMAGE_DESC_OFFSET + offsetof(esp_app_desc_t, project_name) == OTA_IMAGE_PROJECT_OFFSET,
               "project name offset");
_Static_assert(OTA_IMAGE_DESC_OFFSET + sizeof(esp_app_desc_t) == OTA_IMAGE_HEADER_LEN, "app description size");

static esp_ota_handle_t s_handle;
static const esp_partition_t *s_part;
static volatile bool s_active;          /* between begin and end/abort */
static bool s_flashing;                 /* esp_ota_begin done: header accepted */
static size_t s_size;
static uint8_t s_hdr[OTA_IMAGE_HEADER_LEN];
static size_t s_hdr_len;
static char s_reason[96];               /* why the last image was refused, "" if none */
static volatile bool s_pending;         /* running image not yet marked valid */

bool ota_allowed(char *why, size_t len)
{
    const char *reason = NULL;
    if (s_active) {
        reason = "Another update is in progress";
    } else if (hvac_controller_get_mode() != HVAC_MODE_OFF) {
        reason = "Switch the system OFF first";
    } else if (bts7960_get_speed() != 0 || bts7960_is_ramping()) {
        reason = "Wait until the blower has stopped";
    }
    if (reason && why && len) {
        snprintf(why, len, "%s", reason);
    }
    return reason == NULL;
}

esp_err_t ota_display_begin(size_t image_size)
{
    if (s_active) {
        return ESP_ERR_INVALID_STATE;
    }
    s_part = esp_ota_get_next_update_partition(NULL);
    if (!s_part) {
        return ESP_ERR_NOT_FOUND;
    }
    if (image_size == 0 || image_size > s_part->size) {
        return ESP_ERR_INVALID_SIZE;
    }
    /* esp_ota_begin (which erases the slot) waits until the header has been checked */
    s_size = image_size;
    s_hdr_len = 0;
    s_flashing = false;
    s_reason[0] = '\0';
    s_active = true;
    event_log_add(EVT_OTA, "Display OTA begin, %u bytes -> %s", (unsigned)image_size, s_part->label);
    return ESP_OK;
}

static esp_err_t refuse(const char *reason)
{
    snprintf(s_reason, sizeof(s_reason), "%s", reason);
    ESP_LOGE(TAG, "Image refused: %s", s_reason);
    event_log_add(EVT_OTA, "Display OTA refused: %s", s_reason);
    return ESP_ERR_INVALID_VERSION;
}

// Accepts the image only once its first bytes show an ESP32-S3 app of this project
static esp_err_t check_and_begin(void)
{
    char why[96];
    if (!ota_image_check(s_hdr, s_hdr_len, esp_app_get_description()->project_name, why, sizeof(why))) {
        return refuse(why);
    }
    esp_err_t err = esp_ota_begin(s_part, s_size, &s_handle);
    if (err != ESP_OK) {
        event_log_add(EVT_OTA, "Display OTA begin failed: %s", esp_err_to_name(err));
        return err;
    }
    s_flashing = true;
    return esp_ota_write(s_handle, s_hdr, s_hdr_len);
}

esp_err_t ota_display_write(const void *data, size_t len)
{
    if (!s_active) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint8_t *p = data;
    if (!s_flashing) {
        size_t n = sizeof(s_hdr) - s_hdr_len < len ? sizeof(s_hdr) - s_hdr_len : len;
        memcpy(&s_hdr[s_hdr_len], p, n);
        s_hdr_len += n;
        p += n;
        len -= n;
        if (s_hdr_len < sizeof(s_hdr)) {
            return ESP_OK;
        }
        esp_err_t err = check_and_begin();
        if (err != ESP_OK) {
            return err;
        }
    }
    return len ? esp_ota_write(s_handle, p, len) : ESP_OK;
}

const char *ota_display_reject_reason(void)
{
    return s_reason;
}

esp_err_t ota_display_end(void)
{
    if (!s_active) {
        return ESP_ERR_INVALID_STATE;
    }
    s_active = false;
    if (!s_flashing) {
        return refuse("Image too small");
    }
    s_flashing = false;
    esp_err_t err = esp_ota_end(s_handle);     /* verifies the image (header, segments, hash) */
    if (err == ESP_OK) {
        err = esp_ota_set_boot_partition(s_part);
    }
    if (err == ESP_OK) {
        event_log_add(EVT_OTA, "Display OTA OK, boots %s next", s_part->label);
    } else {
        event_log_add(EVT_OTA, "Display OTA failed: %s", esp_err_to_name(err));
    }
    return err;
}

void ota_display_abort(void)
{
    if (!s_active) {
        return;
    }
    s_active = false;
    if (s_flashing) {
        s_flashing = false;
        esp_ota_abort(s_handle);
    }
    event_log_add(EVT_OTA, "Display OTA aborted");
}

bool ota_display_pending_validation(void)
{
    return s_pending;
}

static void validation_task(void *arg)
{
    (void)arg;
    uint32_t up_s = 0;
    for (uint32_t t = 0; t < VALIDATE_GIVE_UP_S; t++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        up_s = remote_io_is_connected() ? up_s + 1 : 0;
        if (up_s >= VALIDATE_LINK_UP_S) {
            esp_ota_mark_app_valid_cancel_rollback();
            s_pending = false;
            event_log_add(EVT_OTA, "New display firmware validated");
            vTaskDelete(NULL);
        }
    }
    event_log_add(EVT_OTA, "No IO link for 30 s within 5 min: rolling back");
    vTaskDelay(pdMS_TO_TICKS(200));     /* let the console line out */
    esp_ota_mark_app_invalid_rollback_and_reboot();
    esp_restart();                      /* only reached if no other slot is valid */
}

void ota_display_start_validation(void)
{
    const esp_partition_t *bad = esp_ota_get_last_invalid_partition();
    if (bad) {
        event_log_add(EVT_OTA, "Rolled back: image in %s was rejected", bad->label);
    }
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) != ESP_OK ||
        st != ESP_OTA_IMG_PENDING_VERIFY) {
        return;
    }
    s_pending = true;
    ESP_LOGW(TAG, "New image pending verification: needs %d s of IO link within %d s",
             VALIDATE_LINK_UP_S, VALIDATE_GIVE_UP_S);
    if (xTaskCreate(validation_task, "ota_valid", 3072, NULL, 2, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Validation task not started; image stays pending (next reset rolls back)");
    }
}
