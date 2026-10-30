/* Electric compressor control (main/ac_electric.c). */
#include <math.h>
#include "ac_electric.h"
#include "t.h"

#define NEAR(a, b) (fabsf((a) - (b)) < 0.01f)

/* The settings defaults (settings_table.c) */
static ac_electric_params_t defaults(void)
{
    return (ac_electric_params_t){
        .invert = false, .out_min_pct = 10, .out_max_pct = 100, .ramp_pct_per_s = 20,
        .min_speed_pct = 20, .idle_off_s = 30, .evap_target_c = 4, .evap_off_c = 1, .evap_on_c = 4,
        .kp = 10, .ki = 0.5f,
    };
}

static ac_electric_outputs_t run(ac_electric_state_t *s, const ac_electric_params_t *p, float demand,
                                 float evap, bool interlock, float dt)
{
    ac_electric_inputs_t in = { .demand_pct = demand, .evap_c = evap, .interlock = interlock, .dt_s = dt };
    ac_electric_outputs_t o;
    ac_electric_step(s, p, &in, &o);
    return o;
}

static void test_mapping(void)
{
    ac_electric_params_t p = defaults();
    CHECK(ac_electric_map_output(&p, 0) == 0);
    CHECK(NEAR(ac_electric_map_output(&p, 1), 10.9f));         /* low speed: just above out_min */
    CHECK(NEAR(ac_electric_map_output(&p, 50), 55));
    CHECK(NEAR(ac_electric_map_output(&p, 100), 100));
    CHECK(ac_electric_idle_output(&p) == 0);
    p.out_min_pct = 20; p.out_max_pct = 80;
    CHECK(NEAR(ac_electric_map_output(&p, 50), 50) && NEAR(ac_electric_map_output(&p, 100), 80));
    p.invert = true;
    CHECK(ac_electric_map_output(&p, 0) == 100 && ac_electric_idle_output(&p) == 100);
    CHECK(NEAR(ac_electric_map_output(&p, 1), 79.4f));
    CHECK(NEAR(ac_electric_map_output(&p, 50), 50));
    CHECK(NEAR(ac_electric_map_output(&p, 100), 20));

    /* disabled: the step reports the idle level, inverted or not */
    ac_electric_state_t s;
    ac_electric_init(&s);
    ac_electric_outputs_t o = run(&s, &p, 0, NAN, false, 0.1f);
    CHECK(!o.enable && o.speed_pct == 0 && o.output_pct == 100);
    p.invert = false;
    o = run(&s, &p, 0, NAN, false, 0.1f);
    CHECK(!o.enable && o.output_pct == 0);
    o = run(&s, &p, 100, NAN, false, 0.1f);                     /* enabled: starts at the minimum */
    CHECK(o.enable && NEAR(o.speed_pct, 22) && NEAR(o.output_pct, 20 + 0.22f * 60));
}

static void test_ramp(void)
{
    ac_electric_params_t p = defaults();
    ac_electric_state_t s;
    ac_electric_init(&s);
    ac_electric_outputs_t o = run(&s, &p, 100, NAN, false, 0.1f);
    CHECK(o.enable && NEAR(o.speed_pct, 22));                  /* min 20 + 20 %/s * 0.1 s */
    for (int i = 0; i < 10; i++) o = run(&s, &p, 100, NAN, false, 0.1f);
    CHECK(NEAR(o.speed_pct, 42));                               /* 1 s later: +20 */
    for (int i = 0; i < 40; i++) o = run(&s, &p, 100, NAN, false, 0.1f);
    CHECK(NEAR(o.speed_pct, 100));
    o = run(&s, &p, 50, NAN, false, 1.0f);                      /* down by the ramp too */
    CHECK(NEAR(o.speed_pct, 80));
    o = run(&s, &p, 50, NAN, false, 1.0f);
    o = run(&s, &p, 50, NAN, false, 1.0f);
    CHECK(NEAR(o.speed_pct, 50));
    o = run(&s, &p, 50, NAN, true, 0.1f);                       /* interlock: off at once */
    CHECK(!o.enable && o.speed_pct == 0 && o.output_pct == 0 && o.idle_timer_s == 0);
    o = run(&s, &p, 50, NAN, false, 0.1f);                      /* back: from the minimum again */
    CHECK(o.enable && NEAR(o.speed_pct, 22));
}

