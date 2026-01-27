/**
 * @file hvac_controller.c
 * @brief HVAC Controller Logic Implementation
 */

#include "hvac_controller.h"
#include "hvac_hw_config.h"
#include "hvac_sensors.h"
#include "tb6612fng_driver.h"
#include "bts7960_driver.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <math.h>
#include <string.h>

static const char *TAG = "HVAC_CTRL";

/*******************************************************************************
 * Constants
 ******************************************************************************/
#define HVAC_TASK_PERIOD_MS     100         // Control loop period
#define TEMP_DEADBAND           0.5f        // Temperature deadband (°C)
#define TEMP_MIN_C              16.0f       // Minimum temperature setting
#define TEMP_MAX_C              30.0f       // Maximum temperature setting
#define TEMP_STEP               0.5f        // Temperature adjustment step
#define NVS_NAMESPACE           "hvac"
#define NVS_KEY_SETTINGS        "settings"
#define NVS_KEY_THERMAL_CAL     "thermal_cal"

// Coolant temperature thresholds
#define COOLANT_WARM_THRESHOLD  50.0f       // Coolant warm enough for heating

// Thermal adaptation constants
#define THERMAL_SAMPLE_PERIOD   2000        // 2 seconds between samples
#define THERMAL_LEARN_SAMPLES   30          // 60 seconds of data
#define BASE_COOLANT_GAIN       20.0f       // Base gain for coolant valve
#define MIN_COOLANT_GAIN        10.0f       // Minimum adaptive gain
#define MAX_COOLANT_GAIN        50.0f       // Maximum adaptive gain
#define THERMAL_LEARNING_RATE   0.1f        // Learning rate (0-1)

/*******************************************************************************
 * Private Data
 ******************************************************************************/
static hvac_state_t hvac_state;
static SemaphoreHandle_t state_mutex = NULL;
static TaskHandle_t controller_task_handle = NULL;
static hvac_state_callback_t state_callback = NULL;
static void *callback_user_data = NULL;

// Thermal adaptation state
typedef struct {
    float heating_rate;          // °C per minute (learned)
    float cooling_rate;          // °C per minute (learned)
    float adaptive_coolant_gain; // Adjusted gain based on thermal mass
    float last_cabin_temp;       // Previous temperature
    uint32_t last_sample_time;   // Last measurement time
    uint32_t sample_count;       // Number of samples collected
    bool learning_active;        // Currently learning
    bool calibrated;             // System has been calibrated
} thermal_adaptation_t;

static thermal_adaptation_t thermal_adapt = {
    .heating_rate = 0.5f,        // Default: 0.5°C/min
    .cooling_rate = 0.3f,        // Default: 0.3°C/min
    .adaptive_coolant_gain = BASE_COOLANT_GAIN,
    .last_cabin_temp = NAN,
    .last_sample_time = 0,
    .sample_count = 0,
    .learning_active = false,
    .calibrated = false
};

// Mode names
static const char* mode_names[] = {
    "OFF",
    "Face",
    "Foot",
    "Face+Foot",
    "Defrost",
    "Defrost+Foot",
    "Auto"
};

// Fan level names
static const char* fan_level_names[] = {
    "OFF",
    "Low",
    "Med-Low",
    "Medium",
    "Med-High",
    "High",
    "Auto"
};

/*******************************************************************************
 * Private Functions
 ******************************************************************************/

static void notify_state_change(void)
{
    if (state_callback != NULL) {
        state_callback(&hvac_state, callback_user_data);
    }
}

static void save_thermal_calibration(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err == ESP_OK) {
        nvs_set_blob(nvs_handle, NVS_KEY_THERMAL_CAL, &thermal_adapt, sizeof(thermal_adaptation_t));
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
        ESP_LOGI(TAG, "Thermal calibration saved: heating=%.3f°C/min, cooling=%.3f°C/min, gain=%.1f",
                 thermal_adapt.heating_rate, thermal_adapt.cooling_rate, thermal_adapt.adaptive_coolant_gain);
    }
}

