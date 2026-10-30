/**
 * @file hvac_ui_common.h
 * @brief Small timing helpers shared by the hvac_ui_*.c pages.
 */

#ifndef HVAC_UI_COMMON_H
#define HVAC_UI_COMMON_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_timer.h"

static inline uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/** True (and *last restarted) once period ms have passed since *last. */
static inline bool refresh_due(uint32_t *last, uint32_t period)
{
    uint32_t now = now_ms();
    if ((uint32_t)(now - *last) < period) return false;
    *last = now;
    return true;
}

#endif /* HVAC_UI_COMMON_H */
