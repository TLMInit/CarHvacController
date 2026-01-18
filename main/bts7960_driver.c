/**
 * @file bts7960_driver.c
 * @brief BTS7960 High-Current Motor Driver Implementation
 */

#include "bts7960_driver.h"
#include "hvac_hw_config.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "BTS7960";

/*******************************************************************************
 * Private Data
 ******************************************************************************/
static blower_state_t blower_state = {
    .current_speed = 0,
    .target_speed = 0,
    .is_enabled = false,
    .is_ramping = false,
};

static SemaphoreHandle_t blower_mutex = NULL;
static TaskHandle_t blower_task_handle = NULL;

// Ramp control
static uint32_t ramp_step_delay_ms = 0;
static int16_t ramp_step_size = 0;

// Task timing
#define BLOWER_TASK_PERIOD_MS   20      // 50Hz update rate
#define DEFAULT_RAMP_TIME_MS    500     // Default speed ramp time

// Fan level to speed mapping (percentage)
static const uint8_t fan_level_speeds[] = {
    BTS7960_FAN_PERCENT_OFF,          // HVAC_FAN_OFF
    BTS7960_FAN_PERCENT_LOW,          // HVAC_FAN_LOW
    BTS7960_FAN_PERCENT_MEDIUM_LOW,   // HVAC_FAN_MEDIUM_LOW
    BTS7960_FAN_PERCENT_MEDIUM,       // HVAC_FAN_MEDIUM
    BTS7960_FAN_PERCENT_MEDIUM_HIGH,  // HVAC_FAN_MEDIUM_HIGH
    BTS7960_FAN_PERCENT_HIGH,         // HVAC_FAN_HIGH
    BTS7960_FAN_PERCENT_AUTO_DEFAULT, // HVAC_FAN_AUTO (default)
};

/*******************************************************************************
 * Private Functions
 ******************************************************************************/

static void set_pwm_duty(uint16_t duty)
{
    // BTS7960: R_EN tied to 3V3, L_EN tied to GND
    // Only R_PWM is connected for speed control
    ledc_set_duty(BTS7960_LEDC_MODE, BTS7960_RPWM_CHANNEL, duty);
    ledc_update_duty(BTS7960_LEDC_MODE, BTS7960_RPWM_CHANNEL);
}

static uint16_t percent_to_duty(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }
    return (uint16_t)((BTS7960_PWM_MAX_DUTY * percent) / 100);
}

static uint8_t duty_to_percent(uint16_t duty)
{
    return (uint8_t)((duty * 100) / BTS7960_PWM_MAX_DUTY);
}

/*******************************************************************************
 * Public Functions
 ******************************************************************************/

esp_err_t bts7960_init(void)
{
    ESP_LOGI(TAG, "Initializing BTS7960 blower driver");
    
    // Create mutex for thread safety
    blower_mutex = xSemaphoreCreateMutex();
    if (blower_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create blower mutex");
        return ESP_ERR_NO_MEM;
    }
    
    // Initialize state
    blower_state.current_speed = 0;
    blower_state.target_speed = 0;
    blower_state.is_enabled = false;
    blower_state.is_ramping = false;
    
    // R_EN is hardwired to 3V3, L_EN is hardwired to GND
    // Driver is always enabled, just set PWM to 0
    set_pwm_duty(0);
    
    ESP_LOGI(TAG, "BTS7960 driver initialized (R_EN tied to 3V3)");
    return ESP_OK;
}

