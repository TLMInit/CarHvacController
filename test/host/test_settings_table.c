/* Settings table sanity (main/settings_table.c) plus the defaults DECISIONS.md promises. */
#include <string.h>
#include "settings.h"
#include "t.h"

extern const setting_def_t g_settings_table[SET_COUNT];

static int chars(const char *s)     /* UTF-8 code points: the display counts glyphs, not bytes */
{
    int n = 0;
    for (; *s; s++) n += ((unsigned char)*s & 0xC0) != 0x80;
    return n;
}

static int expected_enum_count(setting_id_t id)
{
    switch (id) {
    case SET_AC_OUT_PIN: return 1;                                  /* SETTINGS_AC_OUT_PINS */
    case SET_TEMP_UNIT: case SET_LANGUAGE: case SET_CONTROL_MODE: return 2;
    case SET_AC_COMPRESSOR: case SET_AC_OUT_FULL_SCALE: case SET_AC_DAC_ADDR: return 2;
    case SET_AC_OUT_TYPE: return 3;
    case SET_POWER_UP_STATE: case SET_AUTO_CURVE: case SET_WIFI_AP_MODE:
    case SET_VALVE_DIR_0: case SET_VALVE_DIR_1: case SET_VALVE_DIR_2: case SET_VALVE_DIR_3: case SET_VALVE_DIR_4: return 3;
    case SET_HEATER_DELAY_FAN: return 6;      /* OFF, LOW, MED-LOW, MEDIUM, MED-HIGH, HIGH */
    default: return -1;
    }
}

static void test_rows_in_order(void)
{
    for (int i = 0; i < SET_COUNT; i++) {
        const setting_def_t *d = &g_settings_table[i];
        CHECKM(d->id == (setting_id_t)i, "row %d has id %d", i, d->id);
        CHECKM(settings_def((setting_id_t)i) == d, "settings_def(%d)", i);
        CHECKM(d->group < SET_GROUP_COUNT, "row %d group", i);
    }
}

static void test_nvs_keys(void)
{
    for (int i = 0; i < SET_COUNT; i++) {
        const setting_def_t *d = &g_settings_table[i];
        if (d->type == SET_TYPE_ACTION) {
            CHECKM(d->nvs_key == NULL, "action row %d has a key", i);
            continue;
        }
        CHECKM(d->nvs_key && d->nvs_key[0], "row %d: empty key", i);
        if (!d->nvs_key) continue;
        CHECKM(strlen(d->nvs_key) <= 15, "row %d: key %s > 15", i, d->nvs_key);
        CHECKM(strcmp(d->nvs_key, "schema") != 0, "row %d uses reserved key", i);
        CHECKM(strcmp(d->nvs_key, "ap_pass_gen") != 0, "row %d collides with ap_pass_gen", i);
        for (int j = 0; j < i; j++) {
            if (g_settings_table[j].nvs_key) {
                CHECKM(strcmp(g_settings_table[j].nvs_key, d->nvs_key) != 0, "rows %d and %d share key %s", j, i, d->nvs_key);
            }
        }
    }
}

static void test_ranges_and_steps(void)
{
    for (int i = 0; i < SET_COUNT; i++) {
        const setting_def_t *d = &g_settings_table[i];
        if (d->type == SET_TYPE_ACTION) continue;
        if (d->type == SET_TYPE_STRING) {
            CHECK(d->def_str != NULL);
            CHECK(d->min <= d->max);
            CHECK(d->max < SETTINGS_STR_MAX);
            continue;
        }
        CHECKM(d->min <= d->def && d->def <= d->max, "row %d (%s): min %g def %g max %g", i, d->nvs_key, d->min, d->def, d->max);
        CHECKM(d->min < d->max || d->type == SET_TYPE_ENUM, "row %d: empty range", i);
        CHECKM(d->step > 0.0f, "row %d (%s): step %g", i, d->nvs_key, d->step);
    }
}

