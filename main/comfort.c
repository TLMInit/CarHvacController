/* Pure comfort maths: see comfort.h. No ESP-IDF. */
#include <math.h>
#include <string.h>
#include "comfort.h"

#define MAGNUS_B 17.62f
#define MAGNUS_C 243.12f

float comfort_dew_point_c(float temp_c, float rh_pct)
{
    if (isnan(temp_c) || isnan(rh_pct) || rh_pct <= 0.0f) {
        return NAN;
    }
    if (rh_pct > 100.0f) {
        rh_pct = 100.0f;
    }
    float g = logf(rh_pct / 100.0f) + MAGNUS_B * temp_c / (MAGNUS_C + temp_c);
    return MAGNUS_C * g / (MAGNUS_B - g);
}

float comfort_glass_temp_c(float outside_c, float offset_c)
{
    return isnan(outside_c) ? NAN : outside_c + offset_c;
}

bool comfort_defog_update(comfort_defog_t *s, float dew_point_c, float glass_c,
                          float margin_c, float exit_hysteresis_c)
{
    if (!s) {
        return false;
    }
    if (isnan(dew_point_c) || isnan(glass_c)) {
        s->active = false;
    } else if (dew_point_c >= glass_c - margin_c) {
        s->active = true;
    } else if (dew_point_c < glass_c - margin_c - exit_hysteresis_c) {
        s->active = false;
    }
    return s->active;
}

void comfort_heater_delay_start(comfort_heater_delay_t *s, float heater_c, float ready_c, uint32_t now_ms)
{
    if (!s) {
        return;
    }
    memset(s, 0, sizeof(*s));
    s->start_ms = now_ms;
    s->active = isnan(heater_c) || heater_c < ready_c;   // NaN: time limit only
    s->done = !s->active;
}

bool comfort_heater_delay_update(comfort_heater_delay_t *s, float heater_c, float ready_c,
                                 uint32_t max_ms, uint32_t now_ms)
{
    if (!s || !s->active) {
        return false;
    }
    if ((uint32_t)(now_ms - s->start_ms) >= max_ms || (!isnan(heater_c) && heater_c >= ready_c)) {
        s->active = false;
        s->done = true;
    }
    return s->active;
}