void bts7960_enable(void)
{
    // R_EN is hardwired to 3V3, driver is always enabled
    ESP_LOGI(TAG, "BTS7960 driver enabled (R_EN tied to 3V3)");
    
    if (xSemaphoreTake(blower_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        blower_state.is_enabled = true;
        xSemaphoreGive(blower_mutex);
    }
}

void bts7960_disable(void)
{
    ESP_LOGI(TAG, "Stopping BTS7960 blower");
    
    // Set PWM to 0 (cannot disable as R_EN is hardwired to 3V3)
    set_pwm_duty(0);
    
    if (xSemaphoreTake(blower_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        blower_state.is_enabled = false;
        blower_state.current_speed = 0;
        blower_state.target_speed = 0;
        blower_state.is_ramping = false;
        xSemaphoreGive(blower_mutex);
    }
}

esp_err_t bts7960_set_speed(uint8_t speed_percent)
{
    if (speed_percent > 100) {
        speed_percent = 100;
    }
    
    uint16_t duty = percent_to_duty(speed_percent);
    
    if (xSemaphoreTake(blower_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (!blower_state.is_enabled && speed_percent > 0) {
            bts7960_enable();
        }
        
        blower_state.target_speed = duty;
        blower_state.current_speed = duty;
        blower_state.is_ramping = false;
        
        set_pwm_duty(duty);
        
        ESP_LOGD(TAG, "Blower speed set to %d%% (duty: %d)", speed_percent, duty);
        xSemaphoreGive(blower_mutex);
    } else {
        return ESP_ERR_TIMEOUT;
    }
    
    return ESP_OK;
}

esp_err_t bts7960_set_speed_ramp(uint8_t speed_percent, uint32_t ramp_time_ms)
{
    if (speed_percent > 100) {
        speed_percent = 100;
    }
    
    uint16_t target_duty = percent_to_duty(speed_percent);
    
    if (xSemaphoreTake(blower_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (!blower_state.is_enabled && speed_percent > 0) {
            bts7960_enable();
        }
        
        blower_state.target_speed = target_duty;
        
        // Calculate ramp parameters
        int16_t duty_diff = (int16_t)target_duty - (int16_t)blower_state.current_speed;
        
        if (duty_diff == 0) {
            blower_state.is_ramping = false;
        } else {
            uint32_t num_steps = ramp_time_ms / BLOWER_TASK_PERIOD_MS;
            if (num_steps == 0) num_steps = 1;
            
            ramp_step_size = duty_diff / (int16_t)num_steps;
            if (ramp_step_size == 0) {
                ramp_step_size = (duty_diff > 0) ? 1 : -1;
            }
            ramp_step_delay_ms = BLOWER_TASK_PERIOD_MS;
            blower_state.is_ramping = true;
            
            ESP_LOGD(TAG, "Ramping to %d%% over %lu ms", speed_percent, ramp_time_ms);
        }
        
        xSemaphoreGive(blower_mutex);
    } else {
        return ESP_ERR_TIMEOUT;
    }
    
    return ESP_OK;
}

uint8_t bts7960_get_speed(void)
{
    uint8_t speed = 0;
    if (xSemaphoreTake(blower_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        speed = duty_to_percent(blower_state.current_speed);
        xSemaphoreGive(blower_mutex);
    }
    return speed;
}

uint8_t bts7960_get_target_speed(void)
{
    uint8_t speed = 0;
    if (xSemaphoreTake(blower_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        speed = duty_to_percent(blower_state.target_speed);
        xSemaphoreGive(blower_mutex);
    }
    return speed;
}

bool bts7960_is_enabled(void)
{
    bool enabled = false;
    if (xSemaphoreTake(blower_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        enabled = blower_state.is_enabled;
        xSemaphoreGive(blower_mutex);
    }
    return enabled;
}

bool bts7960_is_ramping(void)
{
    bool ramping = false;
    if (xSemaphoreTake(blower_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        ramping = blower_state.is_ramping;
        xSemaphoreGive(blower_mutex);
    }
    return ramping;
}

void bts7960_stop(void)
{
    ESP_LOGI(TAG, "Stopping blower");
    bts7960_set_speed_ramp(0, DEFAULT_RAMP_TIME_MS);
}

void bts7960_emergency_stop(void)
{
    ESP_LOGW(TAG, "Emergency stop!");
    
    // Immediate stop without ramping
    set_pwm_duty(0);
    
    if (xSemaphoreTake(blower_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        blower_state.current_speed = 0;
        blower_state.target_speed = 0;
        blower_state.is_ramping = false;
        xSemaphoreGive(blower_mutex);
    }
}

esp_err_t bts7960_set_fan_level(hvac_fan_speed_t fan_level)
{
    if (fan_level >= HVAC_FAN_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    
    uint8_t speed = fan_level_speeds[fan_level];
    ESP_LOGI(TAG, "Setting fan level %d -> %d%%", fan_level, speed);
    
    return bts7960_set_speed_ramp(speed, DEFAULT_RAMP_TIME_MS);
}

void bts7960_get_state(blower_state_t *state)
{
    if (state == NULL) {
        return;
    }
    
    if (xSemaphoreTake(blower_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        *state = blower_state;
        xSemaphoreGive(blower_mutex);
    }
}

void bts7960_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Blower control task started");
    
    while (1) {
        if (xSemaphoreTake(blower_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (blower_state.is_ramping) {
                int16_t current = (int16_t)blower_state.current_speed;
                int16_t target = (int16_t)blower_state.target_speed;
                
                if (current != target) {
                    current += ramp_step_size;
                    
                    // Check if we've reached or passed the target
                    if ((ramp_step_size > 0 && current >= target) ||
                        (ramp_step_size < 0 && current <= target)) {
                        current = target;
                        blower_state.is_ramping = false;
                        ESP_LOGD(TAG, "Ramp complete at %d%%", duty_to_percent(current));
                    }
                    
                    // Clamp values
                    if (current < 0) current = 0;
                    if (current > BTS7960_PWM_MAX_DUTY) current = BTS7960_PWM_MAX_DUTY;
                    
                    blower_state.current_speed = (uint16_t)current;
                    set_pwm_duty(blower_state.current_speed);
                } else {
                    blower_state.is_ramping = false;
                }
            }
            xSemaphoreGive(blower_mutex);
        }
        
        vTaskDelay(pdMS_TO_TICKS(BLOWER_TASK_PERIOD_MS));
    }
}

esp_err_t bts7960_start_task(void)
{
    BaseType_t ret = xTaskCreate(
        bts7960_task,
        "blower_ctrl",
        2048,
        NULL,
        5,
        &blower_task_handle
    );
    
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create blower control task");
        return ESP_FAIL;
    }
    
    return ESP_OK;
}
