/* Cascade temperature control: see cascade.h. No ESP-IDF. */
#include <math.h>
#include <string.h>
#include "cascade.h"

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

void cascade_reset(cascade_state_t *s)
{
    if (s) {
        memset(s, 0, sizeof(*s));
    }
}

float cascade_step(cascade_state_t *s, const cascade_params_t *p,
                   float setpoint_c, float cabin_c, float duct_c, float dt_s)
{
    if (!s || !p) {
        return 0.0f;
    }
    if (isnan(setpoint_c) || isnan(cabin_c)) {
        return s->valve_pct;
    }
    float dt_min = dt_s > 0.0f ? dt_s / 60.0f : 0.0f;
    float span = p->duct_max_c - p->duct_min_c;
    if (span <= 0.0f) {
        return s->valve_pct;
    }

    /* Outer PI. The duct target is centred on the setpoint, so zero error asks for a duct
     * temperature equal to the setpoint plus whatever the integrator needs for heat loss. */
    float err = setpoint_c - cabin_c;
    float raw = setpoint_c + p->outer_kp * err + s->outer_integral;
    float target = clampf(raw, p->duct_min_c, p->duct_max_c);
    /* Anti-windup: do not integrate further into a saturated output */
    if (!((raw > p->duct_max_c && err > 0.0f) || (raw < p->duct_min_c && err < 0.0f))) {
        s->outer_integral = clampf(s->outer_integral + p->outer_ki * err * dt_min, -span, span);
    }
    s->duct_target_c = target;

    float valve;
    if (isnan(duct_c)) {
        /* Open loop: duct_min -> 0 %, duct_max -> 100 %. Keep the inner integral in step
         * so closing the loop again does not jump. */
        valve = (target - p->duct_min_c) / span * 100.0f;
        s->inner_integral = clampf(valve, 0.0f, 100.0f);
    } else {
        float derr = target - duct_c;
        float uraw = p->inner_kp * derr + s->inner_integral;
        if (!((uraw > 100.0f && derr > 0.0f) || (uraw < 0.0f && derr < 0.0f))) {
            s->inner_integral = clampf(s->inner_integral + p->inner_ki * derr * dt_min, -100.0f, 100.0f);
        }
        valve = p->inner_kp * derr + s->inner_integral;
    }
    s->valve_pct = clampf(valve, 0.0f, 100.0f);
    s->initialised = true;
    return s->valve_pct;
}
