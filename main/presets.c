/* One-tap presets: the table and names. Applying one is hvac_controller.c (needs the state lock). */
#include "presets_priv.h"

static const preset_def_t table[HVAC_PRESET_COUNT] = {
    [HVAC_PRESET_DEFOG]    = { HVAC_MODE_DEFROST, SET_COUNT,         HVAC_FAN_HIGH, true,  false },
    [HVAC_PRESET_MAX_HEAT] = { HVAC_MODE_FOOT,    SET_PRESET_HEAT_C, HVAC_FAN_HIGH, false, false },
    [HVAC_PRESET_MAX_COOL] = { HVAC_MODE_FACE,    SET_PRESET_COOL_C, HVAC_FAN_HIGH, true,  true  },
    [HVAC_PRESET_ECO]      = { HVAC_MODE_AUTO,    SET_COUNT,         HVAC_FAN_AUTO, false, false },
};

const preset_def_t *preset_def(hvac_preset_t preset)
{
    return (preset > HVAC_PRESET_NONE && preset < HVAC_PRESET_COUNT) ? &table[preset] : NULL;
}

const char *hvac_controller_preset_name(hvac_preset_t preset)
{
    switch (preset) {
        case HVAC_PRESET_DEFOG:    return "Defog";
        case HVAC_PRESET_MAX_HEAT: return "Max heat";
        case HVAC_PRESET_MAX_COOL: return "Max cool";
        case HVAC_PRESET_ECO:      return "Eco";
        default:                   return "";
    }
}
