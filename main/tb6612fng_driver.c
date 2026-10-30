/**
 * @file tb6612fng_driver.c
 * @brief TB6612FNG Dual Motor Driver Implementation
 */

#include "tb6612fng_driver.h"
#include "hvac_hw_config.h"
#include "hvac_sensors.h"
#include "settings.h"
#include "event_log.h"
#include "valve_cal_store.h"
#include "motor_link.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>

#include "remote_io.h"

_Static_assert(MOTOR_LINK_REFRESH_MS * 2 <= REMOTE_IO_LINK_WINDOW_MS,
               "valve re-assert at most half the link window (remote_io.h), which is also the node's watchdog");

static const char *TAG = "TB6612FNG";

/*******************************************************************************
 * Motor Channel Configuration
 * Uses standard TB6612_* pin defines, routed to the remote IO node
 ******************************************************************************/
typedef struct {
    int in1;                    // GPIO pin number for IN1
    int in2;                    // GPIO pin number for IN2
    int pwm_pin;                // GPIO pin number for PWM
} motor_config_t;

static const motor_config_t motor_configs[HVAC_VALVE_MAX] = {
    // Center valve 1 (TB6612 #1, Motor A)
    {
        .in1 = TB6612_1_AIN1,
        .in2 = TB6612_1_AIN2,
        .pwm_pin = TB6612_1_PWMA,
    },
    // Center valve 2 (TB6612 #1, Motor B)
    {
        .in1 = TB6612_1_BIN1,
        .in2 = TB6612_1_BIN2,
        .pwm_pin = TB6612_1_PWMB,
    },
    // Foot valve (TB6612 #2, Motor A)
    {
        .in1 = TB6612_2_AIN1,
        .in2 = TB6612_2_AIN2,
        .pwm_pin = TB6612_2_PWMA,
    },
    // Windscreen valve (TB6612 #2, Motor B)
    {
        .in1 = TB6612_2_BIN1,
        .in2 = TB6612_2_BIN2,
        .pwm_pin = TB6612_2_PWMB,
    },
    // Coolant valve (TB6612 #3, Motor A)
    {
        .in1 = TB6612_3_AIN1,
        .in2 = TB6612_3_AIN2,
        .pwm_pin = TB6612_3_PWMA,
    },
};

/*******************************************************************************
 * Private Data
 ******************************************************************************/
static valve_state_t valve_states[HVAC_VALVE_MAX];
static SemaphoreHandle_t valve_mutex = NULL;
static TaskHandle_t valve_task_handle = NULL;

// Time constants for valve control (in ms)
// Tolerance, hysteresis, stall/move/retry times, ramp floors and bands, filter, jog, learn and
// calibration speed/timeout are SERVICE settings (s_vc, tb6612fng_apply_settings)
#define VALVE_TASK_PERIOD_MS        25      // Task update period (faster for better response)
#define VALVE_MOTOR_SPEED           80     // Maximum motor speed (0-255)
#define VALVE_WRAP_BLOCK_MS         5000     // A direction blocked after a pot wrap-around is released after this
#define VALVE_WRAP_CLEAR_NEAR_PCT   9        // ...or as soon as the valve is within this of its target
#define VALVE_MOVEMENT_THRESHOLD    2       // Minimum change considered movement
#define VALVE_DEBUG_LOG_MS          300     // Periodic debug log interval
#define VALVE_PLATEAU_ZONE          5      // Dead zone at 0% and 100% where readings are unreliable
// Butterfly valve mapping: center position (48%) = fully open, both extremes (0%, 100%) = closed
#define VALVE_BUTTERFLY_CENTER_PCT  50      // Default raw ADC % when valve is fully OPEN
#define VALVE_OUTLIER_THRESHOLD     15      // Max allowed jump from filtered value before rejecting
#define VALVE_DEADZONE_ADC          5       // ADC dead zone at extreme positions (0-5% and 95-100%)
#define VALVE_DEADZONE_LOW_PCT      2       // Minimum target clamp (%)
#define VALVE_DEADZONE_HIGH_PCT     2       // Maximum target clamp (%)
#define VALVE_ADC_WRAPAROUND_THRESH 40      // ADC jump larger than this indicates potentiometer wrap-around
// A wrap block cleared "near the target" must clear inside the zone a wrap jump cannot reach
_Static_assert(VALVE_WRAP_CLEAR_NEAR_PCT < VALVE_ADC_WRAPAROUND_THRESH,
               "VALVE_WRAP_CLEAR_NEAR_PCT must stay below VALVE_ADC_WRAPAROUND_THRESH");
#define VALVE_BOUNDARY_SAFE_MARGIN  1       // Safety margin from calibrated boundaries (1% = range 1-99%)
                                            // until settings apply; = SET_SAFE_MARGIN_PCT default (settings_table.c)

// Coolant valve specific settings (linear valve, different motor characteristics)
#define COOLANT_MOTOR_SPEED         255     // Maximum motor speed for coolant valve (0-255)

// Settings snapshot, written by tb6612fng_apply_settings() under valve_mutex
typedef struct {
    uint8_t tol, hyst, retries;             // %, %, count
    uint8_t min_pwm, coolant_min_pwm;       // ramp floors (stiction)
    uint8_t ramp, coolant_ramp;             // error % below which speed ramps down
    uint8_t alpha;                          // position EMA weight of a new reading, /256
    uint8_t jog_pwm, cal_pwm;
    uint32_t stall_ms, move_max_ms, retry_ms, learn_ms, cal_timeout_ms;
} valve_cfg_t;
static valve_cfg_t s_vc;
static volatile bool valve_fitted[HVAC_VALVE_MAX];          // SET_VALVE_FITTED_n
static bool valve_unfit_stop[HVAC_VALVE_MAX];               // just set to not fitted: the valve task stops it once
static bool valve_boot_started;                             // tb6612fng_auto_calibrate_all() has run

// Forward declarations
void tb6612fng_calibration_tick(void);
esp_err_t tb6612fng_start_calibration(hvac_valve_id_t valve_id);

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
    {4, 94, 50, 7, 91, false, true},                            // COOLANT (linear, pre-calibrated 4-94%)
};

// Per-valve motor direction inversion flags (legacy, will be replaced by calibration)
// Set to true if FWD actually DECREASES ADC (inverted wiring)
static bool valve_dir_inverted[HVAC_VALVE_MAX] = {
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
static uint8_t valve_adc_last_stable[HVAC_VALVE_MAX] = {50}; // Last stable ADC reading (for wrap-around detection)
static bool valve_at_boundary[HVAC_VALVE_MAX] = {false};    // True if valve hit a calibrated boundary
static int8_t valve_wraparound_blocked_dir[HVAC_VALVE_MAX] = {0}; // Direction blocked due to wraparound: +1=FWD blocked, -1=REV blocked, 0=none
static uint32_t valve_wrap_block_ms[HVAC_VALVE_MAX];           // When that direction was blocked
static bool valve_filter_init[HVAC_VALVE_MAX] = {false};
static tb6612_direction_t last_motor_dir[HVAC_VALVE_MAX] = {TB6612_DIR_STOP};  // Track last direction for logging
static int8_t valve_target_adc_side[HVAC_VALVE_MAX] = {0};  // Latched V-shape side: +1=upper, -1=lower, 0=undecided
static volatile bool valve_calibrating[HVAC_VALVE_MAX] = {false};  // Per-valve calibration flag
static volatile bool valve_feedback_fault[HVAC_VALVE_MAX] = {false}; // Position reads failing: hold the valve
static uint32_t valve_move_start_ms[HVAC_VALVE_MAX];        // Start of the current move in one direction
static uint8_t valve_stall_count[HVAC_VALVE_MAX];           // Stalls since the last successful arrival
static uint32_t valve_retry_at_ms[HVAC_VALVE_MAX];          // When a stalled valve may retry (0 = latched)
static bool valve_stop_requested[HVAC_VALVE_MAX];           // Set by tb6612fng_stop_valve(), applied by the valve task

// Motor outputs: what each valve should do and what the node acknowledged (motor_link.h).
// Every send records its intent there, so the valve task resends whatever was not
// acknowledged and re-asserts every valve once a second, whichever path sent it.
static motor_link_t motor_link[HVAC_VALVE_MAX];

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
    uint8_t stall_ref_adc;      // Pot reading at the last observed movement
    uint32_t stall_ref_ms;      // Time of the last observed movement
    int16_t pre_wrap_net;       // Net ADC change while rotating FWD, up to the first wrap-around
    bool wrap_seen;             // The rotation has crossed the pot's wrap point
    uint8_t speed;              // SET_CAL_SPEED_PWM and SET_CAL_TIMEOUT_S, latched at the start
    uint32_t timeout_ms;
} valve_calib_progress_t;