static void load_thermal_calibration(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err == ESP_OK) {
        size_t size = sizeof(thermal_adaptation_t);
        err = nvs_get_blob(nvs_handle, NVS_KEY_THERMAL_CAL, &thermal_adapt, &size);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Thermal calibration loaded: heating=%.3f°C/min, cooling=%.3f°C/min, gain=%.1f",
                     thermal_adapt.heating_rate, thermal_adapt.cooling_rate, thermal_adapt.adaptive_coolant_gain);
        }
        nvs_close(nvs_handle);
    }
}

static void update_thermal_adaptation(float cabin_temp, bool is_heating, bool is_cooling,
                                      uint8_t blower_speed, bool recirc)
{
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    
    // Skip if temperature invalid or first sample
    if (isnan(cabin_temp) || isnan(thermal_adapt.last_cabin_temp)) {
        thermal_adapt.last_cabin_temp = cabin_temp;
        thermal_adapt.last_sample_time = now;
        return;
    }
    
    // Check if enough time has passed for next sample
    uint32_t elapsed = now - thermal_adapt.last_sample_time;
    if (elapsed < THERMAL_SAMPLE_PERIOD) {
        return;
    }
    
    // Calculate temperature change rate (°C per minute)
    float temp_change = cabin_temp - thermal_adapt.last_cabin_temp;
    float time_min = elapsed / 60000.0f;
    float rate = fabsf(temp_change) / time_min;
    
    // Normalize by airflow contribution (blower + recirc)
    float airflow_factor = 1.0f + ((float)blower_speed / 150.0f);
    if (recirc) {
        airflow_factor += 0.15f;
    }
    if (airflow_factor < 0.2f) {
        airflow_factor = 0.2f;
    }
    float normalized_rate = rate / airflow_factor;
    
    // Only learn during active heating/cooling (not in deadband)
    if ((is_heating || is_cooling) && rate > 0.05f && rate < 5.0f) {
        thermal_adapt.learning_active = true;
        thermal_adapt.sample_count++;
        
        // Update learned rates using exponential moving average
        if (is_heating) {
            thermal_adapt.heating_rate = thermal_adapt.heating_rate * (1.0f - THERMAL_LEARNING_RATE) + 
                                        normalized_rate * THERMAL_LEARNING_RATE;
        } else if (is_cooling) {
            thermal_adapt.cooling_rate = thermal_adapt.cooling_rate * (1.0f - THERMAL_LEARNING_RATE) + 
                                        normalized_rate * THERMAL_LEARNING_RATE;
        }
        
        // Calculate adaptive gain based on thermal response
        // Faster thermal response = lower gain needed (system is more responsive)
        // Slower thermal response = higher gain needed (system needs more aggressive control)
        float avg_rate = (thermal_adapt.heating_rate + thermal_adapt.cooling_rate) / 2.0f;
        float gain_normalizer = avg_rate / 0.4f;  // Normalize around 0.4°C/min typical
        thermal_adapt.adaptive_coolant_gain = BASE_COOLANT_GAIN / fmaxf(0.5f, gain_normalizer);
        if (recirc) {
            thermal_adapt.adaptive_coolant_gain *= 0.9f;  // Recirc improves effectiveness
        }
        thermal_adapt.adaptive_coolant_gain *= (1.0f - ((float)blower_speed / 400.0f));
        
        // Clamp gain to reasonable limits
        if (thermal_adapt.adaptive_coolant_gain < MIN_COOLANT_GAIN) {
            thermal_adapt.adaptive_coolant_gain = MIN_COOLANT_GAIN;
        } else if (thermal_adapt.adaptive_coolant_gain > MAX_COOLANT_GAIN) {
            thermal_adapt.adaptive_coolant_gain = MAX_COOLANT_GAIN;
        }
        
        // Mark as calibrated after enough samples
        if (thermal_adapt.sample_count >= THERMAL_LEARN_SAMPLES) {
            if (!thermal_adapt.calibrated) {
                thermal_adapt.calibrated = true;
                save_thermal_calibration();
                ESP_LOGI(TAG, "Thermal calibration complete!");
            }
            // Periodic save every 50 samples
            if (thermal_adapt.sample_count % 50 == 0) {
                save_thermal_calibration();
            }
        }
    }
    
    thermal_adapt.last_cabin_temp = cabin_temp;
    thermal_adapt.last_sample_time = now;
}