static void test_labels_and_enums(void)
{
    for (int i = 0; i < SET_COUNT; i++) {
        const setting_def_t *d = &g_settings_table[i];
        CHECKM(d->label_en && d->label_en[0] && chars(d->label_en) <= 22, "row %d en label", i);
        CHECKM(d->label_da && d->label_da[0] && chars(d->label_da) <= 22, "row %d da label", i);
        int want = expected_enum_count(d->id);
        if (d->type != SET_TYPE_ENUM) {
            CHECKM(want < 0, "row %d expected to be an enum", i);
            continue;
        }
        CHECKM(want > 0, "enum row %d not in the test's expected-count table", i);
        CHECKM((int)d->max + 1 == want, "row %d: max+1 = %d, labels = %d", i, (int)d->max + 1, want);
        CHECKM(d->min == 0, "enum row %d min", i);
        CHECK(d->enum_en && d->enum_da);
        if (!d->enum_en || !d->enum_da) continue;
        for (int k = 0; k <= (int)d->max; k++) {
            CHECKM(d->enum_en[k] && d->enum_en[k][0] && chars(d->enum_en[k]) <= 22, "row %d en label %d", i, k);
            CHECKM(d->enum_da[k] && d->enum_da[k][0] && chars(d->enum_da[k]) <= 22, "row %d da label %d", i, k);
        }
    }
}

static void test_decision_defaults(void)
{
    CHECK(settings_def(SET_AUTO_REQUESTS_AC)->def == 0);
    CHECK(settings_def(SET_HEATER_DELAY)->def == 0);
    CHECK(settings_def(SET_DEFOG)->def == 0);
    CHECK(settings_def(SET_CONTROL_MODE)->def == SET_CONTROL_CLASSIC);
    CHECK(settings_def(SET_RECIRC_FITTED)->def == 0);
    CHECK(settings_def(SET_DEF_RESTORES_AC)->def == 1);
    CHECK(settings_def(SET_QUIET_NIGHT_CAP)->def == 0);          /* "quiet-night cap off" */
    CHECK(settings_def(SET_HUMIDITY_OFF)->def <= settings_def(SET_HUMIDITY_ON)->def - SETTINGS_HUMIDITY_GAP);
}

