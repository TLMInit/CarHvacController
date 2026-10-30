/**
 * @file settings_table.c
 * @brief The settings table: one row per setting_id_t, in id order.
 *
 * Kept free of ESP-IDF calls so a host test can compile it (with stubs for the
 * apply functions). settings.c checks ids, keys and ranges at init.
 */

#include "settings.h"
#include "tb6612fng_driver.h"
#include "hvac_controller.h"
#include "wifi_ap.h"
#include "remote_io.h"

/* Defined in settings.c (erases the namespaces and reboots). */
void settings_factory_reset_cb(setting_id_t id);

/* apply_cb has the setting id as argument; the targets take none (or a valve). */
static void apply_valves(setting_id_t id)     { (void)id; tb6612fng_apply_settings(); }
static void apply_cal_valve(setting_id_t id)  { (void)tb6612fng_service_recalibrate((hvac_valve_id_t)(id - SET_CAL_VALVE_0)); }
static void apply_controller(setting_id_t id) { (void)id; hvac_controller_apply_settings(); }
static void apply_wifi(setting_id_t id)       { (void)id; wifi_ap_apply_settings(); }
static void apply_node_reboot(setting_id_t id){ (void)id; (void)remote_io_reset_node(); }

static const char *const k_unit_en[]    = { "°C", "°F" };
static const char *const k_lang_en[]    = { "English", "Dansk" };
static const char *const k_powerup_en[] = { "Restore", "Auto", "Off" };
static const char *const k_powerup_da[] = { "Gendan", "Auto", "Fra" };
static const char *const k_curve_en[]   = { "Eco", "Normal", "Max" };
static const char *const k_curve_da[]   = { "Øko", "Normal", "Maks" };
static const char *const k_fan_en[]     = { "Off", "Low", "Med-low", "Medium", "Med-high", "High" };
static const char *const k_fan_da[]     = { "Fra", "Lav", "Mellem-lav", "Mellem", "Mellem-høj", "Høj" };
static const char *const k_ctrl_en[]    = { "Classic", "Cascade" };
static const char *const k_ctrl_da[]    = { "Klassisk", "Kaskade" };
static const char *const k_vdir_en[]    = { "Auto", "Normal", "Inverted" };
static const char *const k_vdir_da[]    = { "Auto", "Normal", "Omvendt" };
static const char *const k_ap_en[]      = { "Off", "Auto off", "Always" };   /* Auto off: SET_AP_IDLE_MIN */
static const char *const k_ap_da[]      = { "Fra", "Auto-fra", "Altid" };

#define ROW(i, g, en, da, t, mn, mx, st, df, ee, ed, u, key, cb) \
    [i] = { .id = i, .group = g, .label_en = en, .label_da = da, .type = t, \
            .min = mn, .max = mx, .step = st, .def = df, .def_str = NULL, \
            .enum_en = ee, .enum_da = ed, .unit = u, .nvs_key = key, .apply_cb = cb }
#define BOOL(i, g, en, da, df, key, cb) \
    ROW(i, g, en, da, SET_TYPE_BOOL, 0, 1, 1, df, NULL, NULL, NULL, key, cb)
#define ENUM(i, g, en, da, n, df, ee, ed, key, cb) \
    ROW(i, g, en, da, SET_TYPE_ENUM, 0, (n) - 1, 1, df, ee, ed, NULL, key, cb)
#define INT(i, g, en, da, mn, mx, st, df, u, key, cb) \
    ROW(i, g, en, da, SET_TYPE_INT, mn, mx, st, df, NULL, NULL, u, key, cb)
#define FLT(i, g, en, da, mn, mx, st, df, u, key, cb) \
    ROW(i, g, en, da, SET_TYPE_FLOAT, mn, mx, st, df, NULL, NULL, u, key, cb)
#define ACTION(i, g, en, da, cb) \
    ROW(i, g, en, da, SET_TYPE_ACTION, 0, 1, 1, 0, NULL, NULL, NULL, NULL, cb)