static void update_valve_positions(hvac_mode_t mode)
{
    uint8_t center = 0, foot = 0, windscreen = 0;
    
    switch (mode) {
        case HVAC_MODE_FACE:
            center = 100;
            foot = 0;
            windscreen = 0;
            break;
            
        case HVAC_MODE_FOOT:
            center = 0;
            foot = 100;
            windscreen = 0;
            break;
            
        case HVAC_MODE_FACE_FOOT:
            center = 70;
            foot = 70;
            windscreen = 0;
            break;
            
        case HVAC_MODE_DEFROST:
            center = 0;
            foot = 0;
            windscreen = 100;
            break;
            
        case HVAC_MODE_DEFROST_FOOT:
            center = 0;
            foot = 50;
            windscreen = 100;
            break;
            
        case HVAC_MODE_AUTO:
            // Auto mode adjusts based on temperature
            center = 50;
            foot = 50;
            windscreen = 0;
            break;
            
        case HVAC_MODE_OFF:
        default:
            center = 0;
            foot = 0;
            windscreen = 0;
            break;
    }
    
    // Apply valve positions
    tb6612fng_set_valve_position(HVAC_VALVE_CENTER_1, center);
    tb6612fng_set_valve_position(HVAC_VALVE_CENTER_2, center);
    tb6612fng_set_valve_position(HVAC_VALVE_FOOT, foot);
    tb6612fng_set_valve_position(HVAC_VALVE_WINDSCREEN, windscreen);
    
    hvac_state.center_valve_pos = center;
    hvac_state.foot_valve_pos = foot;
    hvac_state.windscreen_valve_pos = windscreen;
}

