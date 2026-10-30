/**
 * @file hvac_controller.c
 * @brief HVAC Controller Logic Implementation
 */

#include "hvac_controller.h"
#include "hvac_hw_config.h"
#include "hvac_sensors.h"
#include "tb6612fng_driver.h"
#include "bts7960_driver.h"
#include "settings.h"
#include "sensor_roles.h"
#include "event_log.h"
#include "comfort.h"
#include "cascade.h"
#include "presets_priv.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <math.h>
#include <string.h>
#include <stdio.h>

#include "remote_io.h"

static const char *TAG = "HVAC_CTRL";

/*******************************************************************************
 * Constants
 ******************************************************************************/
#define HVAC_TASK_PERIOD_MS     100         // Control loop period
#define CTRL_REASSERT_MS        1000u       // relay and blower re-sent this often (reassert_outputs)
_Static_assert(CTRL_REASSERT_MS * 2 <= REMOTE_IO_LINK_WINDOW_MS,
               "re-assert at most half the link window (remote_io.h), which is also the node's watchdog");
#define TEMP_MIN_C              ((float)SETTINGS_SETPOINT_MIN_C)    // setpoint limits: one source with the
#define TEMP_MAX_C              ((float)SETTINGS_SETPOINT_MAX_C)    // setpoint and preset ranges (settings_table.c)
#define NVS_NAMESPACE           "hvac"
#define NVS_KEY_SETTINGS        "settings"
#define NVS_KEY_THERMAL_CAL     "thermal_cal"

// Thermal adaptation constants
#define THERMAL_SAMPLE_PERIOD   2000        // 2 seconds between samples
#define THERMAL_LEARN_SAMPLES   30          // 60 seconds of data
// Base gain and its adapted limits: SET_HEAT_GAIN, SET_HEAT_GAIN_MIN, SET_HEAT_GAIN_MAX
#define THERMAL_LEARNING_RATE   0.1f        // Learning rate (0-1)

// A/C compressor control constants
// Humidity on/off thresholds come from SET_HUMIDITY_ON / SET_HUMIDITY_OFF (defaults 65 / 58)
// Clutch minimum off / humidity-only on times: SET_AC_MIN_OFF_S, SET_AC_MIN_ON_S
#define AC_FREEZE_CUTOUT_C      3.0f        // Duct below this with coolant closed: clutch off
#define AC_FREEZE_CUTIN_C       6.0f        // ...until duct is back above this

// Cold-start warm-up (SET_COLD_START_*), the S-08 fallback (SET_SENSOR_FALLBACK_HEAT,
// SET_AUTO_FAN_NO_SENSOR) and the defog release/windscreen floor (SET_DEFOG_*) are settings.
// Evaporator icing guard: the compressor needs this much airflow. Fixed on purpose, not the
// fan minimum, so a lower fan minimum (or night cap) drops the A/C rather than icing it.
// Pair: SET_FAN_MIN_DUTY default (settings_table.c) is 30, so manual Low keeps the A/C with defaults.
#define AC_MIN_BLOWER_PCT            30

/*******************************************************************************
 * Private Data
 ******************************************************************************/
static hvac_state_t hvac_state;
static SemaphoreHandle_t state_mutex = NULL;
static TaskHandle_t controller_task_handle = NULL;

// Settings persistence: loaded at init, applied by hvac_controller_restore_settings();
// changes are written to NVS 5 s after the last one
static hvac_settings_t saved_settings;
static bool have_saved_settings = false;
static uint32_t settings_dirty_ms = 0;  // 0 = nothing to save
#define SETTINGS_SAVE_DELAY_MS  5000

static void mark_settings_dirty(void)
{
    uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    settings_dirty_ms = now_ms ? now_ms : 1;
}

// Cold-start warmup state (for AUTO mode)
typedef struct {
    bool active;                 // Warmup phase is active
    uint32_t start_time_ms;      // When warmup started
    float initial_cabin_temp;    // Cabin temp when warmup started
    float initial_humidity;      // Humidity when warmup started
} warmup_state_t;

static warmup_state_t warmup_state = {
    .active = false,
    .start_time_ms = 0,
    .initial_cabin_temp = NAN,
    .initial_humidity = NAN
};

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
    .adaptive_coolant_gain = 0,  // SET_HEAT_GAIN, set by cfg_refresh() while nothing is learned
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
 * Settings cache (refreshed by hvac_controller_apply_settings() and once a second)
 ******************************************************************************/
typedef struct {
    float hum_on, hum_off, step, default_setpoint;
    int powerup;
    bool auto_requests_ac, recirc_fitted, def_restores_ac;
    int recirc_cancel_min;
    int fan_min, fan_max, night_cap;
    bool night;
    float curve_k;                  // AUTO error slope multiplier
    uint32_t soft_start_ms;
    bool hd_on;
    float hd_temp;
    int hd_fan;
    uint32_t hd_max_ms;
    bool defog_on, defog_uses_ac;
    float defog_margin, defog_offset;
    int control_mode;
    cascade_params_t casc;
    int unit;
    float deadband, feet_first_c, auto_ws_rh, auto_full_c;
    int fallback_heat, auto_fan_nosens, coolant_idle, eco_cap;
    float cold_below, cold_end_c, cold_end_rh, cold_duct_c;
    uint32_t cold_min_ms, cold_max_ms;
    float defog_release;
    int defog_ws;
    float heat_gain, heat_gain_min, heat_gain_max;
    uint32_t ac_min_off_ms, ac_min_on_ms;
    uint8_t air[SETTINGS_AIR_MODES][SETTINGS_AIR_VALVES];  // valve % per manual mode (SET_AIR_*)
    uint8_t warm_foot, warm_centre, cold_centre, cold_ws, cool_foot, balanced;   // AUTO distribution % (SERVICE)
} ctl_cfg_t;

static ctl_cfg_t cfg;
static bool cfg_loaded = false;
static bool air_changed = false;    // grid edited: the next cycle moves the current mode's valves

static comfort_defog_t defog;
static comfort_heater_delay_t heater_delay;
static bool heater_delay_pending = false;   // start the delay on the next AUTO cycle
static cascade_state_t cascade;
static uint32_t cascade_last_ms = 0;
static bool ac_mem_valid = false;           // user A/C flag remembered on entering DEF
static bool ac_mem = false;

// The setting, or the table's own default if it is out of the table's range (each default
// exists once: in the table)
static float cfg_t(setting_id_t id)
{
    const setting_def_t *d = settings_def(id);
    float v = settings_get_float(id);
    return (v >= d->min && v <= d->max) ? v : d->def;
}

static int cfg_ti(setting_id_t id)
{
    return (int)lroundf(cfg_t(id));
}

