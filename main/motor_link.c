/**
 * @file motor_link.c
 * @brief Valve motor command bookkeeping (see motor_link.h). Pure C, host-tested.
 */

#include "motor_link.h"

static bool is_stop(const motor_link_t *m)
{
    return m->want_dir == MOTOR_DIR_STOP || m->want_dir == MOTOR_DIR_BRAKE || m->want_speed == 0;
}

void motor_link_init(motor_link_t *m)
{
    m->want_dir = m->want_speed = MOTOR_LINK_UNKNOWN;
    m->sent_dir = m->sent_speed = MOTOR_LINK_UNKNOWN;
    m->sent_ms = 0;
    m->stop_lost = false;
    m->link_was_down = false;
}

void motor_link_want(motor_link_t *m, int dir, int speed)
{
    m->want_dir = (int16_t)dir;
    m->want_speed = (int16_t)speed;
}

bool motor_link_pending(const motor_link_t *m)
{
    return m->want_dir != MOTOR_LINK_UNKNOWN &&
           (m->sent_dir != m->want_dir || m->sent_speed != m->want_speed);
}

unsigned motor_link_due(motor_link_t *m, uint32_t now_ms, bool link_up)
{
    if (!link_up) {
        m->link_was_down = true;
        return 0;
    }
    if (m->want_dir == MOTOR_LINK_UNKNOWN) {
        return 0;
    }
    if (m->link_was_down || (uint32_t)(now_ms - m->sent_ms) >= MOTOR_LINK_REFRESH_MS) {
        return MOTOR_SEND_DIR | MOTOR_SEND_SPEED;
    }
    unsigned mask = 0;
    if (m->sent_dir != m->want_dir) mask |= MOTOR_SEND_DIR;
    if (m->sent_speed != m->want_speed) mask |= MOTOR_SEND_SPEED;
    return mask;
}

bool motor_link_sent(motor_link_t *m, unsigned mask, bool dir_ok, bool speed_ok, uint32_t now_ms)
{
    if (mask == 0) {
        return false;
    }
    if (mask & MOTOR_SEND_DIR) {
        m->sent_dir = dir_ok ? m->want_dir : MOTOR_LINK_UNKNOWN;
    }
    if (mask & MOTOR_SEND_SPEED) {
        m->sent_speed = speed_ok ? m->want_speed : MOTOR_LINK_UNKNOWN;
    }
    m->sent_ms = now_ms;
    if (mask == (MOTOR_SEND_DIR | MOTOR_SEND_SPEED)) {
        m->link_was_down = false;
    }

    if (!is_stop(m)) {
        m->stop_lost = false;
        return false;
    }
    if (motor_link_pending(m)) {
        m->stop_lost = true;
        return false;
    }
    bool healed = m->stop_lost;
    m->stop_lost = false;
    return healed;
}