static void run_temperature_control(void)
{
    // Get current sensor readings
    hvac_sensor_readings_t readings;
    hvac_sensors_get_all(&readings);
    
    hvac_state.cabin_temp_c = readings.cabin_temp_c;
    hvac_state.cabin_humidity = readings.cabin_humidity;
    hvac_state.coolant_temp_c = readings.coolant_temp_c;
    hvac_state.duct_temp_c = readings.duct_temp_c;
    
    // Get valve position feedback
    hvac_state.center_valve_pos = readings.center_valve_pos;
    hvac_state.center2_valve_pos = readings.center2_valve_pos;
    hvac_state.foot_valve_pos = readings.foot_valve_pos;
    hvac_state.windscreen_valve_pos = readings.windscreen_valve_pos;
    hvac_state.coolant_valve_pos = readings.coolant_valve_pos;
    hvac_state.blower_speed = bts7960_get_target_speed();

    hvac_state.sensor_fault = (readings.cabin_status != SENSOR_STATUS_OK) ||
                              (readings.coolant_status != SENSOR_STATUS_OK) ||
                              (readings.duct_status != SENSOR_STATUS_OK);

    uint8_t fault_mask = 0;
    for (int valve = 0; valve < HVAC_VALVE_MAX; valve++) {
        if (tb6612fng_valve_is_stalled((hvac_valve_id_t)valve)) {
            fault_mask |= (1u << valve);
        }
    }
    hvac_state.valve_fault_mask = fault_mask;
    hvac_state.valve_fault = (fault_mask != 0);
    
    // Skip control if in OFF mode or cabin temp invalid
    if (hvac_state.mode == HVAC_MODE_OFF || isnan(hvac_state.cabin_temp_c)) {
        return;
    }
    
    float error = hvac_state.target_temp_c - hvac_state.cabin_temp_c;
    
    // Check if target reached (within deadband)
    hvac_state.temp_reached = (fabsf(error) <= TEMP_DEADBAND);
    
    // Determine heating or cooling
    if (error > TEMP_DEADBAND) {
        // Need heating
        hvac_state.is_heating = true;
        hvac_state.is_cooling = false;
        
        // Check if coolant is warm enough
        if (!isnan(hvac_state.coolant_temp_c) && 
            hvac_state.coolant_temp_c > COOLANT_WARM_THRESHOLD) {
            // Calculate coolant valve position using adaptive gain
            float adaptive_gain = thermal_adapt.adaptive_coolant_gain;
            uint8_t valve_pos = (uint8_t)fminf(100.0f, fabsf(error) * adaptive_gain);
            tb6612fng_set_valve_position(HVAC_VALVE_COOLANT, valve_pos);
            
            if (thermal_adapt.sample_count < 10) {
                ESP_LOGD(TAG, "Heating: error=%.2f°C, gain=%.1f, valve=%d%%, learning=%d samples",
                         error, adaptive_gain, valve_pos, thermal_adapt.sample_count);
            }
        } else {
            // Coolant not warm, close valve
            tb6612fng_set_valve_position(HVAC_VALVE_COOLANT, 0);
            hvac_state.coolant_valve_pos = 0;
        }
    } else if (error < -TEMP_DEADBAND) {
        // Need cooling
        hvac_state.is_heating = false;
        hvac_state.is_cooling = true;
        
        // Close coolant valve for cooling
        tb6612fng_set_valve_position(HVAC_VALVE_COOLANT, 0);
    } else {
        // Temperature reached
        hvac_state.is_heating = false;
        hvac_state.is_cooling = false;
        
        // Maintain minimal coolant flow
        tb6612fng_set_valve_position(HVAC_VALVE_COOLANT, 10);
    }
    
    // Update thermal adaptation learning
    update_thermal_adaptation(hvac_state.cabin_temp_c, hvac_state.is_heating, hvac_state.is_cooling,
                              hvac_state.blower_speed, hvac_state.recirculate);
    
    // Auto fan control
    if (hvac_state.fan_level == HVAC_FAN_AUTO) {
        // Compute a continuous blower speed between LOW and 100% based on temperature error
        float abs_error = fabsf(error);
        float min_speed = (float)BTS7960_FAN_PERCENT_LOW;  // same as manual LOW
        float auto_speed_f;

        if (abs_error <= 0.5f) {
            // Very close to target: stay at minimum auto speed
            auto_speed_f = min_speed;
        } else if (abs_error >= 5.0f) {
            // Very large error: full speed
            auto_speed_f = 100.0f;
        } else {
            // Linearly scale between min_speed and 100% as error grows
            float t = (abs_error - 0.5f) / (5.0f - 0.5f);
            if (t < 0.0f) t = 0.0f;
            if (t > 1.0f) t = 1.0f;
            auto_speed_f = min_speed + t * (100.0f - min_speed);
        }

        uint8_t auto_speed = (uint8_t)(auto_speed_f + 0.5f); // round to nearest
        bts7960_set_speed(auto_speed);
        hvac_state.blower_speed = auto_speed;
    }
    
    // Auto mode adjustments
    if (hvac_state.mode == HVAC_MODE_AUTO) {
        // Adjust air distribution based on conditions
        if (hvac_state.is_heating && hvac_state.cabin_temp_c < 18.0f) {
            // Cold cabin - more to feet
            tb6612fng_set_valve_position(HVAC_VALVE_CENTER_1, 30);
            tb6612fng_set_valve_position(HVAC_VALVE_CENTER_2, 30);
            tb6612fng_set_valve_position(HVAC_VALVE_FOOT, 100);
        } else if (hvac_state.is_cooling) {
            // Cooling - more to face
            tb6612fng_set_valve_position(HVAC_VALVE_CENTER_1, 100);
            tb6612fng_set_valve_position(HVAC_VALVE_CENTER_2, 100);
            tb6612fng_set_valve_position(HVAC_VALVE_FOOT, 30);
        }
        
        // Auto defrost if high humidity
        if (hvac_state.cabin_humidity > 70.0f) {
            tb6612fng_set_valve_position(HVAC_VALVE_WINDSCREEN, 50);
        }
    }
}

/*******************************************************************************
 * Public Functions
 ******************************************************************************/