static valve_calib_progress_t valve_calib_progress[HVAC_VALVE_MAX] = {0};

// v2.1: settings overrides, calibration source, boot decision and service state
static volatile uint8_t g_safe_margin = VALVE_BOUNDARY_SAFE_MARGIN;
static volatile uint8_t valve_speed_ovr[HVAC_VALVE_MAX];       // 0 = built-in max speed
static volatile uint8_t valve_dir_ovr[HVAC_VALVE_MAX];         // set_valve_dir_t
static valve_cal_source_t valve_cal_source[HVAC_VALVE_MAX] = {
    [HVAC_VALVE_COOLANT] = VALVE_CAL_SRC_FIXED,
};
static volatile bool valve_have_reading[HVAC_VALVE_MAX];       // update_feedback() delivered a value
static volatile bool valve_boot_wait[HVAC_VALVE_MAX];          // boot: waiting for the first reading
static bool valve_boot_force_cal[HVAC_VALVE_MAX];              // boot: calibrate even if a record exists

static volatile bool valve_svc_active[HVAC_VALVE_MAX];         // excluded from normal control
static volatile int8_t valve_svc_jog_dir[HVAC_VALVE_MAX];
static uint32_t valve_svc_jog_ms[HVAC_VALVE_MAX];
static bool valve_svc_recal_req[HVAC_VALVE_MAX];
static bool valve_svc_recal_run[HVAC_VALVE_MAX];
static bool valve_svc_learn_req[HVAC_VALVE_MAX];
static bool valve_svc_learn_run[HVAC_VALVE_MAX];
static uint32_t valve_svc_learn_ms[HVAC_VALVE_MAX];
static uint8_t valve_svc_learn_prev[HVAC_VALVE_MAX];
static int16_t valve_svc_learn_net[HVAC_VALVE_MAX];

static bool valve_effective_inverted(int i)
{
    switch (valve_dir_ovr[i]) {
        case SET_VDIR_NORMAL:   return false;
        case SET_VDIR_INVERTED: return true;
        default:                return valve_calibration[i].calibrated ? valve_calibration[i].is_inverted
                                                                       : valve_dir_inverted[i];
    }
}

static uint8_t valve_max_speed(int i)
{
    if (valve_speed_ovr[i] != 0) {
        return valve_speed_ovr[i];
    }
    return (i == HVAC_VALVE_COOLANT) ? COOLANT_MOTOR_SPEED : VALVE_MOTOR_SPEED;
}

// A speed override is the valve's speed limit: learn, jog and calibration keep their own
// (slower) speeds but never run faster than the override
static uint8_t valve_capped(int i, uint8_t speed)
{
    return (valve_speed_ovr[i] != 0 && valve_speed_ovr[i] < speed) ? valve_speed_ovr[i] : speed;
}

static void valve_save_calibration(int i)
{
    valve_cal_record_t rec = {
        .adc_min = valve_calibration[i].adc_min,
        .adc_max = valve_calibration[i].adc_max,
        .adc_center = valve_calibration[i].adc_center,
        .low_deadzone = valve_calibration[i].adc_low_deadzone,
        .high_deadzone = valve_calibration[i].adc_high_deadzone,
        .is_inverted = valve_calibration[i].is_inverted,
        .speed = valve_speed_ovr[i],
    };
    esp_err_t err = valve_cal_store_save((hvac_valve_id_t)i, &rec);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "V%d: saving calibration failed: %s", i, esp_err_to_name(err));
    }
}

// Forward declarations
static bool any_valve_calibrating(void);
static bool is_valve_enabled(hvac_valve_id_t valve_id);

/*******************************************************************************
 * Private Functions
 ******************************************************************************/

static bool set_motor_direction(hvac_valve_id_t valve_id, const motor_config_t *config, tb6612_direction_t direction, int position)
{
    bool ok = true;
    bool dir_changed = (valve_id < HVAC_VALVE_MAX && last_motor_dir[valve_id] != direction);
    const char *dir_str = "?";
    
    esp_err_t ret1 = ESP_OK, ret2 = ESP_OK;
    
    switch (direction) {
        case TB6612_DIR_STOP:
            ret1 = remote_gpio_set_level(config->in1, 0);
            ret2 = ret1 == ESP_OK ? remote_gpio_set_level(config->in2, 0) : ret1;
            dir_str = "STOP";
            break;
        case TB6612_DIR_FORWARD:
            ret1 = remote_gpio_set_level(config->in1, 1);
            ret2 = ret1 == ESP_OK ? remote_gpio_set_level(config->in2, 0) : ret1;
            dir_str = "FWD";
            break;
        case TB6612_DIR_REVERSE:
            ret1 = remote_gpio_set_level(config->in1, 0);
            ret2 = ret1 == ESP_OK ? remote_gpio_set_level(config->in2, 1) : ret1;
            dir_str = "REV";
            break;
        case TB6612_DIR_BRAKE:
            ret1 = remote_gpio_set_level(config->in1, 1);
            ret2 = ret1 == ESP_OK ? remote_gpio_set_level(config->in2, 1) : ret1;
            dir_str = "BRAKE";
            break;
        default:
            break;
    }
    
    if (ret1 != ESP_OK || ret2 != ESP_OK) {
        ESP_LOGE(TAG, "GPIO set failed: in1=%s, in2=%s",
                 esp_err_to_name(ret1), esp_err_to_name(ret2));
    }
    ok = (ret1 == ESP_OK && ret2 == ESP_OK);

    if (valve_id < HVAC_VALVE_MAX) {
        last_motor_dir[valve_id] = direction;
    }
    
    if (dir_changed) {
        ESP_LOGI(TAG, "Motor dir=%s pos=%d%% (IN1=%d IN2=%d)", dir_str, position, config->in1, config->in2);
    }
    return ok;
}

static bool set_motor_speed(const motor_config_t *config, uint8_t speed)
{
    esp_err_t ret = remote_pwm_set_duty(config->pwm_pin, speed);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PWM set failed for pin %d: %s", config->pwm_pin, esp_err_to_name(ret));
        return false;
    }
    ESP_LOGD(TAG, "PWM pin=%d duty=%d", config->pwm_pin, speed);
    return true;
}

static uint32_t motor_now_ms(void)
{
    return xTaskGetTickCount() * portTICK_PERIOD_MS;
}

// Sends the parts in mask of what the valve should do and records the node's answers
static void motor_send(int valve, unsigned mask, int position)
{
    motor_link_t *m = &motor_link[valve];
    const motor_config_t *config = &motor_configs[valve];
    bool dir_ok = true, speed_ok = true;
    if (mask & MOTOR_SEND_DIR) {
        dir_ok = set_motor_direction((hvac_valve_id_t)valve, config, (tb6612_direction_t)m->want_dir, position);
        if (!dir_ok) {
            mask &= ~MOTOR_SEND_SPEED;      // one failed command per pass is enough; the speed stays pending
        }
    }
    if (mask & MOTOR_SEND_SPEED) {
        speed_ok = set_motor_speed(config, (uint8_t)m->want_speed);
    }
    if (motor_link_sent(m, mask, dir_ok, speed_ok, motor_now_ms())) {
        ESP_LOGW(TAG, "V%d: stop resent after a lost command", valve);
        event_log_add(EVT_FAULT, "V%d stop resent after a lost command", valve);
    }
}

// Direct drive (calibration, service): record the intent and send both parts now
static void motor_now(hvac_valve_id_t valve, tb6612_direction_t direction, uint8_t speed, int position)
{
    motor_link_want(&motor_link[valve], direction, speed);
    motor_send(valve, MOTOR_SEND_DIR | MOTOR_SEND_SPEED, position);
}

// The valve loop decides outputs under valve_mutex and sends them after releasing it
static void queue_motor(int valve, tb6612_direction_t direction, uint8_t speed)
{
    motor_link_want(&motor_link[valve], direction, speed);
}

// Down as soon as any task's last command timed out: the 3 s connected window alone would
// let every valve wait out a 200 ms timeout per command until it closes
static bool valve_link_up(void)
{
    return remote_io_is_connected() && !remote_io_last_command_timed_out();
}