static void cfg_refresh(void)   // caller holds state_mutex (or runs before the task starts)
{
    ctl_cfg_t n;
    memset(&n, 0, sizeof(n));
    n.hum_on = cfg_t(SET_HUMIDITY_ON);
    n.hum_off = cfg_t(SET_HUMIDITY_OFF);
    if (n.hum_off > n.hum_on - SETTINGS_HUMIDITY_GAP) {     // the settings rule already keeps this
        n.hum_off = n.hum_on - SETTINGS_HUMIDITY_GAP;
    }
    n.step = cfg_t(SET_SETPOINT_STEP);
    n.default_setpoint = cfg_t(SET_DEFAULT_SETPOINT);
    n.powerup = cfg_ti(SET_POWER_UP_STATE);
    n.auto_requests_ac = settings_get_bool(SET_AUTO_REQUESTS_AC);
    n.recirc_fitted = settings_get_bool(SET_RECIRC_FITTED);
    n.def_restores_ac = settings_get_bool(SET_DEF_RESTORES_AC);
    n.recirc_cancel_min = cfg_ti(SET_RECIRC_AUTO_CANCEL_MIN);
    n.fan_min = cfg_ti(SET_FAN_MIN_DUTY);
    n.fan_max = cfg_ti(SET_FAN_MAX_DUTY);
    if (n.fan_max < n.fan_min) {
        n.fan_max = n.fan_min;
    }
    n.night_cap = cfg_ti(SET_QUIET_NIGHT_CAP);
    n.night = settings_get_bool(SET_NIGHT_MODE);
    switch (cfg_ti(SET_AUTO_CURVE)) {
        case SET_CURVE_ECO: n.curve_k = 0.6f; break;
        case SET_CURVE_MAX: n.curve_k = 1.4f; break;
        default:            n.curve_k = 1.0f; break;
    }
    n.soft_start_ms = (uint32_t)cfg_ti(SET_SOFT_START_MS);
    n.hd_on = settings_get_bool(SET_HEATER_DELAY);
    n.hd_temp = cfg_t(SET_HEATER_DELAY_TEMP);
    n.hd_fan = cfg_ti(SET_HEATER_DELAY_FAN);
    n.hd_max_ms = (uint32_t)cfg_ti(SET_HEATER_DELAY_MAX_MIN) * 60000u;
    n.defog_on = settings_get_bool(SET_DEFOG);
    n.defog_margin = cfg_t(SET_DEFOG_MARGIN);
    n.defog_uses_ac = settings_get_bool(SET_DEFOG_USES_AC);
    n.defog_offset = cfg_t(SET_DEFOG_GLASS_OFFSET);
    n.control_mode = cfg_ti(SET_CONTROL_MODE);
    n.casc.outer_kp = cfg_t(SET_CASCADE_OUTER_KP);     // zero gain = that term off
    n.casc.outer_ki = cfg_t(SET_CASCADE_OUTER_KI);
    n.casc.inner_kp = cfg_t(SET_CASCADE_INNER_KP);
    n.casc.inner_ki = cfg_t(SET_CASCADE_INNER_KI);
    n.casc.duct_min_c = cfg_t(SET_DUCT_MIN);
    n.casc.duct_max_c = cfg_t(SET_DUCT_MAX);
    n.unit = cfg_ti(SET_TEMP_UNIT);
    n.warm_foot = (uint8_t)cfg_ti(SET_WARMUP_FOOT_PCT);
    n.warm_centre = (uint8_t)cfg_ti(SET_WARMUP_CENTRE_PCT);
    n.cold_centre = (uint8_t)cfg_ti(SET_COLD_HEAT_CENTRE_PCT);
    n.cold_ws = (uint8_t)cfg_ti(SET_COLD_HEAT_WS_PCT);
    n.cool_foot = (uint8_t)cfg_ti(SET_COOLING_FOOT_PCT);
    n.balanced = (uint8_t)cfg_ti(SET_BALANCED_PCT);
    n.deadband = cfg_t(SET_TEMP_DEADBAND);
    n.feet_first_c = cfg_t(SET_FEET_FIRST_BELOW);
    n.auto_ws_rh = cfg_t(SET_AUTO_WS_RH);
    n.auto_full_c = cfg_t(SET_AUTO_FAN_FULL_C);
    n.fallback_heat = (int)cfg_t(SET_SENSOR_FALLBACK_HEAT);
    n.auto_fan_nosens = (int)cfg_t(SET_AUTO_FAN_NO_SENSOR);
    n.coolant_idle = (int)cfg_t(SET_COOLANT_IDLE_PCT);
    n.eco_cap = (int)cfg_t(SET_ECO_FAN_CAP);
    n.cold_below = cfg_t(SET_COLD_START_BELOW);
    n.cold_min_ms = (uint32_t)cfg_t(SET_COLD_START_MIN_S) * 1000u;
    n.cold_max_ms = (uint32_t)cfg_t(SET_COLD_START_MAX_MIN) * 60000u;
    n.cold_end_c = cfg_t(SET_COLD_START_END_C);
    n.cold_end_rh = cfg_t(SET_COLD_START_END_RH);
    n.cold_duct_c = cfg_t(SET_COLD_START_DUCT_C);
    n.defog_release = cfg_t(SET_DEFOG_RELEASE_C);
    n.defog_ws = (int)cfg_t(SET_DEFOG_WS_PCT);
    n.heat_gain = cfg_t(SET_HEAT_GAIN);
    n.heat_gain_min = cfg_t(SET_HEAT_GAIN_MIN);
    n.heat_gain_max = fmaxf(n.heat_gain_min, cfg_t(SET_HEAT_GAIN_MAX));   // the settings rule already keeps this
    n.ac_min_off_ms = (uint32_t)cfg_t(SET_AC_MIN_OFF_S) * 1000u;
    n.ac_min_on_ms = (uint32_t)cfg_t(SET_AC_MIN_ON_S) * 1000u;
    if (thermal_adapt.sample_count == 0) {
        thermal_adapt.adaptive_coolant_gain = n.heat_gain;  // nothing learned yet: the base gain
    }
    for (int m = 0; m < SETTINGS_AIR_MODES; m++) {
        for (int v = 0; v < SETTINGS_AIR_VALVES; v++) {
            n.air[m][v] = (uint8_t)cfg_t(SETTINGS_AIR_ID(m, v));
        }
    }
    if (cfg_loaded && memcmp(n.air, cfg.air, sizeof(n.air)) != 0) {
        air_changed = true;
    }

    if (!cfg_loaded || n.control_mode != cfg.control_mode) {
        cascade_reset(&cascade);
        if (cfg_loaded) {
            event_log_add(EVT_INFO, "Control mode: %s", n.control_mode == SET_CONTROL_CASCADE ? "Cascade" : "Classic");
        }
    }
    if (!cfg_loaded || n.unit != cfg.unit) {
        hvac_state.temp_unit = n.unit == SET_UNIT_F ? TEMP_UNIT_FAHRENHEIT : TEMP_UNIT_CELSIUS;
    }
    cfg = n;
    cfg_loaded = true;
    hvac_state.control_mode = (uint8_t)n.control_mode;
}

static bool is_defrost(hvac_mode_t mode)
{
    return mode == HVAC_MODE_DEFROST || mode == HVAC_MODE_DEFROST_FOOT;
}