/* Item 11: every new row's default equals the constant or literal it replaced (now deleted). */
static void test_old_constant_defaults(void)
{
    static const struct { setting_id_t id; float old; const char *was; } k[] = {
        { SET_TEMP_DEADBAND, 0.5f, "TEMP_DEADBAND (hvac_controller.c)" },
        { SET_FEET_FIRST_BELOW, 18, "feet-first literal 18.0f (hvac_controller.c)" },
        { SET_AUTO_WS_RH, 70, "AUTO windscreen literal 70.0f %RH (hvac_controller.c)" },
        { SET_SENSOR_FALLBACK_HEAT, 50, "SENSOR_FALLBACK_COOLANT_PCT" },
        { SET_PRESET_HEAT_C, 30, "presets.c MAX_HEAT 30.0f" },
        { SET_PRESET_COOL_C, 16, "presets.c MAX_COOL 16.0f" },
        { SET_COLD_START_BELOW, 15, "WARMUP_COLD_CABIN_THRESHOLD" },
        { SET_COLD_START_MIN_S, 60, "WARMUP_MIN_DURATION_MS 60000" },
        { SET_COLD_START_MAX_MIN, 5, "WARMUP_MAX_DURATION_MS 300000" },
        { SET_COLD_START_END_C, 18, "WARMUP_EXIT_CABIN_TEMP" },
        { SET_COLD_START_END_RH, 60, "WARMUP_EXIT_HUMIDITY" },
        { SET_COLD_START_DUCT_C, 40, "WARMUP_DUCT_TEMP_READY" },
        { SET_DEFOG_RELEASE_C, 2, "DEFOG_EXIT_HYSTERESIS_C" },
        { SET_DEFOG_WS_PCT, 50, "DEFOG_MIN_WINDSCREEN_PCT" },
        { SET_ECO_FAN_CAP, 60, "ECO_FAN_CAP_PCT (= BTS7960_FAN_PERCENT_MEDIUM)" },
        { SET_AUTO_FAN_FULL_C, 5, "AUTO fan knee literal 5.0f (hvac_controller.c)" },
        { SET_FAN_LEVEL_MLOW, 45, "BTS7960_FAN_PERCENT_MEDIUM_LOW" },
        { SET_FAN_LEVEL_MED, 60, "BTS7960_FAN_PERCENT_MEDIUM" },
        { SET_FAN_LEVEL_MHIGH, 80, "BTS7960_FAN_PERCENT_MEDIUM_HIGH" },
        { SET_AUTO_FAN_NO_SENSOR, 50, "BTS7960_FAN_PERCENT_AUTO_DEFAULT" },
        { SET_COOLANT_IDLE_PCT, 10, "coolant-at-setpoint literal 10 (hvac_controller.c)" },
        { SET_HOLD_DELAY_MS, 400, "LVGL long_press_time default" },
        { SET_HOLD_REPEAT_MS, 100, "LVGL long_press_repeat_time default" },
        { SET_NIGHT_OFF_S, 30, "UI_SCREEN_OFF_DEFAULT_S (hvac_ui.c)" },
        { SET_CHART_INTERVAL_S, 10, "CHART_PERIOD_MS 10000 (hvac_ui_diag.c)" },
        { SET_AP_IDLE_MIN, 10, "AP_IDLE_TIMEOUT_US 10 min (wifi_ap.c)" },
        { SET_HEAT_GAIN, 20, "BASE_COOLANT_GAIN" },
        { SET_HEAT_GAIN_MIN, 10, "MIN_COOLANT_GAIN" },
        { SET_HEAT_GAIN_MAX, 50, "MAX_COOLANT_GAIN" },
        { SET_AC_MIN_OFF_S, 10, "AC_MIN_OFF_MS 10000" },
        { SET_AC_MIN_ON_S, 30, "AC_MIN_ON_MS 30000" },
        { SET_AP_CHANNEL, 1, "AP_CHANNEL (wifi_ap.c)" },
    };
    for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
        CHECKM(settings_def(k[i].id)->def == k[i].old, "%s: default %g, old %g", k[i].was,
               (double)settings_def(k[i].id)->def, (double)k[i].old);
    }
    CHECK(settings_def(SET_HEAT_GAIN)->group == SET_GROUP_ADVANCED);
    CHECK(settings_def(SET_AP_CHANNEL)->group == SET_GROUP_ADVANCED);
}

/* Item 12: the grid's defaults are the old fixed table of update_valve_positions()
 * (hvac_controller.c): rows FACE, FOOT, FACE_FOOT, DEFROST, DEFROST_FOOT; columns centre 1,
 * centre 2, foot, windscreen. */
static void test_air_grid_defaults(void)
{
    static const float old[SETTINGS_AIR_MODES][SETTINGS_AIR_VALVES] = {
        { 100, 100, 0, 0 }, { 0, 0, 100, 0 }, { 70, 70, 70, 0 }, { 0, 0, 0, 100 }, { 0, 0, 50, 100 },
    };
    CHECK(SETTINGS_AIR_ID(0, 0) == SET_AIR_FACE_C1);
    CHECK(SETTINGS_AIR_ID(2, 2) == SET_AIR_FF_FOOT);
    CHECK(SETTINGS_AIR_ID(4, 3) == SET_AIR_DF_WS);
    for (int m = 0; m < SETTINGS_AIR_MODES; m++) {
        for (int v = 0; v < SETTINGS_AIR_VALVES; v++) {
            const setting_def_t *d = settings_def(SETTINGS_AIR_ID(m, v));
            CHECKM(d->def == old[m][v], "air %d/%d: default %g, old %g", m, v, (double)d->def, (double)old[m][v]);
            CHECK(d->group == SET_GROUP_AIR && d->type == SET_TYPE_INT && d->min == 0 && d->max == 100 && d->step == 5);
        }
    }
}