#define D SET_GROUP_DISPLAY
#define CL SET_GROUP_CLIMATE
#define FN SET_GROUP_FAN
#define CF SET_GROUP_COMFORT
#define CT SET_GROUP_CONTROL
#define VL SET_GROUP_VALVES
#define CN SET_GROUP_CONNECT
#define SY SET_GROUP_SYSTEM
#define AD SET_GROUP_ADVANCED

const setting_def_t g_settings_table[SET_COUNT] = {
    /* Display: no callback, the UI polls settings_seq() */
    ENUM(SET_TEMP_UNIT,        D, "Temperature unit", "Temperaturenhed", 2, SET_UNIT_C, k_unit_en, k_unit_en, "temp_unit", NULL),
    ENUM(SET_LANGUAGE,         D, "Language", "Sprog", 2, SET_LANG_EN, k_lang_en, k_lang_en, "language", NULL),
    BOOL(SET_SHOW_BLOWER_PCT,  D, "Show blower %", "Vis blæser %", 1, "show_blower", NULL),
    BOOL(SET_NIGHT_MODE,       D, "Night mode", "Nattilstand", 0, "night_mode", NULL),
    INT (SET_BRIGHT_DAY,       D, "Brightness day", "Lysstyrke dag", 10, 100, 5, 80, "%", "bright_day", NULL),
    INT (SET_BRIGHT_NIGHT,     D, "Brightness night", "Lysstyrke nat", 1, 100, 5, 20, "%", "bright_night", NULL),
    INT (SET_AUTO_DIM_S,       D, "Auto dim after", "Auto-dæmp efter", 0, 600, 10, 0, "s", "auto_dim_s", NULL),
    BOOL(SET_NIGHT_SCREEN_OFF, D, "Night: screen off", "Nat: skærm slukket", 0, "night_scr_off", NULL),

    /* Climate */
    FLT (SET_SETPOINT_STEP,    CL, "Setpoint step", "Trin for setpunkt", 0.1f, 1.0f, 0.1f, 0.5f, "°C", "sp_step", apply_controller),
    FLT (SET_DEFAULT_SETPOINT, CL, "Default setpoint", "Standard setpunkt", 16, 30, 0.5f, 22, "°C", "sp_default", apply_controller),
    ENUM(SET_POWER_UP_STATE,   CL, "At power-up", "Ved opstart", 3, SET_POWERUP_RESTORE, k_powerup_en, k_powerup_da, "power_up", apply_controller),
    BOOL(SET_AUTO_REQUESTS_AC, CL, "AUTO uses A/C", "AUTO bruger A/C", 0, "auto_ac", apply_controller),
    FLT (SET_HUMIDITY_ON,      CL, "Humidity A/C on", "Fugt A/C til", 50, 90, 1, 65, "%RH", "hum_on", apply_controller),
    FLT (SET_HUMIDITY_OFF,     CL, "Humidity A/C off", "Fugt A/C fra", 40, 85, 1, 58, "%RH", "hum_off", apply_controller),
    BOOL(SET_RECIRC_FITTED,    CL, "Recirc flap fitted", "Recirk.-spjæld", 0, "recirc_fitted", apply_controller),
    INT (SET_RECIRC_AUTO_CANCEL_MIN, CL, "Recirc auto-cancel", "Recirk. auto-stop", 0, 60, 1, 0, "min", "recirc_cancel", apply_controller),
    BOOL(SET_DEF_RESTORES_AC,  CL, "DEF restores A/C", "DEF gendanner A/C", 1, "def_restore_ac", apply_controller),

    /* Fan */
    INT (SET_FAN_MIN_DUTY,     FN, "Fan minimum", "Blæser minimum", 5, 100, 5, 30, "%", "fan_min", apply_controller),
    INT (SET_FAN_MAX_DUTY,     FN, "Fan maximum", "Blæser maksimum", 30, 100, 5, 100, "%", "fan_max", apply_controller),
    ENUM(SET_AUTO_CURVE,       FN, "AUTO fan curve", "AUTO blæserkurve", 3, SET_CURVE_NORMAL, k_curve_en, k_curve_da, "auto_curve", apply_controller),
    INT (SET_QUIET_NIGHT_CAP,  FN, "Night fan cap", "Blæsergrænse nat", 0, 100, 5, 0, "%", "quiet_cap", apply_controller),
    INT (SET_SOFT_START_MS,    FN, "Fan soft start", "Blæser blødstart", 0, 3000, 100, 500, "ms", "soft_start_ms", apply_controller),

    /* Comfort */
    BOOL(SET_HEATER_DELAY,     CF, "Heater delay", "Varmeforsinkelse", 0, "htr_delay", apply_controller),
    FLT (SET_HEATER_DELAY_TEMP, CF, "Delay ends at", "Forsinkelse slut", 20, 70, 1, 40, "°C", "htr_delay_temp", apply_controller),
    ENUM(SET_HEATER_DELAY_FAN, CF, "Fan during delay", "Blæser i forsink.", 6, 1 /* HVAC_FAN_LOW */, k_fan_en, k_fan_da, "htr_delay_fan", apply_controller),
    INT (SET_HEATER_DELAY_MAX_MIN, CF, "Delay maximum", "Forsinkelse maks", 1, 20, 1, 8, "min", "htr_delay_max", apply_controller),
    BOOL(SET_DEFOG,            CF, "Auto defog", "Auto-afdug", 0, "defog", apply_controller),
    FLT (SET_DEFOG_MARGIN,     CF, "Defog margin", "Afdug margin", 0, 10, 0.5f, 3, "°C", "defog_margin", apply_controller),
    BOOL(SET_DEFOG_USES_AC,    CF, "Defog uses A/C", "Afdug bruger A/C", 1, "defog_ac", apply_controller),
    FLT (SET_DEFOG_GLASS_OFFSET, CF, "Glass temp offset", "Rudetemp. offset", -5, 10, 0.5f, 2, "°C", "defog_glass_off", apply_controller),

    /* Control */
    ENUM(SET_CONTROL_MODE,     CT, "Control mode", "Regulering", 2, SET_CONTROL_CLASSIC, k_ctrl_en, k_ctrl_da, "control_mode", apply_controller),
    FLT (SET_CASCADE_OUTER_KP, CT, "Cabin loop Kp", "Kabinesløjfe Kp", 0, 20, 0.5f, 6, NULL, "outer_kp", apply_controller),
    FLT (SET_CASCADE_OUTER_KI, CT, "Cabin loop Ki", "Kabinesløjfe Ki", 0, 5, 0.1f, 0.5f, "/min", "outer_ki", apply_controller),
    FLT (SET_CASCADE_INNER_KP, CT, "Duct loop Kp", "Kanalsløjfe Kp", 0, 20, 0.5f, 3, NULL, "inner_kp", apply_controller),
    FLT (SET_CASCADE_INNER_KI, CT, "Duct loop Ki", "Kanalsløjfe Ki", 0, 10, 0.1f, 1, "/min", "inner_ki", apply_controller),
    FLT (SET_DUCT_MIN,         CT, "Duct temp minimum", "Kanaltemp. min", 0, 30, 1, 10, "°C", "duct_min", apply_controller),
    FLT (SET_DUCT_MAX,         CT, "Duct temp maximum", "Kanaltemp. maks", 30, 80, 1, 60, "°C", "duct_max", apply_controller),

    /* Valves */
    INT (SET_VALVE_SPEED_0,    VL, "Speed center 1", "Hastighed midt 1", 0, 255, 5, 0, NULL, "vspeed0", apply_valves),
    INT (SET_VALVE_SPEED_1,    VL, "Speed center 2", "Hastighed midt 2", 0, 255, 5, 0, NULL, "vspeed1", apply_valves),
    INT (SET_VALVE_SPEED_2,    VL, "Speed foot", "Hastighed fod", 0, 255, 5, 0, NULL, "vspeed2", apply_valves),
    INT (SET_VALVE_SPEED_3,    VL, "Speed windscreen", "Hastighed rude", 0, 255, 5, 0, NULL, "vspeed3", apply_valves),
    INT (SET_VALVE_SPEED_4,    VL, "Speed coolant", "Hastighed kølevand", 0, 255, 5, 0, NULL, "vspeed4", apply_valves),
    ENUM(SET_VALVE_DIR_0,      VL, "Direction center 1", "Retning midt 1", 3, SET_VDIR_AUTO, k_vdir_en, k_vdir_da, "vdir0", apply_valves),
    ENUM(SET_VALVE_DIR_1,      VL, "Direction center 2", "Retning midt 2", 3, SET_VDIR_AUTO, k_vdir_en, k_vdir_da, "vdir1", apply_valves),
    ENUM(SET_VALVE_DIR_2,      VL, "Direction foot", "Retning fod", 3, SET_VDIR_AUTO, k_vdir_en, k_vdir_da, "vdir2", apply_valves),
    ENUM(SET_VALVE_DIR_3,      VL, "Direction windscreen", "Retning rude", 3, SET_VDIR_AUTO, k_vdir_en, k_vdir_da, "vdir3", apply_valves),
    ENUM(SET_VALVE_DIR_4,      VL, "Direction coolant", "Retning kølevand", 3, SET_VDIR_AUTO, k_vdir_en, k_vdir_da, "vdir4", apply_valves),
    INT (SET_SAFE_MARGIN_PCT,  VL, "End-stop margin", "Margin til endestop", 0, 10, 1, 1, "%", "safe_margin", apply_valves),
    ACTION(SET_CAL_VALVE_0,    VL, "Calibrate center 1", "Kalibrér midt 1", apply_cal_valve),
    ACTION(SET_CAL_VALVE_1,    VL, "Calibrate center 2", "Kalibrér midt 2", apply_cal_valve),
    ACTION(SET_CAL_VALVE_2,    VL, "Calibrate foot", "Kalibrér fod", apply_cal_valve),
    ACTION(SET_CAL_VALVE_3,    VL, "Calibrate windscreen", "Kalibrér rude", apply_cal_valve),
    ACTION(SET_CAL_VALVE_4,    VL, "Calibrate coolant", "Kalibrér kølevand", apply_cal_valve),
    BOOL(SET_RECAL_ALL_NEXT_BOOT, VL, "Recalibrate at boot", "Kalibrér ved start", 0, "recal_all", apply_valves),

    /* Connectivity */
    ENUM(SET_WIFI_AP_MODE,     CN, "Phone Wi-Fi", "Telefon-Wi-Fi", 3, SET_AP_OFF, k_ap_en, k_ap_da, "ap_mode", apply_wifi),
    [SET_AP_PASSWORD] = { .id = SET_AP_PASSWORD, .group = CN, .label_en = "Wi-Fi password", .label_da = "Wi-Fi-adgangskode",
                          .type = SET_TYPE_STRING, .min = 8, .max = SETTINGS_STR_MAX - 1, .step = 0, .def = 0,
                          .def_str = "", .unit = NULL, .nvs_key = "ap_pass", .apply_cb = apply_wifi },

    /* System */
    ACTION(SET_FACTORY_RESET,  SY, "Factory reset", "Fabriksnulstil", settings_factory_reset_cb),
    ACTION(SET_NODE_REBOOT,    SY, "Restart IO node", "Genstart IO-node", apply_node_reboot),

    /* User rows that were constants (INVENTORY.md section 2); defaults = the old constants */
    FLT (SET_TEMP_DEADBAND,     CL, "Setpoint deadband", "Setpunkt dødbånd", 0.2f, 2.0f, 0.1f, 0.5f, "°C", "deadband", apply_controller),
    FLT (SET_FEET_FIRST_BELOW,  CL, "Feet-first below", "Fod først under", 10, 25, 0.5f, 18, "°C", "feet_below", apply_controller),
    INT (SET_AUTO_WS_RH,        CL, "Windscreen air above", "Rudeluft over", 50, 95, 5, 70, "%RH", "auto_ws_rh", apply_controller),
    INT (SET_SENSOR_FALLBACK_HEAT, CL, "Heat w/o cabin sensor", "Varme u. kabinesensor", 0, 100, 5, 50, "%", "fallback_heat", apply_controller),
    /* read when a preset is applied */
    FLT (SET_PRESET_HEAT_C,     CL, "Max heat preset", "Maks varme forvalg", 25, 30, 0.5f, 30, "°C", "preset_heat", NULL),
    FLT (SET_PRESET_COOL_C,     CL, "Max cool preset", "Maks kold forvalg", 16, 22, 0.5f, 16, "°C", "preset_cool", NULL),
    FLT (SET_COLD_START_BELOW,  CF, "Cold start below", "Koldstart under", 0, 20, 1, 15, "°C", "cold_below", apply_controller),
    INT (SET_COLD_START_MIN_S,  CF, "Cold start minimum", "Koldstart minimum", 0, 600, 30, 60, "s", "cold_min_s", apply_controller),
    INT (SET_COLD_START_MAX_MIN, CF, "Cold start maximum", "Koldstart maksimum", 1, 15, 1, 5, "min", "cold_max_min", apply_controller),
    FLT (SET_COLD_START_END_C,  CF, "Cold start ends at", "Koldstart slut ved", 10, 25, 0.5f, 18, "°C", "cold_end_c", apply_controller),
    INT (SET_COLD_START_END_RH, CF, "Cold start ends below", "Koldstart slut under", 30, 90, 5, 60, "%RH", "cold_end_rh", apply_controller),
    FLT (SET_COLD_START_DUCT_C, CF, "Cold start duct warm", "Koldstart kanal varm", 20, 70, 1, 40, "°C", "cold_duct_c", apply_controller),
    FLT (SET_DEFOG_RELEASE_C,   CF, "Defog release band", "Afdug slip-bånd", 0.5f, 5, 0.5f, 2, "°C", "defog_release", apply_controller),
    INT (SET_DEFOG_WS_PCT,      CF, "Defog windscreen air", "Afdug rudeluft", 30, 100, 10, 50, "%", "defog_ws", apply_controller),
    INT (SET_ECO_FAN_CAP,       FN, "Eco preset fan cap", "Eco-blæsergrænse", 30, 100, 5, 60, "%", "eco_cap", apply_controller),
    FLT (SET_AUTO_FAN_FULL_C,   FN, "AUTO full fan at", "AUTO fuld blæser ved", 2, 10, 0.5f, 5, "°C", "auto_full_c", apply_controller),
    INT (SET_FAN_LEVEL_MLOW,    FN, "Fan level Med-low", "Blæser mellem-lav", 10, 100, 5, 45, "%", "fan_mlow", apply_controller),
    INT (SET_FAN_LEVEL_MED,     FN, "Fan level Medium", "Blæser mellem", 10, 100, 5, 60, "%", "fan_med", apply_controller),
    INT (SET_FAN_LEVEL_MHIGH,   FN, "Fan level Med-high", "Blæser mellem-høj", 10, 100, 5, 80, "%", "fan_mhigh", apply_controller),
    INT (SET_AUTO_FAN_NO_SENSOR, FN, "AUTO fan, no sensor", "AUTO-blæser u. sensor", 0, 100, 5, 50, "%", "auto_fan_nosens", apply_controller),
    INT (SET_COOLANT_IDLE_PCT,  CT, "Coolant at setpoint", "Kølevand ved setpunkt", 0, 30, 1, 10, "%", "coolant_idle", apply_controller),
    /* Display: the UI polls settings_seq() */
    INT (SET_HOLD_DELAY_MS,     D,  "Hold delay", "Holdeforsinkelse", 200, 1000, 50, 400, "ms", "hold_delay_ms", NULL),
    INT (SET_HOLD_REPEAT_MS,    D,  "Hold repeat", "Holdegentagelse", 50, 500, 10, 100, "ms", "hold_rep_ms", NULL),
    INT (SET_NIGHT_OFF_S,       D,  "Night screen off after", "Nat: skærm fra efter", 10, 600, 10, 30, "s", "night_off_s", NULL),
    INT (SET_CHART_INTERVAL_S,  D,  "Trend point every", "Kurvepunkt hver", 5, 60, 5, 10, "s", "chart_s", NULL),
    INT (SET_AP_IDLE_MIN,       CN, "Wi-Fi auto-off after", "Wi-Fi auto-fra efter", 1, 60, 1, 10, "min", "ap_idle_min", apply_wifi),

    /* ADVANCED: phone page only */
    FLT (SET_HEAT_GAIN,         AD, "Classic heat gain", "Klassisk varmegain", 5, 50, 1, 20, "%/°C", "heat_gain", apply_controller),
    FLT (SET_HEAT_GAIN_MIN,     AD, "Heat gain minimum", "Varmegain minimum", 5, 50, 1, 10, "%/°C", "heat_gain_min", apply_controller),
    FLT (SET_HEAT_GAIN_MAX,     AD, "Heat gain maximum", "Varmegain maksimum", 10, 100, 1, 50, "%/°C", "heat_gain_max", apply_controller),
    INT (SET_AC_MIN_OFF_S,      AD, "A/C minimum off", "A/C minimum fra", 5, 60, 5, 10, "s", "ac_min_off_s", apply_controller),
    INT (SET_AC_MIN_ON_S,       AD, "A/C min on (humidity)", "A/C min til (fugt)", 10, 300, 10, 30, "s", "ac_min_on_s", apply_controller),
    /* used at the next AP start (like the password): wifi_ap.c reads it in ap_start() */
    INT (SET_AP_CHANNEL,        AD, "Wi-Fi channel", "Wi-Fi-kanal", 1, 13, 1, 1, NULL, "ap_channel", NULL),
};

