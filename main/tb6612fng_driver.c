/**
 * @file tb6612fng_driver.c
 * @brief TB6612FNG Dual Motor Driver Implementation
 */

#include "tb6612fng_driver.h"
#include "hvac_hw_config.h"
#include "hvac_sensors.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#if USE_REMOTE_IO
#include "remote_io.h"
#endif

static const char *TAG = "TB6612FNG";

/*******************************************************************************
 * Motor Channel Configuration
 * Uses standard TB6612_* pin defines - routed via remote IO when USE_REMOTE_IO=1
 ******************************************************************************/
typedef struct {
    int in1;                    // GPIO pin number for IN1
    int in2;                    // GPIO pin number for IN2
    int pwm_pin;                // GPIO pin number for PWM
    ledc_channel_t pwm_channel; // LEDC channel (used when local IO)
} motor_config_t;

static const motor_config_t motor_configs[HVAC_VALVE_MAX] = {
    // Center valve 1 (TB6612 #1, Motor A)
    {
        .in1 = TB6612_1_AIN1,
        .in2 = TB6612_1_AIN2,
        .pwm_pin = TB6612_1_PWMA,
        .pwm_channel = TB6612_1_PWMA_CHANNEL,
    },
    // Center valve 2 (TB6612 #1, Motor B)
    {
        .in1 = TB6612_1_BIN1,
        .in2 = TB6612_1_BIN2,
        .pwm_pin = TB6612_1_PWMB,
        .pwm_channel = TB6612_1_PWMB_CHANNEL,
    },
    // Foot valve (TB6612 #2, Motor A)
    {
        .in1 = TB6612_2_AIN1,
        .in2 = TB6612_2_AIN2,
        .pwm_pin = TB6612_2_PWMA,
        .pwm_channel = TB6612_2_PWMA_CHANNEL,
    },
    // Windscreen valve (TB6612 #2, Motor B)
    {
        .in1 = TB6612_2_BIN1,
        .in2 = TB6612_2_BIN2,
        .pwm_pin = TB6612_2_PWMB,
        .pwm_channel = TB6612_2_PWMB_CHANNEL,
    },
    // Coolant valve (TB6612 #3, Motor A)
    {
        .in1 = TB6612_3_AIN1,
        .in2 = TB6612_3_AIN2,
        .pwm_pin = TB6612_3_PWMA,
        .pwm_channel = TB6612_3_PWMA_CHANNEL,
    },
};

/*******************************************************************************
 * Private Data
 ******************************************************************************/
static valve_state_t valve_states[HVAC_VALVE_MAX];
static SemaphoreHandle_t valve_mutex = NULL;
static TaskHandle_t valve_task_handle = NULL;

// Time constants for valve control (in ms)
#define VALVE_FULL_TRAVEL_TIME_MS   10000    // Time for 0-100% travel
#define VALVE_TASK_PERIOD_MS        25      // Task update period (faster for better response)
#define VALVE_MOTOR_SPEED           100     // Maximum motor speed (0-255)
#define VALVE_POSITION_TOLERANCE    2       // Acceptable error window (%) - tighter tolerance
#define VALVE_HYSTERESIS            2       // Extra margin before restarting (%) - prevents hunting
#define VALVE_STALL_TIMEOUT_MS      6000     // No movement timeout
#define VALVE_MOVEMENT_THRESHOLD    2       // Minimum change considered movement
#define VALVE_MIN_SPEED             20      // Minimum PWM to overcome stiction
#define VALVE_RAMP_START_ERROR      20      // Start ramping down speed when error is below this % (was 20)
#define VALVE_DEBUG_LOG_MS          300     // Periodic debug log interval
#define VALVE_PLATEAU_ZONE          5      // Dead zone at 0% and 100% where readings are unreliable
#define VALVE_EMA_ALPHA             128      // EMA filter: 128/256 = 50% = moderate filtering (faster response)
// Butterfly valve mapping: center position (48%) = fully open, both extremes (0%, 100%) = closed
#define VALVE_BUTTERFLY_CENTER_PCT  50      // Default raw ADC % when valve is fully OPEN
#define VALVE_OUTLIER_THRESHOLD     15      // Max allowed jump from filtered value before rejecting
#define VALVE_DEADZONE_ADC          5       // ADC dead zone at extreme positions (0-5% and 95-100%)
#define VALVE_DEADZONE_LOW_PCT      2       // Minimum target clamp (%)
#define VALVE_DEADZONE_HIGH_PCT     2       // Maximum target clamp (%)

// Forward declarations
void tb6612fng_calibration_tick(void);

// Valve calibration data structure
typedef struct {
    uint8_t adc_min;            // Minimum ADC reading observed
    uint8_t adc_max;            // Maximum ADC reading observed
    uint8_t adc_center;         // ADC reading at fully open position (center)
    uint8_t adc_low_deadzone;   // ADC value where low deadzone ends
    uint8_t adc_high_deadzone;  // ADC value where high deadzone starts
    bool is_inverted;           // True if FWD decreases ADC
    bool calibrated;            // True if calibration data is valid
} valve_calibration_t;

// Per-valve calibration data (default to uncalibrated)
// For butterfly valves: V-shaped mapping where center = fully open
static valve_calibration_t valve_calibration[HVAC_VALVE_MAX] = {
    {0, 100, VALVE_BUTTERFLY_CENTER_PCT, 5, 95, false, false},  // CENTER_1
    {0, 100, VALVE_BUTTERFLY_CENTER_PCT, 5, 95, false, false},  // CENTER_2
    {0, 100, VALVE_BUTTERFLY_CENTER_PCT, 5, 95, false, false},  // FOOT
    {0, 100, VALVE_BUTTERFLY_CENTER_PCT, 5, 95, true, false},   // WINDSCREEN (inverted)
    {0, 100, 50, 5, 95, false, false},                          // COOLANT (linear)
};

// Per-valve motor direction inversion flags (legacy, will be replaced by calibration)
// Set to true if FWD actually DECREASES ADC (inverted wiring)
static const bool valve_dir_inverted[HVAC_VALVE_MAX] = {
    true,  // HVAC_VALVE_CENTER_1 - FWD increases ADC
    true,  // HVAC_VALVE_CENTER_2 - FWD increases ADC
    true,  // HVAC_VALVE_FOOT - FWD increases ADC
    true,   // HVAC_VALVE_WINDSCREEN - INVERTED: FWD decreases ADC
    false,  // HVAC_VALVE_COOLANT - FWD increases ADC
};

static uint32_t valve_last_log_ms[HVAC_VALVE_MAX] = {0};
static uint16_t valve_filtered_pos[HVAC_VALVE_MAX] = {0};  // Filtered position * 256 for precision
static uint8_t valve_raw_pos[HVAC_VALVE_MAX] = {0};         // Last scaled position (unfiltered) for immediate stop
static uint8_t valve_adc_pos[HVAC_VALVE_MAX] = {0};         // Last raw ADC reading for debugging
static bool valve_filter_init[HVAC_VALVE_MAX] = {false};
static tb6612_direction_t last_motor_dir[HVAC_VALVE_MAX] = {TB6612_DIR_STOP};  // Track last direction for logging
static volatile bool valve_calibrating[HVAC_VALVE_MAX] = {false};  // Per-valve calibration flag

// Per-valve calibration state (for background calibration)
typedef enum {
    CALIB_STATE_IDLE = 0,
    CALIB_STATE_SEEKING_MARKER,
    CALIB_STATE_AT_MARKER,
    CALIB_STATE_ROTATING,
    CALIB_STATE_COMPLETE,
    CALIB_STATE_FAILED
} valve_calib_state_t;

typedef struct {
    valve_calib_state_t state;
    uint8_t global_min;
    uint8_t global_max;
    uint8_t start_adc;
    int sample_count;
    int last_direction;
    uint8_t last_adc;
    uint32_t phase_start_ms;
} valve_calib_progress_t;

