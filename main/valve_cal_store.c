/**
 * @file valve_cal_store.c
 * @brief Persisted per-valve calibration in NVS namespace "hvac_cal".
 */

#include "valve_cal_store.h"
#include "settings.h"
#include "esp_log.h"
#include "nvs.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "VALVE_CAL";

static nvs_handle_t s_h;
static bool s_open;
static uint32_t s_boot_counter;

static uint16_t crc16_ccitt(const uint8_t *p, size_t n)
{
    uint16_t crc = 0xFFFF;
    while (n--) {
        crc ^= (uint16_t)(*p++) << 8;
        for (int i = 0; i < 8; i++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static uint16_t record_crc(const valve_cal_record_t *rec)
{
    return crc16_ccitt((const uint8_t *)rec, offsetof(valve_cal_record_t, crc));
}

static void key_for(hvac_valve_id_t valve, char *key)
{
    snprintf(key, 4, "v%d", (int)valve);
}

esp_err_t valve_cal_store_init(void)
{
    esp_err_t err = settings_ns_open(VALVE_CAL_NAMESPACE, VALVE_CAL_SCHEMA, NULL, &s_h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "open %s failed: %s", VALVE_CAL_NAMESPACE, esp_err_to_name(err));
        return err;
    }
    s_open = true;
    uint32_t n = 0;
    if (nvs_get_u32(s_h, "boot", &n) != ESP_OK) {
        n = 0;
    }
    s_boot_counter = n + 1;
    nvs_set_u32(s_h, "boot", s_boot_counter);
    nvs_commit(s_h);
    ESP_LOGI(TAG, "boot #%lu", (unsigned long)s_boot_counter);
    return ESP_OK;
}

uint32_t valve_cal_store_boot_counter(void)
{
    return s_boot_counter;
}

esp_err_t valve_cal_store_load(hvac_valve_id_t valve, valve_cal_record_t *out)
{
    if (!s_open || valve >= HVAC_VALVE_MAX || out == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    char key[4];
    key_for(valve, key);
    valve_cal_record_t rec;
    size_t len = sizeof(rec);
    esp_err_t err = nvs_get_blob(s_h, key, &rec, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;
    }
    if (err != ESP_OK) {
        return err;
    }
    if (len != sizeof(rec) || rec.crc != record_crc(&rec)) {
        return ESP_ERR_INVALID_CRC;
    }
    *out = rec;
    return ESP_OK;
}

esp_err_t valve_cal_store_save(hvac_valve_id_t valve, const valve_cal_record_t *rec)
{
    if (!s_open || valve >= HVAC_VALVE_MAX || rec == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    valve_cal_record_t r = *rec;
    r.boot_counter = s_boot_counter;
    r.crc = record_crc(&r);
    char key[4];
    key_for(valve, key);
    esp_err_t err = nvs_set_blob(s_h, key, &r, sizeof(r));
    if (err == ESP_OK) {
        err = nvs_commit(s_h);
    }
    return err;
}

esp_err_t valve_cal_store_erase(hvac_valve_id_t valve)
{
    if (!s_open || valve >= HVAC_VALVE_MAX) {
        return ESP_ERR_INVALID_STATE;
    }
    char key[4];
    key_for(valve, key);
    esp_err_t err = nvs_erase_key(s_h, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(s_h);
    }
    return err;
}

esp_err_t valve_cal_store_erase_all(void)
{
    esp_err_t result = ESP_OK;
    for (int i = 0; i < HVAC_VALVE_MAX; i++) {
        esp_err_t err = valve_cal_store_erase((hvac_valve_id_t)i);
        if (err != ESP_OK) {
            result = err;
        }
    }
    return result;
}

// Range only, with 3 % slack: a closed valve rests at either end of the pot, often in a
// deadzone, so the deadzones must not reject a record (finding 2)
bool valve_cal_store_plausible(const valve_cal_record_t *rec, uint8_t current_adc)
{
    return rec != NULL
        && current_adc + 3 >= rec->adc_min && current_adc <= rec->adc_max + 3;
}
