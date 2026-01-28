/**
 * @file hvac_sensors.c
 * @brief HVAC Sensor Management Implementation
 * 
 * Uses ESP-IDF managed components:
 * - BME280 for cabin temperature and humidity (via remote I2C)
 * - espressif/ds18b20 and espressif/onewire_bus for DS18B20 duct sensor
 * - ADS1115 driver for valve position feedback via analog inputs
 */

#include "hvac_sensors.h"
#include "hvac_hw_config.h"
#include "ads1115_driver.h"
#include "tb6612fng_driver.h"
#include "hvac_ui.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <math.h>
#include <string.h>

#if !USE_REMOTE_IO
#include "driver/i2c_master.h"
// DS18B20 library from espressif components
#include "onewire_bus.h"
#include "ds18b20.h"
#else
// Remote IO for sensor reading via UART
#include "remote_io.h"
#endif

static const char *TAG = "HVAC_SENSORS";

/*******************************************************************************
 * SHT30 Register Definitions (using new I2C master API)
 ******************************************************************************/
#define SHT30_CMD_MEASURE_HIGH_REP      0x2400  // High repeatability, no clock stretch
#define SHT30_CMD_SOFT_RESET            0x30A2
#define SHT30_CMD_READ_STATUS           0xF32D

#if !USE_REMOTE_IO
// Store the sensor I2C bus handle
static i2c_master_bus_handle_t sensor_i2c_bus_handle = NULL;
static i2c_master_dev_handle_t sht30_dev_handle = NULL;
static bool sht30_initialized = false;
static onewire_bus_handle_t onewire_bus = NULL;
static ds18b20_device_handle_t ds18b20_duct = NULL;
#endif

// Sensor data storage
static sensor_data_t sensor_data[SENSOR_MAX];
static SemaphoreHandle_t sensor_mutex = NULL;
static TaskHandle_t sensor_task_handle = NULL;

// Valve position storage (5 valves: center1, center2, foot, windscreen, coolant)
static uint8_t valve_positions[5] = {0, 0, 0, 0, 0};
static SemaphoreHandle_t valve_mutex = NULL;

// Configuration
static uint32_t sensor_interval_ms = 100;   // Default 100ms for responsive valve control
static bool calibration_mode = false;       // Fast valve reading during calibration
#define CALIB_SENSOR_INTERVAL_MS 25         // 25ms during calibration

/*******************************************************************************
 * Private Functions
 ******************************************************************************/

static uint32_t get_time_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

#if !USE_REMOTE_IO

// CRC-8 calculation for SHT30
static uint8_t sht30_crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 0x80) {
                crc = (crc << 1) ^ 0x31;
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

static esp_err_t init_sht30(void)
{
    ESP_LOGI(TAG, "Initializing SHT30-D sensor (new I2C master API)");
    
    if (sensor_i2c_bus_handle == NULL) {
        ESP_LOGE(TAG, "Sensor I2C bus not initialized");
        sensor_data[SENSOR_CABIN_TEMP].status = SENSOR_STATUS_NOT_FOUND;
        sensor_data[SENSOR_CABIN_HUMIDITY].status = SENSOR_STATUS_NOT_FOUND;
        return ESP_ERR_INVALID_STATE;
    }
    
    // Add SHT30 device to the I2C bus
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = SHT30_I2C_ADDR,
        .scl_speed_hz = SHT30_I2C_SPEED_HZ,
    };
    
    esp_err_t ret = i2c_master_bus_add_device(sensor_i2c_bus_handle, &dev_cfg, &sht30_dev_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add SHT30 to I2C bus: %s", esp_err_to_name(ret));
        sensor_data[SENSOR_CABIN_TEMP].status = SENSOR_STATUS_NOT_FOUND;
        sensor_data[SENSOR_CABIN_HUMIDITY].status = SENSOR_STATUS_NOT_FOUND;
        return ret;
    }
    
    // Send soft reset command
    uint8_t cmd[2] = {(SHT30_CMD_SOFT_RESET >> 8) & 0xFF, SHT30_CMD_SOFT_RESET & 0xFF};
    ret = i2c_master_transmit(sht30_dev_handle, cmd, 2, 100);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SHT30 soft reset failed (sensor may not be connected): %s", esp_err_to_name(ret));
        sensor_data[SENSOR_CABIN_TEMP].status = SENSOR_STATUS_NOT_FOUND;
        sensor_data[SENSOR_CABIN_HUMIDITY].status = SENSOR_STATUS_NOT_FOUND;
        return ret;
    }
    
    vTaskDelay(pdMS_TO_TICKS(20));  // Wait for reset
    
    sht30_initialized = true;
    ESP_LOGI(TAG, "SHT30-D initialized successfully");
    return ESP_OK;
}