esp_err_t hvac_controller_init(void)
{
    ESP_LOGI(TAG, "Initializing HVAC controller");
    
    // Create mutex
    state_mutex = xSemaphoreCreateMutex();
    if (state_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create state mutex");
        return ESP_ERR_NO_MEM;
    }
    
    // Initialize state with defaults
    memset(&hvac_state, 0, sizeof(hvac_state));
    hvac_state.mode = HVAC_MODE_OFF;
    hvac_state.fan_level = HVAC_FAN_OFF;
    hvac_state.target_temp_c = 22.0f;
    hvac_state.temp_unit = TEMP_UNIT_CELSIUS;
    hvac_state.cabin_temp_c = NAN;
    hvac_state.cabin_humidity = NAN;
    hvac_state.coolant_temp_c = NAN;
    hvac_state.duct_temp_c = NAN;
    
    // Try to load saved settings
    hvac_controller_load_settings();
    
    // Load thermal calibration data
    load_thermal_calibration();
    
    if (thermal_adapt.calibrated) {
        ESP_LOGI(TAG, "Using saved thermal calibration");
    } else {
        ESP_LOGI(TAG, "Thermal adaptation will learn during operation");
    }
    
    ESP_LOGI(TAG, "HVAC controller initialized");
    return ESP_OK;
}

void hvac_controller_deinit(void)
{
    if (state_mutex) {
        vSemaphoreDelete(state_mutex);
        state_mutex = NULL;
    }
}

