/* main/comfort.c: dew point, glass temperature, defog hysteresis, heater delay. */
#include <math.h>
#include "comfort.h"
#include "t.h"

static int near(float a, float b, float tol) { return fabsf(a - b) <= tol; }

static void test_dew_point(void)
{
    CHECK(near(comfort_dew_point_c(20.0f, 50.0f), 9.3f, 0.2f));
    CHECK(near(comfort_dew_point_c(25.0f, 80.0f), 21.3f, 0.2f));
    CHECK(near(comfort_dew_point_c(30.0f, 60.0f), 21.4f, 0.2f));
    CHECK(near(comfort_dew_point_c(0.0f, 100.0f), 0.0f, 0.05f));
    CHECK(near(comfort_dew_point_c(-5.0f, 70.0f), -9.4f, 0.3f));
    CHECK(near(comfort_dew_point_c(20.0f, 150.0f), 20.0f, 0.05f));    /* RH above 100 is clamped */
    CHECK(comfort_dew_point_c(20.0f, 50.0f) < 20.0f);
    CHECK(isnan(comfort_dew_point_c(NAN, 50.0f)));
    CHECK(isnan(comfort_dew_point_c(20.0f, NAN)));
    CHECK(isnan(comfort_dew_point_c(20.0f, 0.0f)));
    CHECK(isnan(comfort_dew_point_c(20.0f, -3.0f)));
}

static void test_glass(void)
{
    CHECK(comfort_glass_temp_c(5.0f, 2.0f) == 7.0f);
    CHECK(comfort_glass_temp_c(-10.0f, -5.0f) == -15.0f);
    CHECK(isnan(comfort_glass_temp_c(NAN, 2.0f)));
}

static void test_defog_hysteresis(void)
{
    comfort_defog_t s = { false };
    /* glass 10, margin 3 -> trigger at dew >= 7; release below 5 (margin + 2) */
    CHECK(!comfort_defog_update(&s, 6.9f, 10.0f, 3.0f, 2.0f));
    CHECK(comfort_defog_update(&s, 7.0f, 10.0f, 3.0f, 2.0f));      /* boundary triggers */
    CHECK(comfort_defog_update(&s, 6.0f, 10.0f, 3.0f, 2.0f));      /* in the hysteresis band: stays on */
    CHECK(comfort_defog_update(&s, 5.0f, 10.0f, 3.0f, 2.0f));      /* exactly glass - margin - hyst: still on */
    CHECK(!comfort_defog_update(&s, 4.9f, 10.0f, 3.0f, 2.0f));     /* below: released */
    CHECK(!comfort_defog_update(&s, 6.0f, 10.0f, 3.0f, 2.0f));     /* band again: stays off */
    CHECK(comfort_defog_update(&s, 8.0f, 10.0f, 3.0f, 2.0f));
    CHECK(!comfort_defog_update(&s, NAN, 10.0f, 3.0f, 2.0f));      /* no data: release */
    CHECK(comfort_defog_update(&s, 8.0f, 10.0f, 3.0f, 2.0f));
    CHECK(!comfort_defog_update(&s, 8.0f, NAN, 3.0f, 2.0f));
    CHECK(!comfort_defog_update(NULL, 8.0f, 10.0f, 3.0f, 2.0f));
}

static void test_heater_delay_by_temperature(void)
{
    comfort_heater_delay_t s;
    comfort_heater_delay_start(&s, 20.0f, 40.0f, 1000);
    CHECK(s.active && !s.done);
    CHECK(comfort_heater_delay_update(&s, 25.0f, 40.0f, 480000, 2000));
    CHECK(comfort_heater_delay_update(&s, 39.9f, 40.0f, 480000, 3000));
    CHECK(!comfort_heater_delay_update(&s, 40.0f, 40.0f, 480000, 4000));    /* ready: release */
    CHECK(s.done && !s.active);
    CHECK(!comfort_heater_delay_update(&s, 20.0f, 40.0f, 480000, 5000));    /* does not restart */

    comfort_heater_delay_start(&s, 50.0f, 40.0f, 0);                        /* already warm: never starts */
    CHECK(!s.active && s.done);
    CHECK(!comfort_heater_delay_update(&s, 50.0f, 40.0f, 480000, 10));
}

static void test_heater_delay_by_time(void)
{
    comfort_heater_delay_t s;
    comfort_heater_delay_start(&s, 20.0f, 40.0f, 1000);
    CHECK(comfort_heater_delay_update(&s, 20.0f, 40.0f, 8000, 8999));
    CHECK(!comfort_heater_delay_update(&s, 20.0f, 40.0f, 8000, 9000));      /* start + max */
    CHECK(s.done);

    comfort_heater_delay_start(&s, NAN, 40.0f, 5);                          /* NaN: time limit only */
    CHECK(s.active);
    CHECK(comfort_heater_delay_update(&s, NAN, 40.0f, 100, 50));
    CHECK(!comfort_heater_delay_update(&s, NAN, 40.0f, 100, 105));

    comfort_heater_delay_start(&s, 20.0f, 40.0f, 0xFFFFFF00u);              /* millisecond counter wrap */
    CHECK(comfort_heater_delay_update(&s, 20.0f, 40.0f, 1000, 0x10u));
    CHECK(!comfort_heater_delay_update(&s, 20.0f, 40.0f, 1000, 0x400u));
}

int main(void)
{
    RUN(test_dew_point);
    RUN(test_glass);
    RUN(test_defog_hysteresis);
    RUN(test_heater_delay_by_temperature);
    RUN(test_heater_delay_by_time);
    T_DONE();
}
