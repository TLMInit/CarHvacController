/* Placeholder implementations of wifi_ap.h, web_server.h, ota_display.h until WP-G lands. WP-G deletes this file. */
#include <stdio.h>
#include "wifi_ap.h"
#include "web_server.h"
#include "ota_display.h"

esp_err_t wifi_ap_init(void) { return ESP_OK; }
void wifi_ap_apply_settings(void) {}
bool wifi_ap_is_on(void) { return false; }
const char *wifi_ap_ssid(void) { return ""; }
int wifi_ap_client_count(void) { return 0; }
uint32_t wifi_ap_remaining_s(void) { return 0; }

esp_err_t web_server_start(void) { return ESP_ERR_NOT_SUPPORTED; }
void web_server_stop(void) {}

bool ota_allowed(char *why, size_t len) { if (why && len) snprintf(why, len, "Updates not available in this build"); return false; }
esp_err_t ota_display_begin(size_t image_size) { (void)image_size; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t ota_display_write(const void *data, size_t len) { (void)data; (void)len; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t ota_display_end(void) { return ESP_ERR_NOT_SUPPORTED; }
void ota_display_abort(void) {}
void ota_display_start_validation(void) {}
bool ota_display_pending_validation(void) { return false; }
