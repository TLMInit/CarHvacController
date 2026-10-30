/**
 * @file ac_electric.h
 * @brief Electric compressor control, pure (no ESP-IDF): host-testable.
 *
 * Used only with SET_AC_COMPRESSOR = electric; the clutch path does not call it.
 * The caller (hvac_controller.c) fills ac_electric_params_t from the settings, builds the
 * demand with ac_electric_demand() from the same requests the clutch logic uses, and calls
 * ac_electric_step() once per control cycle. It then drives:
 *   - enable  -> the A/C relay (AC_RELAY_PIN, node GPIO6), the compressor's run/enable line;
 *   - output_pct -> the speed output (SET_AC_OUT_TYPE on SET_AC_OUT_PIN / the DAC), as % of
 *     full scale, already inverted when params.invert. With enable false output_pct is the
 *     idle level ac_electric_idle_output(): 0 %, or 100 % when inverted.
 *
 * Behaviour of one step (in this order):
 *  1. Freeze latch (needs evap_c): evap_c <= evap_off_c latches a cut, evap_c > evap_on_c
 *     clears it. Without a reading (NaN) the latch is clear and there is no limiter: the
 *     caller keeps the duct-proxy freeze cutout of the clutch path and passes it as interlock.
 *  2. interlock or freeze cut: enable off and speed 0 at once (no ramp, no idle-off time),
 *     integral and idle timer reset. The caller's interlocks: mode OFF, blower below Low,
 *     duct-proxy cutout, and the minimum off time (SET_AC_MIN_OFF_S) after enable dropped.
 *  3. Idle-off: demand 0 while enabled keeps the compressor at min_speed_pct until demand has
 *     been 0 for idle_off_s (0 = at once); then enable drops and speed is 0. Demand > 0
 *     resets the timer. Enable comes back as soon as there is demand again.
 *  4. Target speed: demand 0 (idling) -> min_speed_pct. With demand and a valid evap_c a PI
 *     loop (anti-windup) holds max(evap_target_c, evap_on_c) between min_speed_pct and
 *     max(demand, min_speed_pct): the demand is the upper bound. Without evap_c the target is
 *     max(demand, min_speed_pct): a demand between 0 and the minimum runs at the minimum.
 *  5. Ramp: from off the speed starts at min_speed_pct; then it moves toward the target by at
 *     most ramp_pct_per_s * dt_s, up and down.
 *  6. Limiter (needs evap_c): allowed = 100 % at or above evap_on_c, linear down to 0 % at
 *     evap_off_c. The speed is capped to it at once (not ramped) and may go below
 *     min_speed_pct; enable stays on until the cut in step 1.
 *  Output: output_pct = 0 (idle) when !enable or speed 0, else min_pct + speed/100 *
 *  (max_pct - min_pct); invert gives 100 - that (idle 100 %).
 *
 * PI gains: kp in % speed per degC above target, ki in % per degC per second. Defaults
 * (SET_AC_EVAP_KP 10, SET_AC_EVAP_KI 0.5): 1 degC too warm asks 10 % more at once and
 * 30 %/min more over time; Ti = kp / ki = 20 s, about an evaporator's time constant.
 */

#ifndef AC_ELECTRIC_H
#define AC_ELECTRIC_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Fixed demand levels (%) per request; the demand is the highest active one. */
#define AC_DEMAND_HUMIDITY_PCT  40.0f   /* AUTO humidity rule (SET_HUMIDITY_ON/OFF) */
#define AC_DEMAND_DEFOG_PCT     60.0f   /* dew-point defog with SET_DEFOG_USES_AC, AUTO cold-start warm-up */
#define AC_DEMAND_DEFROST_PCT   60.0f   /* DEF / DEF+foot mode */
#define AC_DEMAND_USER_PCT      100.0f  /* A/C button on (what the clutch did: full) */
#define AC_DEMAND_MAX_COOL_PCT  100.0f  /* Max cool preset active */

typedef struct {
    bool invert;                /* SET_AC_OUT_INVERT */
    float out_min_pct;          /* SET_AC_OUT_MIN_PCT: output at the lowest running speed */
    float out_max_pct;          /* SET_AC_OUT_MAX_PCT: output at 100 % speed */
    float ramp_pct_per_s;       /* SET_AC_RAMP_PCT_S */
    float min_speed_pct;        /* SET_AC_MIN_SPEED_PCT */
    float idle_off_s;           /* SET_AC_IDLE_OFF_S */
    float evap_target_c;        /* SET_EVAP_TARGET (used as max(target, evap_on_c)) */
    float evap_off_c;           /* SET_EVAP_OFF */
    float evap_on_c;            /* SET_EVAP_ON (>= evap_off_c + 1, a settings rule) */
    float kp;                   /* SET_AC_EVAP_KP, % per degC */
    float ki;                   /* SET_AC_EVAP_KI, % per degC per s */
} ac_electric_params_t;

typedef struct {
    bool enabled;
    bool frozen;                /* freeze cut latched (evap_off reached, not yet above evap_on) */
    float speed_pct;            /* commanded speed 0..100 after ramp and limiter */
    float integral;             /* PI integral, % */
    float idle_timer_s;         /* time with demand 0 while enabled */
} ac_electric_state_t;

typedef struct {
    float demand_pct;           /* 0..100, from ac_electric_demand() */
    float evap_c;               /* SENSOR_ROLE_EVAPORATOR, NaN if none */
    bool interlock;             /* true = off now (see step 2) */
    float dt_s;                 /* > 0; time since the last step */
} ac_electric_inputs_t;

typedef struct {
    bool enable;                /* relay / enable line */
    float speed_pct;            /* 0..100 logical speed */
    float output_pct;           /* 0..100 % of output full scale, inverted if params.invert */
    bool limiter_active;        /* evaporator below evap_on: speed capped (or cut) */
    bool freeze_cut;            /* = state.frozen */
    float idle_timer_s;
} ac_electric_outputs_t;

/* The requests the clutch logic in hvac_controller.c evaluates. */
typedef struct {
    bool auto_cooling;          /* auto_requests_ac && mode AUTO && is_cooling */
    float cabin_error_c;        /* setpoint - cabin (negative when too warm); NaN if none */
    float deadband_c;           /* SET_TEMP_DEADBAND */
    float full_c;               /* error for 100 %: SET_AUTO_FAN_FULL_C (the AUTO fan's knee) */
    bool humidity;              /* humidity rule active */
    bool defog;                 /* dew-point defog with A/C, or AUTO warm-up */
    bool defrost;               /* DEF / DEF+foot */
    bool user;                  /* A/C button on and mode not OFF */
    bool max_cool;              /* Max cool preset active */
} ac_demand_inputs_t;

void ac_electric_init(ac_electric_state_t *s);

/** One control step (see the file comment). s, p, in, out must not be NULL. */
void ac_electric_step(ac_electric_state_t *s, const ac_electric_params_t *p,
                      const ac_electric_inputs_t *in, ac_electric_outputs_t *out);

/** Speed % -> output % of full scale (0 speed = idle level). */
float ac_electric_map_output(const ac_electric_params_t *p, float speed_pct);

/** Output level while the compressor is off: 0 %, 100 % when inverted. */
float ac_electric_idle_output(const ac_electric_params_t *p);

/** Demand 0..100 %: the highest of the active requests. AUTO cooling maps |error| from the
 * deadband (0 %) to full_c (100 %), only while cooling (error < -deadband). */
float ac_electric_demand(const ac_demand_inputs_t *in);

#ifdef __cplusplus
}
#endif

#endif /* AC_ELECTRIC_H */
