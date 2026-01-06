/**
 * @file hvac_sensors.c
 * @brief HVAC Sensor Management Implementation
 * 
 * Uses ESP-IDF managed components:
 * - esp-idf-lib/sht3x for SHT30-D sensor
 * - espressif/ds18b20 and espressif/onewire_bus for DS18B20 sensors
 * - ADS1115 driver for valve position feedback via analog inputs
 */

#include "hvac_sensors.h"
#include "hvac_hw_config.h"
#include "ads1115_driver.h"
#include "tb6612fng_driver.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <math.h>
#include <string.h>

// SHT3x library from esp-idf-lib/sht3x component
#include "sht3x.h"

// DS18B20 library from espressif components
#include "onewire_bus.h"
#include "ds18b20.h"

static const char *TAG = "HVAC_SENSORS";

/*******************************************************************************
 * Private Data
 ******************************************************************************/

// Sensor handles
static sht3x_t sht30_dev;  // Device descriptor for esp-idf-lib/sht3x
static bool sht30_initialized = false;
static onewire_bus_handle_t onewire_bus = NULL;  // Shared 1-Wire bus for both DS18B20 sensors
static ds18b20_device_handle_t ds18b20_coolant = NULL;
static ds18b20_device_handle_t ds18b20_duct = NULL;

// Sensor data storage
static sensor_data_t sensor_data[SENSOR_MAX];
static SemaphoreHandle_t sensor_mutex = NULL;
static TaskHandle_t sensor_task_handle = NULL;

// Valve position storage
static uint8_t valve_positions[4] = {0, 0, 0, 0};  // center, foot, windscreen, coolant
static SemaphoreHandle_t valve_mutex = NULL;

// Configuration
static uint32_t sensor_interval_ms = 1000;  // Default 1 second

/*******************************************************************************
 * Private Functions
 ******************************************************************************/

static uint32_t get_time_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static esp_err_t init_sht30(void)
{
    ESP_LOGI(TAG, "Initializing SHT30-D sensor");
    
    // Initialize device descriptor with esp-idf-lib API
    esp_err_t ret = sht3x_init_desc(&sht30_dev, SHT30_I2C_ADDR, SHT30_I2C_PORT, 
                                     SHT30_PIN_SDA, SHT30_PIN_SCL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init SHT30 descriptor: %s", esp_err_to_name(ret));
        sensor_data[SENSOR_CABIN_TEMP].status = SENSOR_STATUS_NOT_FOUND;
        sensor_data[SENSOR_CABIN_HUMIDITY].status = SENSOR_STATUS_NOT_FOUND;
        return ret;
    }
    
    // Initialize the sensor
    ret = sht3x_init(&sht30_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SHT30-D: %s", esp_err_to_name(ret));
        sensor_data[SENSOR_CABIN_TEMP].status = SENSOR_STATUS_NOT_FOUND;
        sensor_data[SENSOR_CABIN_HUMIDITY].status = SENSOR_STATUS_NOT_FOUND;
        return ret;
    }
    
    sht30_initialized = true;
    ESP_LOGI(TAG, "SHT30-D initialized successfully");
    return ESP_OK;
}

