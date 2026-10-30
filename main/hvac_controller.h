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
 * @brief One-tap presets (v2.1, WP-E). Applying one sets mode/setpoint/fan/A/C as
 * listed; recirc only when SET_RECIRC_FITTED. The preset stays "active" (shown on
 * the status line) until the user changes mode, setpoint, fan or A/C.
 *   DEFOG:    DEF, A/C on, fan High
 *   MAX_HEAT: Foot, 30 °C, fan High, A/C off
 *   MAX_COOL: Face, 16 °C, A/C on, recirc on (if fitted), fan High
 *   ECO:      AUTO, A/C off, fan AUTO capped at Medium
 */
typedef enum {
    HVAC_PRESET_NONE = 0,
    HVAC_PRESET_DEFOG,
    HVAC_PRESET_MAX_HEAT,
    HVAC_PRESET_MAX_COOL,
    HVAC_PRESET_ECO,
    HVAC_PRESET_COUNT
} hvac_preset_t;

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

    // v2.1 (WP-E fills these; NaN/false/NONE until it does)
    hvac_preset_t preset;           // Active preset, NONE once the user changes something
    uint8_t control_mode;           // set_control_mode_t: CLASSIC or CASCADE
    bool heater_delay_active;       // AUTO fan held low until the heater is warm
    bool defog_active;              // Dew-point defog is overriding distribution/A/C
    float dew_point_c;              // Cabin dew point (Magnus), NaN without T/RH
    float outside_temp_c;           // Role OUTSIDE, NaN if not assigned
    float coolant_temp_c;           // Role COOLANT, NaN if not assigned
    float duct_target_c;            // Cascade: duct temperature target, NaN in classic
    char ac_reason[32];             // Why the compressor is on/off, e.g. "Interlock: blower below LOW"
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
 * Function Declarations
 ******************************************************************************/

/** @brief Apply a preset (v2.1, WP-E). HVAC_PRESET_NONE only clears the active marker. */
esp_err_t hvac_controller_apply_preset(hvac_preset_t preset);

/** @brief Active preset, or HVAC_PRESET_NONE. */
hvac_preset_t hvac_controller_get_preset(void);

/** @brief "Defog", "Max heat", "Max cool", "Eco" ("" for NONE), English. */
const char *hvac_controller_preset_name(hvac_preset_t preset);

/** @brief Re-read controller-related settings (settings apply_cb). Cheap; any task. */
void hvac_controller_apply_settings(void);

/**
 * @brief Initialize HVAC controller
 * @return ESP_OK on success
 */
esp_err_t hvac_controller_init(void);

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
 * @brief Set target temperature
 * @param temp_c Temperature in Celsius
 * @return ESP_OK on success
 */
esp_err_t hvac_controller_set_target_temp(float temp_c);

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
 * @brief Enable/disable recirculation
 * @param enabled true to enable
 */
void hvac_controller_set_recirculate(bool enabled);

/**
 * @brief Convert Celsius to Fahrenheit
 * @param celsius Temperature in Celsius
 * @return Temperature in Fahrenheit
 */
float hvac_controller_c_to_f(float celsius);

/**
 * @brief Quick defrost mode
 */
void hvac_controller_quick_defrost(void);

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
