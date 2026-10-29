/* Placeholder implementations of the WP-C interfaces (remote_io.h v2.1, sensor_roles.h). WP-C deletes this file. */
#include <math.h>
#include <string.h>
#include "remote_io.h"
#include "sensor_roles.h"

esp_err_t remote_io_hello(uint8_t *node_proto, uint8_t *node_caps)
{
    if (node_proto) *node_proto = 1;
    if (node_caps) *node_caps = 0;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t remote_ds18b20_search(uint8_t roms[][8], int max, int *count) { (void)roms; (void)max; if (count) *count = 0; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t remote_io_ota_node(const uint8_t *image, size_t len, const uint8_t sha256[32],
                             remote_ota_progress_cb_t cb, void *ctx)
{
    (void)image; (void)len; (void)sha256; (void)cb; (void)ctx;
    return ESP_ERR_NOT_SUPPORTED;
}
void remote_io_get_health(remote_io_health_t *out) { if (out) { memset(out, 0, sizeof(*out)); strcpy(out->node_version, "?"); } }

void sensor_roles_request_search(void) {}
bool sensor_roles_search_pending(void) { return false; }
size_t sensor_roles_get_found(sensor_rom_info_t *out, size_t max) { (void)out; (void)max; return 0; }
esp_err_t sensor_roles_assign(const uint8_t rom[8], sensor_role_t role) { (void)rom; (void)role; return ESP_ERR_NOT_SUPPORTED; }
float sensor_roles_get_temp(sensor_role_t role) { (void)role; return NAN; }
const char *sensor_role_name(sensor_role_t role) { (void)role; return "?"; }
uint32_t sensor_roles_seq(void) { return 0; }
