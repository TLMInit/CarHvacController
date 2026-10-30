/* main/cascade.c: two-loop PI with anti-windup. */
#include <math.h>
#include <string.h>
#include "cascade.h"
#include "t.h"

static const cascade_params_t P = { .outer_kp = 6, .outer_ki = 0.5f, .inner_kp = 3, .inner_ki = 1, .duct_min_c = 10, .duct_max_c = 60 };

static void test_reset(void)
{
    cascade_state_t s;
    memset(&s, 0xA5, sizeof(s));
    cascade_reset(&s);
    CHECK(s.outer_integral == 0 && s.inner_integral == 0 && s.duct_target_c == 0 && s.valve_pct == 0 && !s.initialised);
    cascade_reset(NULL);                                       /* must not crash */
}

static void test_zero_error_is_stable(void)
{
    cascade_state_t s;
    cascade_reset(&s);
    /* setpoint == cabin, duct at the target the outer loop asks for (= setpoint) */
    float v = cascade_step(&s, &P, 22.0f, 22.0f, 22.0f, 1.0f);
    CHECK(s.initialised);
    CHECK(s.duct_target_c == 22.0f);
    CHECK(v == 0.0f);
    for (int i = 0; i < 1000; i++) {
        float v2 = cascade_step(&s, &P, 22.0f, 22.0f, 22.0f, 1.0f);
        CHECK(v2 == v);
    }
    CHECK(s.outer_integral == 0.0f && s.inner_integral == 0.0f && s.duct_target_c == 22.0f);
}

static void test_duct_target_clamped(void)
{
    cascade_state_t s;
    cascade_reset(&s);
    cascade_step(&s, &P, 30.0f, 0.0f, 30.0f, 1.0f);            /* raw = 30 + 6*30 */
    CHECK(s.duct_target_c == 60.0f);
    cascade_reset(&s);
    cascade_step(&s, &P, 16.0f, 40.0f, 30.0f, 1.0f);           /* raw = 16 - 6*24 */
    CHECK(s.duct_target_c == 10.0f);
    cascade_reset(&s);
    cascade_step(&s, &P, 5.0f, 5.0f, 30.0f, 1.0f);             /* zero error but setpoint below duct_min */
    CHECK(s.duct_target_c == 10.0f);
    cascade_reset(&s);
    cascade_step(&s, &P, 70.0f, 70.0f, 30.0f, 1.0f);
    CHECK(s.duct_target_c == 60.0f);
}

static void test_valve_clamped(void)
{
    cascade_state_t s;
    cascade_reset(&s);
    CHECK(cascade_step(&s, &P, 30.0f, 0.0f, -50.0f, 1.0f) == 100.0f);     /* huge positive duct error */
    cascade_reset(&s);
    CHECK(cascade_step(&s, &P, 16.0f, 40.0f, 200.0f, 1.0f) == 0.0f);      /* huge negative */
    for (int i = 0; i < 5000; i++) {
        float v = cascade_step(&s, &P, 22.0f + (i % 17), 10.0f + (i % 29), (i % 7) * 9.0f, 0.5f);
        CHECK(v >= 0.0f && v <= 100.0f);
    }
}

/* Saturate the valve for a long time, then reverse. The inner integral must not have wound up:
 * with kp = 1 it would otherwise climb to its +-100 clamp. */
static void test_anti_windup(void)
{
    cascade_params_t p = P;
    p.inner_kp = 1.0f;
    cascade_state_t s;
    cascade_reset(&s);
    for (int i = 0; i < 20000; i++) {
        float v = cascade_step(&s, &p, 30.0f, 0.0f, 30.0f, 1.0f);   /* target 60, duct 30: derr 30 */
        if (i > 400) CHECK(v == 100.0f);
    }
    CHECK(s.outer_integral == 0.0f);                    /* outer was saturated from step 1 */
    CHECK(s.inner_integral < 75.0f);                    /* stops near 100 - kp*derr = 70 */
    CHECK(s.inner_integral > 60.0f);

    int steps = 0;
    float v = 100.0f;
    while (v >= 100.0f && steps < 100) {                /* now the cabin is far too warm: error reverses */
        v = cascade_step(&s, &p, 30.0f, 40.0f, 30.0f, 1.0f);
        steps++;
    }
    CHECKM(steps <= 3, "left saturation after %d steps", steps);
    CHECK(v < 100.0f);

    /* same on the outer loop: weak Kp so the output can sit just above max while error is small */
    cascade_params_t q = P;
    q.outer_kp = 1.0f; q.outer_ki = 5.0f; q.duct_max_c = 30.0f;
    cascade_reset(&s);
    for (int i = 0; i < 20000; i++) cascade_step(&s, &q, 30.0f, 28.0f, 25.0f, 1.0f);   /* raw = 30+2 > 30, err > 0 */
    CHECKM(fabsf(s.outer_integral) <= 1.0f, "outer integral %g", s.outer_integral);
    steps = 0;
    while (steps < 100) {
        cascade_step(&s, &q, 30.0f, 31.0f, 25.0f, 1.0f);                                 /* err -1 -> raw = 29 + I */
        if (s.duct_target_c < q.duct_max_c) break;
        steps++;
    }
    CHECKM(steps <= 2, "outer left saturation after %d steps", steps);
}

static void test_nan_handling(void)
{
    cascade_state_t s;
    cascade_reset(&s);
    /* NaN duct: open loop, target 35 -> (35-10)/50*100 = 50 % */
    CHECK(fabsf(cascade_step(&s, &P, 35.0f, 35.0f, NAN, 1.0f) - 50.0f) < 1e-3f);
    CHECK(fabsf(s.inner_integral - 50.0f) < 1e-3f);        /* kept in step so closing the loop does not jump */
    cascade_reset(&s);
    CHECK(cascade_step(&s, &P, 10.0f, 10.0f, NAN, 1.0f) == 0.0f);
    cascade_reset(&s);
    CHECK(cascade_step(&s, &P, 60.0f, 60.0f, NAN, 1.0f) == 100.0f);

    cascade_reset(&s);
    float v = cascade_step(&s, &P, 35.0f, 35.0f, NAN, 1.0f);
    CHECK(cascade_step(&s, &P, NAN, 35.0f, 30.0f, 1.0f) == v);    /* NaN setpoint or cabin holds the output */
    CHECK(cascade_step(&s, &P, 35.0f, NAN, 30.0f, 1.0f) == v);
    CHECK(cascade_step(NULL, &P, 35.0f, 35.0f, 30.0f, 1.0f) == 0.0f);
}

int main(void)
{
    RUN(test_reset);
    RUN(test_zero_error_is_stable);
    RUN(test_duct_target_clamped);
    RUN(test_valve_clamped);
    RUN(test_anti_windup);
    RUN(test_nan_handling);
    T_DONE();
}
