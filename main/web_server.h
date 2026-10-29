/**
 * @file web_server.h
 * @brief Phone page and JSON API, served while the AP is on.
 *
 * INTERFACE (lead). Implemented by WP-G in web_server.c (+ web/index.html via EMBED_FILES).
 * Endpoints: GET /, GET /api/state, GET+POST /api/settings, GET /api/log,
 * GET /api/diag, POST /api/ota/display, POST /api/ota/node.
 */

#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Started/stopped by wifi_ap.c when the AP goes on/off. */
esp_err_t web_server_start(void);
void web_server_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* WEB_SERVER_H */
