/**
 * @file hvac_sensors.h
 * @brief HVAC Sensor Management Module
 * 
 * Manages temperature and humidity sensors:
 * - SHT30-D: Cabin temperature and humidity (I2C)
 * - DS18B20 #1: Coolant temperature (1-Wire)
 * - DS18B20 #2: Duct air temperature (1-Wire)
 */

#ifndef HVAC_SENSORS_H
#define HVAC_SENSORS_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*******************************************************************************
 * Type Definitions
 ******************************************************************************/

/**
 * @brief Sensor identifier
 */
typedef enum {
    SENSOR_CABIN_TEMP = 0,      // SHT30-D temperature
    SENSOR_CABIN_HUMIDITY,      // SHT30-D humidity
    SENSOR_COOLANT_TEMP,        // DS18B20 coolant
    SENSOR_DUCT_TEMP,           // DS18B20 duct air
    SENSOR_MAX
} hvac_sensor_id_t;

/**
 * @brief Sensor status
 */
typedef enum {
    SENSOR_STATUS_OK = 0,
    SENSOR_STATUS_ERROR,
    SENSOR_STATUS_NOT_FOUND,
    SENSOR_STATUS_TIMEOUT,
    SENSOR_STATUS_CRC_ERROR,
} sensor_status_t;

/**
 * @brief Sensor data structure
 */
typedef struct {
    float value;                // Sensor value (°C or %RH)
    sensor_status_t status;     // Sensor status
    uint32_t last_update_ms;    // Last successful update timestamp
} sensor_data_t;

/**
 * @brief All sensor readings structure
 */
typedef struct {
    float cabin_temp_c;         // Cabin temperature in °C
    float cabin_humidity;       // Cabin humidity in %RH
    float coolant_temp_c;       // Coolant temperature in °C
    float duct_temp_c;          // Duct air temperature in °C
    uint8_t center_valve_pos;   // Center valve position (0-100%)
    uint8_t foot_valve_pos;     // Foot valve position (0-100%)
    uint8_t windscreen_valve_pos;  // Windscreen valve position (0-100%)
    uint8_t coolant_valve_pos;  // Coolant control valve position (0-100%)
    sensor_status_t cabin_status;
    sensor_status_t coolant_status;
    sensor_status_t duct_status;
} hvac_sensor_readings_t;

/*******************************************************************************
 * Function Declarations
 ******************************************************************************/

/**
 * @brief Initialize all HVAC sensors
 * @return ESP_OK on success
 */
esp_err_t hvac_sensors_init(void);

/**
 * @brief Deinitialize all sensors
 */
void hvac_sensors_deinit(void);

/**
 * @brief Read all sensors (call periodically)
 * @return ESP_OK if at least one sensor read successfully
 */
esp_err_t hvac_sensors_read_all(void);

/**
 * @brief Get specific sensor value
 * @param sensor_id Sensor identifier
 * @return Sensor value (NAN if error)
 */
float hvac_sensors_get_value(hvac_sensor_id_t sensor_id);

/**
 * @brief Get specific sensor status
 * @param sensor_id Sensor identifier
 * @return Sensor status
 */
sensor_status_t hvac_sensors_get_status(hvac_sensor_id_t sensor_id);

/**
 * @brief Get all sensor readings
 * @param readings Pointer to readings structure
 */
void hvac_sensors_get_all(hvac_sensor_readings_t *readings);

/**
 * @brief Get cabin temperature
 * @return Temperature in °C (NAN if error)
 */
float hvac_sensors_get_cabin_temp(void);

/**
 * @brief Get cabin humidity
 * @return Humidity in %RH (NAN if error)
 */
float hvac_sensors_get_cabin_humidity(void);

/**
 * @brief Get coolant temperature
 * @return Temperature in °C (NAN if error)
 */
float hvac_sensors_get_coolant_temp(void);

/**
 * @brief Get duct air temperature
 * @return Temperature in °C (NAN if error)
 */
float hvac_sensors_get_duct_temp(void);

/**
 * @brief Get center valve position feedback
 * @return Position in percent (0-100)
 */
uint8_t hvac_sensors_get_center_valve_pos(void);

/**
 * @brief Get foot valve position feedback
 * @return Position in percent (0-100)
 */
uint8_t hvac_sensors_get_foot_valve_pos(void);

/**
 * @brief Get windscreen valve position feedback
 * @return Position in percent (0-100)
 */
uint8_t hvac_sensors_get_windscreen_valve_pos(void);

/**
 * @brief Get coolant control valve position feedback
 * @return Position in percent (0-100)
 */
uint8_t hvac_sensors_get_coolant_valve_pos(void);

/**
 * @brief Check if sensors are healthy
 * @return true if all sensors are working
 */
bool hvac_sensors_all_healthy(void);

/**
 * @brief Sensor reading task
 * @param pvParameters Task parameters (not used)
 */
void hvac_sensors_task(void *pvParameters);

/**
 * @brief Start sensor reading task
 * @return ESP_OK on success
 */
esp_err_t hvac_sensors_start_task(void);

/**
 * @brief Get sensor reading interval
 * @return Reading interval in milliseconds
 */
uint32_t hvac_sensors_get_interval_ms(void);

/**
 * @brief Set sensor reading interval
 * @param interval_ms Interval in milliseconds (min 100ms)
 */
void hvac_sensors_set_interval_ms(uint32_t interval_ms);

#ifdef __cplusplus
}
#endif

#endif // HVAC_SENSORS_H