static void test_min_speed_and_idle_off(void)
{
    ac_electric_params_t p = defaults();
    ac_electric_state_t s;
    ac_electric_init(&s);
    ac_electric_outputs_t o;
    for (int i = 0; i < 50; i++) o = run(&s, &p, 5, NAN, false, 0.1f);
    CHECK(o.enable && NEAR(o.speed_pct, 20));                   /* demand 5 % runs at the 20 % minimum */
    for (int i = 0; i < 50; i++) o = run(&s, &p, 60, NAN, false, 0.1f);
    CHECK(NEAR(o.speed_pct, 60));
    /* demand 0: down to the minimum, still enabled until 30 s without demand */
    for (int i = 0; i < 290; i++) o = run(&s, &p, 0, NAN, false, 0.1f);
    CHECK(o.enable && NEAR(o.speed_pct, 20) && NEAR(o.idle_timer_s, 29.0f));
    o = run(&s, &p, 30, NAN, false, 0.1f);                      /* demand back: timer restarts */
    CHECK(o.enable && o.idle_timer_s == 0);
    for (int i = 0; i < 299; i++) o = run(&s, &p, 0, NAN, false, 0.1f);
    CHECK(o.enable);
    for (int i = 0; i < 2; i++) o = run(&s, &p, 0, NAN, false, 0.1f);
    CHECK(!o.enable && o.speed_pct == 0 && o.output_pct == 0);
    o = run(&s, &p, 0, NAN, false, 0.1f);
    CHECK(!o.enable);                                           /* stays off without demand */
    p.idle_off_s = 0;
    o = run(&s, &p, 40, NAN, false, 0.1f);
    CHECK(o.enable);
    o = run(&s, &p, 0, NAN, false, 0.1f);
    CHECK(!o.enable);                                           /* 0 s: off with the demand */
}

static void test_freeze_limiter(void)
{
    ac_electric_params_t p = defaults();
    ac_electric_state_t s;
    ac_electric_init(&s);
    ac_electric_outputs_t o;
    for (int i = 0; i < 100; i++) o = run(&s, &p, 100, 20, false, 0.1f);
    CHECK(NEAR(o.speed_pct, 100) && !o.limiter_active);         /* far above target: PI at the bound */
    o = run(&s, &p, 100, 2.5f, false, 0.1f);                    /* half way from on (4) to off (1) */
    CHECK(o.enable && o.limiter_active && NEAR(o.speed_pct, 50));
    o = run(&s, &p, 100, 1.3f, false, 0.1f);                    /* below the minimum speed: limiter wins */
    CHECK(o.enable && NEAR(o.speed_pct, 10));
    o = run(&s, &p, 100, 1.0f, false, 0.1f);                    /* cut at evap_off */
    CHECK(!o.enable && o.speed_pct == 0 && o.freeze_cut && o.output_pct == 0);
    o = run(&s, &p, 100, 3.0f, false, 0.1f);
    CHECK(!o.enable && o.freeze_cut);
    o = run(&s, &p, 100, 4.0f, false, 0.1f);                    /* at evap_on: still latched */
    CHECK(!o.enable && o.freeze_cut);
    o = run(&s, &p, 100, 4.5f, false, 0.1f);                    /* above evap_on: restarts at the minimum */
    CHECK(o.enable && !o.freeze_cut && NEAR(o.speed_pct, 20));   /* PI asks less than the minimum */
    o = run(&s, &p, 100, NAN, false, 0.1f);                     /* no sensor: no limiter, demand level */
    CHECK(o.enable && !o.limiter_active && !o.freeze_cut);
}