void hvac_controller_get_state(hvac_state_t *state)
{
    if (state == NULL) {
        return;
    }
    
    if (xSemaphoreTake(state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        memcpy(state, &hvac_state, sizeof(hvac_state_t));
        xSemaphoreGive(state_mutex);
    }
}

esp_err_t hvac_controller_set_mode(hvac_mode_t mode)
{
    if (mode >= HVAC_MODE_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (xSemaphoreTake(state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        hvac_state.mode = mode;
        
        if (mode == HVAC_MODE_OFF) {
            // Turn off everything
            bts7960_stop();
            tb6612fng_stop_all_valves();
            hvac_state.blower_speed = 0;
        } else {
            // Enable motor drivers
            tb6612fng_enable_all();
            bts7960_enable();
            
            // Set valve positions for mode
            update_valve_positions(mode);
            
            // Set fan if not already set
            if (hvac_state.fan_level == HVAC_FAN_OFF) {
                hvac_state.fan_level = HVAC_FAN_MEDIUM;
                bts7960_set_fan_level(HVAC_FAN_MEDIUM);
            }
        }
        
        ESP_LOGI(TAG, "Mode set to: %s", mode_names[mode]);
        notify_state_change();
        xSemaphoreGive(state_mutex);
    } else {
        return ESP_ERR_TIMEOUT;
    }
    
    return ESP_OK;
}

hvac_mode_t hvac_controller_get_mode(void)
{
    hvac_mode_t mode = HVAC_MODE_OFF;
    if (xSemaphoreTake(state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        mode = hvac_state.mode;
        xSemaphoreGive(state_mutex);
    }
    return mode;
}

esp_err_t hvac_controller_set_fan_level(hvac_fan_speed_t level)
{
    if (level >= HVAC_FAN_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (xSemaphoreTake(state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        hvac_state.fan_level = level;
        
        if (level != HVAC_FAN_AUTO) {
            bts7960_set_fan_level(level);
            hvac_state.blower_speed = bts7960_get_target_speed();
        }
        
        ESP_LOGI(TAG, "Fan level set to: %s", fan_level_names[level]);
        notify_state_change();
        xSemaphoreGive(state_mutex);
    } else {
        return ESP_ERR_TIMEOUT;
    }
    
    return ESP_OK;
}

hvac_fan_speed_t hvac_controller_get_fan_level(void)
{
    hvac_fan_speed_t level = HVAC_FAN_OFF;
    if (xSemaphoreTake(state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        level = hvac_state.fan_level;
        xSemaphoreGive(state_mutex);
    }
    return level;
}

esp_err_t hvac_controller_set_target_temp(float temp_c)
{
    if (temp_c < TEMP_MIN_C || temp_c > TEMP_MAX_C) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (xSemaphoreTake(state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        hvac_state.target_temp_c = temp_c;
        ESP_LOGI(TAG, "Target temperature set to: %.1f°C", temp_c);
        notify_state_change();
        xSemaphoreGive(state_mutex);
    } else {
        return ESP_ERR_TIMEOUT;
    }
    
    return ESP_OK;
}

float hvac_controller_get_target_temp(void)
{
    float temp = 22.0f;
    if (xSemaphoreTake(state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        temp = hvac_state.target_temp_c;
        xSemaphoreGive(state_mutex);
    }
    return temp;
}

void hvac_controller_temp_up(void)
{
    if (xSemaphoreTake(state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        float new_temp = hvac_state.target_temp_c + TEMP_STEP;
        if (new_temp <= TEMP_MAX_C) {
            hvac_state.target_temp_c = new_temp;
            notify_state_change();
        }
        xSemaphoreGive(state_mutex);
    }
}

void hvac_controller_temp_down(void)
{
    if (xSemaphoreTake(state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        float new_temp = hvac_state.target_temp_c - TEMP_STEP;
        if (new_temp >= TEMP_MIN_C) {
            hvac_state.target_temp_c = new_temp;
            notify_state_change();
        }
        xSemaphoreGive(state_mutex);
    }
}

void hvac_controller_set_ac(bool enabled)
{
    if (xSemaphoreTake(state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        hvac_state.ac_enabled = enabled;
        ESP_LOGI(TAG, "A/C %s", enabled ? "enabled" : "disabled");
        notify_state_change();
        xSemaphoreGive(state_mutex);
    }
}

bool hvac_controller_get_ac(void)
{
    bool enabled = false;
    if (xSemaphoreTake(state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        enabled = hvac_state.ac_enabled;
        xSemaphoreGive(state_mutex);
    }
    return enabled;
}

void hvac_controller_set_recirculate(bool enabled)
{
    if (xSemaphoreTake(state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        hvac_state.recirculate = enabled;
        ESP_LOGI(TAG, "Recirculation %s", enabled ? "enabled" : "disabled");
        notify_state_change();
        xSemaphoreGive(state_mutex);
    }
}

bool hvac_controller_get_recirculate(void)
{
    bool enabled = false;
    if (xSemaphoreTake(state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        enabled = hvac_state.recirculate;
        xSemaphoreGive(state_mutex);
    }
    return enabled;
}

void hvac_controller_set_temp_unit(temp_unit_t unit)
{
    if (xSemaphoreTake(state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        hvac_state.temp_unit = unit;
        notify_state_change();
        xSemaphoreGive(state_mutex);
    }
}

temp_unit_t hvac_controller_get_temp_unit(void)
{
    temp_unit_t unit = TEMP_UNIT_CELSIUS;
    if (xSemaphoreTake(state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        unit = hvac_state.temp_unit;
        xSemaphoreGive(state_mutex);
    }
    return unit;
}

esp_err_t hvac_controller_get_thermal_status(float *heating_rate, float *cooling_rate, 
                                               float *adaptive_gain, uint32_t *sample_count, 
                                               bool *calibrated)
{
    if (heating_rate) *heating_rate = thermal_adapt.heating_rate;
    if (cooling_rate) *cooling_rate = thermal_adapt.cooling_rate;
    if (adaptive_gain) *adaptive_gain = thermal_adapt.adaptive_coolant_gain;
    if (sample_count) *sample_count = thermal_adapt.sample_count;
    if (calibrated) *calibrated = thermal_adapt.calibrated;
    return ESP_OK;
}

esp_err_t hvac_controller_reset_thermal_cal(void)
{
    thermal_adapt.heating_rate = 0.5f;
    thermal_adapt.cooling_rate = 0.3f;
    thermal_adapt.adaptive_coolant_gain = BASE_COOLANT_GAIN;
    thermal_adapt.last_cabin_temp = NAN;
    thermal_adapt.last_sample_time = 0;
    thermal_adapt.sample_count = 0;
    thermal_adapt.learning_active = false;
    thermal_adapt.calibrated = false;
    
    // Erase from NVS
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err == ESP_OK) {
        nvs_erase_key(nvs_handle, NVS_KEY_THERMAL_CAL);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }
    
    ESP_LOGI(TAG, "Thermal calibration reset");
    return ESP_OK;
}

esp_err_t hvac_controller_save_thermal_cal(void)
{
    save_thermal_calibration();
    return ESP_OK;
}

float hvac_controller_c_to_f(float celsius)
{
    return (celsius * 9.0f / 5.0f) + 32.0f;
}

float hvac_controller_f_to_c(float fahrenheit)
{
    return (fahrenheit - 32.0f) * 5.0f / 9.0f;
}

void hvac_controller_off(void)
{
    hvac_controller_set_mode(HVAC_MODE_OFF);
    hvac_controller_set_fan_level(HVAC_FAN_OFF);
}

void hvac_controller_quick_defrost(void)
{
    ESP_LOGI(TAG, "Quick defrost activated");
    hvac_controller_set_mode(HVAC_MODE_DEFROST);
    hvac_controller_set_fan_level(HVAC_FAN_HIGH);
    hvac_controller_set_ac(true);  // A/C helps dehumidify
    
    // Open coolant valve fully for max heat
    tb6612fng_set_valve_position(HVAC_VALVE_COOLANT, 100);
}

void hvac_controller_quick_cool(void)
{
    ESP_LOGI(TAG, "Quick cool activated");
    hvac_controller_set_mode(HVAC_MODE_FACE);
    hvac_controller_set_fan_level(HVAC_FAN_HIGH);
    hvac_controller_set_ac(true);
    hvac_controller_set_recirculate(true);
    
    // Close coolant valve
    tb6612fng_set_valve_position(HVAC_VALVE_COOLANT, 0);
}

void hvac_controller_register_callback(hvac_state_callback_t callback, void *user_data)
{
    state_callback = callback;
    callback_user_data = user_data;
}

void hvac_controller_task(void *pvParameters)
{
    ESP_LOGI(TAG, "HVAC controller task started");
    
    while (1) {
        if (xSemaphoreTake(state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            run_temperature_control();
            xSemaphoreGive(state_mutex);
        }
        
        vTaskDelay(pdMS_TO_TICKS(HVAC_TASK_PERIOD_MS));
    }
}

esp_err_t hvac_controller_start_task(void)
{
    BaseType_t ret = xTaskCreate(
        hvac_controller_task,
        "hvac_ctrl",
        4096,
        NULL,
        5,
        &controller_task_handle
    );
    
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create controller task");
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

esp_err_t hvac_controller_save_settings(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(ret));
        return ret;
    }
    
    hvac_settings_t settings;
    if (xSemaphoreTake(state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        settings.target_temp_c = hvac_state.target_temp_c;
        settings.mode = hvac_state.mode;
        settings.fan_level = hvac_state.fan_level;
        settings.ac_enabled = hvac_state.ac_enabled;
        settings.recirculate = hvac_state.recirculate;
        settings.temp_unit = hvac_state.temp_unit;
        xSemaphoreGive(state_mutex);
    }
    
    ret = nvs_set_blob(nvs_handle, NVS_KEY_SETTINGS, &settings, sizeof(settings));
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs_handle);
    }
    
    nvs_close(nvs_handle);
    
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Settings saved to NVS");
    }
    
    return ret;
}

esp_err_t hvac_controller_load_settings(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "No saved settings found");
        return ret;
    }
    
    hvac_settings_t settings;
    size_t size = sizeof(settings);
    ret = nvs_get_blob(nvs_handle, NVS_KEY_SETTINGS, &settings, &size);
    
    nvs_close(nvs_handle);
    
    if (ret == ESP_OK && size == sizeof(settings)) {
        if (xSemaphoreTake(state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            hvac_state.target_temp_c = settings.target_temp_c;
            hvac_state.temp_unit = settings.temp_unit;
            // Don't restore mode and fan - start in OFF state
            xSemaphoreGive(state_mutex);
        }
        ESP_LOGI(TAG, "Settings loaded from NVS");
    }
    
    return ret;
}

const char* hvac_controller_get_mode_name(hvac_mode_t mode)
{
    if (mode >= HVAC_MODE_MAX) {
        return "Unknown";
    }
    return mode_names[mode];
}

const char* hvac_controller_get_fan_level_name(hvac_fan_speed_t level)
{
    if (level >= HVAC_FAN_MAX) {
        return "Unknown";
    }
    return fan_level_names[level];
}
