/* Stored-calibration plausibility (main/valve_cal_store.c): finding 2, a closed valve rests at
 * either end of the pot and must keep its stored calibration. */
#include "valve_cal_store.h"
#include "t.h"

static void test_range_only_with_slack(void)
{
    valve_cal_record_t rec = { .adc_min = 12, .adc_max = 88, .adc_center = 50,
                               .low_deadzone = 15, .high_deadzone = 85 };
    CHECK(valve_cal_store_plausible(&rec, 12));     /* at adc_min, inside the low deadzone */
    CHECK(valve_cal_store_plausible(&rec, 88));     /* at adc_max, inside the high deadzone */
    CHECK(valve_cal_store_plausible(&rec, 10));     /* 2 % below */
    CHECK(valve_cal_store_plausible(&rec, 90));     /* 2 % above */
    CHECK(!valve_cal_store_plausible(&rec, 7));     /* 5 % below */
    CHECK(!valve_cal_store_plausible(&rec, 93));    /* 5 % above */
    CHECK(valve_cal_store_plausible(&rec, 50));
}

static void test_ends_near_pot_limits(void)
{
    valve_cal_record_t rec = { .adc_min = 1, .adc_max = 254 };
    CHECK(valve_cal_store_plausible(&rec, 0));      /* no unsigned wrap below 0 */
    CHECK(valve_cal_store_plausible(&rec, 255));    /* no uint8 overflow above */
    CHECK(!valve_cal_store_plausible(NULL, 50));
}

/* The pot's dead gap reads 0 % or 98 % on the bench: calibration must not call it a stall. */
static void test_pot_gap(void)
{
    CHECK(valve_cal_in_pot_gap(0));
    CHECK(valve_cal_in_pot_gap(VALVE_POT_GAP_LOW_PCT));
    CHECK(!valve_cal_in_pot_gap(VALVE_POT_GAP_LOW_PCT + 1));
    CHECK(!valve_cal_in_pot_gap(50));
    CHECK(!valve_cal_in_pot_gap(VALVE_POT_GAP_HIGH_PCT - 1));
    CHECK(valve_cal_in_pot_gap(98));
    CHECK(valve_cal_in_pot_gap(100));
}

int main(void)
{
    RUN(test_pot_gap);
    RUN(test_range_only_with_slack);
    RUN(test_ends_near_pot_limits);
    T_DONE();
}