static esp_err_t init_ds18b20_sensors(void)
{
    ESP_LOGI(TAG, "Initializing DS18B20 sensors on shared 1-Wire bus (GPIO %d)", DS18B20_ONEWIRE_PIN);
    esp_err_t ret;
    
    // Initialize shared 1-Wire bus for both DS18B20 sensors
    onewire_bus_config_t bus_config = {
        .bus_gpio_num = DS18B20_ONEWIRE_PIN,
    };
    onewire_bus_rmt_config_t rmt_config = {
        .max_rx_bytes = 10,
    };
    
    ret = onewire_new_bus_rmt(&bus_config, &rmt_config, &onewire_bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create 1-Wire bus: %s", esp_err_to_name(ret));
        sensor_data[SENSOR_COOLANT_TEMP].status = SENSOR_STATUS_NOT_FOUND;
        sensor_data[SENSOR_DUCT_TEMP].status = SENSOR_STATUS_NOT_FOUND;
        return ret;
    }
    
    // Search for all DS18B20 devices on the shared bus
    onewire_device_iter_handle_t iter = NULL;
    ret = onewire_new_device_iter(onewire_bus, &iter);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create device iterator");
        return ret;
    }
    
    onewire_device_t devices[DS18B20_MAX_SENSORS];
    int device_count = 0;
    
    // Find all devices on the bus
    while (device_count < DS18B20_MAX_SENSORS) {
        ret = onewire_device_iter_get_next(iter, &devices[device_count]);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Found 1-Wire device #%d: %016llX", 
                     device_count + 1, devices[device_count].address);
            device_count++;
        } else {
            break;  // No more devices
        }
    }
    
    onewire_del_device_iter(iter);
    
    ESP_LOGI(TAG, "Found %d DS18B20 sensor(s) on 1-Wire bus", device_count);
    
    // Assign devices to coolant and duct sensors
    // First device found = coolant, second = duct
    ds18b20_config_t ds_config = {};
    
    if (device_count >= 1) {
        ret = ds18b20_new_device_from_enumeration(&devices[0], &ds_config, &ds18b20_coolant);
        if (ret == ESP_OK) {
            ds18b20_set_resolution(ds18b20_coolant, DS18B20_RESOLUTION_12B);
            ESP_LOGI(TAG, "DS18B20 coolant sensor initialized (address: %016llX)", 
                     devices[0].address);
        } else {
            ESP_LOGE(TAG, "Failed to create DS18B20 coolant device");
            sensor_data[SENSOR_COOLANT_TEMP].status = SENSOR_STATUS_NOT_FOUND;
        }
    } else {
        ESP_LOGW(TAG, "No DS18B20 found for coolant sensor");
        sensor_data[SENSOR_COOLANT_TEMP].status = SENSOR_STATUS_NOT_FOUND;
    }
    
    if (device_count >= 2) {
        ret = ds18b20_new_device_from_enumeration(&devices[1], &ds_config, &ds18b20_duct);
        if (ret == ESP_OK) {
            ds18b20_set_resolution(ds18b20_duct, DS18B20_RESOLUTION_12B);
            ESP_LOGI(TAG, "DS18B20 duct sensor initialized (address: %016llX)", 
                     devices[1].address);
        } else {
            ESP_LOGE(TAG, "Failed to create DS18B20 duct device");
            sensor_data[SENSOR_DUCT_TEMP].status = SENSOR_STATUS_NOT_FOUND;
        }
    } else {
        ESP_LOGW(TAG, "No second DS18B20 found for duct sensor");
        sensor_data[SENSOR_DUCT_TEMP].status = SENSOR_STATUS_NOT_FOUND;
    }
    
    return ESP_OK;
}

static void read_sht30(void)
{
    if (!sht30_initialized) {
        return;
    }
    
    float temperature, humidity;
    // Use sht3x_measure which handles everything (start, wait, read)
    esp_err_t ret = sht3x_measure(&sht30_dev, &temperature, &humidity);
    
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
            ESP_LOGW(TAG, "SHT30 read error: %s", esp_err_to_name(ret));
        }
        xSemaphoreGive(sensor_mutex);
    }
}