/* Item 11 (SERVICE): every default equals the deleted constant or literal it replaced. */
static void test_service_defaults(void)
{
    static const struct { setting_id_t id; float old; const char *was; } k[] = {
        { SET_VALVE_FITTED_0, 1, "VALVE_CENTER_1_ENABLED (hvac_hw_config.h)" },
        { SET_VALVE_FITTED_1, 1, "VALVE_CENTER_2_ENABLED" },
        { SET_VALVE_FITTED_2, 1, "VALVE_FOOT_ENABLED" },
        { SET_VALVE_FITTED_3, 1, "VALVE_WINDSCREEN_ENABLED" },
        { SET_VALVE_FITTED_4, 1, "VALVE_COOLANT_ENABLED" },
        { SET_WARMUP_FOOT_PCT, 70, "warm-up foot literal 70 (hvac_controller.c)" },
        { SET_WARMUP_CENTRE_PCT, 30, "warm-up centre literal 30" },
        { SET_COLD_HEAT_CENTRE_PCT, 30, "AUTO cold heating centre literal 30" },
        { SET_COLD_HEAT_WS_PCT, 30, "AUTO cold heating windscreen literal 30" },
        { SET_COOLING_FOOT_PCT, 30, "AUTO cooling foot literal 30" },
        { SET_BALANCED_PCT, 50, "AUTO balanced / AUTO entry literal 50" },
        { SET_VALVE_TOL_PCT, 2, "VALVE_POSITION_TOLERANCE" },
        { SET_VALVE_HYST_PCT, 2, "VALVE_HYSTERESIS" },
        { SET_VALVE_STALL_S, 6, "VALVE_STALL_TIMEOUT_MS 6000" },
        { SET_VALVE_MOVE_MAX_S, 20, "VALVE_MAX_MOVE_MS 2 x VALVE_FULL_TRAVEL_TIME_MS 10000" },
        { SET_VALVE_STALL_RETRIES, 3, "VALVE_STALL_RETRIES" },
        { SET_VALVE_RETRY_S, 10, "VALVE_STALL_RETRY_MS 10000" },
        { SET_VALVE_MIN_PWM, 30, "VALVE_MIN_SPEED" },
        { SET_VALVE_RAMP_PCT, 40, "VALVE_RAMP_START_ERROR" },
        { SET_VALVE_FILTER, 128, "VALVE_EMA_ALPHA" },
        { SET_COOLANT_MIN_PWM, 100, "COOLANT_MIN_SPEED" },
        { SET_COOLANT_RAMP_PCT, 20, "COOLANT_RAMP_START_ERROR" },
        { SET_JOG_PWM, 60, "SERVICE_JOG_SPEED (40; raised to 60 in v2.4.1)" },
        { SET_LEARN_MS, 800, "SERVICE_LEARN_MS" },
        { SET_CAL_SPEED_PWM, 75, "CALIB_MOTOR_SPEED (50; raised to 75 in v2.4.1)" },
        { SET_CAL_TIMEOUT_S, 120, "CALIB_MAX_ROTATION_TIME 120000" },
    };
    for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
        const setting_def_t *d = settings_def(k[i].id);
        CHECKM(d->def == k[i].old, "%s: default %g, old %g", k[i].was, (double)d->def, (double)k[i].old);
        CHECKM(d->group == SET_GROUP_SERVICE, "%s: group %d", k[i].was, d->group);
    }
}

