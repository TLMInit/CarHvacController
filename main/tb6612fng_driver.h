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

// Calibration timing. Each valve runs its own state machine (valves calibrate in parallel):
// seek the marker (<= SET_CAL_TIMEOUT_S), settle, rotate once (<= SET_CAL_TIMEOUT_S).
#define CALIB_SETTLE_TIME_MS        500     // Wait for motor to settle after direction change
// A service jog not repeated within this stops (dead-man); hvac_ui_service.c JOG_REPEAT_MS must stay below it
#define SERVICE_JOG_TIMEOUT_MS      300

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
 * @brief Mark a valve's position feedback lost (true) or back (false).
 * While lost, the valve's motor is stopped and the valve is not moved.
 */
void tb6612fng_set_feedback_fault(hvac_valve_id_t valve_id, bool fault);

/**
 * @brief True if the valve is stalled or its position feedback is lost
 */
bool tb6612fng_valve_has_fault(hvac_valve_id_t valve_id);

/**
 * @brief Clear stall flag for a valve
 * @param valve_id Valve identifier
 */
void tb6612fng_clear_stall(hvac_valve_id_t valve_id);

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
 * @brief Start non-blocking auto-calibration for all butterfly valves
 */
esp_err_t tb6612fng_auto_calibrate_all(void);

/**
 * @brief Check if any valve is currently calibrating
 */
bool tb6612fng_is_calibrating(void);

/** Longest valve-task pass in ms since the previous call (resets it). For the status line. */
uint32_t tb6612fng_take_max_pass_ms(void);

// ==================== v2.1 interfaces (implemented by WP-D) ====================

/** Where a valve's calibration came from at boot. */
typedef enum {
    VALVE_CAL_SRC_DEFAULT = 0,      /* built-in defaults (never calibrated, or calibration failed) */
    VALVE_CAL_SRC_STORED,           /* loaded from hvac_cal and plausible */
    VALVE_CAL_SRC_CALIBRATED,       /* calibrated this boot (and stored) */
    VALVE_CAL_SRC_FIXED,            /* not calibrated by design (coolant, linear) */
} valve_cal_source_t;

/** Everything the valve service page and the self-test show about one valve. */
typedef struct {
    valve_cal_source_t cal_source;
    bool calibrating;
    bool service_active;            /* jog / learn / recalibrate in progress */
    uint8_t adc_min, adc_max, adc_center;
    bool is_inverted;               /* effective (after the per-valve direction setting) */
    uint8_t speed;                  /* effective PWM (after the per-valve speed setting) */
    uint8_t adc_pos;                /* last raw reading, ADC % */
    uint8_t position;               /* open % */
    uint8_t target;                 /* open % */
    bool moving, stalled, feedback_fault;
} tb6612_valve_info_t;

void tb6612fng_get_valve_info(hvac_valve_id_t valve_id, tb6612_valve_info_t *out);

/**
 * Service API (valve service page). The caller enforces "mode OFF" (the UI refuses
 * otherwise); the driver additionally refuses while the valve calibrates.
 * jog: dir +1 FWD, -1 REV, 0 stop. A jog must be repeated at least every 300 ms
 * (while the button is held); the driver stops the motor if it is not.
 */
esp_err_t tb6612fng_service_jog(hvac_valve_id_t valve_id, int dir);
esp_err_t tb6612fng_service_stop(hvac_valve_id_t valve_id);
/** Recalibrate one valve now (non-blocking; result into the event log and hvac_cal). */
esp_err_t tb6612fng_service_recalibrate(hvac_valve_id_t valve_id);
/** Short move, observe the ADC sign, store is_inverted (non-blocking). */
esp_err_t tb6612fng_service_learn_direction(hvac_valve_id_t valve_id);

/**
 * Re-read the valve settings into the driver's snapshot (settings apply_cb, and at init), under
 * valve_mutex so a valve-task pass never sees half a change. Calibration speed and timeout are
 * latched when a calibration starts. SET_VALVE_FITTED_n: not fitted = stopped once and then never
 * driven, calibrated or self-tested; fitted again after boot = the boot decision runs for it
 * (stored calibration if it still fits the pot, else it calibrates).
 */
void tb6612fng_apply_settings(void);

/** SET_VALVE_FITTED_n as the driver last applied it. */
bool tb6612fng_valve_fitted(hvac_valve_id_t valve_id);

/** Longest calibration of one valve in ms with the current settings (two phases + settle). */
uint32_t tb6612fng_calib_max_ms(void);

#ifdef __cplusplus
}
#endif

#endif // TB6612FNG_DRIVER_H
