/**
 * @file wifi_ap.h
 * @brief Phone access point "HVAC-<mac4>", WPA2, mode from SET_WIFI_AP_MODE.
 *
 * INTERFACE (lead). Implemented by WP-G in wifi_ap.c.
 * OFF: radio off. 10MIN: on at boot (and when the setting is changed to it),
 * off after SET_AP_IDLE_MIN (default 10 min) without a connected client. ALWAYS: on.
 * SET_AP_CHANNEL and the password take effect at the next AP start.
 */

#ifndef WIFI_AP_H
#define WIFI_AP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Call once after settings_init(); applies the current mode. */
esp_err_t wifi_ap_init(void);

/** Re-read SET_WIFI_AP_MODE / SET_AP_PASSWORD (settings apply_cb). */
void wifi_ap_apply_settings(void);

bool wifi_ap_is_on(void);
/** Last AP start failure ("esp_wifi_start: ESP_ERR_NO_MEM"), "" when none. Retried every second. */
const char *wifi_ap_last_error(void);
/** The AP's own IPv4 address from esp_netif ("192.168.4.1"); false (buf untouched) while the AP is off. */
bool wifi_ap_address(char *buf, size_t len);
const char *wifi_ap_ssid(void);             /* "HVAC-1A2B" */
int wifi_ap_client_count(void);
uint32_t wifi_ap_remaining_s(void);         /* 10MIN mode: seconds left without a client, else 0 */

#ifdef __cplusplus
}
#endif

#endif /* WIFI_AP_H */