static esp_err_t init_ds18b20_sensors(void)
{
    ESP_LOGI(TAG, "Initializing DS18B20 duct sensor on 1-Wire bus (GPIO %d)", DS18B20_ONEWIRE_PIN);
    esp_err_t ret;
    
    // Initialize 1-Wire bus for DS18B20 duct sensor
    onewire_bus_config_t bus_config = {
        .bus_gpio_num = DS18B20_ONEWIRE_PIN,
    };
    onewire_bus_rmt_config_t rmt_config = {
        .max_rx_bytes = 10,
    };
    
    ret = onewire_new_bus_rmt(&bus_config, &rmt_config, &onewire_bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create 1-Wire bus: %s", esp_err_to_name(ret));
        sensor_data[SENSOR_DUCT_TEMP].status = SENSOR_STATUS_NOT_FOUND;
        return ret;
    }
    
    // Search for DS18B20 device on the bus
    onewire_device_iter_handle_t iter = NULL;
    ret = onewire_new_device_iter(onewire_bus, &iter);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create device iterator");
        return ret;
    }
    
    onewire_device_t device;
    ret = onewire_device_iter_get_next(iter, &device);
    onewire_del_device_iter(iter);
    
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "No DS18B20 found for duct sensor");
        sensor_data[SENSOR_DUCT_TEMP].status = SENSOR_STATUS_NOT_FOUND;
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Found 1-Wire device: %016llX", device.address);
    
    // Initialize duct temperature sensor
    ds18b20_config_t ds_config = {};
    ret = ds18b20_new_device_from_enumeration(&device, &ds_config, &ds18b20_duct);
    if (ret == ESP_OK) {
        ds18b20_set_resolution(ds18b20_duct, DS18B20_RESOLUTION_12B);
        ESP_LOGI(TAG, "DS18B20 duct sensor initialized (address: %016llX)", device.address);
    } else {
        ESP_LOGE(TAG, "Failed to create DS18B20 duct device");
        sensor_data[SENSOR_DUCT_TEMP].status = SENSOR_STATUS_NOT_FOUND;
    }
    
    return ESP_OK;
}

static void read_sht30(void)
{
    if (!sht30_initialized || sht30_dev_handle == NULL) {
        return;
    }
    
    esp_err_t ret;
    float temperature = NAN, humidity = NAN;
    
    // Send measurement command (high repeatability, no clock stretching)
    uint8_t cmd[2] = {(SHT30_CMD_MEASURE_HIGH_REP >> 8) & 0xFF, SHT30_CMD_MEASURE_HIGH_REP & 0xFF};
    ret = i2c_master_transmit(sht30_dev_handle, cmd, 2, 100);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SHT30 measure command failed: %s", esp_err_to_name(ret));
        goto update_status;
    }
    
    // Wait for measurement (max 15ms for high repeatability)
    vTaskDelay(pdMS_TO_TICKS(20));
    
    // Read 6 bytes: temp_msb, temp_lsb, temp_crc, hum_msb, hum_lsb, hum_crc
    uint8_t data[6];
    ret = i2c_master_receive(sht30_dev_handle, data, 6, 100);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SHT30 read failed: %s", esp_err_to_name(ret));
        goto update_status;
    }
    
    // Verify CRC for temperature
    if (sht30_crc8(data, 2) != data[2]) {
        ESP_LOGW(TAG, "SHT30 temperature CRC error");
        ret = ESP_ERR_INVALID_CRC;
        goto update_status;
    }
    
    // Verify CRC for humidity
    if (sht30_crc8(data + 3, 2) != data[5]) {
        ESP_LOGW(TAG, "SHT30 humidity CRC error");
        ret = ESP_ERR_INVALID_CRC;
        goto update_status;
    }
    
    // Convert raw values to temperature and humidity
    uint16_t temp_raw = (data[0] << 8) | data[1];
    uint16_t hum_raw = (data[3] << 8) | data[4];
    
    temperature = -45.0f + 175.0f * ((float)temp_raw / 65535.0f);
    humidity = 100.0f * ((float)hum_raw / 65535.0f);