/* Toy evaporator: first order towards 20 degC, cooled in proportion to speed (44 % holds 4 degC). */
static void test_pi_holds_target(void)
{
    ac_electric_params_t p = defaults();
    ac_electric_state_t s;
    ac_electric_init(&s);
    ac_electric_outputs_t o = { 0 };
    float t = 20.0f, lo = 99, dt = 0.1f;
    for (int i = 0; i < 6000; i++) {
        o = run(&s, &p, 100, t, false, dt);
        t += dt * ((20.0f - t) / 30.0f - 0.0121f * o.speed_pct);
        if (i > 100 && t < lo) lo = t;
    }
    CHECKM(fabsf(t - 4.0f) < 0.2f, "evaporator %.2f", (double)t);
    CHECKM(fabsf(o.speed_pct - 44.1f) < 3.0f, "speed %.1f", (double)o.speed_pct);
    CHECKM(lo > p.evap_off_c, "undershoot to %.2f", (double)lo);  /* never into the cut */
    CHECK(o.enable && !o.freeze_cut);

    /* the demand is the upper bound */
    for (int i = 0; i < 3000; i++) {
        o = run(&s, &p, 30, t, false, dt);
        t += dt * ((20.0f - t) / 30.0f - 0.0121f * o.speed_pct);
    }
    CHECKM(NEAR(o.speed_pct, 30) && t > 7.0f, "speed %.1f evap %.2f", (double)o.speed_pct, (double)t);

    /* target below the cut-in is held at the cut-in */
    p.evap_target_c = 2;
    for (int i = 0; i < 6000; i++) {
        o = run(&s, &p, 100, t, false, dt);
        t += dt * ((20.0f - t) / 30.0f - 0.0121f * o.speed_pct);
    }
    CHECKM(fabsf(t - 4.0f) < 0.3f, "evaporator %.2f", (double)t);
}

static void test_demand(void)
{
    ac_demand_inputs_t in = { .cabin_error_c = NAN, .deadband_c = 0.5f, .full_c = 5 };
    CHECK(ac_electric_demand(&in) == 0);
    in.auto_cooling = true;
    in.cabin_error_c = -0.5f;
    CHECK(ac_electric_demand(&in) == 0);                        /* inside the deadband */
    in.cabin_error_c = -2.75f;
    CHECK(NEAR(ac_electric_demand(&in), 50));
    in.cabin_error_c = -8;
    CHECK(ac_electric_demand(&in) == 100);
    in.cabin_error_c = 3;                                       /* heating: no cooling demand */
    CHECK(ac_electric_demand(&in) == 0);
    in.humidity = true;
    CHECK(ac_electric_demand(&in) == AC_DEMAND_HUMIDITY_PCT);
    in.defog = true;
    CHECK(ac_electric_demand(&in) == AC_DEMAND_DEFOG_PCT);
    in.cabin_error_c = -4.1f;                                   /* 80 %: the maximum wins */
    CHECK(NEAR(ac_electric_demand(&in), 80));
    in = (ac_demand_inputs_t){ .cabin_error_c = NAN, .defrost = true };
    CHECK(ac_electric_demand(&in) == AC_DEMAND_DEFROST_PCT);
    in.max_cool = true;
    CHECK(ac_electric_demand(&in) == 100);
    in = (ac_demand_inputs_t){ .cabin_error_c = NAN, .user = true };
    CHECK(ac_electric_demand(&in) == AC_DEMAND_USER_PCT);
}

int main(void)
{
    RUN(test_mapping);
    RUN(test_ramp);
    RUN(test_min_speed_and_idle_off);
    RUN(test_freeze_limiter);
    RUN(test_pi_holds_target);
    RUN(test_demand);
    T_DONE();
}
