/**
 * @file hvac_sensors.c
 * @brief HVAC Sensor Management Implementation
 * 
 * Sensors are read on the remote IO node via UART:
 * - BME280 for cabin temperature and humidity (via remote I2C)
 * - DS18B20 duct sensor (via remote 1-Wire)
 * - ADS1115 driver for valve position feedback via analog inputs
 */

#include "hvac_sensors.h"
#include "hvac_hw_config.h"
#include "ads1115_driver.h"
#include "tb6612fng_driver.h"
#include "hvac_ui.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <math.h>
#include <string.h>

// Remote IO for sensor reading via UART
#include "remote_io.h"
#include "cobs_protocol.h"
#include "sensor_roles.h"
#include "nvs.h"
#include <stdio.h>

static const char *TAG = "HVAC_SENSORS";

// Sensor data storage
static sensor_data_t sensor_data[SENSOR_MAX];
static SemaphoreHandle_t sensor_mutex = NULL;
static TaskHandle_t sensor_task_handle = NULL;

// Valve position storage (5 valves: center1, center2, foot, windscreen, coolant)

// Configuration
static uint32_t sensor_interval_ms = 100;   // Default 100ms for responsive valve control
static bool calibration_mode = false;       // Fast valve reading during calibration
#define CALIB_SENSOR_INTERVAL_MS 25         // 25ms during calibration
#define SENSOR_STALE_MS 5000    // A reading older than this is no reading

/*******************************************************************************
 * Private Functions
 ******************************************************************************/

static uint32_t get_time_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// Remote sensor reading functions using UART protocol

// --- BME280 calibration helpers (remote I2C) ---
typedef struct {
    uint16_t dig_T1;
    int16_t  dig_T2;
    int16_t  dig_T3;
    uint8_t  dig_H1;
    int16_t  dig_H2;
    uint8_t  dig_H3;
    int16_t  dig_H4;
    int16_t  dig_H5;
    int8_t   dig_H6;
    bool     loaded;
} bme280_calib_t;

static bme280_calib_t bme_calib = {0};

static esp_err_t bme280_load_calibration(uint8_t addr)
{
    // Read temp/pressure calibration (0x88..0xA1) and humidity (0xE1..0xE7)
    uint8_t reg = 0x88;
    uint8_t buf1[26];
    esp_err_t ret = remote_i2c_write_read(0, addr, &reg, 1, buf1, sizeof(buf1));
    if (ret != ESP_OK) {
        return ret;
    }

    reg = 0xE1;
    uint8_t buf2[7];
    ret = remote_i2c_write_read(0, addr, &reg, 1, buf2, sizeof(buf2));
    if (ret != ESP_OK) {
        return ret;
    }

    bme_calib.dig_T1 = (uint16_t)(buf1[1] << 8 | buf1[0]);
    bme_calib.dig_T2 = (int16_t)(buf1[3] << 8 | buf1[2]);
    bme_calib.dig_T3 = (int16_t)(buf1[5] << 8 | buf1[4]);

    bme_calib.dig_H1 = buf1[25]; // 0xA1
    bme_calib.dig_H2 = (int16_t)(buf2[1] << 8 | buf2[0]);
    bme_calib.dig_H3 = buf2[2];
    bme_calib.dig_H4 = (int16_t)((buf2[3] << 4) | (buf2[4] & 0x0F));
    bme_calib.dig_H5 = (int16_t)((buf2[5] << 4) | (buf2[4] >> 4));
    bme_calib.dig_H6 = (int8_t)buf2[6];

    bme_calib.loaded = true;
    return ESP_OK;
}

static float bme280_compensate_temperature(int32_t adc_T, int32_t *t_fine)
{
    int32_t var1 = ((((adc_T >> 3) - ((int32_t)bme_calib.dig_T1 << 1))) * ((int32_t)bme_calib.dig_T2)) >> 11;
    int32_t var2 = (((((adc_T >> 4) - ((int32_t)bme_calib.dig_T1)) * ((adc_T >> 4) - ((int32_t)bme_calib.dig_T1))) >> 12) * ((int32_t)bme_calib.dig_T3)) >> 14;
    *t_fine = var1 + var2;
    int32_t T = (*t_fine * 5 + 128) >> 8; // T in 0.01 C
    return T / 100.0f;
}

static float bme280_compensate_humidity(int32_t adc_H, int32_t t_fine)
{
    int32_t v_x1 = t_fine - ((int32_t)76800);
    v_x1 = (((((adc_H << 14) - (((int32_t)bme_calib.dig_H4) << 20) - (((int32_t)bme_calib.dig_H5) * v_x1)) + ((int32_t)16384)) >> 15)
            * (((((((v_x1 * ((int32_t)bme_calib.dig_H6)) >> 10) * (((v_x1 * ((int32_t)bme_calib.dig_H3)) >> 11) + ((int32_t)32768))) >> 10) + ((int32_t)2097152))
                * ((int32_t)bme_calib.dig_H2) + 8192) >> 14));
    v_x1 = v_x1 - (((((v_x1 >> 15) * (v_x1 >> 15)) >> 7) * ((int32_t)bme_calib.dig_H1)) >> 4);
    v_x1 = (v_x1 < 0 ? 0 : v_x1);
    v_x1 = (v_x1 > 419430400 ? 419430400 : v_x1);
    float h = (v_x1 >> 12) / 1024.0f; // %RH
    if (h < 0.0f) h = 0.0f;
    if (h > 100.0f) h = 100.0f;
    return h;
}