static void read_ds18b20_coolant(void)
{
    if (ds18b20_coolant == NULL) {
        return;
    }
    
    float temperature;
    esp_err_t ret = ds18b20_trigger_temperature_conversion(ds18b20_coolant);
    if (ret == ESP_OK) {
        // Wait for conversion (750ms max for 12-bit)
        vTaskDelay(pdMS_TO_TICKS(100));
        ret = ds18b20_get_temperature(ds18b20_coolant, &temperature);
    }
    
    if (xSemaphoreTake(sensor_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (ret == ESP_OK) {
            sensor_data[SENSOR_COOLANT_TEMP].value = temperature;
            sensor_data[SENSOR_COOLANT_TEMP].status = SENSOR_STATUS_OK;
            sensor_data[SENSOR_COOLANT_TEMP].last_update_ms = get_time_ms();
            ESP_LOGD(TAG, "Coolant temp: %.1f°C", temperature);
        } else {
            sensor_data[SENSOR_COOLANT_TEMP].status = SENSOR_STATUS_ERROR;
            ESP_LOGW(TAG, "DS18B20 coolant read error");
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

static void read_valve_positions(void)
{
    // Read all valve positions from ADS1115 modules
    uint8_t positions[4];
    
    // Read center valve position (ADS1115 #1, channel 0)
    if (ads1115_read_valve_position(ADS1115_CH_CENTER_VALVE_1_POS, &positions[0]) != ESP_OK) {
        positions[0] = 0;
    }
    
    // Read foot valve position (ADS1115 #1, channel 2)
    if (ads1115_read_valve_position(ADS1115_CH_FOOT_VALVE_POS, &positions[1]) != ESP_OK) {
        positions[1] = 0;
    }
    
    // Read windscreen valve position (ADS1115 #1, channel 3)
    if (ads1115_read_valve_position(ADS1115_CH_WINDSCREEN_VALVE_POS, &positions[2]) != ESP_OK) {
        positions[2] = 0;
    }
    
    // Read coolant valve position (ADS1115 #2, channel 0)
    if (ads1115_read_valve_position(ADS1115_CH_COOLANT_VALVE_POS, &positions[3]) != ESP_OK) {
        positions[3] = 0;
    }
    
    // Store valve positions with mutex protection
    if (xSemaphoreTake(valve_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        for (int i = 0; i < 4; i++) {
            valve_positions[i] = positions[i];
        }
        xSemaphoreGive(valve_mutex);
    }

    tb6612fng_update_feedback(HVAC_VALVE_CENTER_1, positions[0]);
    tb6612fng_update_feedback(HVAC_VALVE_CENTER_2, positions[0]);
    tb6612fng_update_feedback(HVAC_VALVE_FOOT, positions[1]);
    tb6612fng_update_feedback(HVAC_VALVE_WINDSCREEN, positions[2]);
    tb6612fng_update_feedback(HVAC_VALVE_COOLANT, positions[3]);
    
    ESP_LOGD(TAG, "Valve positions - Center: %d%%, Foot: %d%%, Windscreen: %d%%, Coolant: %d%%",
             positions[0], positions[1], positions[2], positions[3]);
}

/*******************************************************************************
 * Public Functions
 ******************************************************************************/

esp_err_t hvac_sensors_init(void)
{
    ESP_LOGI(TAG, "Initializing HVAC sensors");
    
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
    
    // Initialize SHT30-D (esp-idf-lib version creates its own I2C bus)
    init_sht30();
    
    // Initialize DS18B20 sensors
    init_ds18b20_sensors();
    
    ESP_LOGI(TAG, "HVAC sensors initialization complete");
    return ESP_OK;
}

void hvac_sensors_deinit(void)
{
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
    if (sht30_initialized) {
        sht3x_free_desc(&sht30_dev);
        sht30_initialized = false;
    }
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
    read_sht30();
    read_ds18b20_coolant();
    read_ds18b20_duct();
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
        readings->coolant_temp_c = sensor_data[SENSOR_COOLANT_TEMP].value;
        readings->duct_temp_c = sensor_data[SENSOR_DUCT_TEMP].value;
        
        readings->cabin_status = sensor_data[SENSOR_CABIN_TEMP].status;
        readings->coolant_status = sensor_data[SENSOR_COOLANT_TEMP].status;
        readings->duct_status = sensor_data[SENSOR_DUCT_TEMP].status;
        xSemaphoreGive(sensor_mutex);
    }
    
    // Get valve positions separately with valve_mutex
    if (xSemaphoreTake(valve_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        readings->center_valve_pos = valve_positions[0];
        readings->foot_valve_pos = valve_positions[1];
        readings->windscreen_valve_pos = valve_positions[2];
        readings->coolant_valve_pos = valve_positions[3];
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

float hvac_sensors_get_coolant_temp(void)
{
    return hvac_sensors_get_value(SENSOR_COOLANT_TEMP);
}

float hvac_sensors_get_duct_temp(void)
{
    return hvac_sensors_get_value(SENSOR_DUCT_TEMP);
}

uint8_t hvac_sensors_get_center_valve_pos(void)
{
    uint8_t pos = 0;
    if (xSemaphoreTake(valve_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        pos = valve_positions[0];
        xSemaphoreGive(valve_mutex);
    }
    return pos;
}

uint8_t hvac_sensors_get_foot_valve_pos(void)
{
    uint8_t pos = 0;
    if (xSemaphoreTake(valve_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        pos = valve_positions[1];
        xSemaphoreGive(valve_mutex);
    }
    return pos;
}

uint8_t hvac_sensors_get_windscreen_valve_pos(void)
{
    uint8_t pos = 0;
    if (xSemaphoreTake(valve_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        pos = valve_positions[2];
        xSemaphoreGive(valve_mutex);
    }
    return pos;
}

uint8_t hvac_sensors_get_coolant_valve_pos(void)
{
    uint8_t pos = 0;
    if (xSemaphoreTake(valve_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        pos = valve_positions[3];
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
    
    while (1) {
        hvac_sensors_read_all();
        vTaskDelay(pdMS_TO_TICKS(sensor_interval_ms));
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
