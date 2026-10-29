/**
 * @file hvac_controller.h
 * @brief HVAC Controller Logic Module
 * 
 * Coordinates all HVAC operations including:
 * - Temperature control (automatic and manual)
 * - Air distribution (valve control)
 * - Fan speed control
 * - Mode management
 */

#ifndef HVAC_CONTROLLER_H
#define HVAC_CONTROLLER_H

#include "esp_err.h"
#include "hvac_hw_config.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*******************************************************************************
 * Type Definitions
 ******************************************************************************/

/**
 * @brief Temperature unit
 */
typedef enum {
    TEMP_UNIT_CELSIUS = 0,
    TEMP_UNIT_FAHRENHEIT,
} temp_unit_t;

/**
 * @brief HVAC controller state
 */
typedef struct {
    // Mode settings
    hvac_mode_t mode;               // Current operating mode
    hvac_fan_speed_t fan_level;     // Fan speed level
    bool ac_enabled;                // A/C compressor request
    bool recirculate;               // Recirculation mode
    
    // Temperature settings
    float target_temp_c;            // Target cabin temperature (°C)
    temp_unit_t temp_unit;          // Display unit
    
    // Sensor readings (cached)
    float cabin_temp_c;             // Current cabin temperature
    float cabin_humidity;           // Current humidity
    float duct_temp_c;              // Duct outlet temperature
    
    // Valve positions (0-100%)
    uint8_t center_valve_pos;       // Center vent 1 position
    uint8_t center2_valve_pos;      // Center vent 2 position
    uint8_t foot_valve_pos;         // Foot vent position
    uint8_t windscreen_valve_pos;   // Windscreen vent position
    uint8_t coolant_valve_pos;      // Coolant valve position
    
    // Blower
    uint8_t blower_speed;           // Current blower speed (0-100%)
    
    // Status flags
    bool is_heating;                // Currently in heating mode
    bool is_cooling;                // Currently in cooling mode
    bool temp_reached;              // Target temperature reached
    bool sensor_fault;              // True if any sensor reported error
    bool valve_fault;               // True if valve stall detected
    uint8_t valve_fault_mask;       // Bitmask of stalled valves
    
    // A/C compressor state
    bool ac_compressor_on;          // Actual compressor state (may differ from ac_enabled)
} hvac_state_t;

/**
 * @brief HVAC settings structure (for NVS storage)
 */
typedef struct {
    float target_temp_c;
    hvac_mode_t mode;
    hvac_fan_speed_t fan_level;
    bool ac_enabled;
    bool recirculate;
    temp_unit_t temp_unit;
} hvac_settings_t;

/*******************************************************************************
 * Callback Type Definitions
 ******************************************************************************/

/**
 * @brief State change callback function type
 */
typedef void (*hvac_state_callback_t)(const hvac_state_t *state, void *user_data);

/*******************************************************************************
 * Function Declarations
 ******************************************************************************/

/**
 * @brief Initialize HVAC controller
 * @return ESP_OK on success
 */
esp_err_t hvac_controller_init(void);

/**
 * @brief Deinitialize HVAC controller
 */
void hvac_controller_deinit(void);

/**
 * @brief Get current HVAC state
 * @param state Pointer to state structure
 * @return false if the controller held the state for 100 ms (state left untouched)
 */
bool hvac_controller_get_state(hvac_state_t *state);

/** * @brief Get thermal adaptation status
 * 
 * @param heating_rate Pointer to store heating rate (°C/min)
 * @param cooling_rate Pointer to store cooling rate (°C/min)
 * @param adaptive_gain Pointer to store adaptive gain
 * @param sample_count Pointer to store sample count
 * @param calibrated Pointer to store calibration status
 * @return ESP_OK on success
 */
esp_err_t hvac_controller_get_thermal_status(float *heating_rate, float *cooling_rate, 
                                               float *adaptive_gain, uint32_t *sample_count, 
                                               bool *calibrated);

/**
 * @brief Reset thermal adaptation calibration
 * 
 * @return ESP_OK on success
 */
esp_err_t hvac_controller_reset_thermal_cal(void);

/**
 * @brief Force save current thermal calibration
 * 
 * @return ESP_OK on success
 */
