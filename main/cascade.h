/**
 * @file cascade.h
 * @brief Cascade temperature control, pure (no ESP-IDF): host-testable.
 *
 * INTERFACE (lead). Implemented by WP-E in cascade.c; tested by WP-T.
 *
 * Outer loop: cabin error (setpoint - cabin) -> duct temperature target,
 *   PI with anti-windup, clamped to [duct_min, duct_max].
 * Inner loop: duct error (target - duct) -> coolant valve %, PI with anti-windup,
 *   clamped to [0, 100].
 * Gains are per minute for I so they read naturally in the settings.
 * With no duct reading (NaN) the inner loop falls back to an open-loop map from
 * the duct target to a valve % (linear between duct_min -> 0 % and duct_max -> 100 %).
 */

#ifndef CASCADE_H
#define CASCADE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float outer_kp;     /* °C duct per °C cabin error */
    float outer_ki;     /* per minute */
    float inner_kp;     /* % valve per °C duct error */
    float inner_ki;     /* per minute */
    float duct_min_c;
    float duct_max_c;
} cascade_params_t;

typedef struct {
    float outer_integral;   /* °C */
    float inner_integral;   /* % */
    float duct_target_c;    /* last output of the outer loop */
    float valve_pct;        /* last output of the inner loop */
    bool initialised;
} cascade_state_t;

void cascade_reset(cascade_state_t *s);

/**
 * One step. dt_s > 0. Returns the coolant valve command 0..100 %.
 * NaN setpoint or cabin: holds the last output (valve_pct) and does not integrate.
 */
float cascade_step(cascade_state_t *s, const cascade_params_t *p,
                   float setpoint_c, float cabin_c, float duct_c, float dt_s);

#ifdef __cplusplus
}
#endif

#endif /* CASCADE_H */
