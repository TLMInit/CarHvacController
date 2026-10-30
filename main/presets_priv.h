/* Private to hvac_controller.c / presets.c: the preset table. */
#ifndef PRESETS_PRIV_H
#define PRESETS_PRIV_H

#include <stdbool.h>
#include "hvac_controller.h"
#include "settings.h"

typedef struct {
    hvac_mode_t mode;
    setting_id_t setpoint_id;   /* setpoint setting; SET_COUNT = leave the setpoint alone */
    hvac_fan_speed_t fan;
    bool ac;
    bool recirc_on;             /* switch recirculation on, only if SET_RECIRC_FITTED */
} preset_def_t;

/** NULL for NONE / out of range. */
const preset_def_t *preset_def(hvac_preset_t preset);

#endif