static valve_calib_progress_t valve_calib_progress[HVAC_VALVE_MAX] = {0};

// Forward declarations
static bool any_valve_calibrating(void);
static bool is_valve_enabled(hvac_valve_id_t valve_id);

/*******************************************************************************
 * Private Functions
 ******************************************************************************/

static void set_motor_direction(hvac_valve_id_t valve_id, const motor_config_t *config, tb6612_direction_t direction, int position)
{
    bool dir_changed = (valve_id < HVAC_VALVE_MAX && last_motor_dir[valve_id] != direction);
    const char *dir_str = "?";
    
#if USE_REMOTE_IO
    esp_err_t ret1 = ESP_OK, ret2 = ESP_OK;
    
    switch (direction) {
        case TB6612_DIR_STOP:
            ret1 = remote_gpio_set_level(config->in1, 0);
            ret2 = remote_gpio_set_level(config->in2, 0);
            dir_str = "STOP";
            break;
        case TB6612_DIR_FORWARD:
            ret1 = remote_gpio_set_level(config->in1, 1);
            ret2 = remote_gpio_set_level(config->in2, 0);
            dir_str = "FWD";
            break;
        case TB6612_DIR_REVERSE:
            ret1 = remote_gpio_set_level(config->in1, 0);
            ret2 = remote_gpio_set_level(config->in2, 1);
            dir_str = "REV";
            break;
        case TB6612_DIR_BRAKE:
            ret1 = remote_gpio_set_level(config->in1, 1);
            ret2 = remote_gpio_set_level(config->in2, 1);
            dir_str = "BRAKE";
            break;
        default:
            break;
    }
    
    if (ret1 != ESP_OK || ret2 != ESP_OK) {
        ESP_LOGE(TAG, "GPIO set failed: in1=%s, in2=%s", 
                 esp_err_to_name(ret1), esp_err_to_name(ret2));
    }
#else
    switch (direction) {
        case TB6612_DIR_STOP:
            gpio_set_level(config->in1, 0);
            gpio_set_level(config->in2, 0);
            dir_str = "STOP";
            break;
        case TB6612_DIR_FORWARD:
            gpio_set_level(config->in1, 1);
            gpio_set_level(config->in2, 0);
            dir_str = "FWD";
            break;
        case TB6612_DIR_REVERSE:
            gpio_set_level(config->in1, 0);
            gpio_set_level(config->in2, 1);
            dir_str = "REV";
            break;
        case TB6612_DIR_BRAKE:
            gpio_set_level(config->in1, 1);
            gpio_set_level(config->in2, 1);
            dir_str = "BRAKE";
            break;
    }
#endif

    if (valve_id < HVAC_VALVE_MAX) {
        last_motor_dir[valve_id] = direction;
    }
    
    if (dir_changed) {
        ESP_LOGI(TAG, "Motor dir=%s pos=%d%% (IN1=%d IN2=%d)", dir_str, position, config->in1, config->in2);
    }
}

static void set_motor_speed(const motor_config_t *config, uint8_t speed)
{
#if USE_REMOTE_IO
    esp_err_t ret = remote_pwm_set_duty(config->pwm_pin, speed);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PWM set failed for pin %d: %s", config->pwm_pin, esp_err_to_name(ret));
    } else if (speed > 0) {
        ESP_LOGI(TAG, "PWM pin=%d duty=%d", config->pwm_pin, speed);
    }
#else
    ledc_set_duty(TB6612_LEDC_MODE, config->pwm_channel, speed);
    ledc_update_duty(TB6612_LEDC_MODE, config->pwm_channel);
#endif
}

static uint8_t clamp_valve_target(uint8_t position)
{
    uint8_t min_pos = VALVE_DEADZONE_LOW_PCT;
    uint8_t max_pos = (uint8_t)(100 - VALVE_DEADZONE_HIGH_PCT);

    if (max_pos < min_pos) {
        max_pos = min_pos;
    }

    if (position < min_pos) {
        return min_pos;
    }
    if (position > max_pos) {
        return max_pos;
    }
    return position;
}

/*******************************************************************************
 * Public Functions
 ******************************************************************************/

esp_err_t tb6612fng_init(void)
{
    ESP_LOGI(TAG, "Initializing TB6612FNG valve driver");
    
    // Create mutex for thread safety
    valve_mutex = xSemaphoreCreateMutex();
    if (valve_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create valve mutex");
        return ESP_ERR_NO_MEM;
    }
    
    // Initialize all valve states
    memset(valve_states, 0, sizeof(valve_states));
    
    ESP_LOGI(TAG, "TB6612FNG driver initialized");
    return ESP_OK;
}

void tb6612fng_enable_all(void)
{
    // STBY pins are hardwired to 3V3, always enabled
    ESP_LOGI(TAG, "TB6612FNG drivers always enabled (STBY tied to 3V3)");
}

void tb6612fng_disable_all(void)
{
    // STBY pins are hardwired to 3V3, cannot disable
    // Stop all motors instead by setting PWM to 0
    ESP_LOGI(TAG, "Stopping all TB6612FNG motors");
    for (int i = 0; i < HVAC_VALVE_MAX; i++) {
        tb6612fng_stop_valve((hvac_valve_id_t)i);
    }
}

