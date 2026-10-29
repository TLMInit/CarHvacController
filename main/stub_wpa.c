/* Placeholder implementations of settings.h until WP-A lands. WP-A deletes this file. */
#include <string.h>
#include "settings.h"

static setting_def_t s_stub_def;

esp_err_t settings_init(void) { return ESP_OK; }
const setting_def_t *settings_def(setting_id_t id) { s_stub_def.id = id; return &s_stub_def; }
void settings_iterate(bool (*cb)(const setting_def_t *def, void *ctx), void *ctx) { (void)cb; (void)ctx; }
int32_t settings_get_int(setting_id_t id) { (void)id; return 0; }
float settings_get_float(setting_id_t id) { (void)id; return 0.0f; }
bool settings_get_bool(setting_id_t id) { (void)id; return false; }
void settings_get_str(setting_id_t id, char *buf, size_t len) { (void)id; if (buf && len) buf[0] = '\0'; }
esp_err_t settings_set_int(setting_id_t id, int32_t value) { (void)id; (void)value; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t settings_set_float(setting_id_t id, float value) { (void)id; (void)value; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t settings_set_str(setting_id_t id, const char *value) { (void)id; (void)value; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t settings_step(setting_id_t id, int direction) { (void)id; (void)direction; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t settings_reset(setting_id_t id) { (void)id; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t settings_reset_all(void) { return ESP_ERR_NOT_SUPPORTED; }
void settings_format(setting_id_t id, char *buf, size_t len) { (void)id; if (buf && len) buf[0] = '\0'; }
const char *settings_label(setting_id_t id) { (void)id; return ""; }
uint32_t settings_seq(void) { return 0; }
esp_err_t settings_ns_open(const char *ns, uint16_t schema, settings_migrate_fn migrate, nvs_handle_t *out)
{
    (void)schema; (void)migrate;
    return nvs_open(ns, NVS_READWRITE, out);
}