static uint8_t fan_level_pct(hvac_fan_speed_t level)
{
    switch (level) {
        case HVAC_FAN_LOW: case HVAC_FAN_MEDIUM_LOW: case HVAC_FAN_MEDIUM:
        case HVAC_FAN_MEDIUM_HIGH: case HVAC_FAN_HIGH:   // Low = fan minimum, High = fan maximum
            return (uint8_t)lroundf(settings_operand_value(&g_fan_levels[level - HVAC_FAN_LOW]));
        case HVAC_FAN_AUTO:        return (uint8_t)cfg.auto_fan_nosens;
        default:                   return BTS7960_FAN_PERCENT_OFF;
    }
}

/**
 * @brief Send a blower command through the fan min/max clamp, the quiet-night cap and (AUTO fan
 * under the ECO preset) the Eco cap, ramped by SET_SOFT_START_MS. Sends only on change.
 */
static void blower_command(uint8_t pct, bool auto_fan)
{
    if (pct > 0) {
        if (pct < cfg.fan_min) pct = (uint8_t)cfg.fan_min;
        if (pct > cfg.fan_max) pct = (uint8_t)cfg.fan_max;
        if (cfg.night_cap > 0 && cfg.night && pct > cfg.night_cap) pct = (uint8_t)cfg.night_cap;
        if (auto_fan && hvac_state.preset == HVAC_PRESET_ECO && pct > cfg.eco_cap) pct = (uint8_t)cfg.eco_cap;
    }
    if (pct != bts7960_get_target_speed()) {
        bts7960_set_speed_ramp(pct, cfg.soft_start_ms);
    }
    hvac_state.blower_speed = pct;
}

/*******************************************************************************
 * Private Functions
 ******************************************************************************/

/**
 * @brief Set A/C compressor relay state
 * @param on true to turn on compressor, false to turn off
 */
static bool ac_relay_on = false;    // Relay level last acknowledged by the node
static uint32_t ac_changed_ms = 0;  // When the relay last changed (minimum on/off times)

static int ac_relay_level(bool on)
{
    return on ? (AC_RELAY_ACTIVE_HIGH ? 1 : 0) : (AC_RELAY_ACTIVE_HIGH ? 0 : 1);
}

static void set_ac_compressor(bool on)
{
    bool last_state = ac_relay_on;

    if (on == last_state) {
        return;  // No change needed
    }

    int level = ac_relay_level(on);
    esp_err_t ret = remote_gpio_set_level(AC_RELAY_PIN, level);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set A/C relay: %s", esp_err_to_name(ret));
        return;
    }
    
    ac_relay_on = on;
    ac_changed_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    hvac_state.ac_compressor_on = on;
    ESP_LOGI(TAG, "A/C compressor %s", on ? "ON" : "OFF");
}

/**
 * @brief Re-send relay level and blower duty once a second.
 * They are otherwise only sent on change; after a node reset or link-watchdog trip
 * (all outputs off) the node would stay off while the UI shows them running.
 */
static void reassert_outputs(void)
{
    static uint32_t last_ms = 0;
    uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    if (now_ms - last_ms < CTRL_REASSERT_MS) {
        return;
    }
    last_ms = now_ms;
    remote_gpio_set_level(AC_RELAY_PIN, ac_relay_level(ac_relay_on));
    bts7960_refresh();
}

/**
 * @brief Initialize A/C compressor relay GPIO
 */
static esp_err_t init_ac_compressor_gpio(void)
{
    esp_err_t ret = remote_gpio_set_mode(AC_RELAY_PIN, REMOTE_GPIO_OUTPUT);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init A/C relay GPIO: %s", esp_err_to_name(ret));
        return ret;
    }
    // Start with A/C off
    set_ac_compressor(false);
    return ESP_OK;
}

