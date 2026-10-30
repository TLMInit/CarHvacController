/**
 * @file io_ota.c
 * @brief Node OTA: the display streams an image into the inactive app slot.
 *
 * Every step must answer within the display's 200 ms command timeout, so the
 * slot is erased sector by sector as data arrives (OTA_WITH_SEQUENTIAL_WRITES)
 * and the sha256 is computed incrementally instead of re-reading the slot.
 *
 * A freshly switched image boots PENDING_VERIFY (bootloader rollback). It is
 * confirmed once it has answered a PING and a VERSION, i.e. the display can
 * talk to it; otherwise the timer rolls back to the previous slot.
 */

#include <stdatomic.h>
#include <stddef.h>
#include <string.h>
#include "io_ota.h"
#include "io_hw.h"
#include "node_config.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "esp_app_desc.h"
#include "ota_image.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "psa/crypto.h"

static const char *TAG = "OTA";

static ota_state_t s_state = OTA_STATE_IDLE;
static esp_ota_handle_t s_handle;
static bool s_handle_open;
static const esp_partition_t *s_target;
static uint32_t s_size;
static uint32_t s_received;
static uint8_t s_expected_sha[32];
static psa_hash_operation_t s_hash = PSA_HASH_OPERATION_INIT;
/* The first OTA_IMAGE_HEADER_LEN bytes are held here until they show an ESP32-S3 app of
 * this project; only then is the slot opened and written */
static uint8_t s_hdr[OTA_IMAGE_HEADER_LEN];

_Static_assert(offsetof(esp_image_header_t, chip_id) == OTA_IMAGE_CHIP_ID_OFFSET, "image header layout");
_Static_assert(sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) == OTA_IMAGE_DESC_OFFSET,
               "app description offset");
_Static_assert(OTA_IMAGE_DESC_OFFSET + offsetof(esp_app_desc_t, project_name) == OTA_IMAGE_PROJECT_OFFSET,
               "project name offset");
_Static_assert(OTA_IMAGE_DESC_OFFSET + sizeof(esp_app_desc_t) == OTA_IMAGE_HEADER_LEN, "app description size");

/* Cleared by whichever of confirm or rollback runs first */
static atomic_bool s_pending;
static bool s_seen_ping, s_seen_version;
static esp_timer_handle_t s_rollback_timer;

static void rollback_timer_cb(void *arg)
{
    if (!atomic_exchange(&s_pending, false)) {
        return;
    }
    ESP_LOGE(TAG, "New image not confirmed by the display within %d s: rolling back",
             OTA_CONFIRM_TIMEOUT_MS / 1000);
    io_outputs_safe_state();
    esp_ota_mark_app_invalid_rollback_and_reboot();
    ESP_LOGE(TAG, "Rollback failed (no previous image?); keeping this one");
}

esp_err_t io_ota_init(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(running, &st) != ESP_OK || st != ESP_OTA_IMG_PENDING_VERIFY) {
        return ESP_OK;
    }
    const esp_timer_create_args_t args = {
        .callback = rollback_timer_cb,
        .name = "ota_rollback",
    };
    esp_err_t err = esp_timer_create(&args, &s_rollback_timer);
    if (err == ESP_OK) {
        err = esp_timer_start_once(s_rollback_timer, (uint64_t)OTA_CONFIRM_TIMEOUT_MS * 1000);
    }
    if (err != ESP_OK) {
        /* Without the timer a broken link would never roll back; the bootloader still
         * does on the next reset, because the image stays unconfirmed. */
        ESP_LOGE(TAG, "rollback timer: %s", esp_err_to_name(err));
    }
    atomic_store(&s_pending, true);
    s_state = OTA_STATE_PENDING;
    ESP_LOGW(TAG, "Running %s, pending verify: needs PING + VERSION within %d s or it rolls back",
             running->label, OTA_CONFIRM_TIMEOUT_MS / 1000);
    return err;
}

bool io_ota_supported(void)
{
    return esp_ota_get_next_update_partition(NULL) != NULL;
}

void io_ota_link_answered(uint8_t cmd)
{
    if (s_state != OTA_STATE_PENDING) {
        return;
    }
    s_seen_ping |= (cmd == MSG_PING);
    s_seen_version |= (cmd == MSG_VERSION);
    if (!s_seen_ping || !s_seen_version || !atomic_exchange(&s_pending, false)) {
        return;
    }
    esp_timer_stop(s_rollback_timer);
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    s_state = OTA_STATE_IDLE;
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "New image confirmed (PING and VERSION answered)");
    } else {
        ESP_LOGE(TAG, "mark valid: %s", esp_err_to_name(err));
    }
}

static void abort_transfer(void)
{
    if (s_handle_open) {
        esp_ota_abort(s_handle);
        s_handle_open = false;
    }
    psa_hash_abort(&s_hash);
}

static cobs_error_t fail(const char *what, esp_err_t err, cobs_error_t code)
{
    ESP_LOGE(TAG, "%s: %s", what, esp_err_to_name(err));
    abort_transfer();
    s_state = OTA_STATE_ERROR;
    return code;
}