/* ---- Cross-field rules (see settings.h) ---- */

#define SETTING(i)          { .id = (i) }
#define FIXED(v, en, da)    { .id = SET_COUNT, .k = (v), .name_en = (en), .name_da = (da) }

/* Low = fan minimum, High = fan maximum. */
const setting_operand_t g_fan_levels[SETTINGS_FAN_LEVELS] = {
    SETTING(SET_FAN_MIN_DUTY),
    SETTING(SET_FAN_LEVEL_MLOW),
    SETTING(SET_FAN_LEVEL_MED),
    SETTING(SET_FAN_LEVEL_MHIGH),
    SETTING(SET_FAN_MAX_DUTY),
};

/* A valve speed override is 0 (built-in default) or at least the stiction floor: below it the
 * driver lowers its ramp minimum to the override and the valve may not move at all. */
const setting_floor_t g_settings_floors[] = {
    { SET_VALVE_SPEED_0, VALVE_MIN_SPEED },
    { SET_VALVE_SPEED_1, VALVE_MIN_SPEED },
    { SET_VALVE_SPEED_2, VALVE_MIN_SPEED },
    { SET_VALVE_SPEED_3, VALVE_MIN_SPEED },
    { SET_VALVE_SPEED_4, COOLANT_MIN_SPEED },
};
const size_t g_settings_floor_count = sizeof(g_settings_floors) / sizeof(g_settings_floors[0]);

