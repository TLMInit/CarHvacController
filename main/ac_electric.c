/* Electric compressor control: see ac_electric.h. No ESP-IDF. */
#include <math.h>
#include <string.h>
#include "ac_electric.h"

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

void ac_electric_init(ac_electric_state_t *s)
{
    memset(s, 0, sizeof(*s));
}

float ac_electric_idle_output(const ac_electric_params_t *p)
{
    return p->invert ? 100.0f : 0.0f;
}

float ac_electric_map_output(const ac_electric_params_t *p, float speed_pct)
{
    if (!(speed_pct > 0.0f)) {
        return ac_electric_idle_output(p);
    }
    float out = p->out_min_pct + clampf(speed_pct, 0.0f, 100.0f) / 100.0f * (p->out_max_pct - p->out_min_pct);
    out = clampf(out, 0.0f, 100.0f);
    return p->invert ? 100.0f - out : out;
}

float ac_electric_demand(const ac_demand_inputs_t *in)
{
    float d = 0.0f;
    if (in->auto_cooling && !isnan(in->cabin_error_c) && in->cabin_error_c < -in->deadband_c) {
        float span = in->full_c - in->deadband_c;
        float c = span > 0.0f ? (-in->cabin_error_c - in->deadband_c) / span * 100.0f : 100.0f;
        d = clampf(c, 0.0f, 100.0f);
    }
    if (in->humidity) d = fmaxf(d, AC_DEMAND_HUMIDITY_PCT);
    if (in->defog) d = fmaxf(d, AC_DEMAND_DEFOG_PCT);
    if (in->defrost) d = fmaxf(d, AC_DEMAND_DEFROST_PCT);
    if (in->user) d = fmaxf(d, AC_DEMAND_USER_PCT);
    if (in->max_cool) d = fmaxf(d, AC_DEMAND_MAX_COOL_PCT);
    return d;
}

static void stop(ac_electric_state_t *s)
{
    s->enabled = false;
    s->speed_pct = 0.0f;
    s->integral = 0.0f;
    s->idle_timer_s = 0.0f;
}

void ac_electric_step(ac_electric_state_t *s, const ac_electric_params_t *p,
                      const ac_electric_inputs_t *in, ac_electric_outputs_t *out)
{
    float dt = in->dt_s > 0.0f ? in->dt_s : 0.0f;
    float demand = isnan(in->demand_pct) ? 0.0f : clampf(in->demand_pct, 0.0f, 100.0f);
    float min_speed = clampf(p->min_speed_pct, 0.0f, 100.0f);
    bool have_evap = !isnan(in->evap_c);

    /* 1. freeze latch with hysteresis */
    if (!have_evap) {
        s->frozen = false;
    } else if (in->evap_c <= p->evap_off_c) {
        s->frozen = true;
    } else if (in->evap_c > p->evap_on_c) {
        s->frozen = false;
    }
    float allowed = 100.0f;
    if (have_evap && in->evap_c < p->evap_on_c) {
        float span = p->evap_on_c - p->evap_off_c;
        allowed = span > 0.0f ? clampf((in->evap_c - p->evap_off_c) / span * 100.0f, 0.0f, 100.0f) : 0.0f;
    }
    if (s->frozen) allowed = 0.0f;

    /* 2. interlock / freeze cut: off at once */
    if (in->interlock || s->frozen) {
        stop(s);
    } else {
        /* 3. idle-off */
        if (demand > 0.0f) {
            s->idle_timer_s = 0.0f;
            s->enabled = true;
        } else if (s->enabled) {
            s->idle_timer_s += dt;
            if (s->idle_timer_s >= p->idle_off_s) stop(s);
        }
    }

    if (s->enabled) {
        /* 4. target */
        float target;
        if (demand <= 0.0f) {
            target = min_speed;
            s->integral = s->speed_pct;     /* bumpless when demand returns */
        } else if (have_evap) {
            float hi = fmaxf(demand, min_speed);
            float err = in->evap_c - fmaxf(p->evap_target_c, p->evap_on_c);    /* > 0: too warm */
            float raw = p->kp * err + s->integral;
            if (!((raw > hi && err > 0.0f) || (raw < min_speed && err < 0.0f))) {
                s->integral = clampf(s->integral + p->ki * err * dt, 0.0f, hi);
            }
            target = clampf(p->kp * err + s->integral, min_speed, hi);
        } else {
            target = fmaxf(demand, min_speed);
            s->integral = s->speed_pct;
        }

        /* 5. ramp; from off straight to the minimum */
        float speed = s->speed_pct;
        if (speed <= 0.0f) {
            speed = fminf(min_speed, target);
        }
        float step = p->ramp_pct_per_s * dt;
        speed = target > speed ? fminf(target, speed + step) : fmaxf(target, speed - step);

        /* 6. limiter: immediate cap */
        s->speed_pct = fminf(speed, allowed);
    }

    out->enable = s->enabled;
    out->speed_pct = s->enabled ? s->speed_pct : 0.0f;
    out->output_pct = s->enabled ? ac_electric_map_output(p, s->speed_pct) : ac_electric_idle_output(p);
    out->limiter_active = have_evap && allowed < 100.0f;
    out->freeze_cut = s->frozen;
    out->idle_timer_s = s->idle_timer_s;
}
