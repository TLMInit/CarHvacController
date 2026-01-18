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

// Fan speed percentage mapping
#define BTS7960_FAN_PERCENT_OFF          0
#define BTS7960_FAN_PERCENT_LOW          30
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
 * @brief Disable BTS7960 driver
 */
void bts7960_disable(void);

/**
 * @brief Set blower motor speed
 * @param speed_percent Speed 0-100%
 * @return ESP_OK on success
 */
esp_err_t bts7960_set_speed(uint8_t speed_percent);

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
 * @brief Check if blower is enabled
 * @return true if enabled
 */
bool bts7960_is_enabled(void);

/**
 * @brief Check if blower is ramping
 * @return true if speed ramping in progress
 */
bool bts7960_is_ramping(void);

/**
 * @brief Stop blower motor immediately
 */
void bts7960_stop(void);

/**
 * @brief Emergency stop with brake
 */
void bts7960_emergency_stop(void);

/**
 * @brief Set blower speed from fan level
 * @param fan_level HVAC fan speed level
 * @return ESP_OK on success
 */
esp_err_t bts7960_set_fan_level(hvac_fan_speed_t fan_level);

/**
 * @brief Get blower state
 * @param state Pointer to state structure
 */
void bts7960_get_state(blower_state_t *state);

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
