/**
 * @file io_ota.h
 * @brief Node OTA over the link: write the inactive app slot, verify, switch, confirm.
 *
 * All functions run in the link task. Each returns ERR_NONE or the protocol
 * error code to send back; the caller replies OTA_ACK with io_ota_status().
 */

#ifndef IO_OTA_H
#define IO_OTA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "cobs_protocol.h"

/** Call once at boot: if this image is pending verification, arm the rollback timer. */
esp_err_t io_ota_init(void);

/** False when the partition table has no second app slot (OTA impossible). */
bool io_ota_supported(void);

cobs_error_t io_ota_begin(uint32_t size, const uint8_t sha256[32]);
cobs_error_t io_ota_data(uint32_t offset, const uint8_t *data, size_t len);
cobs_error_t io_ota_end(void);

void io_ota_status(ota_state_t *state, uint32_t *bytes);

/** The link task answered this command; PING plus VERSION confirm a pending image. */
void io_ota_link_answered(uint8_t cmd);

#endif /* IO_OTA_H */