update_status:
    if (xSemaphoreTake(sensor_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (ret == ESP_OK) {
            sensor_data[SENSOR_CABIN_TEMP].value = temperature;
            sensor_data[SENSOR_CABIN_TEMP].status = SENSOR_STATUS_OK;
            sensor_data[SENSOR_CABIN_TEMP].last_update_ms = get_time_ms();
            
            sensor_data[SENSOR_CABIN_HUMIDITY].value = humidity;
            sensor_data[SENSOR_CABIN_HUMIDITY].status = SENSOR_STATUS_OK;
            sensor_data[SENSOR_CABIN_HUMIDITY].last_update_ms = get_time_ms();
            
            ESP_LOGD(TAG, "SHT30: %.1f°C, %.1f%%RH", temperature, humidity);
        } else {
            sensor_data[SENSOR_CABIN_TEMP].status = SENSOR_STATUS_ERROR;
            sensor_data[SENSOR_CABIN_HUMIDITY].status = SENSOR_STATUS_ERROR;
        }
        xSemaphoreGive(sensor_mutex);
    }
}

static void read_ds18b20_duct(void)
{
    if (ds18b20_duct == NULL) {
        return;
    }
    
    float temperature;
    esp_err_t ret = ds18b20_trigger_temperature_conversion(ds18b20_duct);
    if (ret == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(100));
        ret = ds18b20_get_temperature(ds18b20_duct, &temperature);
    }
    
    if (xSemaphoreTake(sensor_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (ret == ESP_OK) {
            sensor_data[SENSOR_DUCT_TEMP].value = temperature;
            sensor_data[SENSOR_DUCT_TEMP].status = SENSOR_STATUS_OK;
            sensor_data[SENSOR_DUCT_TEMP].last_update_ms = get_time_ms();
            ESP_LOGD(TAG, "Duct temp: %.1f°C", temperature);
        } else {
            sensor_data[SENSOR_DUCT_TEMP].status = SENSOR_STATUS_ERROR;
            ESP_LOGW(TAG, "DS18B20 duct read error");
        }
        xSemaphoreGive(sensor_mutex);
    }
}

#endif // !USE_REMOTE_IO

#if USE_REMOTE_IO
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