// Called by every setter of a persisted setting
static void notify_state_change(void)
{
    mark_settings_dirty();
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
        thermal_adapt.adaptive_coolant_gain = cfg.heat_gain / fmaxf(0.5f, gain_normalizer);
        if (recirc) {
            thermal_adapt.adaptive_coolant_gain *= 0.9f;  // Recirc improves effectiveness
        }
        thermal_adapt.adaptive_coolant_gain *= (1.0f - ((float)blower_speed / 400.0f));
        
        // Clamp gain to reasonable limits
        if (thermal_adapt.adaptive_coolant_gain < cfg.heat_gain_min) {
            thermal_adapt.adaptive_coolant_gain = cfg.heat_gain_min;
        } else if (thermal_adapt.adaptive_coolant_gain > cfg.heat_gain_max) {
            thermal_adapt.adaptive_coolant_gain = cfg.heat_gain_max;
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

// FACE..DEFROST_FOOT take their air valves from the grid (SET_AIR_*); AUTO and OFF do not
static bool is_grid_mode(hvac_mode_t mode)
{
    return mode >= HVAC_MODE_FACE && mode <= HVAC_MODE_DEFROST_FOOT;
}

static uint8_t air_pct(hvac_mode_t mode, hvac_valve_id_t valve)
{
    return cfg.air[mode - HVAC_MODE_FACE][valve];
}

static void update_valve_positions(hvac_mode_t mode)
{
    // OFF closes everything; AUTO starts balanced and then runs its own law
    uint8_t pos[SETTINGS_AIR_VALVES] = { 0 };
    if (mode == HVAC_MODE_AUTO) {
        pos[HVAC_VALVE_CENTER_1] = pos[HVAC_VALVE_CENTER_2] = pos[HVAC_VALVE_FOOT] = cfg.balanced;
    } else if (is_grid_mode(mode)) {
        for (int v = 0; v < SETTINGS_AIR_VALVES; v++) {
            pos[v] = air_pct(mode, (hvac_valve_id_t)v);
        }
    }
    for (int v = 0; v < SETTINGS_AIR_VALVES; v++) {
        tb6612fng_set_valve_position((hvac_valve_id_t)v, pos[v]);
    }

    hvac_state.center_valve_pos = pos[HVAC_VALVE_CENTER_1];
    hvac_state.foot_valve_pos = pos[HVAC_VALVE_FOOT];
    hvac_state.windscreen_valve_pos = pos[HVAC_VALVE_WINDSCREEN];
}

// Valve targets decided in this control cycle (-1 = no request). Rules write in order,
// later ones override earlier ones; the result is applied once at the end of the cycle,
// so a valve never gets two different targets 100 ms apart.
static int16_t cycle_targets[HVAC_VALVE_MAX];

static void want_valve(hvac_valve_id_t valve, uint8_t position)
{
    cycle_targets[valve] = position;
}

static void apply_cycle_targets(void)
{
    for (int v = 0; v < HVAC_VALVE_MAX; v++) {
        if (cycle_targets[v] >= 0) {
            tb6612fng_set_valve_position((hvac_valve_id_t)v, (uint8_t)cycle_targets[v]);
        }
    }
}

static void run_temperature_control(void)
{
    for (int v = 0; v < HVAC_VALVE_MAX; v++) {
        cycle_targets[v] = -1;
    }

    // Get current sensor readings
    hvac_sensor_readings_t readings;
    hvac_sensors_get_all(&readings);
    
    hvac_state.cabin_temp_c = readings.cabin_temp_c;
    hvac_state.cabin_humidity = readings.cabin_humidity;
    hvac_state.duct_temp_c = readings.duct_temp_c;
    
    // Get valve position feedback
    hvac_state.center_valve_pos = readings.center_valve_pos;
    hvac_state.center2_valve_pos = readings.center2_valve_pos;
    hvac_state.foot_valve_pos = readings.foot_valve_pos;
    hvac_state.windscreen_valve_pos = readings.windscreen_valve_pos;
    hvac_state.coolant_valve_pos = readings.coolant_valve_pos;
    hvac_state.blower_speed = bts7960_get_target_speed();

    static bool sensor_fallback = false;    // S-08 fallback running (logged once per episode)
    uint32_t cycle_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

    // Settings cache: re-read once a second as a safety net for a missing apply callback
    static uint32_t last_cfg_ms = 0;
    if (cycle_ms - last_cfg_ms >= 1000) {
        last_cfg_ms = cycle_ms;
        cfg_refresh();
    }

    // Recirc auto-cancel (only when a recirc flap is fitted)
    static bool prev_recirc = false;
    static uint32_t recirc_since_ms = 0;
    if (hvac_state.recirculate && !prev_recirc) {
        recirc_since_ms = cycle_ms;
    }
    prev_recirc = hvac_state.recirculate;
    if (hvac_state.recirculate && cfg.recirc_fitted && cfg.recirc_cancel_min > 0 &&
        cycle_ms - recirc_since_ms >= (uint32_t)cfg.recirc_cancel_min * 60000u) {
        hvac_state.recirculate = false;
        prev_recirc = false;
        event_log_add(EVT_INFO, "Recirc auto-cancelled after %d min", cfg.recirc_cancel_min);
        notify_state_change();
    }

    // Extra sensors and the cabin dew point (informational in every mode)
    hvac_state.outside_temp_c = sensor_roles_get_temp(SENSOR_ROLE_OUTSIDE);
    hvac_state.coolant_temp_c = sensor_roles_get_temp(SENSOR_ROLE_COOLANT);
    hvac_state.dew_point_c = comfort_dew_point_c(hvac_state.cabin_temp_c, hvac_state.cabin_humidity);

    // NOT_FOUND in the first seconds after boot means "not read yet", not a fault
    bool boot_grace = (xTaskGetTickCount() * portTICK_PERIOD_MS) < 10000;
    hvac_state.sensor_fault =
        (readings.cabin_status != SENSOR_STATUS_OK && !(boot_grace && readings.cabin_status == SENSOR_STATUS_NOT_FOUND)) ||
        (readings.duct_status != SENSOR_STATUS_OK && !(boot_grace && readings.duct_status == SENSOR_STATUS_NOT_FOUND));

    uint8_t fault_mask = 0;
    for (int valve = 0; valve < HVAC_VALVE_MAX; valve++) {
        if (tb6612fng_valve_has_fault((hvac_valve_id_t)valve)) {
            fault_mask |= (1u << valve);
        }
    }
    hvac_state.valve_fault_mask = fault_mask;
    hvac_state.valve_fault = (fault_mask != 0);
    
    // Air distribution grid edited: a manual mode moves to the new values now (defog below
    // still raises the windscreen in this cycle)
    if (air_changed) {
        air_changed = false;
        if (is_grid_mode(hvac_state.mode)) {
            for (int v = 0; v < SETTINGS_AIR_VALVES; v++) {
                want_valve((hvac_valve_id_t)v, air_pct(hvac_state.mode, (hvac_valve_id_t)v));
            }
        }
    }

    // Skip control in OFF mode (set_mode(OFF) already stopped everything)
    if (hvac_state.mode == HVAC_MODE_OFF) {
        defog.active = false;
        hvac_state.defog_active = false;
        hvac_state.heater_delay_active = false;
        hvac_state.duct_target_c = NAN;
        strcpy(hvac_state.ac_reason, "System OFF");
        return;
    }

    // Dew-point defog (needs the outside sensor; without it the feature stays inactive)
    bool defog_was_active = defog.active;
    if (cfg.defog_on) {
        comfort_defog_update(&defog, hvac_state.dew_point_c,
                             comfort_glass_temp_c(hvac_state.outside_temp_c, cfg.defog_offset),
                             cfg.defog_margin, cfg.defog_release);
    } else {
        defog.active = false;
    }
    hvac_state.defog_active = defog.active;
    if (defog.active != defog_was_active) {
        ESP_LOGI(TAG, "Dew-point defog %s (dew=%.1f, outside=%.1f)", defog.active ? "ON" : "OFF",
                 hvac_state.dew_point_c, hvac_state.outside_temp_c);
    }

    // Heater delay: AUTO mode with automatic fan only (defrost modes are never AUTO)
    hvac_state.heater_delay_active = false;
    if (!cfg.hd_on || hvac_state.mode != HVAC_MODE_AUTO) {
        heater_delay_pending = false;
        heater_delay.active = false;
    } else {
        float heater_c = isnan(hvac_state.coolant_temp_c) ? hvac_state.duct_temp_c : hvac_state.coolant_temp_c;
        if (heater_delay_pending) {
            heater_delay_pending = false;
            comfort_heater_delay_start(&heater_delay, heater_c, cfg.hd_temp, cycle_ms);
            if (heater_delay.active) {
                ESP_LOGI(TAG, "Heater delay: fan held until heater >= %.0f C (heater=%.1f)", cfg.hd_temp, heater_c);
            }
        } else if (heater_delay.active &&
                   !comfort_heater_delay_update(&heater_delay, heater_c, cfg.hd_temp, cfg.hd_max_ms, cycle_ms)) {
            ESP_LOGI(TAG, "Heater delay: released (heater=%.1f)", heater_c);
        }
        hvac_state.heater_delay_active = heater_delay.active && hvac_state.fan_level == HVAC_FAN_AUTO;
    }

    // Manual fan: re-applied through the clamps so night/min/max changes take effect
    if (hvac_state.fan_level != HVAC_FAN_AUTO) {
        blower_command(fan_level_pct(hvac_state.fan_level), false);
    }

    // No cabin temperature (failed or stale): fixed fallback instead of freezing the last outputs
    if (isnan(hvac_state.cabin_temp_c)) {
        hvac_state.is_heating = false;
        hvac_state.is_cooling = false;
        hvac_state.temp_reached = false;
        warmup_state.active = false;
        hvac_state.duct_target_c = NAN;
        if (!sensor_fallback && !boot_grace) {
            sensor_fallback = true;
            event_log_add(EVT_FAULT, "Cabin sensor lost: fallback");
        }
        want_valve(HVAC_VALVE_COOLANT, (uint8_t)cfg.fallback_heat);
        if (hvac_state.fan_level == HVAC_FAN_AUTO) {
            blower_command((uint8_t)cfg.auto_fan_nosens, true);
        }
        goto compressor;   // A/C rules that need no cabin reading (defrost, user request) still apply
    }

    sensor_fallback = false;
    float error = hvac_state.target_temp_c - hvac_state.cabin_temp_c;
    
    // Check if target reached (within deadband)
    hvac_state.temp_reached = (fabsf(error) <= cfg.deadband);
    
    // Cascade replaces only the coolant-valve law; heating/cooling flags stay deadband based
    bool cascade_on = (cfg.control_mode == SET_CONTROL_CASCADE);
    if (cascade_on) {
        float dt_s = (cycle_ms - cascade_last_ms) / 1000.0f;
        if (cascade_last_ms == 0 || dt_s <= 0.0f || dt_s > 1.0f) {
            dt_s = HVAC_TASK_PERIOD_MS / 1000.0f;   // first step, or the loop was not running
        }
        cascade_last_ms = cycle_ms ? cycle_ms : 1;
        float v = cascade_step(&cascade, &cfg.casc, hvac_state.target_temp_c, hvac_state.cabin_temp_c,
                               hvac_state.duct_temp_c, dt_s);
        want_valve(HVAC_VALVE_COOLANT, (uint8_t)(v + 0.5f));
        hvac_state.duct_target_c = cascade.duct_target_c;
    } else {
        hvac_state.duct_target_c = NAN;
    }

    // Determine heating or cooling
    if (error > cfg.deadband) {
        // Need heating
        hvac_state.is_heating = true;
        hvac_state.is_cooling = false;
        
        // Calculate coolant valve position using adaptive gain
        float adaptive_gain = thermal_adapt.adaptive_coolant_gain;
        uint8_t valve_pos = (uint8_t)fminf(100.0f, fabsf(error) * adaptive_gain);
        if (!cascade_on) {
            want_valve(HVAC_VALVE_COOLANT, valve_pos);
        }
        
        if (thermal_adapt.sample_count < 10) {
            ESP_LOGD(TAG, "Heating: error=%.2f°C, gain=%.1f, valve=%d%%, learning=%d samples",
                     error, adaptive_gain, valve_pos, thermal_adapt.sample_count);
        }
    } else if (error < -cfg.deadband) {
        // Need cooling
        hvac_state.is_heating = false;
        hvac_state.is_cooling = true;
        
        // Close coolant valve for cooling
        if (!cascade_on) {
            want_valve(HVAC_VALVE_COOLANT, 0);
        }
    } else {
        // Temperature reached
        hvac_state.is_heating = false;
        hvac_state.is_cooling = false;
        
        // Maintain minimal coolant flow
        if (!cascade_on) {
            want_valve(HVAC_VALVE_COOLANT, (uint8_t)cfg.coolant_idle);
        }
    }
    
    // Update thermal adaptation learning
    update_thermal_adaptation(hvac_state.cabin_temp_c, hvac_state.is_heating, hvac_state.is_cooling,
                              hvac_state.blower_speed, hvac_state.recirculate);
    
    // Auto fan control
    if (hvac_state.fan_level == HVAC_FAN_AUTO) {
        // Compute a continuous blower speed between LOW and 100% based on temperature error
        float abs_error = fabsf(error);
        float min_speed = (float)cfg.fan_min;  // = manual LOW
        float auto_speed_f;

        if (abs_error <= cfg.deadband) {
            // Very close to target: stay at minimum auto speed
            auto_speed_f = min_speed;
        } else if (abs_error >= cfg.auto_full_c) {
            // Very large error: full speed
            auto_speed_f = 100.0f;
        } else {
            // Linearly scale between min_speed and 100% as error grows (here deadband < full)
            float t = (abs_error - cfg.deadband) / (cfg.auto_full_c - cfg.deadband);
            if (t < 0.0f) t = 0.0f;
            if (t > 1.0f) t = 1.0f;
            auto_speed_f = min_speed + t * (100.0f - min_speed);
        }
        // Curve strength scales the slope above the minimum (NORMAL = 1.0 = unchanged)
        auto_speed_f = fminf(100.0f, min_speed + (auto_speed_f - min_speed) * cfg.curve_k);

        uint8_t auto_speed = (uint8_t)(auto_speed_f + 0.5f); // round to nearest
        if (hvac_state.heater_delay_active) {
            auto_speed = fan_level_pct((hvac_fan_speed_t)cfg.hd_fan);   // heater still cold: hold low
        }
        blower_command(auto_speed, true);
    }
    
    // Auto mode adjustments
    if (hvac_state.mode == HVAC_MODE_AUTO) {
        uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        
        // =====================================================================
        // COLD-START WARMUP PHASE
        // When entering AUTO mode with a cold cabin, start with aggressive 
        // defrost to prevent/clear windscreen fogging, then transition to 
        // normal comfort operation.
        // =====================================================================
        
        // Check if we should START warmup phase
        if (!warmup_state.active && hvac_state.cabin_temp_c < cfg.cold_below) {
            // Cold cabin detected - start warmup phase
            warmup_state.active = true;
            warmup_state.start_time_ms = now_ms;
            warmup_state.initial_cabin_temp = hvac_state.cabin_temp_c;
            warmup_state.initial_humidity = hvac_state.cabin_humidity;
            ESP_LOGI(TAG, "AUTO: Cold start detected (cabin=%.1f°C, humidity=%.1f%%), starting warmup phase",
                     hvac_state.cabin_temp_c, hvac_state.cabin_humidity);
        }
        
        // Check if we should EXIT warmup phase
        if (warmup_state.active) {
            uint32_t elapsed_ms = now_ms - warmup_state.start_time_ms;
            bool min_time_passed = (elapsed_ms >= cfg.cold_min_ms);
            bool max_time_reached = (elapsed_ms >= cfg.cold_max_ms);
            bool cabin_warm = (hvac_state.cabin_temp_c >= cfg.cold_end_c);
            bool humidity_ok = (!isnan(hvac_state.cabin_humidity) && 
                               hvac_state.cabin_humidity < cfg.cold_end_rh);
            bool duct_warm = (!isnan(hvac_state.duct_temp_c) && 
                             hvac_state.duct_temp_c >= cfg.cold_duct_c);
            
            // Exit conditions (after minimum time):
            // 1. Max time reached (force exit)
            // 2. Cabin warm enough AND humidity acceptable
            // 3. Cabin warm enough AND duct is hot (heater ready)
            bool should_exit = max_time_reached || 
                              (min_time_passed && cabin_warm && humidity_ok) ||
                              (min_time_passed && cabin_warm && duct_warm);
            
            if (should_exit) {
                warmup_state.active = false;
                ESP_LOGI(TAG, "AUTO: Warmup complete after %lus (cabin=%.1f°C, humidity=%.1f%%, duct=%.1f°C)",
                         elapsed_ms / 1000, hvac_state.cabin_temp_c, 
                         hvac_state.cabin_humidity, hvac_state.duct_temp_c);
            }
        }
        
        // Apply warmup or normal AUTO behavior
        if (warmup_state.active) {
            // *** WARMUP PHASE: Aggressive defrost mode ***
            // Full heat + windscreen focus + A/C for dehumidification
            
            // Full heat output
            want_valve(HVAC_VALVE_COOLANT, 100);
            
            // Air distribution: prioritize windscreen, some to feet for comfort
            want_valve(HVAC_VALVE_WINDSCREEN, 100);
            want_valve(HVAC_VALVE_FOOT, cfg.warm_foot);
            want_valve(HVAC_VALVE_CENTER_1, cfg.warm_centre);
            want_valve(HVAC_VALVE_CENTER_2, cfg.warm_centre);

            // Override fan to high speed during warmup (if in AUTO fan mode)
            if (hvac_state.fan_level == HVAC_FAN_AUTO && !hvac_state.heater_delay_active) {
                blower_command(BTS7960_FAN_PERCENT_HIGH, true);
            }
            
            // Log warmup progress periodically
            static uint32_t last_warmup_log = 0;
            if (now_ms - last_warmup_log >= 10000) {  // Every 10 seconds
                last_warmup_log = now_ms;
                uint32_t elapsed = (now_ms - warmup_state.start_time_ms) / 1000;
                ESP_LOGI(TAG, "AUTO warmup: %lus elapsed, cabin=%.1f°C, humidity=%.1f%%, duct=%.1f°C",
                         elapsed, hvac_state.cabin_temp_c, hvac_state.cabin_humidity, hvac_state.duct_temp_c);
            }
        } else {
            // *** NORMAL AUTO OPERATION ***
            // Adjust air distribution based on conditions
            if (hvac_state.is_heating && hvac_state.cabin_temp_c < cfg.feet_first_c) {
                // Cold cabin - more to feet
                want_valve(HVAC_VALVE_CENTER_1, cfg.cold_centre);
                want_valve(HVAC_VALVE_CENTER_2, cfg.cold_centre);
                want_valve(HVAC_VALVE_FOOT, 100);
                want_valve(HVAC_VALVE_WINDSCREEN, cfg.cold_ws);
            } else if (hvac_state.is_cooling) {
                // Cooling - more to face
                want_valve(HVAC_VALVE_CENTER_1, 100);
                want_valve(HVAC_VALVE_CENTER_2, 100);
                want_valve(HVAC_VALVE_FOOT, cfg.cool_foot);
                want_valve(HVAC_VALVE_WINDSCREEN, 0);
            } else {
                // Temperature reached - balanced distribution
                want_valve(HVAC_VALVE_CENTER_1, cfg.balanced);
                want_valve(HVAC_VALVE_CENTER_2, cfg.balanced);
                want_valve(HVAC_VALVE_FOOT, cfg.balanced);
                want_valve(HVAC_VALVE_WINDSCREEN, 0);
            }
            
            // Auto defrost if high humidity (even after warmup)
            if (hvac_state.cabin_humidity > cfg.auto_ws_rh) {
                want_valve(HVAC_VALVE_WINDSCREEN, 50);
            }
        }
    }
    
compressor:
    // Dew-point defog: windscreen valve to at least SET_DEFOG_WS_PCT after the mode rules; on release a
    // manual mode gets its own windscreen value back (AUTO sets it every cycle)
    if (defog.active) {
        int16_t ws = cycle_targets[HVAC_VALVE_WINDSCREEN] >= 0
                     ? cycle_targets[HVAC_VALVE_WINDSCREEN]
                     : tb6612fng_get_valve_target(HVAC_VALVE_WINDSCREEN);
        if (ws < cfg.defog_ws) {
            want_valve(HVAC_VALVE_WINDSCREEN, (uint8_t)cfg.defog_ws);
        }
    } else if (defog_was_active && hvac_state.mode != HVAC_MODE_AUTO) {
        want_valve(HVAC_VALVE_WINDSCREEN, air_pct(hvac_state.mode, HVAC_VALVE_WINDSCREEN));
    }

    // =========================================================================
    // A/C Compressor Control
    // =========================================================================
    // The A/C removes moisture from the air, essential for defogging in humid/cold conditions.
    // Run A/C when:
    // 1. User has enabled A/C (ac_enabled) and system is not in OFF mode
    // 2. In DEFROST or DEFROST_FOOT mode (always run A/C to defog)
    // 3. AUTO mode with high humidity (above threshold)
    // 4. AUTO mode warmup phase (always run A/C to prevent initial fogging)
    // 
    // For Danish truck with limited fresh air: A/C dehumidifies recirculated air,
    // which combined with heat creates warm, dry air for effective defrost.
    // =========================================================================
    
    bool should_run_ac = false;
    const char *ac_reason = "OFF";
    
    // Check if mode requires A/C
    bool is_defrost_mode = (hvac_state.mode == HVAC_MODE_DEFROST || 
                            hvac_state.mode == HVAC_MODE_DEFROST_FOOT);
    
    // AUTO mode warmup phase: Always run A/C for dehumidification
    if (hvac_state.mode == HVAC_MODE_AUTO && warmup_state.active) {
        should_run_ac = true;
        ac_reason = "AUTO warmup";
    }
    
    // Defrost modes: Always run A/C for defogging, regardless of ac_enabled setting
    // Heat + A/C is the most effective defogging combination
    if (is_defrost_mode) {
        should_run_ac = true;
        ac_reason = "DEFROST mode";
        // Also ensure coolant valve is fully open for maximum heat during defrost
        want_valve(HVAC_VALVE_COOLANT, 100);
    }
    
    // Auto mode: Run A/C when humidity is high (proactive defogging), with hysteresis
    static bool humidity_high = false;
    if (isnan(hvac_state.cabin_humidity) || hvac_state.cabin_humidity < cfg.hum_off) {
        humidity_high = false;
    } else if (hvac_state.cabin_humidity > cfg.hum_on) {
        humidity_high = true;
    }
    bool humidity_request = (hvac_state.mode == HVAC_MODE_AUTO && humidity_high);
    if (humidity_request) {
        should_run_ac = true;
        ac_reason = "High humidity";
    }

    // Dew-point defog and AUTO cooling ask for the compressor; the coolant valve keeps following
    // the heating law meanwhile (reheat), nothing here closes it
    bool defog_request = defog.active && cfg.defog_uses_ac;
    if (defog_request) {
        should_run_ac = true;
        ac_reason = "Dew-point defog";
    }
    bool auto_cool_request = cfg.auto_requests_ac && hvac_state.mode == HVAC_MODE_AUTO && hvac_state.is_cooling;
    if (auto_cool_request) {
        should_run_ac = true;
        ac_reason = "AUTO cooling";
    }

    // User-enabled A/C: Run if user toggled A/C on (for cooling or manual dehumidification)
    bool user_request = (hvac_state.ac_enabled && hvac_state.mode != HVAC_MODE_OFF);
    if (user_request) {
        should_run_ac = true;
        ac_reason = "User request";
    }
    bool other_request = user_request || is_defrost_mode || defog_request || auto_cool_request ||
                         (hvac_state.mode == HVAC_MODE_AUTO && warmup_state.active);

    // Interlocks: override every request immediately, regardless of the minimum on-time
    bool interlock = false;
    if (hvac_state.mode == HVAC_MODE_OFF) {
        interlock = true;
        ac_reason = "System OFF";
    } else if (bts7960_get_speed() < AC_MIN_BLOWER_PCT) {
        // No airflow over the evaporator: it ices up
        interlock = true;
        ac_reason = "Interlock: blower below 30 %";
    } else {
        // Crude freeze protection: duct air near 0 C while the heater adds no heat
        static bool freeze_lock = false;
        int16_t coolant_target = cycle_targets[HVAC_VALVE_COOLANT] >= 0
                                 ? cycle_targets[HVAC_VALVE_COOLANT]
                                 : tb6612fng_get_valve_target(HVAC_VALVE_COOLANT);
        if (!isnan(hvac_state.duct_temp_c)) {
            if (hvac_state.duct_temp_c < AC_FREEZE_CUTOUT_C && coolant_target <= 5) {
                freeze_lock = true;
            } else if (hvac_state.duct_temp_c > AC_FREEZE_CUTIN_C) {
                freeze_lock = false;
            }
        }
        if (freeze_lock) {
            interlock = true;
            ac_reason = "Interlock: duct freeze (<3 C)";
        }
    }
    // The minimum on-time is only for the humidity rule, the one request that comes and
    // goes by itself; a user A/C-off, a mode change or an interlock drops the clutch at once.
    static bool on_for_humidity_only = false;
    if (interlock) {
        should_run_ac = false;
    } else {
        // Clutch cycle limits: no restart within the minimum off-time, no humidity stop within the minimum on-time
        uint32_t since_change = xTaskGetTickCount() * portTICK_PERIOD_MS - ac_changed_ms;
        if (should_run_ac && !ac_relay_on && since_change < cfg.ac_min_off_ms) {
            should_run_ac = false;
            ac_reason = "Minimum off-time";
        } else if (!should_run_ac && ac_relay_on && on_for_humidity_only && since_change < cfg.ac_min_on_ms) {
            should_run_ac = true;
            ac_reason = "Minimum on-time (humidity)";
        }
    }
    on_for_humidity_only = should_run_ac && !other_request && (humidity_request || on_for_humidity_only);

    // Log A/C request change with reason
    static bool last_ac_request = false;
    if (should_run_ac != last_ac_request) {
        if (should_run_ac) {
            ESP_LOGI(TAG, "A/C REQUEST: ON - Reason: %s (humidity=%.1f%%)", 
                     ac_reason, hvac_state.cabin_humidity);
        } else {
            ESP_LOGI(TAG, "A/C REQUEST: OFF - Reason: %s", ac_reason);
        }
        last_ac_request = should_run_ac;
    }
    
    // Apply A/C compressor state
    set_ac_compressor(should_run_ac);
    if (strcmp(hvac_state.ac_reason, ac_reason) != 0) {
        snprintf(hvac_state.ac_reason, sizeof(hvac_state.ac_reason), "%s", ac_reason);
    }

    apply_cycle_targets();
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
    hvac_state.duct_temp_c = NAN;
    hvac_state.ac_compressor_on = false;
    hvac_state.preset = HVAC_PRESET_NONE;
    hvac_state.outside_temp_c = NAN;
    hvac_state.coolant_temp_c = NAN;
    hvac_state.dew_point_c = NAN;
    hvac_state.duct_target_c = NAN;
    strcpy(hvac_state.ac_reason, "System OFF");
    cfg_loaded = false;
    cfg_refresh();
    hvac_state.target_temp_c = cfg.default_setpoint;

    // Initialize A/C compressor relay GPIO
    esp_err_t ret = init_ac_compressor_gpio();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "A/C relay GPIO init failed: %s (continuing anyway)", esp_err_to_name(ret));
    }
    
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

bool hvac_controller_get_state(hvac_state_t *state)
{
    if (state == NULL) {
        return false;
    }

    if (xSemaphoreTake(state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        memcpy(state, &hvac_state, sizeof(hvac_state_t));
        xSemaphoreGive(state_mutex);
        return true;
    }
    return false;
}

// *_locked: caller holds state_mutex; they never touch the active preset (the public
// wrappers clear it, apply_preset keeps it)
static void set_mode_locked(hvac_mode_t mode)
{
    hvac_mode_t old_mode = hvac_state.mode;
    hvac_state.mode = mode;

    // DEF forces the compressor: remember the user A/C flag on entry, restore it on leaving
    if (is_defrost(mode) && !is_defrost(old_mode)) {
        ac_mem = hvac_state.ac_enabled;
        ac_mem_valid = true;
    } else if (!is_defrost(mode) && is_defrost(old_mode)) {
        if (ac_mem_valid && cfg.def_restores_ac) {
            hvac_state.ac_enabled = ac_mem;
        }
        ac_mem_valid = false;
    }

    if (old_mode == HVAC_MODE_OFF || mode == HVAC_MODE_OFF) {
        cascade_reset(&cascade);
    }
    heater_delay.active = false;
    heater_delay_pending = (mode == HVAC_MODE_AUTO && old_mode != HVAC_MODE_AUTO);

    // Reset warmup state when leaving AUTO mode or entering AUTO fresh
    if (old_mode == HVAC_MODE_AUTO && mode != HVAC_MODE_AUTO) {
        // Leaving AUTO - reset warmup
        warmup_state.active = false;
        ESP_LOGI(TAG, "Left AUTO mode, warmup reset");
    } else if (mode == HVAC_MODE_AUTO && old_mode != HVAC_MODE_AUTO) {
        // Entering AUTO - reset warmup so it can detect cold start
        warmup_state.active = false;
        warmup_state.start_time_ms = 0;
        warmup_state.initial_cabin_temp = NAN;
        warmup_state.initial_humidity = NAN;
        // AUTO climate starts with the automatic fan; the fan slider switches it back to manual
        hvac_state.fan_level = HVAC_FAN_AUTO;
        ESP_LOGI(TAG, "Entered AUTO mode, warmup state reset, fan AUTO");
    }

    if (mode == HVAC_MODE_OFF) {
        // Turn off everything
        bts7960_stop(cfg.soft_start_ms);
        tb6612fng_stop_all_valves();
        set_ac_compressor(false);  // Turn off A/C when system is OFF
        hvac_state.blower_speed = 0;
        warmup_state.active = false;  // Also reset warmup
    } else {
        // Enable motor drivers
        tb6612fng_enable_all();
        bts7960_enable();

        // A mode change is the user retry for valves latched after repeated stalls
        for (int v = 0; v < HVAC_VALVE_MAX; v++) {
            tb6612fng_clear_stall((hvac_valve_id_t)v);
        }

        // Set valve positions for mode
        update_valve_positions(mode);

        // Set fan if not already set
        if (hvac_state.fan_level == HVAC_FAN_OFF) {
            hvac_state.fan_level = HVAC_FAN_MEDIUM;
            blower_command(fan_level_pct(HVAC_FAN_MEDIUM), false);
        }
    }

    ESP_LOGI(TAG, "Mode set to: %s", mode_names[mode]);
    notify_state_change();
}

static void set_fan_locked(hvac_fan_speed_t level)
{
    hvac_state.fan_level = level;
    if (level != HVAC_FAN_AUTO) {
        blower_command(fan_level_pct(level), false);
    }
    ESP_LOGI(TAG, "Fan level set to: %s", fan_level_names[level]);
    notify_state_change();
}

static void set_target_locked(float temp_c)
{
    hvac_state.target_temp_c = temp_c;
    ESP_LOGI(TAG, "Target temperature set to: %.1f°C", temp_c);
    notify_state_change();
}

static void set_ac_locked(bool enabled)
{
    hvac_state.ac_enabled = enabled;
    ESP_LOGI(TAG, "A/C %s", enabled ? "enabled" : "disabled");
    notify_state_change();
}

// A user change of mode/setpoint/fan/A/C ends the active preset
static void user_changed(void)
{
    hvac_state.preset = HVAC_PRESET_NONE;
}

esp_err_t hvac_controller_set_mode(hvac_mode_t mode)
{
    if (mode >= HVAC_MODE_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(state_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    user_changed();
    set_mode_locked(mode);
    xSemaphoreGive(state_mutex);
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
    if (xSemaphoreTake(state_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    user_changed();
    set_fan_locked(level);
    xSemaphoreGive(state_mutex);
    return ESP_OK;
}

esp_err_t hvac_controller_set_target_temp(float temp_c)
{
    if (!(temp_c >= TEMP_MIN_C && temp_c <= TEMP_MAX_C)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(state_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    user_changed();
    set_target_locked(temp_c);
    xSemaphoreGive(state_mutex);
    return ESP_OK;
}

static void temp_step(float direction)
{
    if (xSemaphoreTake(state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        float new_temp = roundf((hvac_state.target_temp_c + direction * cfg.step) * 100.0f) / 100.0f;
        if (new_temp >= TEMP_MIN_C - 0.001f && new_temp <= TEMP_MAX_C + 0.001f) {
            hvac_state.target_temp_c = new_temp;
            user_changed();
            notify_state_change();
        }
        xSemaphoreGive(state_mutex);
    }
}

void hvac_controller_temp_up(void)
{
    temp_step(1.0f);
}

void hvac_controller_temp_down(void)
{
    temp_step(-1.0f);
}

void hvac_controller_set_ac(bool enabled)
{
    if (xSemaphoreTake(state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        user_changed();
        ac_mem_valid = false;   // a choice made inside DEF stands; nothing to restore
        set_ac_locked(enabled);
        xSemaphoreGive(state_mutex);
    }
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
    thermal_adapt.adaptive_coolant_gain = cfg.heat_gain;
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

void hvac_controller_quick_defrost(void)
{
    ESP_LOGI(TAG, "Quick defrost activated");
    if (xSemaphoreTake(state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        user_changed();
        set_mode_locked(HVAC_MODE_DEFROST);   // remembers the A/C flag from before
        set_fan_locked(HVAC_FAN_HIGH);
        set_ac_locked(true);                  // A/C helps dehumidify; restored when DEF is left
        xSemaphoreGive(state_mutex);
    }

    // Open coolant valve fully for max heat
    tb6612fng_set_valve_position(HVAC_VALVE_COOLANT, 100);
}

esp_err_t hvac_controller_apply_preset(hvac_preset_t preset)
{
    if (preset >= HVAC_PRESET_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(state_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    const preset_def_t *d = preset_def(preset);
    if (d != NULL) {
        hvac_state.preset = preset;   // before the fan command: ECO caps the AUTO blower
        set_mode_locked(d->mode);
        if (d->setpoint_id < SET_COUNT) {
            set_target_locked(settings_get_float(d->setpoint_id));
        }
        set_fan_locked(d->fan);
        set_ac_locked(d->ac);   // after the mode, so DEF keeps the A/C flag from before the preset
        if (d->recirc_on && cfg.recirc_fitted) {
            hvac_state.recirculate = true;
        }
        event_log_add(EVT_INFO, "Preset: %s", hvac_controller_preset_name(preset));
    }
    hvac_state.preset = preset;
    notify_state_change();
    xSemaphoreGive(state_mutex);
    return ESP_OK;
}

hvac_preset_t hvac_controller_get_preset(void)
{
    hvac_preset_t p = HVAC_PRESET_NONE;
    if (xSemaphoreTake(state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        p = hvac_state.preset;
        xSemaphoreGive(state_mutex);
    }
    return p;
}

void hvac_controller_apply_settings(void)
{
    if (state_mutex != NULL && xSemaphoreTake(state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        cfg_refresh();
        if (hvac_state.mode != HVAC_MODE_OFF && hvac_state.fan_level != HVAC_FAN_AUTO) {
            blower_command(fan_level_pct(hvac_state.fan_level), false);
        }
        xSemaphoreGive(state_mutex);
    }
}

void hvac_controller_task(void *pvParameters)
{
    ESP_LOGI(TAG, "HVAC controller task started");
    esp_task_wdt_add(NULL);

    while (1) {
        esp_task_wdt_reset();
        if (xSemaphoreTake(state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            run_temperature_control();
            xSemaphoreGive(state_mutex);
        }
        reassert_outputs();

        // One NVS write per burst of changes (e.g. holding temp +), 5 s after the last one
        if (settings_dirty_ms != 0 &&
            xTaskGetTickCount() * portTICK_PERIOD_MS - settings_dirty_ms >= SETTINGS_SAVE_DELAY_MS) {
            settings_dirty_ms = 0;
            hvac_controller_save_settings();
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
        if (settings.mode >= HVAC_MODE_MAX || settings.fan_level >= HVAC_FAN_MAX ||
            settings.temp_unit > TEMP_UNIT_FAHRENHEIT || !(settings.target_temp_c >= TEMP_MIN_C) ||
            !(settings.target_temp_c <= TEMP_MAX_C)) {
            ESP_LOGW(TAG, "Saved settings out of range, using defaults");
            return ESP_ERR_INVALID_STATE;
        }
        // Applied by hvac_controller_restore_settings() through the setters
        saved_settings = settings;
        have_saved_settings = true;
        ESP_LOGI(TAG, "Settings loaded: mode=%s, fan=%s, target=%.1f°C",
                 mode_names[settings.mode], fan_level_names[settings.fan_level], settings.target_temp_c);
    }

    return ret;
}

void hvac_controller_restore_settings(void)
{
    hvac_controller_apply_settings();
    // The temperature unit follows SET_TEMP_UNIT, not the value saved with the climate state
    if (cfg.powerup == SET_POWERUP_RESTORE && have_saved_settings) {
        hvac_controller_set_target_temp(saved_settings.target_temp_c);
        hvac_controller_set_ac(saved_settings.ac_enabled);
        hvac_controller_set_recirculate(saved_settings.recirculate);
        hvac_controller_set_mode(saved_settings.mode);
        hvac_controller_set_fan_level(saved_settings.fan_level);
    } else {
        hvac_controller_set_target_temp(cfg.default_setpoint);
        if (cfg.powerup != SET_POWERUP_OFF) {
            hvac_controller_set_mode(HVAC_MODE_AUTO);
        }
    }
    settings_dirty_ms = 0;  // what we just applied is what NVS holds
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