esp_err_t hvac_controller_save_thermal_cal(void);

/** * @brief Set operating mode
 * @param mode Operating mode
 * @return ESP_OK on success
 */
esp_err_t hvac_controller_set_mode(hvac_mode_t mode);

/**
 * @brief Get current operating mode
 * @return Current mode
 */
hvac_mode_t hvac_controller_get_mode(void);

/**
 * @brief Set fan speed level
 * @param level Fan speed level
 * @return ESP_OK on success
 */
esp_err_t hvac_controller_set_fan_level(hvac_fan_speed_t level);

/**
 * @brief Get current fan level
 * @return Current fan level
 */
hvac_fan_speed_t hvac_controller_get_fan_level(void);

/**
 * @brief Set target temperature
 * @param temp_c Temperature in Celsius
 * @return ESP_OK on success
 */
esp_err_t hvac_controller_set_target_temp(float temp_c);

/**
 * @brief Get target temperature
 * @return Target temperature in Celsius
 */
float hvac_controller_get_target_temp(void);

/**
 * @brief Increase target temperature by 0.5°C
 */
void hvac_controller_temp_up(void);

/**
 * @brief Decrease target temperature by 0.5°C
 */
void hvac_controller_temp_down(void);

/**
 * @brief Enable/disable A/C
 * @param enabled true to enable
 */
void hvac_controller_set_ac(bool enabled);

/**
 * @brief Get A/C state
 * @return true if enabled
 */
bool hvac_controller_get_ac(void);

/**
 * @brief Enable/disable recirculation
 * @param enabled true to enable
 */
void hvac_controller_set_recirculate(bool enabled);

/**
 * @brief Get recirculation state
 * @return true if enabled
 */
bool hvac_controller_get_recirculate(void);

/**
 * @brief Set temperature display unit
 * @param unit Temperature unit
 */
void hvac_controller_set_temp_unit(temp_unit_t unit);

/**
 * @brief Get temperature display unit
 * @return Temperature unit
 */
temp_unit_t hvac_controller_get_temp_unit(void);

/**
 * @brief Convert Celsius to Fahrenheit
 * @param celsius Temperature in Celsius
 * @return Temperature in Fahrenheit
 */
float hvac_controller_c_to_f(float celsius);

/**
 * @brief Convert Fahrenheit to Celsius
 * @param fahrenheit Temperature in Fahrenheit
 * @return Temperature in Celsius
 */
float hvac_controller_f_to_c(float fahrenheit);

/**
 * @brief Turn HVAC system off
 */
void hvac_controller_off(void);

/**
 * @brief Quick defrost mode
 */
void hvac_controller_quick_defrost(void);

/**
 * @brief Quick cool mode (max A/C)
 */
void hvac_controller_quick_cool(void);

/**
 * @brief Register state change callback
 * @param callback Callback function
 * @param user_data User data passed to callback
 */
void hvac_controller_register_callback(hvac_state_callback_t callback, void *user_data);

/**
 * @brief HVAC controller task - runs control loop
 * @param pvParameters Task parameters (not used)
 */
void hvac_controller_task(void *pvParameters);

/**
 * @brief Start HVAC controller task
 * @return ESP_OK on success
 */
esp_err_t hvac_controller_start_task(void);

/**
 * @brief Save current settings to NVS
 * @return ESP_OK on success
 */
esp_err_t hvac_controller_save_settings(void);

/**
 * @brief Load settings from NVS
 * @return ESP_OK on success
 */
esp_err_t hvac_controller_load_settings(void);

/**
 * @brief Apply the settings loaded at init through the normal setters
 * (commands blower and valves); AUTO at 22 °C when nothing was saved
 */
void hvac_controller_restore_settings(void);

/**
 * @brief Get mode name string
 * @param mode HVAC mode
 * @return Mode name string
 */
const char* hvac_controller_get_mode_name(hvac_mode_t mode);

/**
 * @brief Get fan level name string
 * @param level Fan level
 * @return Fan level name string
 */
const char* hvac_controller_get_fan_level_name(hvac_fan_speed_t level);

#ifdef __cplusplus
}
#endif

#endif // HVAC_CONTROLLER_H