esp_err_t tb6612fng_set_valve_position(hvac_valve_id_t valve_id, uint8_t position)
{
    if (valve_id >= HVAC_VALVE_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (position > 100) {
        position = 100;
    }

    position = clamp_valve_target(position);
    
    // Use shorter timeout to avoid blocking other tasks for too long
    if (xSemaphoreTake(valve_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        valve_state_t *state = &valve_states[valve_id];
        uint8_t old_target = state->target_position;
        if (abs((int)old_target - (int)position) <= VALVE_POSITION_TOLERANCE) {
            xSemaphoreGive(valve_mutex);
            return ESP_OK;
        }
        state->target_position = position;
        state->stalled = false;
        state->last_motion_ms = 0;
        ESP_LOGI(TAG, "Valve %d target: %d%% -> %d%% (pos=%d%%)", 
                 valve_id, old_target, position, state->position);
        xSemaphoreGive(valve_mutex);
    } else {
        ESP_LOGE(TAG, "Valve %d target set FAILED - mutex timeout! (wanted %d%%)", 
                 valve_id, position);
        return ESP_ERR_TIMEOUT;
    }
    
    return ESP_OK;
}

uint8_t tb6612fng_get_valve_position(hvac_valve_id_t valve_id)
{
    if (valve_id >= HVAC_VALVE_MAX) {
        return 0;
    }
    
    uint8_t pos = 0;
    if (xSemaphoreTake(valve_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        pos = valve_states[valve_id].position;
        xSemaphoreGive(valve_mutex);
    }
    return pos;
}

uint8_t tb6612fng_get_valve_target(hvac_valve_id_t valve_id)
{
    if (valve_id >= HVAC_VALVE_MAX) {
        return 0;
    }
    
    uint8_t target = 0;
    if (xSemaphoreTake(valve_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        target = valve_states[valve_id].target_position;
        xSemaphoreGive(valve_mutex);
    }
    return target;
}

bool tb6612fng_valve_is_moving(hvac_valve_id_t valve_id)
{
    if (valve_id >= HVAC_VALVE_MAX) {
        return false;
    }
    
    bool moving = false;
    if (xSemaphoreTake(valve_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        moving = valve_states[valve_id].is_moving;
        xSemaphoreGive(valve_mutex);
    }
    return moving;
}

void tb6612fng_stop_valve(hvac_valve_id_t valve_id)
{
    if (valve_id >= HVAC_VALVE_MAX) {
        return;
    }
    
    const motor_config_t *config = &motor_configs[valve_id];
    set_motor_direction(valve_id, config, TB6612_DIR_BRAKE, valve_states[valve_id].position);
    set_motor_speed(config, 0);
    
    if (xSemaphoreTake(valve_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        valve_states[valve_id].is_moving = false;
        valve_states[valve_id].stalled = false;
        valve_states[valve_id].target_position = valve_states[valve_id].position;
        valve_states[valve_id].last_motion_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        xSemaphoreGive(valve_mutex);
    }
}

void tb6612fng_stop_all_valves(void)
{
    ESP_LOGI(TAG, "Stopping all valves");
    for (int i = 0; i < HVAC_VALVE_MAX; i++) {
        tb6612fng_stop_valve((hvac_valve_id_t)i);
    }
}

void tb6612fng_motor_set(hvac_valve_id_t valve_id, tb6612_direction_t direction, uint8_t speed)
{
    if (valve_id >= HVAC_VALVE_MAX) {
        return;
    }
    
    const motor_config_t *config = &motor_configs[valve_id];
    set_motor_direction(valve_id, config, direction, valve_states[valve_id].position);
    set_motor_speed(config, speed);
}

void tb6612fng_valve_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Valve control task started");
    
    while (1) {
        // Run per-valve calibration state machines (non-blocking)
        tb6612fng_calibration_tick();
        
        if (xSemaphoreTake(valve_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
            for (int i = 0; i < HVAC_VALVE_MAX; i++) {
                // Skip disabled valves
                if (!is_valve_enabled((hvac_valve_id_t)i)) {
                    continue;
                }
                
                // Skip this valve if it's currently calibrating
                if (valve_calibrating[i]) {
                    continue;
                }
                
                valve_state_t *state = &valve_states[i];
                const motor_config_t *config = &motor_configs[i];
                
                // Get current ADC reading
                uint8_t adc_pos = valve_adc_pos[i];
                
                // Calculate error based on valve type
                int16_t error;
                int16_t abs_error;
                
                if (i == HVAC_VALVE_COOLANT) {
                    // Coolant valve: LINEAR mapping - target is position target
                    // position = ADC%, so error = target_position - position
                    error = (int16_t)state->target_position - (int16_t)state->position;
                } else {
                    // Butterfly valves: target_position is a POSITION target (100%=open, 0%=closed)
                    // Need to convert position target to ADC target
                    // V-shape: pos = 100 - |adc - center| * 100 / half_range
                    // Inverse: |adc - center| = (100 - pos) * half_range / 100
                    valve_calibration_t *cal = &valve_calibration[i];
                    uint8_t center = cal->calibrated ? cal->adc_center : 50;
                    uint8_t half_range = cal->calibrated ? 
                        (cal->adc_max - cal->adc_min) / 2 : 50;
                    
                    // Calculate how far from center the target ADC should be
                    uint8_t target_offset = (100 - state->target_position) * half_range / 100;
                    
                    // Choose direction: go toward current ADC position (shortest path)
                    int16_t target_adc;
                    if (adc_pos >= center) {
                        // Currently above center, target above center
                        target_adc = center + target_offset;
                    } else {
                        // Currently below center, target below center
                        target_adc = center - target_offset;
                    }
                    
                    // Clamp target_adc to valid range
                    if (target_adc < 0) target_adc = 0;
                    if (target_adc > 100) target_adc = 100;
                    
                    error = target_adc - (int16_t)adc_pos;
                }
                abs_error = abs(error);

                // Get raw position for predictive stop
                uint8_t raw_pos = valve_raw_pos[i];
                
                // Calculate difference between raw and filtered position
                int16_t raw_filtered_diff = (int16_t)raw_pos - (int16_t)state->position;
                if (raw_filtered_diff < 0) raw_filtered_diff = -raw_filtered_diff;
                
                // Only use raw position for predictive stop if it's reasonably close to filtered
                // If raw differs too much from filtered, the ADC reading is likely invalid/noisy
                bool raw_is_valid = (raw_filtered_diff <= 20);  // Within 20% of filtered
                
                // Predictive stop: check if ADC has crossed the target ADC position
                // For butterfly valves, we need to check ADC position, not scaled position
                // because the scaled position is non-monotonic (V-shaped around center)
                bool should_stop = false;
                
                // Predictive stop using raw position (only for coolant valve with linear mapping)
                // Butterfly valves have extremely noisy ADC readings that cause false stops,
                // so we rely solely on filtered position tolerance for them.
                if (state->is_moving && raw_is_valid && i == HVAC_VALVE_COOLANT) {
                    tb6612_direction_t current_dir = last_motor_dir[i];
                    
                    // Linear valve: check scaled position directly
                    int16_t raw_error = (int16_t)state->target_position - (int16_t)raw_pos;
                    if (current_dir == TB6612_DIR_FORWARD && raw_error <= 0) {
                        should_stop = true;
                    } else if (current_dir == TB6612_DIR_REVERSE && raw_error >= 0) {
                        should_stop = true;
                    }
                }
                
                // Always check filtered position tolerance
                if (abs_error <= VALVE_POSITION_TOLERANCE) {
                    should_stop = true;
                }
                
                // BOUNDARY ENFORCEMENT: For calibrated butterfly valves, stop if ADC is at calibrated boundary
                // This prevents the valve from going into the noisy multi-rotation region
                if (i != HVAC_VALVE_COOLANT && valve_calibration[i].calibrated) {
                    uint8_t adc_pos = valve_adc_pos[i];
                    uint8_t adc_min = valve_calibration[i].adc_min;
                    uint8_t adc_max = valve_calibration[i].adc_max;
                    
                    // Add small margin to account for motor momentum
                    uint8_t boundary_margin = 2;
                    
                    // If we're at or beyond the boundary, we're at maximum closed position
                    // The scaled position will already be 0% (fully closed) due to clamping
                    if (adc_pos <= adc_min + boundary_margin || adc_pos >= adc_max - boundary_margin) {
                        // At boundary - if target is 0% (closed), we're done
                        if (state->target_position == 0) {
                            should_stop = true;
                            ESP_LOGD(TAG, "V%d: At calibrated boundary (adc=%d%%, range=%d-%d), target reached",
                                     i, adc_pos, adc_min, adc_max);
                        }
                    }
                }
                
                // Hysteresis: stop if within tolerance, only restart if outside tolerance+hysteresis
                if (should_stop || abs_error <= VALVE_POSITION_TOLERANCE) {
                    // Within tolerance or crossed target - stop motor
                    if (state->is_moving) {
                        set_motor_direction((hvac_valve_id_t)i, config, TB6612_DIR_STOP, state->position);
                        set_motor_speed(config, 0);
                        state->is_moving = false;
                        state->stalled = false;
                        ESP_LOGI(TAG, "Valve %d reached target (open=%d%%, adc=%d%%, target=%d%%)", 
                                 i, state->position, adc_pos, state->target_position);
                    }
                    continue;
                }

                // Only start moving if outside hysteresis band (or already moving)
                if (!state->is_moving && abs_error <= (VALVE_POSITION_TOLERANCE + VALVE_HYSTERESIS)) {
                    // Inside hysteresis band and not moving - don't start
                    continue;
                }

                // Reset stall flag when we have error to correct
                state->stalled = false;

                tb6612_direction_t desired_dir;
                bool direction_blocked = false;  // Will be set if we're at a boundary
                
                // Direction logic for all valves since error is ADC-based:
                // error > 0 means target_adc > current_adc → need to INCREASE ADC
                // error < 0 means target_adc < current_adc → need to DECREASE ADC
                // During calibration, FORWARD decreased ADC (62%→0%), so:
                //   FORWARD = decreases ADC, REVERSE = increases ADC
                // Therefore: error < 0 (decrease ADC) → FORWARD, error > 0 (increase ADC) → REVERSE
                desired_dir = (error > 0) ? TB6612_DIR_REVERSE : TB6612_DIR_FORWARD;
                
                // Boundary protection for calibrated butterfly valves
                if (i != HVAC_VALVE_COOLANT && valve_calibration[i].calibrated) {
                    uint8_t adc_min = valve_calibration[i].adc_min;
                    uint8_t adc_max = valve_calibration[i].adc_max;
                    uint8_t boundary_margin = 3;
                    
                    if (error > 0 && adc_pos >= adc_max - boundary_margin) {
                        // Want to increase ADC but at upper boundary
                        direction_blocked = true;
                        ESP_LOGD(TAG, "V%d: Blocked at upper boundary (adc=%d%%, max=%d%%)", 
                                 i, adc_pos, adc_max);
                    } else if (error < 0 && adc_pos <= adc_min + boundary_margin) {
                        // Want to decrease ADC but at lower boundary
                        direction_blocked = true;
                        ESP_LOGD(TAG, "V%d: Blocked at lower boundary (adc=%d%%, min=%d%%)", 
                                 i, adc_pos, adc_min);
                    }
                }
                
                // If direction is blocked due to boundary, stop the motor
                if (direction_blocked) {
                    if (state->is_moving) {
                        set_motor_direction((hvac_valve_id_t)i, config, TB6612_DIR_STOP, state->position);
                        set_motor_speed(config, 0);
                        state->is_moving = false;
                        ESP_LOGI(TAG, "V%d: Stopped at calibrated boundary (pos=%d%%, adc=%d%%)", 
                                 i, state->position, valve_adc_pos[i]);
                    }
                    continue;
                }
                
                // Apply per-valve direction inversion
                // Use calibrated inversion if available, else use hardcoded array
                bool inverted = valve_calibration[i].calibrated ? 
                               valve_calibration[i].is_inverted : 
                               valve_dir_inverted[i];
                
                if (inverted) {
                    desired_dir = (desired_dir == TB6612_DIR_FORWARD) ? TB6612_DIR_REVERSE : TB6612_DIR_FORWARD;
                }
                
                // Proportional speed control: ramp down as we approach target
                // Full speed when error >= VALVE_RAMP_START_ERROR (40%)
                // Linearly decrease to VALVE_MIN_SPEED as error approaches tolerance
                uint8_t speed;
                if (abs_error >= VALVE_RAMP_START_ERROR) {
                    speed = VALVE_MOTOR_SPEED;  // Full speed
                } else {
                    // Linear interpolation: MIN_SPEED at tolerance, MAX_SPEED at RAMP_START_ERROR
                    // speed = MIN + (error - tolerance) * (MAX - MIN) / (RAMP_START - tolerance)
                    int range = VALVE_RAMP_START_ERROR - VALVE_POSITION_TOLERANCE;
                    int error_from_tolerance = abs_error - VALVE_POSITION_TOLERANCE;
                    if (error_from_tolerance < 0) error_from_tolerance = 0;
                    speed = VALVE_MIN_SPEED + (error_from_tolerance * (VALVE_MOTOR_SPEED - VALVE_MIN_SPEED)) / range;
                }
                if (speed < VALVE_MIN_SPEED) {
                    speed = VALVE_MIN_SPEED;
                }
                if (speed > VALVE_MOTOR_SPEED) {
                    speed = VALVE_MOTOR_SPEED;
                }

                set_motor_direction((hvac_valve_id_t)i, config, desired_dir, state->position);
                set_motor_speed(config, speed);

                if (!state->is_moving) {
                    state->is_moving = true;
                    state->last_motion_ms = now_ms;
                    state->last_sample_pos = state->position;;
                } else {
                    if (abs((int16_t)state->position - (int16_t)state->last_sample_pos) >= VALVE_MOVEMENT_THRESHOLD) {
                        state->last_motion_ms = now_ms;
                        state->last_sample_pos = state->position;
                    }
                    // Stall detection disabled - just keep trying
                }

                if (now_ms - valve_last_log_ms[i] >= VALVE_DEBUG_LOG_MS) {
                    valve_last_log_ms[i] = now_ms;
                    uint8_t adc_pos = valve_adc_pos[i];
                    if (i == HVAC_VALVE_COOLANT) {
                        // Linear valve - no center concept
                        ESP_LOGI(TAG, "V%d: pos=%d%% adc=%d%% tgt=%d%% err=%d spd=%d %s",
                                 i, state->position, adc_pos,
                                 state->target_position, error, speed,
                                 (desired_dir == TB6612_DIR_FORWARD) ? "FWD" : "REV");
                    } else {
                        // Butterfly valve - show calibration range if available
                        if (valve_calibration[i].calibrated) {
                            ESP_LOGI(TAG, "V%d: pos=%d%% adc=%d%% tgt=%d%% err=%d spd=%d %s [cal:%d-%d-%d]",
                                     i, state->position, adc_pos,
                                     state->target_position, error, speed,
                                     (desired_dir == TB6612_DIR_FORWARD) ? "FWD" : "REV",
                                     valve_calibration[i].adc_min,
                                     valve_calibration[i].adc_center,
                                     valve_calibration[i].adc_max);
                        } else {
                            ESP_LOGI(TAG, "V%d: pos=%d%% adc=%d%% tgt=%d%% err=%d spd=%d %s [uncal]",
                                     i, state->position, adc_pos,
                                     state->target_position, error, speed,
                                     (desired_dir == TB6612_DIR_FORWARD) ? "FWD" : "REV");
                        }
                    }
                }
            }
            xSemaphoreGive(valve_mutex);
        }
        
        vTaskDelay(pdMS_TO_TICKS(VALVE_TASK_PERIOD_MS));
    }
}

/*******************************************************************************
 * Independent Per-Valve Calibration State Machine
 ******************************************************************************/

// Calibration sampling parameters
#define CALIB_SAMPLE_PERIOD_MS    50     // Sample every 50ms for better resolution
#define CALIB_SETTLE_TIME_MS      500    // Wait for motor to settle after direction change
#define CALIB_MARKER_ADC          60     // ADC% to use as rotation marker
#define CALIB_MARKER_TOLERANCE    2      // +/- tolerance for marker detection
#define CALIB_MAX_ROTATION_TIME   120000 // Max time for one rotation (120 seconds at slower speed)
#define CALIB_MIN_SAMPLES         200    // Minimum samples before looking for marker again (ensure full rotation)
#define CALIB_MOTOR_SPEED         50     // Slow speed during calibration for better ADC capture
#define CALIB_MIN_RANGE           20     // Minimum ADC range required for valid calibration

// Process calibration state machine for one valve (called from valve task)
static void calibrate_valve_tick(hvac_valve_id_t valve_id)
{
    if (valve_id >= HVAC_VALVE_MAX || !valve_calibrating[valve_id]) {
        return;
    }
    
    valve_calib_progress_t *prog = &valve_calib_progress[valve_id];
    const motor_config_t *config = &motor_configs[valve_id];
    uint8_t current_adc = valve_adc_pos[valve_id];
    uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    uint32_t elapsed = now_ms - prog->phase_start_ms;
    
    switch (prog->state) {
        case CALIB_STATE_SEEKING_MARKER: {
            // Check timeout
            if (elapsed > CALIB_MAX_ROTATION_TIME) {
                ESP_LOGW(TAG, "V%d: Calibration timeout seeking marker (at %d%%)", valve_id, current_adc);
                set_motor_direction(valve_id, config, TB6612_DIR_BRAKE, current_adc);
                set_motor_speed(config, 0);
                prog->state = CALIB_STATE_FAILED;
                break;
            }
            
            // Log progress periodically while seeking marker
            if (elapsed % 5000 < VALVE_TASK_PERIOD_MS) {
                ESP_LOGI(TAG, "V%d: Seeking marker... ADC=%d%% (target=%d%%)", 
                         valve_id, current_adc, CALIB_MARKER_ADC);
            }
            
            // Check if reached marker
            if (abs((int)current_adc - CALIB_MARKER_ADC) <= CALIB_MARKER_TOLERANCE) {
                set_motor_direction(valve_id, config, TB6612_DIR_BRAKE, current_adc);
                set_motor_speed(config, 0);
                ESP_LOGI(TAG, "V%d: Reached marker at ADC=%d%%", valve_id, current_adc);
                prog->state = CALIB_STATE_AT_MARKER;
                prog->phase_start_ms = now_ms;  // Start settle timer
            }
            break;
        }
        
        case CALIB_STATE_AT_MARKER: {
            // Wait for settle time, then start rotation
            if (elapsed >= CALIB_SETTLE_TIME_MS) {
                prog->start_adc = valve_adc_pos[valve_id];
                prog->last_adc = prog->start_adc;
                prog->global_min = prog->start_adc;
                prog->global_max = prog->start_adc;
                prog->sample_count = 0;
                prog->last_direction = 0;
                
                // Start rotating forward
                set_motor_direction(valve_id, config, TB6612_DIR_FORWARD, prog->start_adc);
                set_motor_speed(config, CALIB_MOTOR_SPEED);
                
                ESP_LOGI(TAG, "V%d: Starting rotation from ADC=%d%%", valve_id, prog->start_adc);
                prog->state = CALIB_STATE_ROTATING;
                prog->phase_start_ms = now_ms;
            }
            break;
        }
        
        case CALIB_STATE_ROTATING: {
            // Check timeout
            if (elapsed > CALIB_MAX_ROTATION_TIME) {
                ESP_LOGW(TAG, "V%d: Calibration timeout during rotation", valve_id);
                set_motor_direction(valve_id, config, TB6612_DIR_BRAKE, current_adc);
                set_motor_speed(config, 0);
                prog->state = CALIB_STATE_FAILED;
                break;
            }
            
            prog->sample_count++;
            
            // Track min/max
            if (current_adc < prog->global_min) prog->global_min = current_adc;
            if (current_adc > prog->global_max) prog->global_max = current_adc;
            
            // Track direction of ADC change
            int diff = (int)current_adc - (int)prog->last_adc;
            if (abs(diff) > 2) {
                prog->last_direction = (diff > 0) ? 1 : -1;
            }
            prog->last_adc = current_adc;
            
            // Check if rotation complete (back to marker)
            if (prog->sample_count > CALIB_MIN_SAMPLES) {
                if (abs((int)current_adc - CALIB_MARKER_ADC) <= CALIB_MARKER_TOLERANCE) {
                    set_motor_direction(valve_id, config, TB6612_DIR_BRAKE, current_adc);
                    set_motor_speed(config, 0);
                    ESP_LOGI(TAG, "V%d: Rotation complete at ADC=%d%% (%d samples)", 
                             valve_id, current_adc, prog->sample_count);
                    prog->state = CALIB_STATE_COMPLETE;
                }
            }
            
            // Log progress periodically
            if (elapsed % 5000 < VALVE_TASK_PERIOD_MS) {
                ESP_LOGI(TAG, "V%d: Calibrating... ADC=%d%% min=%d%% max=%d%% samples=%d", 
                         valve_id, current_adc, prog->global_min, prog->global_max, prog->sample_count);
            }
            break;
        }
        
        case CALIB_STATE_COMPLETE: {
            // FIRST: Ensure motor is stopped briefly before normal control resumes
            set_motor_direction(valve_id, config, TB6612_DIR_BRAKE, current_adc);
            set_motor_speed(config, 0);
            
            // Validate calibration range - reject if too narrow
            uint8_t adc_range = prog->global_max - prog->global_min;
            if (adc_range < CALIB_MIN_RANGE) {
                ESP_LOGW(TAG, "V%d: Calibration REJECTED - range too narrow (%d%%, need %d%%), min=%d%% max=%d%%",
                         valve_id, adc_range, CALIB_MIN_RANGE, prog->global_min, prog->global_max);
                prog->state = CALIB_STATE_FAILED;
                break;
            }
            
            // Store calibration results
            uint8_t adc_center = (prog->global_min + prog->global_max) / 2;
            uint8_t deadzone_margin = 3;
            bool is_inverted = (prog->last_direction < 0);
            
            valve_calibration[valve_id].adc_min = prog->global_min;
            valve_calibration[valve_id].adc_max = prog->global_max;
            valve_calibration[valve_id].adc_center = adc_center;
            valve_calibration[valve_id].adc_low_deadzone = prog->global_min + deadzone_margin;
            valve_calibration[valve_id].adc_high_deadzone = prog->global_max - deadzone_margin;
            valve_calibration[valve_id].is_inverted = is_inverted;
            valve_calibration[valve_id].calibrated = true;
            
            // Reset valve state - keep the target_position that was requested!
            // The valve will immediately start seeking to this target after calibration
            if (xSemaphoreTake(valve_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                uint8_t requested_target = valve_states[valve_id].target_position;
                
                // Reset position to let feedback update it with new calibration
                valve_states[valve_id].position = 50;  // Will be corrected by feedback
                // Keep target_position as-is - valve will seek to requested position
                valve_states[valve_id].is_moving = false;
                valve_states[valve_id].stalled = false;
                valve_filter_init[valve_id] = false;
                
                ESP_LOGI(TAG, "V%d: CALIBRATED - ADC %d%%-%d%%, center=%d%%, inverted=%s, seeking to %d%%",
                         valve_id, prog->global_min, prog->global_max, adc_center,
                         is_inverted ? "YES" : "NO", requested_target);
                
                xSemaphoreGive(valve_mutex);
            } else {
                ESP_LOGI(TAG, "V%d: CALIBRATED - ADC %d%%-%d%%, center=%d%%, inverted=%s",
                         valve_id, prog->global_min, prog->global_max, adc_center,
                         is_inverted ? "YES" : "NO");
            }
            
            // Calibration done, release valve for normal control
            // Valve will now seek to its target_position
            valve_calibrating[valve_id] = false;
            prog->state = CALIB_STATE_IDLE;
            
            // Check if all calibrations are done
            if (!any_valve_calibrating()) {
                hvac_sensors_set_calibration_mode(false);
            }
            break;
        }
        
        case CALIB_STATE_FAILED: {
            // Stop motor on failure too
            set_motor_direction(valve_id, config, TB6612_DIR_BRAKE, current_adc);
            set_motor_speed(config, 0);
            
            ESP_LOGW(TAG, "V%d: Calibration FAILED - using defaults", valve_id);
            valve_calibrating[valve_id] = false;
            prog->state = CALIB_STATE_IDLE;
            
            // Check if all calibrations are done
            if (!any_valve_calibrating()) {
                hvac_sensors_set_calibration_mode(false);
            }
            break;
        }
        
        default:
            break;
    }
}

// Called from valve task to process all calibrating valves
void tb6612fng_calibration_tick(void)
{
    for (int i = 0; i < HVAC_VALVE_MAX; i++) {
        if (valve_calibrating[i]) {
            calibrate_valve_tick((hvac_valve_id_t)i);
        }
    }
}

// Start background calibration for a single valve (non-blocking)
esp_err_t tb6612fng_start_calibration(hvac_valve_id_t valve_id)
{
    if (valve_id >= HVAC_VALVE_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Skip coolant valve (linear, doesn't need calibration)
    if (valve_id == HVAC_VALVE_COOLANT) {
        ESP_LOGI(TAG, "V%d: Skipping calibration (linear valve)", valve_id);
        return ESP_OK;
    }
    
    // Already calibrating?
    if (valve_calibrating[valve_id]) {
        ESP_LOGW(TAG, "V%d: Already calibrating", valve_id);
        return ESP_ERR_INVALID_STATE;
    }
    
    // Enable fast valve position reads during calibration
    hvac_sensors_set_calibration_mode(true);
    
    // Initialize calibration progress
    valve_calib_progress_t *prog = &valve_calib_progress[valve_id];
    prog->state = CALIB_STATE_SEEKING_MARKER;
    prog->global_min = 255;
    prog->global_max = 0;
    prog->sample_count = 0;
    prog->last_direction = 0;
    prog->phase_start_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    
    // Mark valve as calibrating (valve task will skip it)
    valve_calibrating[valve_id] = true;
    
    // Start moving toward marker
    const motor_config_t *config = &motor_configs[valve_id];
    uint8_t current_adc = valve_adc_pos[valve_id];
    
    ESP_LOGI(TAG, "V%d: Starting background calibration (marker=%d%%, current_adc=%d%%)", 
             valve_id, CALIB_MARKER_ADC, current_adc);
    
    tb6612_direction_t seek_dir;
    if (current_adc < CALIB_MARKER_ADC) {
        seek_dir = TB6612_DIR_FORWARD;
    } else {
        seek_dir = TB6612_DIR_REVERSE;
    }
    
    set_motor_direction(valve_id, config, seek_dir, current_adc);
    set_motor_speed(config, CALIB_MOTOR_SPEED);
    
    return ESP_OK;
}

// Check if a valve is enabled in hardware config
static bool is_valve_enabled(hvac_valve_id_t valve_id)
{
    switch (valve_id) {
        case HVAC_VALVE_CENTER_1:   return VALVE_CENTER_1_ENABLED;
        case HVAC_VALVE_CENTER_2:   return VALVE_CENTER_2_ENABLED;
        case HVAC_VALVE_FOOT:       return VALVE_FOOT_ENABLED;
        case HVAC_VALVE_WINDSCREEN: return VALVE_WINDSCREEN_ENABLED;
        case HVAC_VALVE_COOLANT:    return VALVE_COOLANT_ENABLED;
        default:                    return false;
    }
}

// Start background calibration for all butterfly valves (non-blocking)
esp_err_t tb6612fng_start_calibration_all(void)
{
    ESP_LOGI(TAG, "====== STARTING INDEPENDENT CALIBRATION FOR ENABLED VALVES ======");
    
    // Enable fast valve position reads during calibration
    hvac_sensors_set_calibration_mode(true);
    
    bool any_started = false;
    esp_err_t result = ESP_OK;
    for (int i = 0; i < HVAC_VALVE_MAX; i++) {
        if (i == HVAC_VALVE_COOLANT) continue;  // Skip coolant (linear)
        if (!is_valve_enabled((hvac_valve_id_t)i)) {
            ESP_LOGI(TAG, "V%d: Skipping calibration (disabled in config)", i);
            continue;
        }
        
        esp_err_t ret = tb6612fng_start_calibration((hvac_valve_id_t)i);
        if (ret == ESP_OK) {
            any_started = true;
        } else if (ret != ESP_ERR_INVALID_STATE) {
            result = ret;
        }
    }
    
    // If no valves to calibrate, disable calibration mode immediately
    if (!any_started) {
        hvac_sensors_set_calibration_mode(false);
    }
    
    return result;
}

// Helper to check if any valve is calibrating (internal, no side effects)
static bool any_valve_calibrating(void)
{
    for (int i = 0; i < HVAC_VALVE_MAX; i++) {
        if (valve_calibrating[i]) {
            return true;
        }
    }
    return false;
}

// Check if any valve is still calibrating
bool tb6612fng_is_calibrating(void)
{
    bool calibrating = any_valve_calibrating();
    if (!calibrating) {
        // If no valves calibrating, disable calibration mode
        hvac_sensors_set_calibration_mode(false);
    }
    return calibrating;
}

// Check if a specific valve is calibrating
bool tb6612fng_is_valve_calibrating(hvac_valve_id_t valve_id)
{
    if (valve_id >= HVAC_VALVE_MAX) {
        return false;
    }
    return valve_calibrating[valve_id];
}

esp_err_t tb6612fng_start_task(void)
{
    BaseType_t ret = xTaskCreate(
        tb6612fng_valve_task,
        "valve_ctrl",
        4096,
        NULL,
        5,
        &valve_task_handle
    );
    
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create valve control task");
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

esp_err_t tb6612fng_calibrate_valve(hvac_valve_id_t valve_id)
{
    if (valve_id >= HVAC_VALVE_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "Calibrating valve %d", valve_id);
    
    const motor_config_t *config = &motor_configs[valve_id];
    
    // Move to fully closed position
    set_motor_direction(valve_id, config, TB6612_DIR_REVERSE, -1);
    set_motor_speed(config, VALVE_MOTOR_SPEED);
    vTaskDelay(pdMS_TO_TICKS(VALVE_FULL_TRAVEL_TIME_MS + 500));
    
    // Stop
    set_motor_direction(valve_id, config, TB6612_DIR_BRAKE, 0);
    set_motor_speed(config, 0);
    vTaskDelay(pdMS_TO_TICKS(200));
    
    // Update state
    if (xSemaphoreTake(valve_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        valve_states[valve_id].position = 0;
        valve_states[valve_id].target_position = 0;
        valve_states[valve_id].is_moving = false;
        xSemaphoreGive(valve_mutex);
    }
    
    ESP_LOGI(TAG, "Valve %d calibrated", valve_id);
    return ESP_OK;
}

// NOTE: Calibration parameters are defined above near the calibration tick function

// Internal calibration function - does not manage calibration_in_progress flag
// Uses "rotation marker" method: move to 60% ADC, then continue until hitting 60% again
// This captures exactly one potentiometer revolution regardless of multi-rotation valves
static esp_err_t __attribute__((unused)) calibrate_valve_internal(hvac_valve_id_t valve_id)
{
    if (valve_id >= HVAC_VALVE_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "====== AUTO-CALIBRATION START: Valve %d ======", valve_id);
    ESP_LOGI(TAG, "Using rotation marker method (marker=%d%%)", CALIB_MARKER_ADC);
    
    const motor_config_t *config = &motor_configs[valve_id];
    
    // Track ADC values during one revolution
    uint8_t global_min = 255;
    uint8_t global_max = 0;
    int sample_count = 0;
    uint8_t start_adc = 0;
    uint8_t end_adc = 0;
    
    // Track direction of ADC change to detect center crossing
    int adc_direction_changes = 0;
    int last_direction = 0;  // 0=unknown, 1=increasing, -1=decreasing
    uint8_t last_adc = 0;
    
    // === PHASE 1: Move to the marker position (60% ADC) ===
    ESP_LOGI(TAG, "Phase 1: Moving to marker position (%d%% ADC)...", CALIB_MARKER_ADC);
    
    uint8_t current_adc = valve_adc_pos[valve_id];
    
    // Choose direction based on current position
    tb6612_direction_t seek_dir;
    if (current_adc < CALIB_MARKER_ADC) {
        // Below marker, need to move in direction that increases ADC
        seek_dir = TB6612_DIR_FORWARD;
        ESP_LOGI(TAG, "  Current=%d%%, moving FORWARD to reach %d%%", current_adc, CALIB_MARKER_ADC);
    } else if (current_adc > CALIB_MARKER_ADC) {
        // Above marker, need to move in direction that decreases ADC
        seek_dir = TB6612_DIR_REVERSE;
        ESP_LOGI(TAG, "  Current=%d%%, moving REVERSE to reach %d%%", current_adc, CALIB_MARKER_ADC);
    } else {
        // Already at marker!
        seek_dir = TB6612_DIR_FORWARD;  // Will still do one full rotation
        ESP_LOGI(TAG, "  Already at marker position!");
    }
    
    set_motor_direction(valve_id, config, seek_dir, current_adc);
    set_motor_speed(config, CALIB_MOTOR_SPEED);
    
    uint32_t phase_start = xTaskGetTickCount();
    bool marker_found = false;
    
    // Keep moving until we hit the marker (or timeout)
    while ((xTaskGetTickCount() - phase_start) * portTICK_PERIOD_MS < CALIB_MAX_ROTATION_TIME) {
        vTaskDelay(pdMS_TO_TICKS(CALIB_SAMPLE_PERIOD_MS));
        
        current_adc = valve_adc_pos[valve_id];
        
        // Check if we've reached the marker
        if (abs((int)current_adc - CALIB_MARKER_ADC) <= CALIB_MARKER_TOLERANCE) {
            marker_found = true;
            ESP_LOGI(TAG, "  Reached marker at ADC=%d%%", current_adc);
            break;
        }
        
        // If we crossed the marker without stopping on it, we might be going wrong direction
        // Give it time to traverse the full pot range
        uint32_t elapsed = (xTaskGetTickCount() - phase_start) * portTICK_PERIOD_MS;
        if (elapsed % 5000 < CALIB_SAMPLE_PERIOD_MS) {
            ESP_LOGI(TAG, "  Seeking marker: ADC=%d%% (target=%d%%)", current_adc, CALIB_MARKER_ADC);
        }
    }
    
    if (!marker_found) {
        ESP_LOGW(TAG, "Could not reach marker position, using current position");
    }
    
    // Brief stop to settle
    set_motor_direction(valve_id, config, TB6612_DIR_BRAKE, current_adc);
    set_motor_speed(config, 0);
    vTaskDelay(pdMS_TO_TICKS(CALIB_SETTLE_TIME_MS));
    
    // === PHASE 2: Complete one full rotation, tracking ADC values ===
    ESP_LOGI(TAG, "Phase 2: Tracking one full rotation...");
    
    start_adc = valve_adc_pos[valve_id];
    last_adc = start_adc;
    global_min = start_adc;
    global_max = start_adc;
    sample_count = 0;
    
    // Always move forward for the rotation mapping
    set_motor_direction(valve_id, config, TB6612_DIR_FORWARD, start_adc);
    set_motor_speed(config, CALIB_MOTOR_SPEED);
    
    phase_start = xTaskGetTickCount();
    bool rotation_complete = false;
    
    while ((xTaskGetTickCount() - phase_start) * portTICK_PERIOD_MS < CALIB_MAX_ROTATION_TIME) {
        vTaskDelay(pdMS_TO_TICKS(CALIB_SAMPLE_PERIOD_MS));
        
        current_adc = valve_adc_pos[valve_id];
        sample_count++;
        
        // Track min/max
        if (current_adc < global_min) {
            global_min = current_adc;
        }
        if (current_adc > global_max) {
            global_max = current_adc;
        }
        
        // Track direction of ADC change
        int diff = (int)current_adc - (int)last_adc;
        if (abs(diff) > 2) {  // Significant change
            int current_dir = (diff > 0) ? 1 : -1;
            if (last_direction != 0 && current_dir != last_direction) {
                adc_direction_changes++;
                ESP_LOGI(TAG, "  Direction change #%d at ADC=%d%%", adc_direction_changes, current_adc);
            }
            last_direction = current_dir;
        }
        last_adc = current_adc;
        
        // Check if we've completed one rotation (back to marker position)
        // Only check after minimum samples to avoid detecting starting position
        if (sample_count > CALIB_MIN_SAMPLES) {
            if (abs((int)current_adc - CALIB_MARKER_ADC) <= CALIB_MARKER_TOLERANCE) {
                rotation_complete = true;
                ESP_LOGI(TAG, "  Completed rotation at ADC=%d%% after %d samples", current_adc, sample_count);
                break;
            }
        }
        
        // Log progress every 5 seconds
        uint32_t elapsed = (xTaskGetTickCount() - phase_start) * portTICK_PERIOD_MS;
        if (elapsed % 5000 < CALIB_SAMPLE_PERIOD_MS) {
            ESP_LOGI(TAG, "  Rotation %lus: ADC=%d%% (min=%d%% max=%d%% dir_changes=%d)", 
                     elapsed/1000, current_adc, global_min, global_max, adc_direction_changes);
        }
    }
    
    end_adc = valve_adc_pos[valve_id];
    
    // Stop motor
    set_motor_direction(valve_id, config, TB6612_DIR_BRAKE, end_adc);
    set_motor_speed(config, 0);
    
    if (!rotation_complete) {
        ESP_LOGW(TAG, "Rotation did not complete (timeout), using collected data");
    }
    
    ESP_LOGI(TAG, "Rotation mapping: start=%d%% end=%d%%, %d samples", start_adc, end_adc, sample_count);
    ESP_LOGI(TAG, "  ADC range: %d%% - %d%% (span=%d%%)", global_min, global_max, global_max - global_min);
    ESP_LOGI(TAG, "  Direction changes: %d", adc_direction_changes);
    
    // === Analyze results ===
    
    uint8_t adc_center;
    bool is_butterfly = (valve_id != HVAC_VALVE_COOLANT);
    
    if (is_butterfly) {
        // Butterfly valve: center is the midpoint of observed range
        adc_center = (global_min + global_max) / 2;
        
        // For butterfly valves, we expect 2 direction changes (one at min, one at max)
        // If we see this pattern, it confirms the V-shaped response
        if (adc_direction_changes >= 2) {
            ESP_LOGI(TAG, "Butterfly valve confirmed: V-shaped response detected");
        }
        
        ESP_LOGI(TAG, "Butterfly valve: calculated center=%d%% (range %d%%-%d%%)",
                 adc_center, global_min, global_max);
    } else {
        // Linear valve: center is arbitrary
        adc_center = 50;
        ESP_LOGI(TAG, "Linear valve: range %d%%-%d%%", global_min, global_max);
    }
    
    // Determine dead zones
    uint8_t deadzone_margin = 3;
    uint8_t low_deadzone = global_min + deadzone_margin;
    uint8_t high_deadzone = global_max - deadzone_margin;
    
    // Determine direction inversion based on how ADC changed during forward movement
    // If ADC mostly decreased during forward movement, it's inverted
    bool is_inverted = (end_adc < start_adc) || (last_direction < 0);
    
    // Store calibration data
    valve_calibration[valve_id].adc_min = global_min;
    valve_calibration[valve_id].adc_max = global_max;
    valve_calibration[valve_id].adc_center = adc_center;
    valve_calibration[valve_id].adc_low_deadzone = low_deadzone;
    valve_calibration[valve_id].adc_high_deadzone = high_deadzone;
    valve_calibration[valve_id].is_inverted = is_inverted;
    valve_calibration[valve_id].calibrated = true;
    
    ESP_LOGI(TAG, "====== CALIBRATION COMPLETE: Valve %d ======", valve_id);
    ESP_LOGI(TAG, "  ADC range: %d%% - %d%% (center=%d%%)", global_min, global_max, adc_center);
    ESP_LOGI(TAG, "  Dead zones: <%d%% and >%d%%", low_deadzone, high_deadzone);
    ESP_LOGI(TAG, "  Direction inverted: %s", is_inverted ? "YES" : "NO");
    
    // Reset valve state
    if (xSemaphoreTake(valve_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        valve_states[valve_id].position = 0;
        valve_states[valve_id].target_position = 0;
        valve_states[valve_id].is_moving = false;
        valve_states[valve_id].stalled = false;
        valve_filter_init[valve_id] = false;
        xSemaphoreGive(valve_mutex);
    }
    
    return ESP_OK;
}

// Public API: start calibration for a single valve (non-blocking)
// Uses the background state machine so other valves keep operating
esp_err_t tb6612fng_auto_calibrate_valve(hvac_valve_id_t valve_id)
{
    if (valve_id >= HVAC_VALVE_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    // Coolant valve is linear and does not need calibration
    if (valve_id == HVAC_VALVE_COOLANT) {
        ESP_LOGI(TAG, "V%d: Skipping calibration (linear valve)", valve_id);
        return ESP_OK;
    }

    return tb6612fng_start_calibration(valve_id);
}

bool tb6612fng_valve_is_calibrated(hvac_valve_id_t valve_id)
{
    if (valve_id >= HVAC_VALVE_MAX) {
        return false;
    }
    return valve_calibration[valve_id].calibrated;
}

esp_err_t tb6612fng_get_calibration(hvac_valve_id_t valve_id, 
                                     uint8_t *adc_min, uint8_t *adc_max, uint8_t *adc_center)
{
    if (valve_id >= HVAC_VALVE_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!valve_calibration[valve_id].calibrated) {
        return ESP_ERR_NOT_FOUND;
    }
    
    if (adc_min) *adc_min = valve_calibration[valve_id].adc_min;
    if (adc_max) *adc_max = valve_calibration[valve_id].adc_max;
    if (adc_center) *adc_center = valve_calibration[valve_id].adc_center;
    
    return ESP_OK;
}

esp_err_t tb6612fng_auto_calibrate_all(void)
{
    // Use the new independent calibration system
    // This is now non-blocking - valves calibrate independently while others can seek
    ESP_LOGI(TAG, "====== STARTING INDEPENDENT AUTO-CALIBRATION ======");
    ESP_LOGI(TAG, "Valves will calibrate independently - others can operate normally");
    
    // Start calibration for all butterfly valves
    esp_err_t ret = tb6612fng_start_calibration_all();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Some valves failed to start calibration");
    }
    
    // Immediately return; calibration continues in the background
    return ESP_OK;
}

void tb6612fng_get_all_valve_states(valve_state_t *states)
{
    if (states == NULL) {
        return;
    }
    
    if (xSemaphoreTake(valve_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        memcpy(states, valve_states, sizeof(valve_states));
        xSemaphoreGive(valve_mutex);
    }
}

void tb6612fng_update_feedback(hvac_valve_id_t valve_id, uint8_t position)
{
    if (valve_id >= HVAC_VALVE_MAX) {
        return;
    }
    
    uint8_t scaled_pos;
    
    // Coolant valve (V4) is linear 0-100%, others are butterfly valves
    if (valve_id == HVAC_VALVE_COOLANT) {
        // Linear valve: raw ADC % = open %
        scaled_pos = position;
    } else {
        // Butterfly valve mapping:
        // - ADC at center = valve FULLY OPEN (logical 100%)
        // - ADC at min = valve FULLY CLOSED (logical 0%)  
        // - ADC at max = valve FULLY CLOSED (logical 0%)
        // Map distance from center to openness: closer to center = more open
        
        // Use calibrated values if available, otherwise use default
        uint8_t center;
        uint8_t adc_min;
        uint8_t adc_max;
        
        if (valve_calibration[valve_id].calibrated) {
            center = valve_calibration[valve_id].adc_center;
            adc_min = valve_calibration[valve_id].adc_min;
            adc_max = valve_calibration[valve_id].adc_max;
        } else {
            center = VALVE_BUTTERFLY_CENTER_PCT;
            adc_min = 0;
            adc_max = 100;
        }
        
        // IMPORTANT: Clamp the ADC reading to the calibrated range
        // Any reading outside this range is noise from multi-rotation or sensor issues
        uint8_t clamped_adc = position;
        if (clamped_adc < adc_min) {
            clamped_adc = adc_min;
            ESP_LOGD(TAG, "V%d: ADC %d%% clamped to min %d%%", valve_id, position, adc_min);
        }
        if (clamped_adc > adc_max) {
            clamped_adc = adc_max;
            ESP_LOGD(TAG, "V%d: ADC %d%% clamped to max %d%%", valve_id, position, adc_max);
        }
        
        // Calculate distance from center using clamped ADC value
        int16_t distance_from_center;
        int16_t max_distance;
        
        if (clamped_adc <= center) {
            // Below center
            distance_from_center = center - clamped_adc;
            max_distance = center - adc_min;
        } else {
            // Above center
            distance_from_center = clamped_adc - center;
            max_distance = adc_max - center;
        }
        
        // Clamp max distance to avoid division by zero
        if (max_distance < 1) max_distance = 1;
        
        if (distance_from_center >= max_distance) {
            scaled_pos = 0;  // Fully closed
        } else {
            // Linear mapping: 0 distance = 100% open, max distance = 0% open
            scaled_pos = (uint8_t)(100 - (distance_from_center * 100 / max_distance));
        }
    }
    
    // Store raw (unfiltered) position immediately for predictive stop
    valve_raw_pos[valve_id] = scaled_pos;
    valve_adc_pos[valve_id] = position;  // Store actual ADC reading for debugging
    
    // Apply EMA filter to smooth noisy ADC readings
    // filtered = (alpha * new + (256 - alpha) * old) / 256
    uint16_t sample_scaled = scaled_pos << 8;  // Scale up for precision
    
    if (!valve_filter_init[valve_id]) {
        // Initialize filter with first sample
        valve_filtered_pos[valve_id] = sample_scaled;
        valve_filter_init[valve_id] = true;
    } else {
        // Outlier rejection: if reading differs too much from filtered, ignore it
        uint8_t current_filtered = (uint8_t)(valve_filtered_pos[valve_id] >> 8);
        int16_t diff = (int16_t)scaled_pos - (int16_t)current_filtered;
        if (diff < 0) diff = -diff;
        
        if (diff > VALVE_OUTLIER_THRESHOLD) {
            // Outlier detected - ignore this reading but let filter drift slowly
            // Just apply a tiny adjustment toward the reading
            if (scaled_pos > current_filtered) {
                valve_filtered_pos[valve_id] += 64;  // Drift up slowly (0.25%)
            } else {
                valve_filtered_pos[valve_id] -= 64;  // Drift down slowly
            }
            ESP_LOGD(TAG, "Valve %d outlier rejected: raw=%u%% scaled=%u%% filtered=%u%% diff=%d",
                     valve_id, position, (unsigned)scaled_pos, current_filtered, diff);
        } else {
            // Normal EMA update
            uint32_t filtered = (uint32_t)VALVE_EMA_ALPHA * sample_scaled + 
                               (uint32_t)(256 - VALVE_EMA_ALPHA) * valve_filtered_pos[valve_id];
            valve_filtered_pos[valve_id] = (uint16_t)(filtered >> 8);
        }
    }
    
    // Convert back to 0-100 range
    uint8_t filtered_position = (uint8_t)(valve_filtered_pos[valve_id] >> 8);
    
    uint8_t target = 0;
    bool got_state = false;
    if (xSemaphoreTake(valve_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        valve_state_t *state = &valve_states[valve_id];
        state->position = filtered_position;  // Use filtered position
        target = state->target_position;
        got_state = true;
        xSemaphoreGive(valve_mutex);
    }

    if (got_state) {
        ESP_LOGD(TAG, "Valve %d feedback: raw=%u%% filtered=%u%% target=%u%%", 
                 valve_id, position, filtered_position, target);
    }
}

bool tb6612fng_valve_is_stalled(hvac_valve_id_t valve_id)
{
    if (valve_id >= HVAC_VALVE_MAX) {
        return false;
    }
    bool stalled = false;
    if (xSemaphoreTake(valve_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        stalled = valve_states[valve_id].stalled;
        xSemaphoreGive(valve_mutex);
    }
    return stalled;
}

void tb6612fng_clear_stall(hvac_valve_id_t valve_id)
{
    if (valve_id >= HVAC_VALVE_MAX) {
        return;
    }
    if (xSemaphoreTake(valve_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        valve_states[valve_id].stalled = false;
        valve_states[valve_id].last_motion_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        xSemaphoreGive(valve_mutex);
    }
}