#define SENSOR_MAX_RATE_C_PER_S  5.0f   // Faster than this is a bad reading, not the cabin
#define SENSOR_REJECT_LIMIT      3      // Rejects in a row before the sensor is marked ERROR

// Range and rate check (SN-02). The rate is measured against the last accepted value
// over the real time gap, so a sensor that was away for a while can come back at a
// different temperature. *rejects counts rejects in a row (saturating).
static bool plausible_reading(float t, float last, uint32_t last_ms, float lo, float hi, uint8_t *rejects)
{
    bool ok = (t >= lo && t <= hi);     // false for NAN too
    if (ok && !isnan(last)) {
        float dt_s = (get_time_ms() - last_ms) / 1000.0f;
        ok = fabsf(t - last) <= SENSOR_MAX_RATE_C_PER_S * fmaxf(dt_s, 1.0f);
    }
    if (ok) {
        *rejects = 0;
        return true;
    }
    if (*rejects < SENSOR_REJECT_LIMIT) {
        (*rejects)++;
    }
    return false;
}

// plausible_reading() for a sensor_data entry, with its log line and ERROR status;
// caller holds sensor_mutex
static bool plausible_temp(hvac_sensor_id_t id, float t, float lo, float hi)
{
    static uint8_t rejects[SENSOR_MAX];
    const sensor_data_t *s = &sensor_data[id];

    if (plausible_reading(t, s->value, s->last_update_ms, lo, hi, &rejects[id])) {
        return true;
    }
    ESP_LOGW(TAG, "Sensor %d: %.2f C rejected as implausible (last %.2f C, %d in a row)",
             id, t, s->value, rejects[id]);
    if (rejects[id] >= SENSOR_REJECT_LIMIT) {
        sensor_data[id].status = SENSOR_STATUS_ERROR;
    }
    return false;
}

// Non-blocking, called about once a second: collects the conversion started on the
// previous call (12-bit needs 750 ms) and starts the next. The blocking read waited
// 750 ms inside every sensor cycle, so valve feedback ran at ~1.1 Hz instead of 10 Hz.
#define DS18B20_CONVERSION_MS   750

/*
 * Sensor roles (sensor_roles.h). The sensors task owns all 1-Wire traffic; the public
 * functions below only touch the cached table under sensor_mutex.
 * Node with NODE_CAP_SEARCH: one Skip-ROM Convert T per cycle, then one Match-ROM read per
 * known ROM on the next cycle. Node without it (protocol 1): 2.0 behaviour, a Skip-ROM
 * read is the DUCT sensor.
 */
#define ROLES_NVS_NS    "hvac_cfg"      // settings namespace (sensor_roles.h)
#define ROLES_NVS_KEY   "rom_roles"

typedef struct __attribute__((packed)) {
    uint8_t rom[8];
    uint8_t role;
} rom_role_rec_t;

typedef struct {
    uint8_t rom[8];
    sensor_role_t role;
    bool present;
    float temp_c;               // last plausible reading, NAN if none
    uint32_t last_ms;           // when temp_c was read
    uint8_t rejects;            // SN-02 rejects in a row
} rom_slot_t;

static rom_slot_t s_roms[SENSOR_ROLES_MAX_ROMS];    // guarded by sensor_mutex
static size_t s_rom_count;
static bool s_roles_stored;                         // "rom_roles" exists in NVS (no 2.0 migration)
static volatile bool s_search_requested;
static volatile uint32_t s_roles_seq;

// ROM in bus order (family code first) for the console
static void rom_str(const uint8_t rom[8], char out[17])
{
    for (int i = 0; i < 8; i++) {
        snprintf(&out[i * 2], 3, "%02X", rom[i]);
    }
}

// Range by role; the DS18B20 itself spans -55..125 C
static void role_range(sensor_role_t role, float *lo, float *hi)
{
    *lo = -40.0f;
    *hi = (role == SENSOR_ROLE_COOLANT) ? 125.0f : 100.0f;
}

