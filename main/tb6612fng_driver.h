/**
 * @file tb6612fng_driver.h
 * @brief TB6612FNG Dual Motor Driver for HVAC Valve Control
 * 
 * Controls 5 valves using three TB6612FNG motor driver ICs:
 * - 2x Center vent valves
 * - 1x Foot vent valve
 * - 1x Windscreen (defrost) valve
 * - 1x Coolant (heater core) valve
 */

#ifndef TB6612FNG_DRIVER_H
#define TB6612FNG_DRIVER_H

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
 * @brief Motor direction enumeration
 */
typedef enum {
    TB6612_DIR_STOP = 0,    // Motor stopped (coast)
    TB6612_DIR_FORWARD,     // Motor forward (open valve)
    TB6612_DIR_REVERSE,     // Motor reverse (close valve)
    TB6612_DIR_BRAKE        // Motor brake (short brake)
} tb6612_direction_t;

/**
 * @brief Valve state structure
 */
typedef struct {
    uint8_t position;           // Last known position 0-100% (feedback)
    uint8_t target_position;    // Target position 0-100%
    bool is_moving;             // Currently moving flag
    bool stalled;               // True if stall detected
    uint32_t last_motion_ms;    // Last time movement was observed
    uint8_t last_sample_pos;    // Position during last movement sample
} valve_state_t;

/*******************************************************************************
 * Function Declarations
 ******************************************************************************/

/**
 * @brief Initialize TB6612FNG motor drivers
 * @return ESP_OK on success
 */
esp_err_t tb6612fng_init(void);

/**
 * @brief Enable all TB6612FNG drivers (exit standby mode)
 */
void tb6612fng_enable_all(void);

/**
 * @brief Disable all TB6612FNG drivers (enter standby mode)
 */
void tb6612fng_disable_all(void);

/**
 * @brief Set valve position (0-100%)
 * @param valve_id Valve identifier
 * @param position Target position 0-100%
 * @return ESP_OK on success
 */
esp_err_t tb6612fng_set_valve_position(hvac_valve_id_t valve_id, uint8_t position);

/**
 * @brief Get current valve position
 * @param valve_id Valve identifier
 * @return Current position 0-100%
 */
uint8_t tb6612fng_get_valve_position(hvac_valve_id_t valve_id);

/**
 * @brief Get valve target position
 * @param valve_id Valve identifier
 * @return Target position 0-100%
 */
uint8_t tb6612fng_get_valve_target(hvac_valve_id_t valve_id);

/**
 * @brief Check if valve is currently moving
 * @param valve_id Valve identifier
 * @return true if moving, false if stationary
 */
bool tb6612fng_valve_is_moving(hvac_valve_id_t valve_id);

/**
 * @brief Stop a specific valve immediately
 * @param valve_id Valve identifier
 */
void tb6612fng_stop_valve(hvac_valve_id_t valve_id);

/**
 * @brief Stop all valves immediately
 */
void tb6612fng_stop_all_valves(void);

/**
 * @brief Update feedback position from ADS1115 measurement
 * @param valve_id Valve identifier
 * @param position Feedback position 0-100%
 */
void tb6612fng_update_feedback(hvac_valve_id_t valve_id, uint8_t position);

/**
 * @brief Query stall status for a valve
 * @param valve_id Valve identifier
 * @return true if stalled
 */
bool tb6612fng_valve_is_stalled(hvac_valve_id_t valve_id);

/**
 * @brief Clear stall flag for a valve
 * @param valve_id Valve identifier
 */
void tb6612fng_clear_stall(hvac_valve_id_t valve_id);

/**
 * @brief Set motor direction and speed directly
 * @param valve_id Valve identifier
 * @param direction Motor direction
 * @param speed PWM duty cycle 0-255
 */
void tb6612fng_motor_set(hvac_valve_id_t valve_id, tb6612_direction_t direction, uint8_t speed);

/**
 * @brief Valve control task - must be called periodically or run as FreeRTOS task
 * @param pvParameters Task parameters (not used)
 */
void tb6612fng_valve_task(void *pvParameters);

/**
 * @brief Start the valve control task
 * @return ESP_OK on success
 */
esp_err_t tb6612fng_start_task(void);

/**
 * @brief Calibrate a valve (move to full open, then full closed)
 * @param valve_id Valve identifier
 * @return ESP_OK on success
 */
esp_err_t tb6612fng_calibrate_valve(hvac_valve_id_t valve_id);

/**
 * @brief Get all valve states
 * @param states Array to store valve states (must be at least HVAC_VALVE_MAX elements)
 */
void tb6612fng_get_all_valve_states(valve_state_t *states);

#ifdef __cplusplus
}
#endif

#endif // TB6612FNG_DRIVER_H
