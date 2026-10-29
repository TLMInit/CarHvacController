/* Placeholder implementations of the WP-E interfaces (presets, comfort.h, cascade.h). WP-E deletes this file. */
#include <math.h>
#include <string.h>
#include "hvac_controller.h"
#include "comfort.h"
#include "cascade.h"

esp_err_t hvac_controller_apply_preset(hvac_preset_t preset) { (void)preset; return ESP_ERR_NOT_SUPPORTED; }
hvac_preset_t hvac_controller_get_preset(void) { return HVAC_PRESET_NONE; }
const char *hvac_controller_preset_name(hvac_preset_t preset) { (void)preset; return ""; }
void hvac_controller_apply_settings(void) {}

float comfort_dew_point_c(float temp_c, float rh_pct) { (void)temp_c; (void)rh_pct; return NAN; }
float comfort_glass_temp_c(float outside_c, float offset_c) { (void)outside_c; (void)offset_c; return NAN; }
bool comfort_defog_update(comfort_defog_t *s, float dew_point_c, float glass_c, float margin_c, float exit_hysteresis_c)
{
    (void)dew_point_c; (void)glass_c; (void)margin_c; (void)exit_hysteresis_c;
    if (s) s->active = false;
    return false;
}
void comfort_heater_delay_start(comfort_heater_delay_t *s, float heater_c, float ready_c, uint32_t now_ms)
{
    (void)heater_c; (void)ready_c; (void)now_ms;
    if (s) memset(s, 0, sizeof(*s));
}
bool comfort_heater_delay_update(comfort_heater_delay_t *s, float heater_c, float ready_c, uint32_t max_ms, uint32_t now_ms)
{
    (void)heater_c; (void)ready_c; (void)max_ms; (void)now_ms;
    return s ? s->active : false;
}

void cascade_reset(cascade_state_t *s) { if (s) memset(s, 0, sizeof(*s)); }
float cascade_step(cascade_state_t *s, const cascade_params_t *p, float setpoint_c, float cabin_c, float duct_c, float dt_s)
{
    (void)p; (void)setpoint_c; (void)cabin_c; (void)duct_c; (void)dt_s;
    return s ? s->valve_pct : 0.0f;
}
