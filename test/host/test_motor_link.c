/* Valve motor command bookkeeping (main/motor_link.c): finding 1, a lost stop must not
 * leave a motor running. The valve task ticks every 25 ms. */
#include "motor_link.h"
#include "t.h"

#define FWD 1
#define TICK 25u

/* Stop frame NAKed: command stays pending, the next tick resends it, and the healed stop
 * is reported once. */
static void test_nak_on_stop_resends_next_tick(void)
{
    motor_link_t m;
    motor_link_init(&m);
    uint32_t t = 5000;

    motor_link_want(&m, FWD, 200);
    unsigned mask = motor_link_due(&m, t, true);
    CHECK(mask == (MOTOR_SEND_DIR | MOTOR_SEND_SPEED));
    CHECK(!motor_link_sent(&m, mask, true, true, t));
    CHECK(!motor_link_pending(&m));

    t += TICK;
    motor_link_want(&m, MOTOR_DIR_STOP, 0);
    mask = motor_link_due(&m, t, true);
    CHECK(mask == (MOTOR_SEND_DIR | MOTOR_SEND_SPEED));
    CHECK(!motor_link_sent(&m, mask, false, false, t));     /* the node never saw the stop */
    CHECK(motor_link_pending(&m));

    t += TICK;
    mask = motor_link_due(&m, t, true);
    CHECK(mask == (MOTOR_SEND_DIR | MOTOR_SEND_SPEED));
    CHECK(motor_link_sent(&m, mask, true, true, t));        /* healed: logged once */
    CHECK(!motor_link_pending(&m));

    t += TICK;
    CHECK(motor_link_due(&m, t, true) == 0);
}

/* Only the speed part was NAKed: only the speed is resent, and it stays pending until then. */
static void test_partial_ack_resends_missing_part(void)
{
    motor_link_t m;
    motor_link_init(&m);
    motor_link_want(&m, MOTOR_DIR_BRAKE, 0);
    unsigned mask = motor_link_due(&m, 100, true);
    motor_link_sent(&m, mask, true, false, 100);
    CHECK(motor_link_pending(&m));
    CHECK(motor_link_due(&m, 125, true) == MOTOR_SEND_SPEED);
}

/* Every commanded valve is re-asserted once a second, idle or not. */
static void test_reassert_once_a_second(void)
{
    motor_link_t m;
    motor_link_init(&m);
    CHECK(motor_link_due(&m, 10000, true) == 0);            /* never commanded: nothing sent */
    motor_link_want(&m, MOTOR_DIR_STOP, 0);
    motor_link_sent(&m, motor_link_due(&m, 10000, true), true, true, 10000);
    CHECK(motor_link_due(&m, 10000 + MOTOR_LINK_REFRESH_MS - 1, true) == 0);
    CHECK(motor_link_due(&m, 10000 + MOTOR_LINK_REFRESH_MS, true) == (MOTOR_SEND_DIR | MOTOR_SEND_SPEED));
}

/* Link down: nothing is due, not even the 1 s re-assert, and no skipped refresh marks a stop
 * lost. On the first tick with the link back, both parts of every commanded valve are due,
 * idle ones included, and only once. */
static void test_link_down_waits_for_link(void)
{
    motor_link_t idle, lost;
    motor_link_init(&idle);
    motor_link_init(&lost);
    motor_link_want(&idle, MOTOR_DIR_STOP, 0);
    CHECK(!motor_link_sent(&idle, motor_link_due(&idle, 2000, true), true, true, 2000));
    motor_link_want(&lost, MOTOR_DIR_STOP, 0);
    motor_link_sent(&lost, motor_link_due(&lost, 2000, true), false, false, 2000);  /* lost as the link broke */

    for (uint32_t t = 2025; t < 2000 + 30000; t += 25) {
        CHECK(motor_link_due(&idle, t, false) == 0);
        CHECK(motor_link_due(&lost, t, false) == 0);
    }
    CHECK(!idle.stop_lost);

    uint32_t back = 2000 + 30000;
    CHECK(motor_link_due(&idle, back, true) == (MOTOR_SEND_DIR | MOTOR_SEND_SPEED));
    CHECK(!motor_link_sent(&idle, MOTOR_SEND_DIR | MOTOR_SEND_SPEED, true, true, back));   /* no event */
    CHECK(motor_link_due(&lost, back, true) == (MOTOR_SEND_DIR | MOTOR_SEND_SPEED));
    CHECK(motor_link_sent(&lost, MOTOR_SEND_DIR | MOTOR_SEND_SPEED, true, true, back));     /* the real one */
    CHECK(motor_link_due(&idle, back + 25, true) == 0);
    CHECK(motor_link_due(&lost, back + 25, true) == 0);
}

/* A NAK on a moving command is not reported as a resent stop. */
static void test_moving_nak_not_reported(void)
{
    motor_link_t m;
    motor_link_init(&m);
    motor_link_want(&m, FWD, 150);
    motor_link_sent(&m, MOTOR_SEND_DIR | MOTOR_SEND_SPEED, false, true, 0);
    CHECK(!motor_link_sent(&m, motor_link_due(&m, 25, true), true, true, 25));
}

int main(void)
{
    RUN(test_nak_on_stop_resends_next_tick);
    RUN(test_partial_ack_resends_missing_part);
    RUN(test_reassert_once_a_second);
    RUN(test_link_down_waits_for_link);
    RUN(test_moving_nak_not_reported);
    T_DONE();
}