/* v2.4 A/C group: defaults keep the clutch, keys as documented, rows moved with their keys. */
static void test_ac_rows(void)
{
    static const struct { setting_id_t id; setting_group_t g; const char *key; float min, max, def; } k[] = {
        { SET_AC_COMPRESSOR, SET_GROUP_AC, "compressor_type", 0, 1, SET_AC_CLUTCH },
        { SET_AC_OUT_TYPE, SET_GROUP_AC, "ac_out_type", 0, 2, SET_AC_OUT_PWM },
        { SET_AC_OUT_PIN, SET_GROUP_AC, "ac_out_pin", 0, 0, 0 },
        { SET_AC_OUT_FREQ_HZ, SET_GROUP_AC, "ac_out_freq_hz", 100, 20000, 1000 },
        { SET_AC_OUT_INVERT, SET_GROUP_AC, "ac_out_invert", 0, 1, 0 },
        { SET_AC_OUT_MIN_PCT, SET_GROUP_AC, "ac_out_min_pct", 0, 100, 10 },
        { SET_AC_OUT_MAX_PCT, SET_GROUP_AC, "ac_out_max_pct", 0, 100, 100 },
        { SET_AC_OUT_FULL_SCALE, SET_GROUP_AC, "ac_out_fs_v", 0, 1, SET_AC_FS_3V3 },
        { SET_AC_DAC_ADDR, SET_GROUP_AC, "ac_dac_addr", 0, 1, SET_AC_DAC_0X60 },
        { SET_AC_RAMP_PCT_S, SET_GROUP_AC, "ac_ramp_pct_s", 1, 100, 20 },
        { SET_AC_MIN_SPEED_PCT, SET_GROUP_AC, "ac_min_speed", 0, 100, 20 },
        { SET_AC_IDLE_OFF_S, SET_GROUP_AC, "ac_idle_off_s", 0, 600, 30 },
        { SET_EVAP_TARGET, SET_GROUP_AC, "evap_target", -2, 10, 4 },
        { SET_EVAP_OFF, SET_GROUP_AC, "evap_off", -2, 8, 1 },
        { SET_EVAP_ON, SET_GROUP_AC, "evap_on", 0, 10, 4 },
        { SET_AC_EVAP_KP, SET_GROUP_ADVANCED, "ac_evap_kp", 0, 50, 10 },
        { SET_AC_EVAP_KI, SET_GROUP_ADVANCED, "ac_evap_ki", 0, 5, 0.5f },
        /* moved from Climate, keys unchanged */
        { SET_AUTO_REQUESTS_AC, SET_GROUP_AC, "auto_ac", 0, 1, 0 },
        { SET_HUMIDITY_ON, SET_GROUP_AC, "hum_on", 50, 90, 65 },
        { SET_HUMIDITY_OFF, SET_GROUP_AC, "hum_off", 40, 85, 58 },
        { SET_DEF_RESTORES_AC, SET_GROUP_AC, "def_restore_ac", 0, 1, 1 },
    };
    for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
        const setting_def_t *d = settings_def(k[i].id);
        CHECKM(d->group == k[i].g && strcmp(d->nvs_key, k[i].key) == 0, "%s: group %d key %s", k[i].key, d->group, d->nvs_key);
        CHECKM(d->min == k[i].min && d->max == k[i].max && d->def == k[i].def, "%s: %g..%g def %g", k[i].key,
               (double)d->min, (double)d->max, (double)d->def);
    }
    CHECK(settings_def(SET_AC_MIN_OFF_S)->group == SET_GROUP_ADVANCED);
    CHECK(settings_def(SET_AC_MIN_ON_S)->group == SET_GROUP_ADVANCED);
    static const int pins[] = SETTINGS_AC_OUT_PINS;
    CHECK((int)(sizeof(pins) / sizeof(pins[0])) == (int)settings_def(SET_AC_OUT_PIN)->max + 1 && pins[0] == 10);
}