static const setting_operand_t k_humidity[] = { SETTING(SET_HUMIDITY_OFF), SETTING(SET_HUMIDITY_ON) };
/* min = max made the cascade hold its last output forever (cascade.c) */
static const setting_operand_t k_duct[] = { SETTING(SET_DUCT_MIN), SETTING(SET_DUCT_MAX) };
static const setting_operand_t k_heat_gain[] = { SETTING(SET_HEAT_GAIN_MIN), SETTING(SET_HEAT_GAIN_MAX) };

const setting_chain_t g_settings_chains[] = {
    { g_fan_levels, SETTINGS_FAN_LEVELS, 0 },
    { k_humidity, 2, SETTINGS_HUMIDITY_GAP },
    { k_duct, 2, SETTINGS_DUCT_GAP },
    { k_heat_gain, 2, 0 },
};
const size_t g_settings_chain_count = sizeof(g_settings_chains) / sizeof(g_settings_chains[0]);

static float op_val(const setting_operand_t *op, const float vals[SET_COUNT])
{
    return op->id < SET_COUNT ? vals[op->id] : op->k;
}

float settings_rules_clamp(setting_id_t id, float v, const float vals[SET_COUNT], setting_rule_hit_t *hit)
{
    setting_rule_hit_t h = { 0 };
    for (size_t c = 0; c < g_settings_chain_count; c++) {
        const setting_chain_t *ch = &g_settings_chains[c];
        for (int i = 0; i < ch->n; i++) {
            if (ch->ops[i].id != id) continue;
            if (i + 1 < ch->n && v > op_val(&ch->ops[i + 1], vals) - ch->gap) {
                v = op_val(&ch->ops[i + 1], vals) - ch->gap;
                h = (setting_rule_hit_t){ .chain = ch, .other = &ch->ops[i + 1], .other_above = true };
            }
            if (i > 0 && v < op_val(&ch->ops[i - 1], vals) + ch->gap) {
                v = op_val(&ch->ops[i - 1], vals) + ch->gap;
                h = (setting_rule_hit_t){ .chain = ch, .other = &ch->ops[i - 1], .other_above = false };
            }
        }
    }
    for (size_t f = 0; f < g_settings_floor_count; f++) {
        const setting_floor_t *fl = &g_settings_floors[f];
        if (fl->id != id || v == 0.0f || v >= fl->floor) continue;
        /* nearest of 0 and the floor; one that is the current value goes the other way, so a
         * -/+ step from the floor reaches 0 and a step from 0 reaches the floor */
        float near = (v < fl->floor / 2.0f) ? 0.0f : fl->floor;
        if (near == vals[id]) near = fl->floor - near;
        v = near;
        h = (setting_rule_hit_t){ .floor = fl };
    }
    if (hit) *hit = h;
    return v;
}

