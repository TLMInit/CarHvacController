/**
 * @file comfort.h
 * @brief Pure comfort maths (no ESP-IDF, no state outside the structs): host-testable.
 *
 * INTERFACE (lead). Implemented by WP-E in comfort.c; tested by WP-T.
 */

#ifndef COMFORT_H
#define COMFORT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Magnus dew point (b = 17.62, c = 243.12 °C). NaN if t or rh is NaN or rh <= 0. */
float comfort_dew_point_c(float temp_c, float rh_pct);

/** Estimated inside-glass temperature: outside + offset. NaN if outside is NaN. */
float comfort_glass_temp_c(float outside_c, float offset_c);

/**
 * Dew-point defog with hysteresis. Triggers when dew_point >= glass - margin;
 * releases when dew_point < glass - margin - exit_hysteresis (2 °C in the controller).
 * Any NaN input releases (no defog without data).
 */
typedef struct {
    bool active;
} comfort_defog_t;

bool comfort_defog_update(comfort_defog_t *s, float dew_point_c, float glass_c,
                          float margin_c, float exit_hysteresis_c);

/**
 * Heater delay: hold the AUTO fan at a low level after a cold start until the heater
 * temperature reaches `ready_c` or `max_ms` has passed since start. Starts only when
 * the heater temperature at start is below ready_c; NaN heater temperature means
 * "time limit only".
 */
typedef struct {
    bool active;
    bool done;
    uint32_t start_ms;
} comfort_heater_delay_t;

void comfort_heater_delay_start(comfort_heater_delay_t *s, float heater_c, float ready_c, uint32_t now_ms);
bool comfort_heater_delay_update(comfort_heater_delay_t *s, float heater_c, float ready_c,
                                 uint32_t max_ms, uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* COMFORT_H */