/* Display order: every id once; the A/C group starts with the compressor type, the moved rows last. */
static void test_display_order(void)
{
    uint16_t order[SET_COUNT];
    int seen[SET_COUNT] = { 0 };
    settings_display_order(order);
    for (int i = 0; i < SET_COUNT; i++) {
        CHECK(order[i] < SET_COUNT);
        if (order[i] < SET_COUNT) seen[order[i]]++;
    }
    for (int i = 0; i < SET_COUNT; i++) CHECKM(seen[i] == 1, "id %d seen %d times", i, seen[i]);
    setting_id_t ac[SET_COUNT];
    int n = 0;
    for (int i = 0; i < SET_COUNT; i++) {
        if (settings_def((setting_id_t)order[i])->group == SET_GROUP_AC) ac[n++] = (setting_id_t)order[i];
    }
    static const setting_id_t want[] = {
        SET_AC_COMPRESSOR, SET_AC_OUT_TYPE, SET_AC_OUT_PIN, SET_AC_OUT_FREQ_HZ, SET_AC_OUT_INVERT,
        SET_AC_OUT_MIN_PCT, SET_AC_OUT_MAX_PCT, SET_AC_OUT_FULL_SCALE, SET_AC_DAC_ADDR, SET_AC_RAMP_PCT_S,
        SET_AC_MIN_SPEED_PCT, SET_AC_IDLE_OFF_S, SET_EVAP_TARGET, SET_EVAP_OFF, SET_EVAP_ON,
        SET_AUTO_REQUESTS_AC, SET_HUMIDITY_ON, SET_HUMIDITY_OFF, SET_DEF_RESTORES_AC,
    };
    CHECK(n == (int)(sizeof(want) / sizeof(want[0])));
    for (int i = 0; i < n && i < (int)(sizeof(want) / sizeof(want[0])); i++) CHECKM(ac[i] == want[i], "A/C row %d: id %d", i, ac[i]);
    /* other groups keep id order */
    int last = -1;
    for (int i = 0; i < SET_COUNT; i++) {
        if (settings_def((setting_id_t)order[i])->group != SET_GROUP_CLIMATE) continue;
        CHECK((int)order[i] > last);
        last = order[i];
    }
}

/* Electric rows only with compressor = electric; per output type the pin / frequency / DAC rows. */
static void test_visibility(void)
{
    float v[SET_COUNT];
    for (int i = 0; i < SET_COUNT; i++) v[i] = settings_def((setting_id_t)i)->def;
    CHECK(settings_rules_visible(SET_AC_COMPRESSOR, v));
    CHECK(settings_rules_visible(SET_EVAP_OFF, v) && settings_rules_visible(SET_EVAP_ON, v));   /* both types */
    CHECK(settings_rules_visible(SET_HUMIDITY_ON, v) && settings_rules_visible(SET_TEMP_UNIT, v));
    for (int id = SET_AC_OUT_TYPE; id <= SET_EVAP_TARGET; id++) CHECKM(!settings_rules_visible((setting_id_t)id, v), "clutch: %d", id);
    CHECK(!settings_rules_visible(SET_AC_EVAP_KP, v) && !settings_rules_visible(SET_AC_EVAP_KI, v));
    v[SET_AC_COMPRESSOR] = SET_AC_ELECTRIC;
    CHECK(settings_rules_visible(SET_AC_OUT_PIN, v) && settings_rules_visible(SET_AC_OUT_FREQ_HZ, v));
    CHECK(!settings_rules_visible(SET_AC_OUT_FULL_SCALE, v) && !settings_rules_visible(SET_AC_DAC_ADDR, v));
    CHECK(settings_rules_visible(SET_EVAP_TARGET, v) && settings_rules_visible(SET_AC_EVAP_KI, v));
    v[SET_AC_OUT_TYPE] = SET_AC_OUT_ANALOG_RC;
    CHECK(settings_rules_visible(SET_AC_OUT_PIN, v) && !settings_rules_visible(SET_AC_OUT_FREQ_HZ, v));
    v[SET_AC_OUT_TYPE] = SET_AC_OUT_DAC_MCP4725;
    CHECK(!settings_rules_visible(SET_AC_OUT_PIN, v) && !settings_rules_visible(SET_AC_OUT_FREQ_HZ, v));
    CHECK(settings_rules_visible(SET_AC_OUT_FULL_SCALE, v) && settings_rules_visible(SET_AC_DAC_ADDR, v));
    CHECK(settings_rules_visible(SET_AC_OUT_MIN_PCT, v) && settings_rules_visible(SET_AC_OUT_INVERT, v));
}

int main(void)
{
    RUN(test_rows_in_order);
    RUN(test_nvs_keys);
    RUN(test_ranges_and_steps);
    RUN(test_labels_and_enums);
    RUN(test_decision_defaults);
    RUN(test_old_constant_defaults);
    RUN(test_air_grid_defaults);
    RUN(test_service_defaults);
    RUN(test_ac_rows);
    RUN(test_display_order);
    RUN(test_visibility);
    T_DONE();
}