int settings_rules_fix(float vals[SET_COUNT])
{
    int changed = 0;
    for (size_t c = 0; c < g_settings_chain_count; c++) {
        const setting_chain_t *ch = &g_settings_chains[c];
        /* top pair first, so a lowered value is checked against the one below it in the same
         * pass; repeat for the rare upward move (a lower operand fixed or at its minimum) */
        for (int pass = 0, moved = 1; moved && pass < ch->n; pass++) {
            moved = 0;
            for (int i = ch->n - 2; i >= 0; i--) {
                const setting_operand_t *lo = &ch->ops[i], *hi = &ch->ops[i + 1];
                float want_lo = op_val(hi, vals) - ch->gap;
                if (op_val(lo, vals) <= want_lo) continue;
                if (lo->id < SET_COUNT && want_lo >= g_settings_table[lo->id].min) {
                    vals[lo->id] = want_lo;
                    moved++;
                } else if (hi->id < SET_COUNT) {
                    vals[hi->id] = op_val(lo, vals) + ch->gap;
                    moved++;
                }
            }
            changed += moved;
        }
    }
    for (size_t f = 0; f < g_settings_floor_count; f++) {
        const setting_floor_t *fl = &g_settings_floors[f];
        float v = vals[fl->id];
        if (v != 0.0f && v < fl->floor) {
            vals[fl->id] = (v < fl->floor / 2.0f) ? 0.0f : fl->floor;
            changed++;
        }
    }
    return changed;
}
