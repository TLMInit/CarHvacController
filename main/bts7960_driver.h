/**
 * @file bts7960_driver.h
 * @brief BTS7960 High-Current Motor Driver for HVAC Blower Motor
 * 
 * Controls the blower motor using BTS7960 H-bridge with PWM speed control
 */

#ifndef BTS7960_DRIVER_H
#define BTS7960_DRIVER_H

#include "esp_err.h"
#include "hvac_hw_config.h"
#include <stdint.h>
#include <stdbool.h>

// Fan speed percentage mapping (Low and High are SET_FAN_MIN_DUTY / SET_FAN_MAX_DUTY, see g_fan_levels)
#define BTS7960_FAN_PERCENT_OFF          0
#define BTS7960_FAN_PERCENT_MEDIUM_LOW   45
#define BTS7960_FAN_PERCENT_MEDIUM       60
#define BTS7960_FAN_PERCENT_MEDIUM_HIGH  80
#define BTS7960_FAN_PERCENT_HIGH         100
#define BTS7960_FAN_PERCENT_AUTO_DEFAULT 50

#ifdef __cplusplus
extern "C" {
#endif

/*******************************************************************************
 * Type Definitions
 ******************************************************************************/

/**
 * @brief Blower motor state structure
 */
typedef struct {
    uint16_t current_speed;     // Current speed 0-1023
    uint16_t target_speed;      // Target speed 0-1023
    bool is_enabled;            // Motor enabled flag
    bool is_ramping;            // Speed ramping in progress
} blower_state_t;

/*******************************************************************************
 * Function Declarations
 ******************************************************************************/

/**
 * @brief Initialize BTS7960 blower motor driver
 * @return ESP_OK on success
 */
esp_err_t bts7960_init(void);

/**
 * @brief Enable BTS7960 driver
 */
void bts7960_enable(void);

/**
 * @brief Set blower motor speed with ramping
 * @param speed_percent Target speed 0-100%
 * @param ramp_time_ms Time to reach target speed in milliseconds
 * @return ESP_OK on success
 */
esp_err_t bts7960_set_speed_ramp(uint8_t speed_percent, uint32_t ramp_time_ms);

/**
 * @brief Get current blower speed
 * @return Current speed 0-100%
 */
uint8_t bts7960_get_speed(void);

/**
 * @brief Get target blower speed
 * @return Target speed 0-100%
 */
uint8_t bts7960_get_target_speed(void);

/**
 * @brief Check if blower is ramping
 * @return true if speed ramping in progress
 */
bool bts7960_is_ramping(void);

/**
 * @brief Ramp the blower to 0
 * @param ramp_time_ms Ramp time (the controller passes SET_SOFT_START_MS, like every other ramp)
 */
void bts7960_stop(uint32_t ramp_time_ms);

/**
 * @brief Re-send the current duty to the node (restores it after a node reset)
 */
void bts7960_refresh(void);

/**
 * @brief True if the node acknowledged the last blower PWM command (always true without remote IO)
 */
bool bts7960_last_command_ok(void);

/**
 * @brief Blower control task - must be called periodically or run as FreeRTOS task
 * @param pvParameters Task parameters (not used)
 */
void bts7960_task(void *pvParameters);

/**
 * @brief Start the blower control task
 * @return ESP_OK on success
 */
esp_err_t bts7960_start_task(void);

#ifdef __cplusplus
}
#endif

#endif // BTS7960_DRIVER_H
