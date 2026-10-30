/**
 * @file protocol_server.h
 * @brief UART link to the display: COBS frames in, replies out, link watchdog.
 */

#ifndef PROTOCOL_SERVER_H
#define PROTOCOL_SERVER_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    uint32_t rx_frames;         /* valid frames received */
    uint32_t tx_frames;         /* replies sent */
    uint32_t bad_frames;        /* CRC or COBS failures */
    uint32_t errors_sent;       /* MSG_ERROR replies */
    uint32_t watchdog_trips;    /* times the outputs were forced safe */
    bool link_up;               /* a valid frame arrived within the link watchdog time */
} protocol_stats_t;

/** Install the UART driver and start the link task (pinned to the second core). */
esp_err_t protocol_server_start(void);

void protocol_server_get_stats(protocol_stats_t *out);

#endif /* PROTOCOL_SERVER_H */