cobs_error_t io_ota_begin(uint32_t size, const uint8_t sha256[32])
{
    if (s_state == OTA_STATE_PENDING) {
        return ERR_OTA_STATE;   /* esp_ota_begin refuses until this image is confirmed */
    }
    /* BEGIN from any other state starts over, so the display can retry a broken transfer */
    abort_transfer();
    s_state = OTA_STATE_IDLE;
    s_received = 0;

    s_target = esp_ota_get_next_update_partition(NULL);
    if (s_target == NULL) {
        return ERR_OTA_STATE;
    }
    if (size < OTA_IMAGE_HEADER_LEN || size > s_target->size) {
        return ERR_INVALID_ARGS;
    }

    io_outputs_safe_state();

    /* esp_ota_begin waits for the header check in io_ota_data() */
    if (psa_hash_setup(&s_hash, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        return fail("psa_hash_setup", ESP_FAIL, ERR_OTA_WRITE);
    }
    s_size = size;
    memcpy(s_expected_sha, sha256, sizeof(s_expected_sha));
    s_state = OTA_STATE_RECEIVING;
    ESP_LOGI(TAG, "Receiving %lu bytes into %s; outputs forced safe", (unsigned long)size, s_target->label);
    return ERR_NONE;
}

cobs_error_t io_ota_data(uint32_t offset, const uint8_t *data, size_t len)
{
    if (s_state != OTA_STATE_RECEIVING) {
        return ERR_OTA_STATE;
    }
    /* A mismatch leaves the transfer intact: the display can ask OTA_STATUS and resume */
    if (offset != s_received || len > s_size - s_received) {
        return ERR_OTA_WRITE;
    }
    if (s_received < OTA_IMAGE_HEADER_LEN) {
        size_t n = OTA_IMAGE_HEADER_LEN - s_received < len ? OTA_IMAGE_HEADER_LEN - s_received : len;
        memcpy(&s_hdr[s_received], data, n);
        if (s_received + n == OTA_IMAGE_HEADER_LEN) {
            char why[96];
            if (!ota_image_check(s_hdr, OTA_IMAGE_HEADER_LEN, esp_app_get_description()->project_name,
                                 why, sizeof(why))) {
                ESP_LOGE(TAG, "Image refused: %s", why);
                abort_transfer();
                s_state = OTA_STATE_ERROR;
                return ERR_OTA_IMAGE;
            }
            esp_err_t err = esp_ota_begin(s_target, OTA_WITH_SEQUENTIAL_WRITES, &s_handle);
            if (err != ESP_OK) {
                return fail("esp_ota_begin", err, ERR_OTA_WRITE);
            }
            s_handle_open = true;
            if ((err = esp_ota_write(s_handle, s_hdr, OTA_IMAGE_HEADER_LEN)) != ESP_OK) {
                return fail("esp_ota_write", err, ERR_OTA_WRITE);
            }
        }
        if (n == len) {
            /* the header bytes go to flash together, once checked */
            if (psa_hash_update(&s_hash, data, len) != PSA_SUCCESS) {
                return fail("psa_hash_update", ESP_FAIL, ERR_OTA_WRITE);
            }
            s_received += len;
            return ERR_NONE;
        }
    }
    size_t skip = s_received < OTA_IMAGE_HEADER_LEN ? OTA_IMAGE_HEADER_LEN - s_received : 0;
    esp_err_t err = esp_ota_write(s_handle, data + skip, len - skip);
    if (err != ESP_OK) {
        return fail("esp_ota_write", err, ERR_OTA_WRITE);
    }
    if (psa_hash_update(&s_hash, data, len) != PSA_SUCCESS) {
        return fail("psa_hash_update", ESP_FAIL, ERR_OTA_WRITE);
    }
    s_received += len;
    return ERR_NONE;
}

cobs_error_t io_ota_end(void)
{
    if (s_state != OTA_STATE_RECEIVING) {
        return ERR_OTA_STATE;
    }
    if (s_received != s_size) {
        ESP_LOGE(TAG, "Size mismatch: %lu of %lu bytes", (unsigned long)s_received, (unsigned long)s_size);
        return fail("verify", ESP_ERR_INVALID_SIZE, ERR_OTA_VERIFY);
    }
    uint8_t sha[32];
    size_t sha_len = 0;
    if (psa_hash_finish(&s_hash, sha, sizeof(sha), &sha_len) != PSA_SUCCESS
        || sha_len != sizeof(sha) || memcmp(sha, s_expected_sha, sizeof(sha)) != 0) {
        return fail("sha256 mismatch", ESP_ERR_INVALID_CRC, ERR_OTA_VERIFY);
    }
    int64_t t0 = esp_timer_get_time();
    s_handle_open = false;      /* esp_ota_end releases the handle whatever it returns */
    esp_err_t err = esp_ota_end(s_handle);
    if (err != ESP_OK) {
        return fail("esp_ota_end (image check)", err, ERR_OTA_VERIFY);
    }
    err = esp_ota_set_boot_partition(s_target);
    if (err != ESP_OK) {
        return fail("esp_ota_set_boot_partition", err, ERR_OTA_WRITE);
    }
    s_state = OTA_STATE_READY;
    /* the display allows 200 ms per reply; this is the slowest step */
    ESP_LOGI(TAG, "Image verified, boot slot %s set in %lld ms; RESET runs it", s_target->label,
             (long long)((esp_timer_get_time() - t0) / 1000));
    return ERR_NONE;
}

void io_ota_status(ota_state_t *state, uint32_t *bytes)
{
    *state = s_state;
    *bytes = (s_state == OTA_STATE_PENDING) ? 0 : s_received;
}