// Sends what is not acknowledged yet, and re-asserts every commanded valve once a second
static void apply_motor(int valve, uint32_t now_ms, bool link_up)
{
    unsigned mask = motor_link_due(&motor_link[valve], now_ms, link_up);
    if (mask) {
        motor_send(valve, mask, valve_states[valve].position);
    }
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


static void valve_service_end(int i, uint32_t now_ms)
{
    valve_svc_active[i] = false;
    valve_svc_jog_dir[i] = 0;
    // The pot may have turned (or wrapped) meanwhile: no stale wrap detection or latched side
    valve_wraparound_blocked_dir[i] = 0;
    valve_at_boundary[i] = false;
    valve_target_adc_side[i] = 0;
    valve_adc_last_stable[i] = valve_adc_pos[i];
    valve_states[i].is_moving = false;
    valve_states[i].last_motion_ms = now_ms;
}

static void valve_service_clear(int i)
{
    valve_svc_active[i] = false;
    valve_svc_jog_dir[i] = 0;
    valve_svc_recal_req[i] = false;
    valve_svc_recal_run[i] = false;
    valve_svc_learn_req[i] = false;
    valve_svc_learn_run[i] = false;
}

// Runs in the valve task under valve_mutex. True = the valve is under service, skip normal control.
// Slow work (NVS, link I/O of a calibration start) is handed out through the masks.
static bool valve_service_tick(int i, uint32_t now_ms, uint32_t *recal_mask, uint32_t *save_mask)
{
    if (!valve_svc_active[i]) {
        return false;
    }
    uint8_t adc = valve_adc_pos[i];

    if (valve_svc_recal_req[i]) {
        valve_svc_recal_req[i] = false;
        valve_svc_recal_run[i] = true;
        *recal_mask |= 1u << i;
        return true;
    }
    if (valve_svc_recal_run[i]) {
        if (valve_calibrating[i] || (*recal_mask & (1u << i))) {
            return true;
        }
        valve_svc_recal_run[i] = false;     // result already in the event log (calibration tick)
        valve_service_end(i, now_ms);
        return false;
    }

    if (valve_svc_learn_req[i]) {
        valve_svc_learn_req[i] = false;
        if (valve_feedback_fault[i] || !valve_have_reading[i]) {
            event_log_add(EVT_CAL, "V%d dir learn failed: no feedback", i);
            valve_service_end(i, now_ms);
            return false;
        }
        valve_svc_learn_run[i] = true;
        valve_svc_learn_ms[i] = now_ms;
        valve_svc_learn_prev[i] = adc;
        valve_svc_learn_net[i] = 0;
        queue_motor(i, TB6612_DIR_FORWARD, valve_capped(i, VALVE_MOTOR_SPEED));
        return true;
    }
    if (valve_svc_learn_run[i]) {
        int d = (int)adc - (int)valve_svc_learn_prev[i];
        valve_svc_learn_prev[i] = adc;
        if (abs(d) <= VALVE_ADC_WRAPAROUND_THRESH) {    // a wrap-around jump says nothing about the direction
            valve_svc_learn_net[i] += d;
        }
        if (valve_feedback_fault[i] || now_ms - valve_svc_learn_ms[i] >= s_vc.learn_ms) {
            int net = valve_svc_learn_net[i];
            queue_motor(i, TB6612_DIR_BRAKE, 0);
            valve_svc_learn_run[i] = false;
            if (valve_feedback_fault[i]) {
                event_log_add(EVT_CAL, "V%d dir learn failed: feedback lost", i);
            } else if (abs(net) < VALVE_MOVEMENT_THRESHOLD) {
                event_log_add(EVT_CAL, "V%d dir learn failed: no movement", i);
            } else {
                bool inv = net > 0;     // FWD raised the ADC
                valve_calibration[i].is_inverted = inv;
                valve_dir_inverted[i] = inv;
                if (valve_calibration[i].calibrated) {
                    *save_mask |= 1u << i;
                }
                event_log_add(EVT_CAL, "V%d dir learned: inverted=%s (%+d%%)", i, inv ? "yes" : "no", net);
            }
            valve_service_end(i, now_ms);
        }
        return true;
    }

    if (valve_svc_jog_dir[i] != 0) {
        if (now_ms - valve_svc_jog_ms[i] >= SERVICE_JOG_TIMEOUT_MS) {
            queue_motor(i, TB6612_DIR_BRAKE, 0);
            valve_service_end(i, now_ms);
            return false;
        }
        queue_motor(i, valve_svc_jog_dir[i] > 0 ? TB6612_DIR_FORWARD : TB6612_DIR_REVERSE, valve_capped(i, s_vc.jog_pwm));
        valve_adc_last_stable[i] = adc;
        return true;
    }

    valve_service_end(i, now_ms);   // active without a job: nothing to wait for
    return false;
}

// Boot decision for one valve, valve task, outside valve_mutex: stored calibration if it still fits
// the pot, else calibrate this valve only (the result is saved by the calibration tick).
static void valve_boot_decide(int i)
{
    valve_cal_record_t rec;
    uint8_t adc = valve_adc_pos[i];
    esp_err_t err = valve_boot_force_cal[i] ? ESP_ERR_INVALID_ARG : valve_cal_store_load((hvac_valve_id_t)i, &rec);
    const char *why;

    if (err == ESP_OK && valve_cal_store_plausible(&rec, adc)) {
        valve_calibration_t *c = &valve_calibration[i];
        c->adc_min = rec.adc_min;
        c->adc_max = rec.adc_max;
        c->adc_center = rec.adc_center;
        c->adc_low_deadzone = rec.low_deadzone;
        c->adc_high_deadzone = rec.high_deadzone;
        c->is_inverted = rec.is_inverted;
        c->calibrated = true;
        valve_cal_source[i] = VALVE_CAL_SRC_STORED;
        valve_filter_init[i] = false;
        valve_adc_last_stable[i] = adc;
        ESP_LOGI(TAG, "V%d: accepted stored (adc %d-%d, center %d, inverted=%s, saved boot #%lu, now adc=%d%%)",
                 i, rec.adc_min, rec.adc_max, rec.adc_center, rec.is_inverted ? "yes" : "no",
                 (unsigned long)rec.boot_counter, adc);
        event_log_add(EVT_CAL, "V%d stored cal used %d-%d", i, rec.adc_min, rec.adc_max);
        return;
    }

    if (valve_boot_force_cal[i])        why = "recalibrate-all requested";
    else if (err == ESP_ERR_NOT_FOUND)  why = "no stored calibration";
    else if (err == ESP_ERR_INVALID_CRC) why = "stored record corrupt";
    else if (err == ESP_OK)             why = "position outside the stored range";
    else                                why = "store unavailable";
    ESP_LOGI(TAG, "V%d: recalibrating: %s (adc=%d%%)", i, why, adc);
    event_log_add(EVT_CAL, "V%d calibrating: %s", i, why);
    if (tb6612fng_start_calibration((hvac_valve_id_t)i) != ESP_OK) {
        valve_cal_source[i] = VALVE_CAL_SRC_DEFAULT;
        event_log_add(EVT_CAL, "V%d calibration not started", i);
    }
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
    for (int i = 0; i < HVAC_VALVE_MAX; i++) {
        motor_link_init(&motor_link[i]);
    }
    tb6612fng_apply_settings();
    
    ESP_LOGI(TAG, "TB6612FNG driver initialized");
    return ESP_OK;
}

void tb6612fng_enable_all(void)
{
    // STBY pins are hardwired to 3V3, always enabled
    ESP_LOGI(TAG, "TB6612FNG drivers always enabled (STBY tied to 3V3)");
}

esp_err_t tb6612fng_set_valve_position(hvac_valve_id_t valve_id, uint8_t position)
{
    if (valve_id >= HVAC_VALVE_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Silently ignore position changes for disabled valves
    if (!is_valve_enabled(valve_id)) {
        return ESP_OK;
    }
    
    if (position > 100) {
        position = 100;
    }

    position = clamp_valve_target(position);
    
    // Use shorter timeout to avoid blocking other tasks for too long
    if (xSemaphoreTake(valve_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        valve_state_t *state = &valve_states[valve_id];
        uint8_t old_target = state->target_position;
        if (abs((int)old_target - (int)position) <= s_vc.tol) {
            xSemaphoreGive(valve_mutex);
            return ESP_OK;
        }
        state->target_position = position;
        // A stall is kept across target changes (AUTO changes targets every few seconds);
        // it clears on its retry timer or via tb6612fng_clear_stall()
        state->last_motion_ms = 0;
        // Reset boundary flag when setting a new target - let valve try to reach it
        valve_at_boundary[valve_id] = false;
        // Reset V-shape side latch when target changes significantly
        // This allows the valve to choose the optimal path to the new target
        valve_target_adc_side[valve_id] = 0;
        // Clear wraparound direction block - new target means fresh attempt
        valve_wraparound_blocked_dir[valve_id] = 0;
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

void tb6612fng_stop_valve(hvac_valve_id_t valve_id)
{
    if (valve_id >= HVAC_VALVE_MAX) {
        return;
    }
    
    // Only the valve task writes motor outputs; it brakes this valve on its next pass.
    // Sending here as well let its own commands interleave with ours on the link.
    if (xSemaphoreTake(valve_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        valve_states[valve_id].stalled = false;
        valve_states[valve_id].target_position = valve_states[valve_id].position;
        valve_states[valve_id].last_motion_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        valve_stall_count[valve_id] = 0;
        valve_retry_at_ms[valve_id] = 0;
        valve_stop_requested[valve_id] = true;
        xSemaphoreGive(valve_mutex);
    } else {
        ESP_LOGE(TAG, "V%d: stop request FAILED - mutex timeout", valve_id);
    }
}

void tb6612fng_stop_all_valves(void)
{
    ESP_LOGI(TAG, "Stopping all valves");
    for (int i = 0; i < HVAC_VALVE_MAX; i++) {
        tb6612fng_stop_valve((hvac_valve_id_t)i);
    }
}

#define VALVE_PASS_EVENT_MS     1000    // a pass this long writes an event (task watchdog is 10 s)
_Static_assert(VALVE_PASS_EVENT_MS * 10 <= CONFIG_ESP_TASK_WDT_TIMEOUT_S * 1000,
               "slow-pass event at most a tenth of the task watchdog (sdkconfig)");

static volatile uint32_t s_pass_max_ms;    // longest pass since tb6612fng_take_max_pass_ms()

uint32_t tb6612fng_take_max_pass_ms(void)
{
    return __atomic_exchange_n(&s_pass_max_ms, 0, __ATOMIC_RELAXED);
}

void tb6612fng_valve_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Valve control task started");
    
    // A hung valve task would leave a motor driving while other tasks keep the link alive
    esp_task_wdt_add(NULL);

    while (1) {
        esp_task_wdt_reset();
        int64_t pass_start_us = esp_timer_get_time();
        uint32_t timeouts_at_start = 0;
        remote_io_get_stats(NULL, NULL, NULL, &timeouts_at_start);

        // Run per-valve calibration state machines (non-blocking)
        tb6612fng_calibration_tick();
        
        if (xSemaphoreTake(valve_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
            uint32_t boot_mask = 0, recal_mask = 0, save_mask = 0;   // slow work done after the mutex is released
            for (int i = 0; i < HVAC_VALVE_MAX; i++) {
                valve_state_t *state = &valve_states[i];

                // Not fitted: skipped; one just set to not fitted is stopped once (calibration and service end)
                if (!is_valve_enabled((hvac_valve_id_t)i)) {
                    if (valve_unfit_stop[i]) {
                        valve_unfit_stop[i] = false;
                        queue_motor(i, TB6612_DIR_BRAKE, 0);
                        state->is_moving = false;
                        state->stalled = false;
                        valve_service_clear(i);
                        valve_boot_wait[i] = false;
                        if (valve_calibrating[i]) {
                            valve_calib_progress[i].state = CALIB_STATE_FAILED;
                        }
                    }
                    continue;
                }

                // Stop requested (e.g. mode OFF): brake now; a running calibration is abandoned
                if (valve_stop_requested[i]) {
                    valve_stop_requested[i] = false;
                    queue_motor(i, TB6612_DIR_BRAKE, 0);
                    state->is_moving = false;
                    valve_service_clear(i);
                    if (valve_calibrating[i]) {
                        ESP_LOGW(TAG, "V%d: Calibration aborted - stop requested", i);
                        valve_calib_progress[i].state = CALIB_STATE_FAILED;
                    }
                    continue;
                }

                // Boot: no control until the first position reading has decided stored-or-calibrate
                if (valve_boot_wait[i]) {
                    if (valve_have_reading[i] && !valve_feedback_fault[i]) {
                        valve_boot_wait[i] = false;
                        boot_mask |= 1u << i;
                    }
                    continue;
                }

                // Service (jog / learn direction / recalibrate): the valve is out of normal control
                if (valve_service_tick(i, now_ms, &recal_mask, &save_mask)) {
                    continue;
                }

                // Skip this valve if it's currently calibrating
                if (valve_calibrating[i]) {
                    continue;
                }

                // No trustworthy position: stop and hold until reads succeed again
                if (valve_feedback_fault[i]) {
                    if (state->is_moving) {
                        queue_motor(i, TB6612_DIR_BRAKE, 0);
                        state->is_moving = false;
                    }
                    continue;
                }

                // Stalled: wait for the retry time; after the last retry stay put until tb6612fng_clear_stall()
                if (state->stalled) {
                    if (valve_retry_at_ms[i] == 0 || (int32_t)(now_ms - valve_retry_at_ms[i]) < 0) {
                        continue;
                    }
                    state->stalled = false;
                    ESP_LOGI(TAG, "V%d: retrying after stall (%d of %d)", i, valve_stall_count[i], s_vc.retries);
                }

                // Get current ADC reading
                uint8_t adc_pos = valve_adc_pos[i];
                
                // A wrap block is temporary: released after 5 s, or once the valve is near its
                // target (a block that outlived its cause kept the valve on the wrong side)
                if (valve_wraparound_blocked_dir[i] != 0) {
                    bool near_target = abs((int)state->target_position - (int)state->position)
                                       <= VALVE_WRAP_CLEAR_NEAR_PCT;
                    if (near_target || now_ms - valve_wrap_block_ms[i] >= VALVE_WRAP_BLOCK_MS) {
                        ESP_LOGI(TAG, "V%d: wrap block released (%s)", i,
                                 near_target ? "near target" : "after 5 s");
                        valve_wraparound_blocked_dir[i] = 0;
                        valve_at_boundary[i] = false;
                    }
                }

                // Wrap-around detection for butterfly valves
                // If ADC jumps by more than threshold, it indicates potentiometer wrap-around
                bool wraparound_detected = false;
                if (i != HVAC_VALVE_COOLANT && valve_calibration[i].calibrated && state->is_moving) {
                    int16_t adc_jump = (int16_t)adc_pos - (int16_t)valve_adc_last_stable[i];
                    if (adc_jump < 0) adc_jump = -adc_jump;
                    
                    if (adc_jump > VALVE_ADC_WRAPAROUND_THRESH) {
                        wraparound_detected = true;
                        valve_at_boundary[i] = true;
                        // Block the direction that caused the wraparound
                        // last_motor_dir tracks what direction we WERE going
                        if (last_motor_dir[i] == TB6612_DIR_FORWARD) {
                            valve_wraparound_blocked_dir[i] = +1;  // Block FWD
                        } else if (last_motor_dir[i] == TB6612_DIR_REVERSE) {
                            valve_wraparound_blocked_dir[i] = -1;  // Block REV
                        }
                        valve_wrap_block_ms[i] = now_ms;
                        ESP_LOGW(TAG, "V%d: ADC wraparound detected! (%d%% -> %d%%), blocking %s direction",
                                 i, valve_adc_last_stable[i], adc_pos,
                                 valve_wraparound_blocked_dir[i] > 0 ? "FWD" : "REV");
                        
                        // Stop motor immediately
                        queue_motor(i, TB6612_DIR_BRAKE, 0);
                        state->is_moving = false;
                        continue;
                    }
                }
                
                // Update last stable ADC for wrap-around detection (only if not a wraparound)
                if (!wraparound_detected) {
                    valve_adc_last_stable[i] = adc_pos;
                }
                
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
                    uint8_t adc_min = cal->calibrated ? cal->adc_min : 0;
                    uint8_t adc_max = cal->calibrated ? cal->adc_max : 100;
                    uint8_t half_range = cal->calibrated ? 
                        (cal->adc_max - cal->adc_min) / 2 : 50;
                    
                    // Calculate safe boundaries with margin to prevent wrap-around
                    uint8_t safe_min = adc_min + g_safe_margin;
                    uint8_t safe_max = (adc_max > g_safe_margin) ? 
                                       adc_max - g_safe_margin : adc_max;
                    
                    // Calculate how far from center the target ADC should be
                    uint8_t target_offset = (100 - state->target_position) * half_range / 100;
                    
                    // Determine which side of V-shape to target (latched to prevent oscillation)
                    // Only decide when side is undecided (0) - once moving, keep the same side
                    int8_t *target_side = &valve_target_adc_side[i];
                    
                    // If a direction is blocked by wraparound, force the opposite side
                    // FWD blocked (+1) means we can't decrease ADC, so target upper side (+1)
                    // REV blocked (-1) means we can't increase ADC, so target lower side (-1)
                    if (valve_wraparound_blocked_dir[i] != 0) {
                        int8_t forced_side = valve_wraparound_blocked_dir[i];  // Same sign: +1→upper, -1→lower
                        if (*target_side != forced_side) {
                            ESP_LOGI(TAG, "V%d: Switching to %s side due to wraparound block",
                                     i, forced_side > 0 ? "upper" : "lower");
                            *target_side = forced_side;
                        }
                    }
                    // If undecided or at center target (100% open), choose based on current position
                    else if (*target_side == 0 || state->target_position >= 98) {
                        // Choose side based on current ADC position (shortest path to target)
                        // Use a small hysteresis band around center to avoid flip-flopping
                        if (adc_pos >= center + 3) {
                            *target_side = +1;  // Upper side
                        } else if (adc_pos <= center - 3) {
                            *target_side = -1;  // Lower side
                        } else if (*target_side == 0) {
                            // In hysteresis band and undecided - pick based on which is closer
                            *target_side = (adc_pos >= center) ? +1 : -1;
                        }
                        // If target_side is already set and we're in hysteresis band, keep current side
                    }
                    
                    // Calculate target ADC based on latched side
                    int16_t target_adc;
                    if (*target_side > 0) {
                        // Target on upper side (above center)
                        target_adc = center + target_offset;
                        // Clamp to safe upper boundary
                        if (target_adc > safe_max) target_adc = safe_max;
                    } else {
                        // Target on lower side (below center)
                        target_adc = center - target_offset;
                        // Clamp to safe lower boundary
                        if (target_adc < safe_min) target_adc = safe_min;
                    }
                    
                    // Additional clamping for safety
                    if (target_adc < safe_min) target_adc = safe_min;
                    if (target_adc > safe_max) target_adc = safe_max;
                    
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
                if (abs_error <= s_vc.tol) {
                    should_stop = true;
                }
                
                // BOUNDARY ENFORCEMENT: For calibrated butterfly valves
                // Only stop if we would move AWAY from center at a boundary
                // Movement toward center is always allowed to escape the boundary
                if (i != HVAC_VALVE_COOLANT && valve_calibration[i].calibrated) {
                    uint8_t adc_pos = valve_adc_pos[i];
                    uint8_t adc_min = valve_calibration[i].adc_min;
                    uint8_t adc_max = valve_calibration[i].adc_max;
                    
                    // Use the safe boundary margin
                    uint8_t safe_min = adc_min + g_safe_margin;
                    uint8_t safe_max = (adc_max > g_safe_margin) ? 
                                       adc_max - g_safe_margin : adc_max;
                    
                    bool at_upper = adc_pos >= safe_max;
                    bool at_lower = adc_pos <= safe_min;
                    
                    // Only stop if at boundary AND error would push us further out
                    // error > 0 means we want to increase ADC (toward upper)
                    // error < 0 means we want to decrease ADC (toward lower)
                    if ((at_upper && error > 0) || (at_lower && error < 0)) {
                        should_stop = true;
                        // Only log once when first hitting boundary
                        if (!valve_at_boundary[i]) {
                            valve_at_boundary[i] = true;
                            ESP_LOGI(TAG, "V%d: At safe boundary (adc=%d%%, safe range=%d-%d%%), blocking outward movement",
                                     i, adc_pos, safe_min, safe_max);
                        }
                    }
                    // Note: We do NOT stop if error would move us back toward center
                }
                
                // Hysteresis: stop if within tolerance, only restart if outside tolerance+hysteresis
                if (should_stop || abs_error <= s_vc.tol) {
                    // Within tolerance or crossed target - stop motor
                    if (state->is_moving) {
                        queue_motor(i, TB6612_DIR_STOP, 0);
                        state->is_moving = false;
                        state->stalled = false;
                        valve_stall_count[i] = 0;
                        ESP_LOGI(TAG, "Valve %d reached target (open=%d%%, adc=%d%%, target=%d%%)",
                                 i, state->position, adc_pos, state->target_position);
                    }
                    continue;
                }

                // Only start moving if outside hysteresis band (or already moving)
                if (!state->is_moving && abs_error <= (s_vc.tol + s_vc.hyst)) {
                    // Inside hysteresis band and not moving - don't start
                    continue;
                }

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
                    
                    // Use safe boundary margins
                    uint8_t safe_min = adc_min + g_safe_margin;
                    uint8_t safe_max = (adc_max > g_safe_margin) ? 
                                       adc_max - g_safe_margin : adc_max;
                    
                    // Check if movement direction is blocked due to previous wraparound
                    // desired_dir: FWD decreases ADC, REV increases ADC
                    // valve_wraparound_blocked_dir: +1 = FWD blocked, -1 = REV blocked
                    if (valve_wraparound_blocked_dir[i] != 0) {
                        bool blocked_by_wraparound = false;
                        if (valve_wraparound_blocked_dir[i] > 0 && desired_dir == TB6612_DIR_FORWARD) {
                            blocked_by_wraparound = true;
                        } else if (valve_wraparound_blocked_dir[i] < 0 && desired_dir == TB6612_DIR_REVERSE) {
                            blocked_by_wraparound = true;
                        }
                        
                        if (blocked_by_wraparound) {
                            direction_blocked = true;
                            // Don't spam the log - just block silently after first message
                        }
                    }
                    
                    // Block movement AWAY from center when at boundary
                    // But ALLOW movement TOWARD center to escape the boundary
                    bool at_upper = adc_pos >= safe_max;
                    bool at_lower = adc_pos <= safe_min;
                    
                    if (at_upper && error > 0) {
                        // At upper boundary and want to go further up - BLOCK
                        direction_blocked = true;
                        if (!valve_at_boundary[i]) {
                            valve_at_boundary[i] = true;
                            ESP_LOGI(TAG, "V%d: Blocked at upper boundary (adc=%d%%, safe_max=%d%%)", 
                                     i, adc_pos, safe_max);
                        }
                    } else if (at_lower && error < 0) {
                        // At lower boundary and want to go further down - BLOCK
                        direction_blocked = true;
                        if (!valve_at_boundary[i]) {
                            valve_at_boundary[i] = true;
                            ESP_LOGI(TAG, "V%d: Blocked at lower boundary (adc=%d%%, safe_min=%d%%)", 
                                     i, adc_pos, safe_min);
                        }
                    } else if (valve_at_boundary[i] && !at_upper && !at_lower && valve_wraparound_blocked_dir[i] == 0) {
                        // Was at boundary but now back in safe zone AND no wraparound block - clear flag
                        valve_at_boundary[i] = false;
                        ESP_LOGI(TAG, "V%d: Escaped boundary zone (adc=%d%%)", i, adc_pos);
                    }
                    // Note: Movement toward center (error<0 at upper, error>0 at lower) is ALLOWED
                }
                
                // If direction is blocked due to boundary, stop the motor
                if (direction_blocked) {
                    if (state->is_moving) {
                        queue_motor(i, TB6612_DIR_STOP, 0);
                        state->is_moving = false;
                        ESP_LOGI(TAG, "V%d: Stopped at calibrated boundary (pos=%d%%, adc=%d%%)", 
                                 i, state->position, valve_adc_pos[i]);
                    }
                    continue;
                }
                
                // Apply per-valve direction inversion
                // Use calibrated inversion if available, else use hardcoded array
                bool inverted = valve_effective_inverted(i);
                
                if (inverted) {
                    desired_dir = (desired_dir == TB6612_DIR_FORWARD) ? TB6612_DIR_REVERSE : TB6612_DIR_FORWARD;
                }
                
                // Proportional speed control: ramp down as we approach target
                // Full speed when error >= RAMP_START_ERROR
                // Linearly decrease to MIN_SPEED as error approaches tolerance
                // Use different settings for coolant valve (linear) vs butterfly valves
                uint8_t max_speed = valve_max_speed(i);
                uint8_t min_speed = (i == HVAC_VALVE_COOLANT) ? s_vc.coolant_min_pwm : s_vc.min_pwm;
                if (min_speed > max_speed) min_speed = max_speed;   // a low speed override must not invert the ramp
                uint8_t ramp_start = (i == HVAC_VALVE_COOLANT) ? s_vc.coolant_ramp : s_vc.ramp;
                
                uint8_t speed;
                if (abs_error >= ramp_start) {
                    speed = max_speed;  // Full speed
                } else {
                    // Linear interpolation: min_speed at tolerance, max_speed at ramp_start
                    int range = ramp_start - s_vc.tol;
                    if (range < 1) range = 1;   // a slow band at or inside the tolerance
                    int error_from_tolerance = abs_error - s_vc.tol;
                    if (error_from_tolerance < 0) error_from_tolerance = 0;
                    speed = min_speed + (error_from_tolerance * (max_speed - min_speed)) / range;
                }
                if (speed < min_speed) {
                    speed = min_speed;
                }
                if (speed > max_speed) {
                    speed = max_speed;
                }

                // Stall detection on the raw pot reading (monotonic through the butterfly's centre).
                // A reversal starts a new move; the no-motion timer keeps running across it.
                if (!state->is_moving) {
                    state->last_motion_ms = now_ms;
                    state->last_sample_pos = adc_pos;
                    valve_move_start_ms[i] = now_ms;
                } else {
                    if (abs((int16_t)adc_pos - (int16_t)state->last_sample_pos) >= VALVE_MOVEMENT_THRESHOLD) {
                        state->last_motion_ms = now_ms;
                        state->last_sample_pos = adc_pos;
                    }
                    if (desired_dir != last_motor_dir[i]) {
                        valve_move_start_ms[i] = now_ms;
                    }
                    uint32_t still_ms = now_ms - state->last_motion_ms;
                    uint32_t move_ms = now_ms - valve_move_start_ms[i];
                    if (still_ms > s_vc.stall_ms || move_ms > s_vc.move_max_ms) {
                        queue_motor(i, TB6612_DIR_BRAKE, 0);
                        state->is_moving = false;
                        state->stalled = true;
                        valve_stall_count[i]++;
                        valve_retry_at_ms[i] = (valve_stall_count[i] <= s_vc.retries)
                                               ? now_ms + s_vc.retry_ms * valve_stall_count[i] : 0;
                        ESP_LOGW(TAG, "V%d: STALL (%s) at pos=%d%% adc=%d%% tgt=%d%%, %s",
                                 i, still_ms > s_vc.stall_ms ? "no movement" : "move too long",
                                 state->position, adc_pos, state->target_position,
                                 valve_retry_at_ms[i] ? "will retry" : "latched until mode change");
                        event_log_add(EVT_STALL, "V%d stall adc=%d%% tgt=%d%%", i, adc_pos, state->target_position);
                        continue;
                    }
                }
                state->is_moving = true;

                queue_motor(i, desired_dir, speed);

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

            // Link I/O and NVS only now, with valve_mutex free for the sensors and controller tasks
            for (int i = 0; i < HVAC_VALVE_MAX; i++) {
                if (boot_mask & (1u << i)) {
                    valve_boot_decide(i);
                }
                if (recal_mask & (1u << i)) {
                    if (tb6612fng_start_calibration((hvac_valve_id_t)i) != ESP_OK) {
                        event_log_add(EVT_CAL, "V%d recalibration not started", i);
                        valve_svc_recal_run[i] = false;
                        valve_svc_active[i] = false;
                    }
                }
                if (save_mask & (1u << i)) {
                    valve_save_calibration(i);
                }
            }
            static uint32_t link_down_since_ms;     // 0 = link up
            bool link_up = valve_link_up();
            for (int i = 0; i < HVAC_VALVE_MAX; i++) {
                apply_motor(i, now_ms, link_up && valve_link_up());
            }
            if (!link_up && link_down_since_ms == 0) {
                link_down_since_ms = now_ms | 1;
                ESP_LOGW(TAG, "Link down: valve commands paused");
            } else if (link_up && link_down_since_ms != 0) {
                ESP_LOGI(TAG, "Link back after %lu ms: every commanded valve re-sent in this pass",
                         (unsigned long)(now_ms - link_down_since_ms));
                link_down_since_ms = 0;
            }
        }

        uint32_t pass_ms = (uint32_t)((esp_timer_get_time() - pass_start_us) / 1000);
        if (pass_ms > s_pass_max_ms) {
            s_pass_max_ms = pass_ms;
        }
        if (pass_ms > VALVE_PASS_EVENT_MS) {
            uint32_t timeouts_now = 0;
            remote_io_get_stats(NULL, NULL, NULL, &timeouts_now);
            ESP_LOGW(TAG, "Valve pass took %lu ms (%lu link timeouts)", (unsigned long)pass_ms,
                     (unsigned long)(timeouts_now - timeouts_at_start));
            event_log_add(EVT_FAULT, "Valve pass %lu ms, %lu timeouts", (unsigned long)pass_ms,
                          (unsigned long)(timeouts_now - timeouts_at_start));
        }

        vTaskDelay(pdMS_TO_TICKS(VALVE_TASK_PERIOD_MS));
    }
}

/*******************************************************************************
 * Independent Per-Valve Calibration State Machine
 ******************************************************************************/

// Calibration sampling parameters
#define CALIB_MARKER_ADC          60     // ADC% to use as rotation marker
#define CALIB_MARKER_TOLERANCE    2      // +/- tolerance for marker detection
#define CALIB_MIN_SAMPLES         200    // Minimum samples before looking for marker again (ensure full rotation)
#define CALIB_MIN_RANGE           20     // Minimum ADC range required for valid calibration

// Process calibration state machine for one valve (called from valve task)
static void calibrate_valve_tick(hvac_valve_id_t valve_id)
{
    if (valve_id >= HVAC_VALVE_MAX || !valve_calibrating[valve_id]) {
        return;
    }
    
    valve_calib_progress_t *prog = &valve_calib_progress[valve_id];
    uint8_t current_adc = valve_adc_pos[valve_id];
    uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    uint32_t elapsed = now_ms - prog->phase_start_ms;

    // Without position feedback min/max would be garbage: give up (the FAILED case stops the motor)
    if (valve_feedback_fault[valve_id] && prog->state != CALIB_STATE_FAILED) {
        ESP_LOGW(TAG, "V%d: Calibration aborted - position feedback lost", valve_id);
        prog->state = CALIB_STATE_FAILED;
    }

    // Same stall rule as normal control while the motor is driven
    if (prog->state == CALIB_STATE_SEEKING_MARKER || prog->state == CALIB_STATE_ROTATING) {
        if (abs((int)current_adc - (int)prog->stall_ref_adc) >= VALVE_MOVEMENT_THRESHOLD) {
            prog->stall_ref_adc = current_adc;
            prog->stall_ref_ms = now_ms;
        } else if (now_ms - prog->stall_ref_ms > s_vc.stall_ms) {
            ESP_LOGW(TAG, "V%d: Calibration aborted - STALL (no movement at adc=%d%%)",
                     valve_id, current_adc);
            event_log_add(EVT_STALL, "V%d cal stall adc=%d%%", valve_id, current_adc);
            prog->state = CALIB_STATE_FAILED;
        }
    }

    switch (prog->state) {
        case CALIB_STATE_SEEKING_MARKER: {
            // Check timeout
            if (elapsed > prog->timeout_ms) {
                ESP_LOGW(TAG, "V%d: Calibration timeout seeking marker (at %d%%)", valve_id, current_adc);
                motor_now(valve_id, TB6612_DIR_BRAKE, 0, current_adc);
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
                motor_now(valve_id, TB6612_DIR_BRAKE, 0, current_adc);
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
                motor_now(valve_id, TB6612_DIR_FORWARD, valve_capped(valve_id, prog->speed), prog->start_adc);
                
                ESP_LOGI(TAG, "V%d: Starting rotation from ADC=%d%%", valve_id, prog->start_adc);
                prog->state = CALIB_STATE_ROTATING;
                prog->phase_start_ms = now_ms;
                prog->stall_ref_adc = prog->start_adc;
                prog->stall_ref_ms = now_ms;
                prog->pre_wrap_net = 0;
                prog->wrap_seen = false;
            }
            break;
        }
        
        case CALIB_STATE_ROTATING: {
            // Check timeout
            if (elapsed > prog->timeout_ms) {
                ESP_LOGW(TAG, "V%d: Calibration timeout during rotation", valve_id);
                motor_now(valve_id, TB6612_DIR_BRAKE, 0, current_adc);
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
            // Net movement before the first wrap-around decides the direction; after the
            // wrap the pot restarts from its other end and a late sample says nothing
            if (!prog->wrap_seen) {
                if (abs(diff) > VALVE_ADC_WRAPAROUND_THRESH) {
                    prog->wrap_seen = true;
                } else {
                    prog->pre_wrap_net += diff;
                }
            }
            prog->last_adc = current_adc;
            
            // Check if rotation complete (back to marker)
            if (prog->sample_count > CALIB_MIN_SAMPLES) {
                if (abs((int)current_adc - CALIB_MARKER_ADC) <= CALIB_MARKER_TOLERANCE) {
                    motor_now(valve_id, TB6612_DIR_BRAKE, 0, current_adc);
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
            motor_now(valve_id, TB6612_DIR_BRAKE, 0, current_adc);
            
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
            // Determine if motor direction is inverted:
            // During calibration we run FWD. If ADC decreased (last_direction < 0), 
            // that's NORMAL behavior (FWD = decrease ADC). If ADC increased, it's inverted.
            // So is_inverted = true when last_direction > 0 (ADC increased during FWD)
            // FWD normally decreases ADC; inverted if the net change before the wrap was upward.
            // Fall back to the last sampled direction only if there was no net movement.
            bool is_inverted = (prog->pre_wrap_net != 0) ? (prog->pre_wrap_net > 0)
                                                          : (prog->last_direction > 0);
            ESP_LOGI(TAG, "V%d: direction from net ADC change before wrap: %+d%% -> inverted=%s",
                     valve_id, prog->pre_wrap_net, is_inverted ? "YES" : "NO");
            
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
                
                // Reset boundary tracking for clean start after calibration
                valve_at_boundary[valve_id] = false;
                valve_adc_last_stable[valve_id] = current_adc;
                
                ESP_LOGI(TAG, "V%d: CALIBRATED - ADC %d%%-%d%%, center=%d%%, inverted=%s, safe range=%d-%d%%, seeking to %d%%",
                         valve_id, prog->global_min, prog->global_max, adc_center,
                         is_inverted ? "YES" : "NO", 
                         prog->global_min + g_safe_margin,
                         prog->global_max - g_safe_margin,
                         requested_target);
                
                xSemaphoreGive(valve_mutex);
            } else {
                ESP_LOGI(TAG, "V%d: CALIBRATED - ADC %d%%-%d%%, center=%d%%, inverted=%s",
                         valve_id, prog->global_min, prog->global_max, adc_center,
                         is_inverted ? "YES" : "NO");
            }
            
            valve_cal_source[valve_id] = VALVE_CAL_SRC_CALIBRATED;
            valve_save_calibration(valve_id);
            event_log_add(EVT_CAL, "V%d calibrated %d-%d c%d inv=%s", valve_id, prog->global_min,
                          prog->global_max, adc_center, is_inverted ? "yes" : "no");

            // Calibration done, release valve for normal control
            // Valve will now seek to its target_position
            valve_calibrating[valve_id] = false;
            prog->state = CALIB_STATE_IDLE;
            break;
        }
        
        case CALIB_STATE_FAILED: {
            // Stop motor on failure too
            motor_now(valve_id, TB6612_DIR_BRAKE, 0, current_adc);
            
            ESP_LOGW(TAG, "V%d: Calibration FAILED - using defaults", valve_id);
            if (!valve_calibration[valve_id].calibrated) {
                valve_cal_source[valve_id] = VALVE_CAL_SRC_DEFAULT;
            }
            event_log_add(EVT_CAL, "V%d calibration FAILED", valve_id);
            valve_calibrating[valve_id] = false;
            prog->state = CALIB_STATE_IDLE;
            break;
        }
        
        default:
            break;
    }
}

// Called from valve task to process all calibrating valves. It alone switches the sensors'
// fast position reads: on while any valve calibrates or learns its direction (finding 5).
void tb6612fng_calibration_tick(void)
{
    static bool fast_reads;
    bool want_fast = false;
    for (int i = 0; i < HVAC_VALVE_MAX; i++) {
        if (valve_calibrating[i]) {
            calibrate_valve_tick((hvac_valve_id_t)i);
        }
        want_fast |= valve_calibrating[i] || valve_svc_learn_run[i];
    }
    if (want_fast != fast_reads) {
        fast_reads = want_fast;
        hvac_sensors_set_calibration_mode(want_fast);   // logs the change
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
    
    // Initialize calibration progress
    valve_calib_progress_t *prog = &valve_calib_progress[valve_id];
    prog->state = CALIB_STATE_SEEKING_MARKER;
    prog->global_min = 255;
    prog->global_max = 0;
    prog->sample_count = 0;
    prog->last_direction = 0;
    prog->phase_start_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    prog->speed = s_vc.cal_pwm;            // the rest of this calibration uses these, whatever is set meanwhile
    prog->timeout_ms = s_vc.cal_timeout_ms;
    prog->stall_ref_adc = valve_adc_pos[valve_id];
    prog->stall_ref_ms = prog->phase_start_ms;
    
    // Mark valve as calibrating (valve task will skip it)
    valve_calibrating[valve_id] = true;
    
    // Start moving toward marker
    uint8_t current_adc = valve_adc_pos[valve_id];
    
    ESP_LOGI(TAG, "V%d: Starting background calibration (marker=%d%%, current_adc=%d%%)", 
             valve_id, CALIB_MARKER_ADC, current_adc);
    
    tb6612_direction_t seek_dir;
    if (current_adc < CALIB_MARKER_ADC) {
        seek_dir = TB6612_DIR_FORWARD;
    } else {
        seek_dir = TB6612_DIR_REVERSE;
    }
    
    motor_now(valve_id, seek_dir, valve_capped(valve_id, prog->speed), current_adc);
    
    return ESP_OK;
}

// SET_VALVE_FITTED_n (snapshot)
static bool is_valve_enabled(hvac_valve_id_t valve_id)
{
    return valve_id < HVAC_VALVE_MAX && valve_fitted[valve_id];
}

bool tb6612fng_valve_fitted(hvac_valve_id_t valve_id)
{
    return is_valve_enabled(valve_id);
}

uint32_t tb6612fng_calib_max_ms(void)
{
    return 2 * s_vc.cal_timeout_ms + CALIB_SETTLE_TIME_MS;
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

// Check if any valve is still calibrating (no side effects: any task may poll it)
bool tb6612fng_is_calibrating(void)
{
    return any_valve_calibrating();
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

// Boot: stored calibration if it still fits the pot, else calibrate that valve only.
// The decision happens in the valve task once the first position reading is in.
esp_err_t tb6612fng_auto_calibrate_all(void)
{
    bool recal_all = settings_get_int(SET_RECAL_ALL_NEXT_BOOT) != 0;
    if (recal_all) {
        ESP_LOGW(TAG, "Recalibrate-all requested: erasing stored calibration");
        valve_cal_store_erase_all();
        settings_set_int(SET_RECAL_ALL_NEXT_BOOT, 0);   // erased first: an interrupted run just calibrates again
    }
    xSemaphoreTake(valve_mutex, portMAX_DELAY);     // vs. a "fitted" change applied meanwhile
    valve_boot_started = true;
    for (int i = 0; i < HVAC_VALVE_MAX; i++) {
        if (i == HVAC_VALVE_COOLANT) {
            valve_cal_source[i] = VALVE_CAL_SRC_FIXED;
            ESP_LOGI(TAG, "V%d: fixed calibration (linear valve)", i);
            continue;
        }
        if (!is_valve_enabled((hvac_valve_id_t)i)) {
            ESP_LOGI(TAG, "V%d: not fitted (setting), no calibration", i);
            continue;
        }
        valve_boot_force_cal[i] = recal_all;
        valve_boot_wait[i] = true;
    }
    xSemaphoreGive(valve_mutex);
    return ESP_OK;
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
    valve_have_reading[valve_id] = true;
    valve_adc_pos[valve_id] = position;  // Store actual ADC reading for debugging
    
    // Apply EMA filter to smooth noisy ADC readings
    // filtered = (alpha * new + (256 - alpha) * old) / 256
    uint16_t sample_scaled = scaled_pos << 8;  // Scale up for precision
    uint8_t alpha = s_vc.alpha;
    
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
            uint32_t filtered = (uint32_t)alpha * sample_scaled + 
                               (uint32_t)(256 - alpha) * valve_filtered_pos[valve_id];
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

void tb6612fng_set_feedback_fault(hvac_valve_id_t valve_id, bool fault)
{
    if (valve_id >= HVAC_VALVE_MAX || valve_feedback_fault[valve_id] == fault) {
        return;
    }
    valve_feedback_fault[valve_id] = fault;
    if (fault) {
        ESP_LOGW(TAG, "V%d: position feedback LOST - motor stopped, valve held", valve_id);
        event_log_add(EVT_STALL, "V%d position feedback lost", valve_id);
    } else {
        ESP_LOGW(TAG, "V%d: position feedback back - control resumes", valve_id);
    }
}

bool tb6612fng_valve_has_fault(hvac_valve_id_t valve_id)
{
    return is_valve_enabled(valve_id) &&     // a valve that is not fitted has no feedback to lose
           (valve_feedback_fault[valve_id] || tb6612fng_valve_is_stalled(valve_id));
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
        valve_stall_count[valve_id] = 0;
        valve_retry_at_ms[valve_id] = 0;
        xSemaphoreGive(valve_mutex);
    }
}

// The settings are range-clamped by settings.c; the snapshot only narrows them to their types
void tb6612fng_apply_settings(void)
{
    const valve_cfg_t c = {
        .tol = (uint8_t)settings_get_int(SET_VALVE_TOL_PCT),
        .hyst = (uint8_t)settings_get_int(SET_VALVE_HYST_PCT),
        .retries = (uint8_t)settings_get_int(SET_VALVE_STALL_RETRIES),
        .min_pwm = (uint8_t)settings_get_int(SET_VALVE_MIN_PWM),
        .coolant_min_pwm = (uint8_t)settings_get_int(SET_COOLANT_MIN_PWM),
        .ramp = (uint8_t)settings_get_int(SET_VALVE_RAMP_PCT),
        .coolant_ramp = (uint8_t)settings_get_int(SET_COOLANT_RAMP_PCT),
        .alpha = (uint8_t)settings_get_int(SET_VALVE_FILTER),
        .jog_pwm = (uint8_t)settings_get_int(SET_JOG_PWM),
        .cal_pwm = (uint8_t)settings_get_int(SET_CAL_SPEED_PWM),
        .stall_ms = (uint32_t)settings_get_int(SET_VALVE_STALL_S) * 1000u,
        .move_max_ms = (uint32_t)settings_get_int(SET_VALVE_MOVE_MAX_S) * 1000u,
        .retry_ms = (uint32_t)settings_get_int(SET_VALVE_RETRY_S) * 1000u,
        .learn_ms = (uint32_t)settings_get_int(SET_LEARN_MS),
        .cal_timeout_ms = (uint32_t)settings_get_int(SET_CAL_TIMEOUT_S) * 1000u,
    };
    // A short wait like tb6612fng_stop_valve(); on a timeout the values are written anyway
    // (a pass may then mix old and new ones once) rather than dropped
    bool locked = xSemaphoreTake(valve_mutex, pdMS_TO_TICKS(500)) == pdTRUE;
    if (!locked) {
        ESP_LOGW(TAG, "apply settings: mutex timeout, writing anyway");
    }
    s_vc = c;
    for (int i = 0; i < HVAC_VALVE_MAX; i++) {
        int32_t speed = settings_get_int((setting_id_t)(SET_VALVE_SPEED_0 + i));
        valve_speed_ovr[i] = (uint8_t)(speed < 0 ? 0 : speed > 255 ? 255 : speed);
        int32_t dir = settings_get_int((setting_id_t)(SET_VALVE_DIR_0 + i));
        valve_dir_ovr[i] = (uint8_t)((dir == SET_VDIR_NORMAL || dir == SET_VDIR_INVERTED) ? dir : SET_VDIR_AUTO);

        bool fit = settings_get_bool((setting_id_t)(SET_VALVE_FITTED_0 + i));
        if (fit == valve_fitted[i]) {
            continue;
        }
        valve_fitted[i] = fit;
        if (!valve_boot_started) {
            continue;   // init: tb6612fng_auto_calibrate_all() decides for the fitted ones
        }
        if (!fit) {
            valve_unfit_stop[i] = true;
        } else {
            // Hold where it is; a butterfly then gets the boot decision (stored cal or calibrate)
            valve_states[i].target_position = valve_states[i].position;
            if (i != HVAC_VALVE_COOLANT) {
                valve_boot_force_cal[i] = false;
                valve_boot_wait[i] = true;
            }
        }
        ESP_LOGI(TAG, "V%d: %s", i, fit ? "fitted (setting)" : "not fitted (setting): stopped");
        event_log_add(EVT_INFO, "V%d %s", i, fit ? "fitted" : "not fitted");
    }
    int32_t margin = settings_get_int(SET_SAFE_MARGIN_PCT);
    g_safe_margin = (uint8_t)(margin < 0 ? 0 : margin > 10 ? 10 : margin);   // 10 = SET_SAFE_MARGIN_PCT max
    if (locked) {
        xSemaphoreGive(valve_mutex);
    }
}

void tb6612fng_get_valve_info(hvac_valve_id_t valve_id, tb6612_valve_info_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (valve_id >= HVAC_VALVE_MAX) {
        return;
    }
    if (xSemaphoreTake(valve_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        const valve_state_t *s = &valve_states[valve_id];
        out->cal_source = valve_cal_source[valve_id];
        out->calibrating = valve_calibrating[valve_id];
        out->service_active = valve_svc_active[valve_id];
        out->adc_min = valve_calibration[valve_id].adc_min;
        out->adc_max = valve_calibration[valve_id].adc_max;
        out->adc_center = valve_calibration[valve_id].adc_center;
        out->is_inverted = valve_effective_inverted(valve_id);
        out->speed = valve_max_speed(valve_id);
        out->adc_pos = valve_adc_pos[valve_id];
        out->position = s->position;
        out->target = s->target_position;
        out->moving = s->is_moving;
        out->stalled = s->stalled;
        out->feedback_fault = valve_feedback_fault[valve_id];
        xSemaphoreGive(valve_mutex);
    }
}

// Common start of a service request: the valve must be idle (not calibrating, no other service job)
static esp_err_t service_begin(hvac_valve_id_t valve_id, bool *took_mutex)
{
    *took_mutex = false;
    if (valve_id >= HVAC_VALVE_MAX || !is_valve_enabled(valve_id)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(valve_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    *took_mutex = true;
    if (valve_calibrating[valve_id] || valve_boot_wait[valve_id]) {
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

esp_err_t tb6612fng_service_jog(hvac_valve_id_t valve_id, int dir)
{
    if (dir == 0) {
        return tb6612fng_service_stop(valve_id);
    }
    bool locked;
    esp_err_t err = service_begin(valve_id, &locked);
    if (locked) {
        if (err == ESP_OK && valve_svc_active[valve_id] && valve_svc_jog_dir[valve_id] == 0) {
            err = ESP_ERR_INVALID_STATE;    // learn / recalibrate running
        }
        if (err == ESP_OK) {
            valve_svc_jog_dir[valve_id] = (dir > 0) ? 1 : -1;
            valve_svc_jog_ms[valve_id] = xTaskGetTickCount() * portTICK_PERIOD_MS;
            valve_svc_active[valve_id] = true;
        }
        xSemaphoreGive(valve_mutex);
    }
    return err;
}

esp_err_t tb6612fng_service_stop(hvac_valve_id_t valve_id)
{
    if (valve_id >= HVAC_VALVE_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    // The stop request is applied (and the service state cleared) by the valve task; the target is kept
    if (xSemaphoreTake(valve_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    valve_stop_requested[valve_id] = true;
    xSemaphoreGive(valve_mutex);
    return ESP_OK;
}

esp_err_t tb6612fng_service_recalibrate(hvac_valve_id_t valve_id)
{
    if (valve_id == HVAC_VALVE_COOLANT) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    bool locked;
    esp_err_t err = service_begin(valve_id, &locked);
    if (locked) {
        if (err == ESP_OK && valve_svc_active[valve_id]) {
            err = ESP_ERR_INVALID_STATE;
        }
        if (err == ESP_OK) {
            valve_svc_recal_req[valve_id] = true;
            valve_svc_active[valve_id] = true;
        }
        xSemaphoreGive(valve_mutex);
    }
    return err;
}

esp_err_t tb6612fng_service_learn_direction(hvac_valve_id_t valve_id)
{
    if (valve_id == HVAC_VALVE_COOLANT) {
        return ESP_ERR_NOT_SUPPORTED;   // linear valve: use the per-valve direction setting
    }
    bool locked;
    esp_err_t err = service_begin(valve_id, &locked);
    if (locked) {
        if (err == ESP_OK && valve_svc_active[valve_id]) {
            err = ESP_ERR_INVALID_STATE;
        }
        if (err == ESP_OK) {
            valve_svc_learn_req[valve_id] = true;
            valve_svc_active[valve_id] = true;
        }
        xSemaphoreGive(valve_mutex);
    }
    return err;
}
