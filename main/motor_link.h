/**
 * @file motor_link.h
 * @brief What a valve motor should do versus what the IO node acknowledged.
 *
 * Pure bookkeeping (no ESP-IDF), so the host tests cover it. The valve driver
 * records every command it wants with motor_link_want(), asks motor_link_due()
 * what to (re)send, and reports the node's answers with motor_link_sent().
 * A command stays pending until both direction and speed were acknowledged, and
 * every wanted command is re-asserted once a second, so a lost stop heals itself.
 */

#ifndef MOTOR_LINK_H
#define MOTOR_LINK_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MOTOR_LINK_UNKNOWN      (-1)    /* nothing wanted yet / not acknowledged */
#define MOTOR_LINK_REFRESH_MS   1000u   /* re-assert period for every wanted command; at most half of
                                           REMOTE_IO_LINK_WINDOW_MS (remote_io.h), asserted in tb6612fng_driver.c */
#define MOTOR_SEND_DIR          0x1u
#define MOTOR_SEND_SPEED        0x2u
#define MOTOR_DIR_STOP          0       /* tb6612_direction_t values: STOP, FORWARD, REVERSE, BRAKE */
#define MOTOR_DIR_BRAKE         3

typedef struct {
    int16_t want_dir, want_speed;       /* MOTOR_LINK_UNKNOWN until the first command */
    int16_t sent_dir, sent_speed;       /* last value the node acknowledged */
    uint32_t sent_ms;                   /* last send attempt */
    bool stop_lost;                     /* a stop was not acknowledged and is being resent */
    bool link_was_down;                 /* nothing sent while down: resend both parts on return */
} motor_link_t;

void motor_link_init(motor_link_t *m);
void motor_link_want(motor_link_t *m, int dir, int speed);

/** MOTOR_SEND_* bits to send now: anything not acknowledged, or both once a second.
 *  While the link is down nothing is due (every failed command costs a full link
 *  timeout); on the first call with the link back, both parts of every commanded valve
 *  are due. Skipped refreshes neither mark a stop lost nor count as sends. */
unsigned motor_link_due(motor_link_t *m, uint32_t now_ms, bool link_up);

/** Record the node's answers for the parts in @p mask. Returns true when a stop that
 *  had been lost is now acknowledged (the caller logs it once). */
bool motor_link_sent(motor_link_t *m, unsigned mask, bool dir_ok, bool speed_ok, uint32_t now_ms);

/** True while the wanted command is not fully acknowledged. */
bool motor_link_pending(const motor_link_t *m);

#ifdef __cplusplus
}
#endif

#endif /* MOTOR_LINK_H */