static void read_remote_ds18b20_duct(void)
{
    float temperature;
    esp_err_t ret = remote_onewire_read_ds18b20(DS18B20_ONEWIRE_PIN, NULL, &temperature);
    bool grace = (get_time_ms() < 5000);
    
    if (xSemaphoreTake(sensor_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (ret == ESP_OK) {
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
        sensor_data[SENSOR_CABIN_TEMP].value = temperature;
        sensor_data[SENSOR_CABIN_TEMP].status = SENSOR_STATUS_OK;
        sensor_data[SENSOR_CABIN_TEMP].last_update_ms = get_time_ms();
        
        sensor_data[SENSOR_CABIN_HUMIDITY].value = humidity;
        sensor_data[SENSOR_CABIN_HUMIDITY].status = SENSOR_STATUS_OK;
        sensor_data[SENSOR_CABIN_HUMIDITY].last_update_ms = get_time_ms();
        
        ESP_LOGI(TAG, "BME280: %.1fC, %.1f%%RH (raw: T=%d, H=%d)", 
                 temperature, humidity, adc_T, adc_H);
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
    // Read duct temperature via DS18B20 (1-Wire)
    read_remote_ds18b20_duct();
    
    // Read ambient (cabin) temp/humidity via BME280 I2C
    read_remote_bme280();
}
#endif

static void read_valve_positions(void)
{
    // Read all valve positions from ADS1115 modules (5 valves total)
    uint8_t positions[5];
    
    // Read center valve 1 position (ADS1115 #1, channel 0)
    if (ads1115_read_valve_position(ADS1115_CH_CENTER_VALVE_1_POS, &positions[0]) != ESP_OK) {
        positions[0] = 0;
    }
    
    // Read center valve 2 position (ADS1115 #1, channel 1)
    if (ads1115_read_valve_position(ADS1115_CH_CENTER_VALVE_2_POS, &positions[1]) != ESP_OK) {
        positions[1] = 0;
    }
    
    // Read foot valve position (ADS1115 #1, channel 2)
    if (ads1115_read_valve_position(ADS1115_CH_FOOT_VALVE_POS, &positions[2]) != ESP_OK) {
        positions[2] = 0;
    }
    
    // Read windscreen valve position (ADS1115 #1, channel 3)
    if (ads1115_read_valve_position(ADS1115_CH_WINDSCREEN_VALVE_POS, &positions[3]) != ESP_OK) {
        positions[3] = 0;
    }
    
    // Read coolant valve position (ADS1115 #2, channel 0)
    if (ads1115_read_valve_position(ADS1115_CH_COOLANT_VALVE_POS, &positions[4]) != ESP_OK) {
        positions[4] = 0;
    }
    
    // Store valve positions with mutex protection
    if (xSemaphoreTake(valve_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        for (int i = 0; i < 5; i++) {
            valve_positions[i] = positions[i];
        }
        xSemaphoreGive(valve_mutex);
    }

    // Update TB6612 driver feedback for each valve
    tb6612fng_update_feedback(HVAC_VALVE_CENTER_1, positions[0]);
    tb6612fng_update_feedback(HVAC_VALVE_CENTER_2, positions[1]);
    tb6612fng_update_feedback(HVAC_VALVE_FOOT, positions[2]);
    tb6612fng_update_feedback(HVAC_VALVE_WINDSCREEN, positions[3]);
    tb6612fng_update_feedback(HVAC_VALVE_COOLANT, positions[4]);
    
    ESP_LOGD(TAG, "Valve positions - C1: %d%%, C2: %d%%, Foot: %d%%, WS: %d%%, Cool: %d%%",
             positions[0], positions[1], positions[2], positions[3], positions[4]);
}

/*******************************************************************************
 * Public Functions
 ******************************************************************************/

esp_err_t hvac_sensors_init(i2c_master_bus_handle_t i2c_bus)
{
    ESP_LOGI(TAG, "Initializing HVAC sensors");
    
#if !USE_REMOTE_IO
    // Store the I2C bus handle for SHT30
    sensor_i2c_bus_handle = i2c_bus;
#endif
    
    // Create mutexes
    sensor_mutex = xSemaphoreCreateMutex();
    if (sensor_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create sensor mutex");
        return ESP_ERR_NO_MEM;
    }
    
    valve_mutex = xSemaphoreCreateMutex();
    if (valve_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create valve mutex");
        return ESP_ERR_NO_MEM;
    }
    
    // Initialize sensor data
    for (int i = 0; i < SENSOR_MAX; i++) {
        sensor_data[i].value = NAN;
        sensor_data[i].status = SENSOR_STATUS_NOT_FOUND;
        sensor_data[i].last_update_ms = 0;
    }
    
#if !USE_REMOTE_IO
    // Initialize SHT30-D using new I2C master API
    init_sht30();
    
    // Initialize DS18B20 sensors
    init_ds18b20_sensors();
#else
    ESP_LOGI(TAG, "Remote IO mode - skipping local sensor initialization");
#endif
    
    ESP_LOGI(TAG, "HVAC sensors initialization complete");
    return ESP_OK;
}

void hvac_sensors_deinit(void)
{
#if !USE_REMOTE_IO
    if (ds18b20_coolant) {
        ds18b20_del_device(ds18b20_coolant);
        ds18b20_coolant = NULL;
    }
    if (ds18b20_duct) {
        ds18b20_del_device(ds18b20_duct);
        ds18b20_duct = NULL;
    }
    if (onewire_bus) {
        onewire_bus_del(onewire_bus);
        onewire_bus = NULL;
    }
    if (sht30_dev_handle) {
        i2c_master_bus_rm_device(sht30_dev_handle);
        sht30_dev_handle = NULL;
        sht30_initialized = false;
    }
#endif
    if (sensor_mutex) {
        vSemaphoreDelete(sensor_mutex);
        sensor_mutex = NULL;
    }
    if (valve_mutex) {
        vSemaphoreDelete(valve_mutex);
        valve_mutex = NULL;
    }
}

esp_err_t hvac_sensors_read_all(void)
{
#if !USE_REMOTE_IO
    read_sht30();
    read_ds18b20_coolant();
    read_ds18b20_duct();
#else
    read_remote_temperature_sensors();
#endif
    read_valve_positions();
    
    // Return OK if at least one sensor is working
    for (int i = 0; i < SENSOR_MAX; i++) {
        if (sensor_data[i].status == SENSOR_STATUS_OK) {
            return ESP_OK;
        }
    }
    
    return ESP_FAIL;
}

float hvac_sensors_get_value(hvac_sensor_id_t sensor_id)
{
    if (sensor_id >= SENSOR_MAX) {
        return NAN;
    }
    
    float value = NAN;
    if (xSemaphoreTake(sensor_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (sensor_data[sensor_id].status == SENSOR_STATUS_OK) {
            value = sensor_data[sensor_id].value;
        }
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
    
    if (xSemaphoreTake(sensor_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        readings->cabin_temp_c = sensor_data[SENSOR_CABIN_TEMP].value;
        readings->cabin_humidity = sensor_data[SENSOR_CABIN_HUMIDITY].value;
        readings->duct_temp_c = sensor_data[SENSOR_DUCT_TEMP].value;
        
        readings->cabin_status = sensor_data[SENSOR_CABIN_TEMP].status;
        readings->duct_status = sensor_data[SENSOR_DUCT_TEMP].status;
        xSemaphoreGive(sensor_mutex);
    }
    
    // Get valve positions separately with valve_mutex
    // Note: indices are 0=center1, 1=center2, 2=foot, 3=windscreen, 4=coolant
    if (xSemaphoreTake(valve_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        readings->center_valve_pos = valve_positions[0];   // Center 1
        readings->center2_valve_pos = valve_positions[1];  // Center 2
        readings->foot_valve_pos = valve_positions[2];
        readings->windscreen_valve_pos = valve_positions[3];
        readings->coolant_valve_pos = valve_positions[4];
        xSemaphoreGive(valve_mutex);
    }
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

uint8_t hvac_sensors_get_center_valve_pos(void)
{
    uint8_t pos = 0;
    if (xSemaphoreTake(valve_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        pos = valve_positions[0];  // Center 1
        xSemaphoreGive(valve_mutex);
    }
    return pos;
}

uint8_t hvac_sensors_get_center2_valve_pos(void)
{
    uint8_t pos = 0;
    if (xSemaphoreTake(valve_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        pos = valve_positions[1];  // Center 2
        xSemaphoreGive(valve_mutex);
    }
    return pos;
}

uint8_t hvac_sensors_get_foot_valve_pos(void)
{
    uint8_t pos = 0;
    if (xSemaphoreTake(valve_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        pos = valve_positions[2];  // Foot
        xSemaphoreGive(valve_mutex);
    }
    return pos;
}

uint8_t hvac_sensors_get_windscreen_valve_pos(void)
{
    uint8_t pos = 0;
    if (xSemaphoreTake(valve_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        pos = valve_positions[3];  // Windscreen
        xSemaphoreGive(valve_mutex);
    }
    return pos;
}

uint8_t hvac_sensors_get_coolant_valve_pos(void)
{
    uint8_t pos = 0;
    if (xSemaphoreTake(valve_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        pos = valve_positions[4];  // Coolant
        xSemaphoreGive(valve_mutex);
    }
    return pos;
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
    
    // Counter for temperature reads during calibration
    // Read temperatures every ~500ms (20 x 25ms intervals)
    uint8_t calib_temp_counter = 0;
    const uint8_t CALIB_TEMP_READ_INTERVAL = 20;
    
    while (1) {
        if (calibration_mode) {
            // During calibration, read valve positions at high frequency
            read_valve_positions();
            
            // But also read temperatures periodically so UI updates
            calib_temp_counter++;
            if (calib_temp_counter >= CALIB_TEMP_READ_INTERVAL) {
                calib_temp_counter = 0;
#if USE_REMOTE_IO
                read_remote_temperature_sensors();
#else
                read_sht30();
                read_ds18b20_coolant();
                read_ds18b20_duct();
#endif
            }
            
            vTaskDelay(pdMS_TO_TICKS(CALIB_SENSOR_INTERVAL_MS));
        } else {
            calib_temp_counter = 0;  // Reset counter when not calibrating
            hvac_sensors_read_all();
            vTaskDelay(pdMS_TO_TICKS(sensor_interval_ms));
        }
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

esp_err_t hvac_sensors_debug_read_onewire(float *temperature_c)
{
    float temp = NAN;
    esp_err_t ret = ESP_ERR_INVALID_STATE;

#if USE_REMOTE_IO
    ret = remote_onewire_read_ds18b20(DS18B20_ONEWIRE_PIN, NULL, &temp);
#else
    if (ds18b20_duct == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    ret = ds18b20_trigger_temperature_conversion(ds18b20_duct);
    if (ret == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(100));
        ret = ds18b20_get_temperature(ds18b20_duct, &temp);
    }
#endif

    if (ret == ESP_OK && xSemaphoreTake(sensor_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        sensor_data[SENSOR_DUCT_TEMP].value = temp;
        sensor_data[SENSOR_DUCT_TEMP].status = SENSOR_STATUS_OK;
        sensor_data[SENSOR_DUCT_TEMP].last_update_ms = get_time_ms();
        xSemaphoreGive(sensor_mutex);
    }

    if (temperature_c) {
        *temperature_c = temp;
    }
    return ret;
}