// Called from main via hvac_sensors_init(), after nvs_flash_init()
static void roles_load(void)
{
    rom_role_rec_t recs[SENSOR_ROLES_MAX_ROMS];
    size_t len = sizeof(recs);
    nvs_handle_t h;
    s_rom_count = 0;
    if (nvs_open(ROLES_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    if (nvs_get_blob(h, ROLES_NVS_KEY, recs, &len) == ESP_OK) {
        s_roles_stored = true;
        for (size_t i = 0; i < len / sizeof(recs[0]) && s_rom_count < SENSOR_ROLES_MAX_ROMS; i++) {
            if (recs[i].role == SENSOR_ROLE_UNUSED || recs[i].role >= SENSOR_ROLE_COUNT) continue;
            rom_slot_t *s = &s_roms[s_rom_count++];
            memcpy(s->rom, recs[i].rom, 8);
            s->role = (sensor_role_t)recs[i].role;
            s->present = false;
            s->temp_c = NAN;
            char hex[17];
            rom_str(s->rom, hex);
            ESP_LOGI(TAG, "ROM %s: %s", hex, sensor_role_name(s->role));
        }
    }
    nvs_close(h);
}

// Assigned ROMs to NVS. Caller does NOT hold sensor_mutex (NVS write can take a while).
static esp_err_t roles_save(const rom_role_rec_t *recs)
{
    nvs_handle_t h;
    esp_err_t ret = nvs_open(ROLES_NVS_NS, NVS_READWRITE, &h);
    if (ret == ESP_OK) {
        ret = nvs_set_blob(h, ROLES_NVS_KEY, recs, SENSOR_ROLES_MAX_ROMS * sizeof(recs[0]));
        if (ret == ESP_OK) ret = nvs_commit(h);
        nvs_close(h);
    }
    if (ret == ESP_OK) {
        s_roles_stored = true;
    } else {
        ESP_LOGE(TAG, "Saving sensor roles failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

// Snapshot of the assignments for roles_save; caller holds sensor_mutex
static void roles_snapshot(rom_role_rec_t *recs)
{
    memset(recs, 0, SENSOR_ROLES_MAX_ROMS * sizeof(recs[0]));
    for (size_t i = 0, n = 0; i < s_rom_count; i++) {
        if (s_roms[i].role != SENSOR_ROLE_UNUSED) {
            memcpy(recs[n].rom, s_roms[i].rom, 8);
            recs[n++].role = (uint8_t)s_roms[i].role;
        }
    }
}

// Search result merged into the table: assigned ROMs stay (present or not), unassigned
// ones are replaced by what was found. Sensors task.
static void roles_apply_search(uint8_t roms[][8], int found)
{
    bool migrate = false;
    rom_role_rec_t recs[SENSOR_ROLES_MAX_ROMS];

    if (xSemaphoreTake(sensor_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }
    size_t keep = 0;
    for (size_t i = 0; i < s_rom_count; i++) {
        if (s_roms[i].role != SENSOR_ROLE_UNUSED) {
            s_roms[keep] = s_roms[i];
            s_roms[keep++].present = false;
        }
    }
    s_rom_count = keep;
    for (int f = 0; f < found; f++) {
        size_t i = 0;
        while (i < s_rom_count && memcmp(s_roms[i].rom, roms[f], 8) != 0) i++;
        if (i == s_rom_count) {
            if (s_rom_count == SENSOR_ROLES_MAX_ROMS) continue;   // table full of assigned ROMs
            memset(&s_roms[i], 0, sizeof(s_roms[i]));
            memcpy(s_roms[i].rom, roms[f], 8);
            s_roms[i].temp_c = NAN;
            s_rom_count++;
        }
        s_roms[i].present = true;
    }
    // First boot after 2.0: nothing stored and one sensor on the bus -> it is the duct sensor
    if (!s_roles_stored && found == 1 && s_rom_count == 1 && s_roms[0].role == SENSOR_ROLE_UNUSED) {
        s_roms[0].role = SENSOR_ROLE_DUCT;
        roles_snapshot(recs);
        migrate = true;
    }
    s_roles_seq++;
    xSemaphoreGive(sensor_mutex);

    ESP_LOGI(TAG, "1-Wire search: %d ROM(s)", found);
    for (int f = 0; f < found; f++) {
        char hex[17];
        rom_str(roms[f], hex);
        ESP_LOGI(TAG, "  ROM %s", hex);
    }
    if (migrate) {
        ESP_LOGI(TAG, "Sensor roles: single ROM assigned DUCT (migration from 2.0)");
        roles_save(recs);
    }
}

// SN-02 for one ROM, then the reading into the table (and into the duct sensor_data for
// role DUCT). Sensors task.
static void roles_store_reading(size_t i, const uint8_t rom[8], esp_err_t ret, float t)
{
    if (xSemaphoreTake(sensor_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }
    if (i >= s_rom_count || memcmp(s_roms[i].rom, rom, 8) != 0) {  // table changed meanwhile
        xSemaphoreGive(sensor_mutex);
        return;
    }
    rom_slot_t *s = &s_roms[i];
    uint32_t now = get_time_ms();
    bool is_duct = (s->role == SENSOR_ROLE_DUCT);
    bool grace = (now < 5000);
    s->present = (ret == ESP_OK);

    float lo, hi;
    role_range(s->role, &lo, &hi);
    if (ret != ESP_OK) {
        if (is_duct) {
            sensor_data[SENSOR_DUCT_TEMP].status = grace ? SENSOR_STATUS_NOT_FOUND : SENSOR_STATUS_ERROR;
        }
        ESP_LOGW(TAG, "DS18B20 %s read error: %s", sensor_role_name(s->role), esp_err_to_name(ret));
    } else if (t == 85.0f && !(s->temp_c > 75.0f)) {
        // 85.00 C is the DS18B20's power-on scratchpad (no conversion since its reset)
        ESP_LOGW(TAG, "DS18B20 %s power-on value 85.00 C ignored", sensor_role_name(s->role));
    } else if (!plausible_reading(t, s->temp_c, s->last_ms, lo, hi, &s->rejects)) {
        ESP_LOGW(TAG, "DS18B20 %s: %.2f C rejected as implausible (last %.2f C, %d in a row)",
                 sensor_role_name(s->role), t, s->temp_c, s->rejects);
        if (is_duct && s->rejects >= SENSOR_REJECT_LIMIT) {
            sensor_data[SENSOR_DUCT_TEMP].status = SENSOR_STATUS_ERROR;
        }
    } else {
        if (s->temp_c != t) s_roles_seq++;
        s->temp_c = t;
        s->last_ms = now;
        if (is_duct) {
            sensor_data[SENSOR_DUCT_TEMP].value = t;
            sensor_data[SENSOR_DUCT_TEMP].status = SENSOR_STATUS_OK;
            sensor_data[SENSOR_DUCT_TEMP].last_update_ms = now;
        }
    }
    xSemaphoreGive(sensor_mutex);
}

// 2.0 path for a node without search: Skip-ROM read is the duct sensor. Sensors task.
static void legacy_store_duct(esp_err_t ret, float temperature)
{
    bool grace = (get_time_ms() < 5000);
    if (xSemaphoreTake(sensor_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        bool power_on_value = (ret == ESP_OK && temperature == 85.0f &&
                               !(sensor_data[SENSOR_DUCT_TEMP].value > 75.0f));
        if (power_on_value) {
            ESP_LOGW(TAG, "DS18B20 power-on value 85.00 C ignored");
        } else if (ret == ESP_OK && !plausible_temp(SENSOR_DUCT_TEMP, temperature, -40.0f, 100.0f)) {
            // rejected and logged; status handled by plausible_temp()
        } else if (ret == ESP_OK) {
            sensor_data[SENSOR_DUCT_TEMP].value = temperature;
            sensor_data[SENSOR_DUCT_TEMP].status = SENSOR_STATUS_OK;
            sensor_data[SENSOR_DUCT_TEMP].last_update_ms = get_time_ms();
            ESP_LOGD(TAG, "Remote duct temp: %.1f°C", temperature);
        } else {
            sensor_data[SENSOR_DUCT_TEMP].status = grace ? SENSOR_STATUS_NOT_FOUND : SENSOR_STATUS_ERROR;
            ESP_LOGW(TAG, "Remote DS18B20 duct read error");
        }
        xSemaphoreGive(sensor_mutex);
    }
}

// Sensors task, once per temperature cycle (1 s)
static void read_remote_ds18b20(void)
{
    static bool converting = false;
    static uint32_t started_ms = 0;
    static bool searched = false;   // one search per boot once the node can search

    if (converting && get_time_ms() - started_ms < DS18B20_CONVERSION_MS) {
        return;     // called early; the result is not ready yet
    }
    remote_io_health_t link;
    remote_io_get_health(&link);
    bool can_search = (link.node_caps & NODE_CAP_SEARCH) != 0;
    bool had_conversion = converting;

    // 1. Collect the conversion started on the previous cycle
    if (converting) {
        if (can_search) {
            for (size_t i = 0; i < SENSOR_ROLES_MAX_ROMS; i++) {
                uint8_t rom[8];
                bool have = false;
                if (xSemaphoreTake(sensor_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                    have = (i < s_rom_count);
                    if (have) memcpy(rom, s_roms[i].rom, 8);
                    xSemaphoreGive(sensor_mutex);
                }
                if (!have) break;
                float t = NAN;
                esp_err_t ret = remote_ds18b20_read_temp(rom, &t);
                roles_store_reading(i, rom, ret, t);
            }
        } else {
            float t = NAN;
            esp_err_t ret = remote_ds18b20_read_temp(NULL, &t);
            legacy_store_duct(ret, t);
        }
    }

    // 2. Requested search (and one at boot), between conversions
    if (s_search_requested || (can_search && !searched && link.connected)) {
        uint8_t roms[SENSOR_ROLES_MAX_ROMS][8];
        int found = 0;
        esp_err_t ret = remote_ds18b20_search(roms, SENSOR_ROLES_MAX_ROMS, &found);
        if (ret == ESP_OK) {
            searched = true;
            roles_apply_search(roms, found);
        } else {
            ESP_LOGW(TAG, "1-Wire search failed: %s", esp_err_to_name(ret));
        }
        s_search_requested = false;
    }

    // 3. Next broadcast Convert T
    esp_err_t start = remote_ds18b20_start_conversion(NULL);
    converting = (start == ESP_OK);
    started_ms = get_time_ms();
    if (!had_conversion && !converting && !can_search) {
        legacy_store_duct(start, NAN);      // as 2.0: no read and no conversion is a duct error
    }
}

void sensor_roles_request_search(void)
{
    s_search_requested = true;
}

bool sensor_roles_search_pending(void)
{
    return s_search_requested;
}

size_t sensor_roles_get_found(sensor_rom_info_t *out, size_t max)
{
    size_t n = 0;
    if (out == NULL || sensor_mutex == NULL) {
        return 0;
    }
    if (xSemaphoreTake(sensor_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        uint32_t now = get_time_ms();
        for (; n < s_rom_count && n < max; n++) {
            const rom_slot_t *s = &s_roms[n];
            memcpy(out[n].rom, s->rom, 8);
            out[n].role = s->role;
            out[n].present = s->present;
            out[n].temp_c = s->temp_c;
            out[n].age_ms = isnan(s->temp_c) ? UINT32_MAX : now - s->last_ms;
        }
        xSemaphoreGive(sensor_mutex);
    }
    return n;
}

// Called from the UI or web task
esp_err_t sensor_roles_assign(const uint8_t rom[8], sensor_role_t role)
{
    if (rom == NULL || role >= SENSOR_ROLE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (sensor_mutex == NULL || xSemaphoreTake(sensor_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    size_t i = 0;
    while (i < s_rom_count && memcmp(s_roms[i].rom, rom, 8) != 0) i++;
    if (i == s_rom_count) {
        if (role == SENSOR_ROLE_UNUSED || s_rom_count == SENSOR_ROLES_MAX_ROMS) {
            xSemaphoreGive(sensor_mutex);
            return role == SENSOR_ROLE_UNUSED ? ESP_OK : ESP_ERR_NO_MEM;
        }
        memset(&s_roms[i], 0, sizeof(s_roms[i]));
        memcpy(s_roms[i].rom, rom, 8);
        s_roms[i].temp_c = NAN;
        s_rom_count++;
    }
    bool duct_changed = (s_roms[i].role == SENSOR_ROLE_DUCT) != (role == SENSOR_ROLE_DUCT);
    for (size_t j = 0; j < s_rom_count; j++) {
        if (role != SENSOR_ROLE_UNUSED && j != i && s_roms[j].role == role) {
            s_roms[j].role = SENSOR_ROLE_UNUSED;    // one ROM per role
        }
    }
    s_roms[i].role = role;
    if (duct_changed) {
        // the duct value must come from the newly assigned ROM's next reading
        sensor_data[SENSOR_DUCT_TEMP].value = NAN;
        sensor_data[SENSOR_DUCT_TEMP].status = SENSOR_STATUS_NOT_FOUND;
        if (role == SENSOR_ROLE_DUCT && !isnan(s_roms[i].temp_c)) {
            sensor_data[SENSOR_DUCT_TEMP].value = s_roms[i].temp_c;
            sensor_data[SENSOR_DUCT_TEMP].status = SENSOR_STATUS_OK;
            sensor_data[SENSOR_DUCT_TEMP].last_update_ms = s_roms[i].last_ms;
        }
    }
    rom_role_rec_t recs[SENSOR_ROLES_MAX_ROMS];
    roles_snapshot(recs);
    s_roles_seq++;
    xSemaphoreGive(sensor_mutex);

    char hex[17];
    rom_str(rom, hex);
    ESP_LOGI(TAG, "ROM %s assigned %s", hex, sensor_role_name(role));
    return roles_save(recs);
}

float sensor_roles_get_temp(sensor_role_t role)
{
    if (role == SENSOR_ROLE_DUCT) {
        return hvac_sensors_get_duct_temp();    // also covers the protocol 1 Skip-ROM path
    }
    float t = NAN;
    if (role == SENSOR_ROLE_UNUSED || role >= SENSOR_ROLE_COUNT || sensor_mutex == NULL) {
        return t;
    }
    if (xSemaphoreTake(sensor_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        for (size_t i = 0; i < s_rom_count; i++) {
            if (s_roms[i].role == role && !isnan(s_roms[i].temp_c) &&
                get_time_ms() - s_roms[i].last_ms <= SENSOR_STALE_MS) {
                t = s_roms[i].temp_c;
            }
        }
        xSemaphoreGive(sensor_mutex);
    }
    return t;
}

const char *sensor_role_name(sensor_role_t role)
{
    switch (role) {
        case SENSOR_ROLE_DUCT:    return "Duct";
        case SENSOR_ROLE_COOLANT: return "Coolant";
        case SENSOR_ROLE_OUTSIDE: return "Outside";
        default:                  return "Unused";
    }
}

uint32_t sensor_roles_seq(void)
{
    return s_roles_seq;
}

// Read BME280 temperature and humidity via remote I2C
// BME280 requires proper initialization and calibration coefficient reading
// For now, this is a simplified version that triggers measurement and reads raw data
static void read_remote_bme280(void)
{
    esp_err_t ret;
    uint8_t addr = 0x76;  // Try 0x76 first, then 0x77
    bool grace = (get_time_ms() < 5000);
    
    // Check if BME280 is present by reading chip ID (0xD0)
    uint8_t chip_id_reg = 0xD0;
    uint8_t chip_id;
    ret = remote_i2c_write_read(0, addr, &chip_id_reg, 1, &chip_id, 1);
    if (ret != ESP_OK || chip_id != 0x60) {
        addr = 0x77;  // Try alternate address
        ret = remote_i2c_write_read(0, addr, &chip_id_reg, 1, &chip_id, 1);
        if (ret != ESP_OK || chip_id != 0x60) {
            ESP_LOGW(TAG, "BME280 not found (chip_id=0x%02X)", chip_id);
            goto mark_error;
        }
    }

    // Load calibration once
    if (!bme_calib.loaded) {
        ret = bme280_load_calibration(addr);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "BME280 calibration read failed");
            goto mark_error;
        }
    }
    
    // BME280: Write ctrl_hum register (0xF2) FIRST - must be written before ctrl_meas
    // osrs_h=x1 (001) for humidity oversampling
    uint8_t ctrl_hum_cmd[2] = {0xF2, 0x01};
    ret = remote_i2c_write(0, addr, ctrl_hum_cmd, 2);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "BME280 ctrl_hum write failed");
        goto mark_error;
    }
    
    // BME280: Write ctrl_meas register (0xF4) to trigger forced mode measurement
    // Forced mode: osrs_t=x1, osrs_p=x1, mode=01
    // Register 0xF4: [osrs_t(2:0), osrs_p(2:0), mode(1:0)] = 0b001_001_01 = 0x25
    // Note: ctrl_hum changes only take effect after writing ctrl_meas
    uint8_t ctrl_cmd[2] = {0xF4, 0x25};
    ret = remote_i2c_write(0, addr, ctrl_cmd, 2);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "BME280 ctrl_meas write failed");
        goto mark_error;
    }
    
    // Wait for measurement (typ 7ms for minimal oversampling)
    vTaskDelay(pdMS_TO_TICKS(10));
    
    // Read data registers 0xF7-0xFE (8 bytes: press + temp + hum)
    uint8_t data_reg = 0xF7;
    uint8_t data[8];
    ret = remote_i2c_write_read(0, addr, &data_reg, 1, data, 8);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "BME280 data read failed");
        goto mark_error;
    }
    
    // Extract raw ADC values (20-bit for temp/press, 16-bit for humidity)
    int32_t adc_T = ((int32_t)data[3] << 12) | ((int32_t)data[4] << 4) | ((int32_t)data[5] >> 4);
    int32_t adc_H = ((int32_t)data[6] << 8) | (int32_t)data[7];
    
    // Check for invalid data (0x80000 means sensor not ready or in sleep)
    if (adc_T == 0x80000 || adc_T == 0 || adc_H == 0) {
        ESP_LOGW(TAG, "BME280 invalid data: T=0x%X, H=0x%X", adc_T, adc_H);
        goto mark_error;
    }
    
    int32_t t_fine = 0;
    float temperature = bme280_compensate_temperature(adc_T, &t_fine);
    float humidity = bme280_compensate_humidity(adc_H, t_fine);
    
    if (xSemaphoreTake(sensor_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        // BME280 operating range; humidity is already clamped to 0-100 by the compensation
        if (plausible_temp(SENSOR_CABIN_TEMP, temperature, -40.0f, 85.0f)) {
            sensor_data[SENSOR_CABIN_TEMP].value = temperature;
            sensor_data[SENSOR_CABIN_TEMP].status = SENSOR_STATUS_OK;
            sensor_data[SENSOR_CABIN_TEMP].last_update_ms = get_time_ms();

            sensor_data[SENSOR_CABIN_HUMIDITY].value = humidity;
            sensor_data[SENSOR_CABIN_HUMIDITY].status = SENSOR_STATUS_OK;
            sensor_data[SENSOR_CABIN_HUMIDITY].last_update_ms = get_time_ms();

            ESP_LOGI(TAG, "BME280: %.1fC, %.1f%%RH (raw: T=%d, H=%d)",
                     temperature, humidity, adc_T, adc_H);
        } else if (sensor_data[SENSOR_CABIN_TEMP].status == SENSOR_STATUS_ERROR) {
            sensor_data[SENSOR_CABIN_HUMIDITY].status = SENSOR_STATUS_ERROR;
        }
        xSemaphoreGive(sensor_mutex);
    }
    return;
    
mark_error:
    if (xSemaphoreTake(sensor_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        sensor_data[SENSOR_CABIN_TEMP].status = grace ? SENSOR_STATUS_NOT_FOUND : SENSOR_STATUS_ERROR;
        sensor_data[SENSOR_CABIN_HUMIDITY].status = grace ? SENSOR_STATUS_NOT_FOUND : SENSOR_STATUS_ERROR;
        xSemaphoreGive(sensor_mutex);
    }
}

static void read_remote_temperature_sensors(void)
{
    // DS18B20s on the node's 1-Wire bus (duct, and coolant/outside by role)
    read_remote_ds18b20();
    
    // Read ambient (cabin) temp/humidity via BME280 I2C
    read_remote_bme280();
}

#define VALVE_FEEDBACK_FAIL_LIMIT   3   // consecutive failed/good reads to declare feedback lost/back

static void read_valve_positions(void)
{
    // ADS channel per valve, indexed by hvac_valve_id_t
    static const ads1115_channel_t channels[HVAC_VALVE_MAX] = {
        ADS1115_CH_CENTER_VALVE_1_POS, ADS1115_CH_CENTER_VALVE_2_POS, ADS1115_CH_FOOT_VALVE_POS,
        ADS1115_CH_WINDSCREEN_VALVE_POS, ADS1115_CH_COOLANT_VALVE_POS,
    };
    static uint8_t fail_streak[HVAC_VALVE_MAX];
    static uint8_t ok_streak[HVAC_VALVE_MAX];

    for (int i = 0; i < HVAC_VALVE_MAX; i++) {
        uint8_t pos;
        if (ads1115_read_valve_position(channels[i], &pos) != ESP_OK) {
            // Keep the last good value: a lost reply is not "position 0"
            ok_streak[i] = 0;
            if (fail_streak[i] < VALVE_FEEDBACK_FAIL_LIMIT && ++fail_streak[i] == VALVE_FEEDBACK_FAIL_LIMIT) {
                tb6612fng_set_feedback_fault((hvac_valve_id_t)i, true);
            }
            continue;
        }
        fail_streak[i] = 0;
        if (ok_streak[i] < VALVE_FEEDBACK_FAIL_LIMIT && ++ok_streak[i] == VALVE_FEEDBACK_FAIL_LIMIT) {
            tb6612fng_set_feedback_fault((hvac_valve_id_t)i, false);
        }
        tb6612fng_update_feedback((hvac_valve_id_t)i, pos);
    }
}

/*******************************************************************************
 * Public Functions
 ******************************************************************************/

esp_err_t hvac_sensors_init(i2c_master_bus_handle_t i2c_bus)
{
    ESP_LOGI(TAG, "Initializing HVAC sensors");
    
    // Create mutexes
    sensor_mutex = xSemaphoreCreateMutex();
    if (sensor_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create sensor mutex");
        return ESP_ERR_NO_MEM;
    }
    
    // Initialize sensor data
    for (int i = 0; i < SENSOR_MAX; i++) {
        sensor_data[i].value = NAN;
        sensor_data[i].status = SENSOR_STATUS_NOT_FOUND;
        sensor_data[i].last_update_ms = 0;
    }
    
    ESP_LOGI(TAG, "Remote IO mode - skipping local sensor initialization");
    roles_load();
    
    ESP_LOGI(TAG, "HVAC sensors initialization complete");
    return ESP_OK;
}

void hvac_sensors_deinit(void)
{
    if (sensor_mutex) {
        vSemaphoreDelete(sensor_mutex);
        sensor_mutex = NULL;
    }
}

esp_err_t hvac_sensors_read_all(void)
{
    read_remote_temperature_sensors();
    read_valve_positions();
    
    // Return OK if at least one sensor is working
    for (int i = 0; i < SENSOR_MAX; i++) {
        if (sensor_data[i].status == SENSOR_STATUS_OK) {
            return ESP_OK;
        }
    }
    
    return ESP_FAIL;
}

// Status with staleness applied; caller holds sensor_mutex
static sensor_status_t fresh_status(hvac_sensor_id_t id)
{
    const sensor_data_t *s = &sensor_data[id];
    if (s->status == SENSOR_STATUS_OK && get_time_ms() - s->last_update_ms > SENSOR_STALE_MS) {
        return SENSOR_STATUS_TIMEOUT;
    }
    return s->status;
}

// Value only while the sensor is OK and fresh, else NAN; caller holds sensor_mutex
static float fresh_value(hvac_sensor_id_t id)
{
    return fresh_status(id) == SENSOR_STATUS_OK ? sensor_data[id].value : NAN;
}

float hvac_sensors_get_value(hvac_sensor_id_t sensor_id)
{
    if (sensor_id >= SENSOR_MAX) {
        return NAN;
    }

    float value = NAN;
    if (xSemaphoreTake(sensor_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        value = fresh_value(sensor_id);
        xSemaphoreGive(sensor_mutex);
    }
    return value;
}

sensor_status_t hvac_sensors_get_status(hvac_sensor_id_t sensor_id)
{
    if (sensor_id >= SENSOR_MAX) {
        return SENSOR_STATUS_ERROR;
    }
    
    sensor_status_t status = SENSOR_STATUS_ERROR;
    if (xSemaphoreTake(sensor_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        status = sensor_data[sensor_id].status;
        xSemaphoreGive(sensor_mutex);
    }
    return status;
}

void hvac_sensors_get_all(hvac_sensor_readings_t *readings)
{
    if (readings == NULL) {
        return;
    }
    
    // A failed or stale sensor reads as NAN, not as its last good value
    readings->cabin_temp_c = NAN;
    readings->cabin_humidity = NAN;
    readings->duct_temp_c = NAN;
    readings->cabin_status = SENSOR_STATUS_TIMEOUT;
    readings->duct_status = SENSOR_STATUS_TIMEOUT;
    if (xSemaphoreTake(sensor_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        readings->cabin_temp_c = fresh_value(SENSOR_CABIN_TEMP);
        readings->cabin_humidity = fresh_value(SENSOR_CABIN_HUMIDITY);
        readings->duct_temp_c = fresh_value(SENSOR_DUCT_TEMP);

        readings->cabin_status = fresh_status(SENSOR_CABIN_TEMP);
        readings->duct_status = fresh_status(SENSOR_DUCT_TEMP);
        xSemaphoreGive(sensor_mutex);
    }
    
    // Open % from the valve driver: the same unit as the targets (finding 4). The raw pot
    // reading of a butterfly valve is not a position: 0-100 % of the pot is a full turn.
    readings->center_valve_pos = tb6612fng_get_valve_position(HVAC_VALVE_CENTER_1);
    readings->center2_valve_pos = tb6612fng_get_valve_position(HVAC_VALVE_CENTER_2);
    readings->foot_valve_pos = tb6612fng_get_valve_position(HVAC_VALVE_FOOT);
    readings->windscreen_valve_pos = tb6612fng_get_valve_position(HVAC_VALVE_WINDSCREEN);
    readings->coolant_valve_pos = tb6612fng_get_valve_position(HVAC_VALVE_COOLANT);
}

float hvac_sensors_get_cabin_temp(void)
{
    return hvac_sensors_get_value(SENSOR_CABIN_TEMP);
}

float hvac_sensors_get_cabin_humidity(void)
{
    return hvac_sensors_get_value(SENSOR_CABIN_HUMIDITY);
}

float hvac_sensors_get_duct_temp(void)
{
    return hvac_sensors_get_value(SENSOR_DUCT_TEMP);
}

bool hvac_sensors_all_healthy(void)
{
    bool healthy = true;
    if (xSemaphoreTake(sensor_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        for (int i = 0; i < SENSOR_MAX; i++) {
            if (sensor_data[i].status != SENSOR_STATUS_OK) {
                healthy = false;
                break;
            }
        }
        xSemaphoreGive(sensor_mutex);
    }
    return healthy;
}

void hvac_sensors_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Sensor reading task started");

    // Valve positions every loop (100 ms, 25 ms while calibrating); temperatures by the
    // clock, not by counting loops, so their cadence no longer depends on the mode
    uint32_t last_temps_ms = 0;
    const uint32_t TEMP_READ_INTERVAL_MS = 1000;

    uint32_t last_node_poll_ms = get_time_ms();
    esp_task_wdt_add(NULL);

    while (1) {
        esp_task_wdt_reset();
        // Node uptime and reset reason: tells a node restart apart from lost replies
        if (get_time_ms() - last_node_poll_ms >= 10000) {
            last_node_poll_ms = get_time_ms();
            remote_io_poll_node();
        }
        read_valve_positions();

        if (get_time_ms() - last_temps_ms >= TEMP_READ_INTERVAL_MS) {
            last_temps_ms = get_time_ms();
            read_remote_temperature_sensors();
        }

        vTaskDelay(pdMS_TO_TICKS(calibration_mode ? CALIB_SENSOR_INTERVAL_MS : sensor_interval_ms));
    }
}

esp_err_t hvac_sensors_start_task(void)
{
    BaseType_t ret = xTaskCreate(
        hvac_sensors_task,
        "sensors",
        4096,
        NULL,
        4,
        &sensor_task_handle
    );
    
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create sensor task");
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

uint32_t hvac_sensors_get_interval_ms(void)
{
    return sensor_interval_ms;
}

void hvac_sensors_set_interval_ms(uint32_t interval_ms)
{
    if (interval_ms < 100) {
        interval_ms = 100;
    }
    sensor_interval_ms = interval_ms;
}

void hvac_sensors_set_calibration_mode(bool enabled)
{
    calibration_mode = enabled;
    if (enabled) {
        ESP_LOGI(TAG, "Calibration mode enabled - fast valve position reads");
    } else {
        ESP_LOGI(TAG, "Calibration mode disabled - normal sensor interval");
    }
}

