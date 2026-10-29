/* Placeholder implementations of the WP-D interfaces (valve_cal_store.h, tb6612fng_driver.h v2.1). WP-D deletes this file. */
#include <string.h>
#include "valve_cal_store.h"
#include "tb6612fng_driver.h"

esp_err_t valve_cal_store_init(void) { return ESP_OK; }
uint32_t valve_cal_store_boot_counter(void) { return 0; }
esp_err_t valve_cal_store_load(hvac_valve_id_t valve, valve_cal_record_t *out) { (void)valve; (void)out; return ESP_ERR_NOT_FOUND; }
esp_err_t valve_cal_store_save(hvac_valve_id_t valve, const valve_cal_record_t *rec) { (void)valve; (void)rec; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t valve_cal_store_erase(hvac_valve_id_t valve) { (void)valve; return ESP_OK; }
esp_err_t valve_cal_store_erase_all(void) { return ESP_OK; }
bool valve_cal_store_plausible(const valve_cal_record_t *rec, uint8_t current_adc) { (void)rec; (void)current_adc; return false; }

void tb6612fng_get_valve_info(hvac_valve_id_t valve_id, tb6612_valve_info_t *out) { (void)valve_id; if (out) memset(out, 0, sizeof(*out)); }
esp_err_t tb6612fng_service_jog(hvac_valve_id_t valve_id, int dir) { (void)valve_id; (void)dir; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t tb6612fng_service_stop(hvac_valve_id_t valve_id) { (void)valve_id; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t tb6612fng_service_recalibrate(hvac_valve_id_t valve_id) { (void)valve_id; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t tb6612fng_service_learn_direction(hvac_valve_id_t valve_id) { (void)valve_id; return ESP_ERR_NOT_SUPPORTED; }
void tb6612fng_apply_settings(void) {}
