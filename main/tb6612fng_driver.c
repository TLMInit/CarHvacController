/**
 * @file tb6612fng_driver.c
 * @brief TB6612FNG Dual Motor Driver Implementation
 */

#include "tb6612fng_driver.h"
#include "hvac_hw_config.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "TB6612FNG";

/*******************************************************************************
 * Motor Channel Configuration
 ******************************************************************************/
typedef struct {
    gpio_num_t in1;
    gpio_num_t in2;
    ledc_channel_t pwm_channel;
} motor_config_t;

static const motor_config_t motor_configs[HVAC_VALVE_MAX] = {
    // Center valve 1 (TB6612 #1, Motor A)
    {
        .in1 = TB6612_1_AIN1,
        .in2 = TB6612_1_AIN2,
        .pwm_channel = TB6612_1_PWMA_CHANNEL,
    },
    // Center valve 2 (TB6612 #1, Motor B)
    {
        .in1 = TB6612_1_BIN1,
        .in2 = TB6612_1_BIN2,
        .pwm_channel = TB6612_1_PWMB_CHANNEL,
    },
    // Foot valve (TB6612 #2, Motor A)
    {
        .in1 = TB6612_2_AIN1,
        .in2 = TB6612_2_AIN2,
        .pwm_channel = TB6612_2_PWMA_CHANNEL,
    },
    // Windscreen valve (TB6612 #2, Motor B)
    {
        .in1 = TB6612_2_BIN1,
        .in2 = TB6612_2_BIN2,
        .pwm_channel = TB6612_2_PWMB_CHANNEL,
    },
    // Coolant valve (TB6612 #3, Motor A)
    {
        .in1 = TB6612_3_AIN1,
        .in2 = TB6612_3_AIN2,
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
#define VALVE_FULL_TRAVEL_TIME_MS   2000    // Time for 0-100% travel
#define VALVE_TASK_PERIOD_MS        50      // Task update period
#define VALVE_MOTOR_SPEED           200     // Default motor speed (0-255)
#define VALVE_POSITION_TOLERANCE    3       // Acceptable error window (% )
#define VALVE_STALL_TIMEOUT_MS      600     // No movement timeout
#define VALVE_MOVEMENT_THRESHOLD    2       // Minimum change considered movement
#define VALVE_MIN_SPEED             120     // Minimum PWM to overcome stiction

/*******************************************************************************
 * Private Functions
 ******************************************************************************/

static void set_motor_direction(const motor_config_t *config, tb6612_direction_t direction)
{
    switch (direction) {
        case TB6612_DIR_STOP:
            gpio_set_level(config->in1, 0);
            gpio_set_level(config->in2, 0);
            break;
        case TB6612_DIR_FORWARD:
            gpio_set_level(config->in1, 1);
            gpio_set_level(config->in2, 0);
            break;
        case TB6612_DIR_REVERSE:
            gpio_set_level(config->in1, 0);
            gpio_set_level(config->in2, 1);
            break;
        case TB6612_DIR_BRAKE:
            gpio_set_level(config->in1, 1);
            gpio_set_level(config->in2, 1);
            break;
    }
}

static void set_motor_speed(const motor_config_t *config, uint8_t speed)
{
    ledc_set_duty(TB6612_LEDC_MODE, config->pwm_channel, speed);
    ledc_update_duty(TB6612_LEDC_MODE, config->pwm_channel);
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
    
    if (xSemaphoreTake(valve_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        valve_state_t *state = &valve_states[valve_id];
        if (abs((int)state->target_position - (int)position) <= VALVE_POSITION_TOLERANCE) {
            xSemaphoreGive(valve_mutex);
            return ESP_OK;
        }
        state->target_position = position;
        state->stalled = false;
        state->last_motion_ms = 0;
        ESP_LOGD(TAG, "Valve %d target set to %d%%", valve_id, position);
        xSemaphoreGive(valve_mutex);
    } else {
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
    set_motor_direction(config, TB6612_DIR_BRAKE);
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
    set_motor_direction(config, direction);
    set_motor_speed(config, speed);
}

void tb6612fng_valve_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Valve control task started");
    
    while (1) {
        if (xSemaphoreTake(valve_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
            for (int i = 0; i < HVAC_VALVE_MAX; i++) {
                valve_state_t *state = &valve_states[i];
                const motor_config_t *config = &motor_configs[i];
                int16_t error = (int16_t)state->target_position - (int16_t)state->position;

                if (abs(error) <= VALVE_POSITION_TOLERANCE || state->stalled) {
                    if (state->is_moving) {
                        set_motor_direction(config, TB6612_DIR_BRAKE);
                        set_motor_speed(config, 0);
                        state->is_moving = false;
                    }
                    continue;
                }

                tb6612_direction_t desired_dir = (error > 0) ? TB6612_DIR_FORWARD : TB6612_DIR_REVERSE;
                uint8_t speed = VALVE_MOTOR_SPEED;
                int16_t mag = abs(error);
                if (mag < 25) {
                    speed = VALVE_MIN_SPEED + (mag * (VALVE_MOTOR_SPEED - VALVE_MIN_SPEED)) / 25;
                }
                if (speed < VALVE_MIN_SPEED) {
                    speed = VALVE_MIN_SPEED;
                }

                set_motor_direction(config, desired_dir);
                set_motor_speed(config, speed);

                if (!state->is_moving) {
                    state->is_moving = true;
                    state->last_motion_ms = now_ms;
                    state->last_sample_pos = state->position;
                } else {
                    if (abs((int16_t)state->position - (int16_t)state->last_sample_pos) >= VALVE_MOVEMENT_THRESHOLD) {
                        state->last_motion_ms = now_ms;
                        state->last_sample_pos = state->position;
                    } else if ((now_ms - state->last_motion_ms) > VALVE_STALL_TIMEOUT_MS) {
                        state->stalled = true;
                        state->is_moving = false;
                        set_motor_direction(config, TB6612_DIR_BRAKE);
                        set_motor_speed(config, 0);
                        ESP_LOGW(TAG, "Valve %d stall detected (target %d%%, feedback %d%%)",
                                 i, state->target_position, state->position);
                    }
                }
            }
            xSemaphoreGive(valve_mutex);
        }
        
        vTaskDelay(pdMS_TO_TICKS(VALVE_TASK_PERIOD_MS));
    }
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
    set_motor_direction(config, TB6612_DIR_REVERSE);
    set_motor_speed(config, VALVE_MOTOR_SPEED);
    vTaskDelay(pdMS_TO_TICKS(VALVE_FULL_TRAVEL_TIME_MS + 500));
    
    // Stop
    set_motor_direction(config, TB6612_DIR_BRAKE);
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

    if (xSemaphoreTake(valve_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        valve_state_t *state = &valve_states[valve_id];
        state->position = position;
        xSemaphoreGive(valve_mutex);
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
