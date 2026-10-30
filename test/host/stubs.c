/* Stubs for the apply callbacks that settings_table.c points at (other firmware modules). */
#include "tb6612fng_driver.h"
#include "hvac_controller.h"
#include "wifi_ap.h"
#include "remote_io.h"

int stub_calls;
void tb6612fng_apply_settings(void) { stub_calls++; }
esp_err_t tb6612fng_service_recalibrate(hvac_valve_id_t v) { (void)v; stub_calls++; return ESP_OK; }
void hvac_controller_apply_settings(void) { stub_calls++; }
void wifi_ap_apply_settings(void) { stub_calls++; }
esp_err_t remote_io_reset_node(void) { stub_calls++; return ESP_OK; }
