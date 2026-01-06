/**
 * @file ads1115_driver.h
 * @brief ADS1115 16-bit ADC driver for valve position monitoring
 * 
 * Provides interface for reading analog feedback from valve position sensors
 * via two ADS1115 modules on I2C.
 */

#ifndef ADS1115_DRIVER_H
#define ADS1115_DRIVER_H

#include "esp_err.h"
#include "driver/i2c_master.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief ADC channel identifiers
 */
typedef enum {
    // ADS1115 #1 (Address 0x48)
    ADS1115_CH_CENTER_VALVE_1_POS = 0,      // Channel A0
    ADS1115_CH_CENTER_VALVE_2_POS = 1,      // Channel A1
    ADS1115_CH_FOOT_VALVE_POS     = 2,      // Channel A2
    ADS1115_CH_WINDSCREEN_VALVE_POS = 3,    // Channel A3
    
    // ADS1115 #2 (Address 0x49)
    ADS1115_CH_COOLANT_VALVE_POS  = 4,      // Channel A0 on second device
    ADS1115_CH_SPARE_1             = 5,      // Channel A1 on second device
    ADS1115_CH_SPARE_2             = 6,      // Channel A2 on second device
    ADS1115_CH_SPARE_3             = 7,      // Channel A3 on second device
    
    ADS1115_CH_MAX                 = 8
} ads1115_channel_t;

/**
 * @brief ADC raw value (16-bit signed)
 */
typedef int16_t ads1115_raw_t;

/**
 * @brief ADC voltage value (millivolts)
 */
typedef uint16_t ads1115_mv_t;

/**
 * @brief Valve position feedback (0-100%)
 */
typedef uint8_t valve_position_percent_t;

/**
 * @brief Initialize ADS1115 driver
 * 
 * @param i2c_bus I2C bus handle (shared I2C_NUM_1 bus with SHT30)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t ads1115_init(i2c_master_bus_handle_t i2c_bus);

/**
 * @brief Read raw ADC value from a channel
 * 
 * @param channel Channel to read (ADS1115_CH_*)
 * @param[out] value Raw 16-bit ADC value
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t ads1115_read_raw(ads1115_channel_t channel, ads1115_raw_t *value);

/**
 * @brief Read ADC voltage in millivolts from a channel
 * 
 * @param channel Channel to read
 * @param[out] voltage Voltage in millivolts (0-3300 mV for 3.3V range)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t ads1115_read_mv(ads1115_channel_t channel, ads1115_mv_t *voltage);

/**
 * @brief Convert ADC voltage to valve position percentage
 * 
 * Assumes 0-3.3V maps to 0-100% position
 * 
 * @param voltage ADC voltage in millivolts
 * @return Valve position as percentage (0-100)
 */
valve_position_percent_t ads1115_voltage_to_position(ads1115_mv_t voltage);

/**
 * @brief Read valve position feedback directly
 * 
 * @param channel Channel to read
 * @param[out] position Valve position as percentage (0-100)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t ads1115_read_valve_position(ads1115_channel_t channel, 
                                      valve_position_percent_t *position);

/**
 * @brief Deinitialize ADS1115 driver
 * 
 * @return ESP_OK on success
 */
esp_err_t ads1115_deinit(void);

#ifdef __cplusplus
}
#endif

#endif // ADS1115_DRIVER_H
