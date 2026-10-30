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
static const char *const k_comp_en[]    = { "Clutch", "Electric" };
static const char *const k_comp_da[]    = { "Kobling", "Elektrisk" };
static const char *const k_acout_en[]   = { "PWM", "Analog (RC)", "DAC MCP4725" };
static const char *const k_acpin_en[]   = { "GPIO10" };            /* = SETTINGS_AC_OUT_PINS */
static const char *const k_acfs_en[]    = { "3.3 V", "5.0 V" };
static const char *const k_acfs_da[]    = { "3,3 V", "5,0 V" };
static const char *const k_dac_en[]     = { "0x60", "0x61" };

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
#define SV SET_GROUP_SERVICE
#define AR SET_GROUP_AIR
#define AC SET_GROUP_AC

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
    FLT (SET_DEFAULT_SETPOINT, CL, "Default setpoint", "Standard setpunkt", SETTINGS_SETPOINT_MIN_C, SETTINGS_SETPOINT_MAX_C, 0.5f, 22, "°C", "sp_default", apply_controller),
    ENUM(SET_POWER_UP_STATE,   CL, "At power-up", "Ved opstart", 3, SET_POWERUP_RESTORE, k_powerup_en, k_powerup_da, "power_up", apply_controller),
    BOOL(SET_AUTO_REQUESTS_AC, AC, "AUTO uses A/C", "AUTO bruger A/C", 0, "auto_ac", apply_controller),
    FLT (SET_HUMIDITY_ON,      AC, "Humidity A/C on", "Fugt A/C til", 50, 90, 1, 65, "%RH", "hum_on", apply_controller),
    FLT (SET_HUMIDITY_OFF,     AC, "Humidity A/C off", "Fugt A/C fra", 40, 85, 1, 58, "%RH", "hum_off", apply_controller),
    BOOL(SET_RECIRC_FITTED,    CL, "Recirc flap fitted", "Recirk.-spjæld", 0, "recirc_fitted", apply_controller),
    INT (SET_RECIRC_AUTO_CANCEL_MIN, CL, "Recirc auto-cancel", "Recirk. auto-stop", 0, 60, 1, 0, "min", "recirc_cancel", apply_controller),
    BOOL(SET_DEF_RESTORES_AC,  AC, "DEF restores A/C", "DEF gendanner A/C", 1, "def_restore_ac", apply_controller),

    /* Fan */
    /* also the A/C blower interlock threshold (hvac_controller.c: blower below Low drops the A/C) */
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
    /* default and max = tb6612fng_driver.c VALVE_BOUNDARY_SAFE_MARGIN (boot value) and the clamp in tb6612fng_apply_settings */
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
    FLT (SET_PRESET_HEAT_C,     CL, "Max heat preset", "Maks varme forvalg", 25, SETTINGS_SETPOINT_MAX_C, 0.5f, SETTINGS_SETPOINT_MAX_C, "°C", "preset_heat", NULL),
    FLT (SET_PRESET_COOL_C,     CL, "Max cool preset", "Maks kold forvalg", SETTINGS_SETPOINT_MIN_C, 22, 0.5f, SETTINGS_SETPOINT_MIN_C, "°C", "preset_cool", NULL),
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

    /* Air distribution per manual mode (item 12); defaults = the old fixed table */
#define AIR(i, en, da, df, key) INT(i, AR, en, da, 0, 100, 5, df, "%", key, apply_controller)
    AIR(SET_AIR_FACE_C1,   "Face: center 1", "Ansigt: midt 1", 100, "ad_f_c1"),
    AIR(SET_AIR_FACE_C2,   "Face: center 2", "Ansigt: midt 2", 100, "ad_f_c2"),
    AIR(SET_AIR_FACE_FOOT, "Face: foot", "Ansigt: fod", 0, "ad_f_ft"),
    AIR(SET_AIR_FACE_WS,   "Face: windscreen", "Ansigt: rude", 0, "ad_f_ws"),
    AIR(SET_AIR_FOOT_C1,   "Foot: center 1", "Fod: midt 1", 0, "ad_ft_c1"),
    AIR(SET_AIR_FOOT_C2,   "Foot: center 2", "Fod: midt 2", 0, "ad_ft_c2"),
    AIR(SET_AIR_FOOT_FOOT, "Foot: foot", "Fod: fod", 100, "ad_ft_ft"),
    AIR(SET_AIR_FOOT_WS,   "Foot: windscreen", "Fod: rude", 0, "ad_ft_ws"),
    AIR(SET_AIR_FF_C1,     "Face+Foot: center 1", "Ansigt+fod: midt 1", 70, "ad_ff_c1"),
    AIR(SET_AIR_FF_C2,     "Face+Foot: center 2", "Ansigt+fod: midt 2", 70, "ad_ff_c2"),
    AIR(SET_AIR_FF_FOOT,   "Face+Foot: foot", "Ansigt+fod: fod", 70, "ad_ff_ft"),
    AIR(SET_AIR_FF_WS,     "Face+Foot: windscreen", "Ansigt+fod: rude", 0, "ad_ff_ws"),
    AIR(SET_AIR_DEF_C1,    "Defrost: center 1", "Afrim: midt 1", 0, "ad_d_c1"),
    AIR(SET_AIR_DEF_C2,    "Defrost: center 2", "Afrim: midt 2", 0, "ad_d_c2"),
    AIR(SET_AIR_DEF_FOOT,  "Defrost: foot", "Afrim: fod", 0, "ad_d_ft"),
    AIR(SET_AIR_DEF_WS,    "Defrost: windscreen", "Afrim: rude", 100, "ad_d_ws"),
    AIR(SET_AIR_DF_C1,     "Def+Foot: center 1", "Afrim+fod: midt 1", 0, "ad_df_c1"),
    AIR(SET_AIR_DF_C2,     "Def+Foot: center 2", "Afrim+fod: midt 2", 0, "ad_df_c2"),
    AIR(SET_AIR_DF_FOOT,   "Def+Foot: foot", "Afrim+fod: fod", 50, "ad_df_ft"),
    AIR(SET_AIR_DF_WS,     "Def+Foot: windscreen", "Afrim+fod: rude", 100, "ad_df_ws"),
#undef AIR

    /* SERVICE: valve service page, editable with mode OFF; defaults = the old constants.
     * Valve rows reach the driver's snapshot through tb6612fng_apply_settings(). */
    BOOL(SET_VALVE_FITTED_0,    SV, "Fitted: center 1", "Monteret: midt 1", 1, "vfit0", apply_valves),
    BOOL(SET_VALVE_FITTED_1,    SV, "Fitted: center 2", "Monteret: midt 2", 1, "vfit1", apply_valves),
    BOOL(SET_VALVE_FITTED_2,    SV, "Fitted: foot", "Monteret: fod", 1, "vfit2", apply_valves),
    BOOL(SET_VALVE_FITTED_3,    SV, "Fitted: windscreen", "Monteret: rude", 1, "vfit3", apply_valves),
    BOOL(SET_VALVE_FITTED_4,    SV, "Fitted: coolant", "Monteret: kølevand", 1, "vfit4", apply_valves),
    INT (SET_WARMUP_FOOT_PCT,   SV, "Warm-up foot air", "Opvarmning fodluft", 0, 100, 10, 70, "%", "warm_foot", apply_controller),
    INT (SET_WARMUP_CENTRE_PCT, SV, "Warm-up centre air", "Opvarmning midtluft", 0, 100, 10, 30, "%", "warm_centre", apply_controller),
    INT (SET_COLD_HEAT_CENTRE_PCT, SV, "Cold heat centre air", "Kold varme midtluft", 0, 100, 10, 30, "%", "cold_centre", apply_controller),
    INT (SET_COLD_HEAT_WS_PCT,  SV, "Cold heat windscreen", "Kold varme rude", 0, 100, 10, 30, "%", "cold_ws", apply_controller),
    INT (SET_COOLING_FOOT_PCT,  SV, "Cooling foot air", "Køling fodluft", 0, 100, 10, 30, "%", "cool_foot", apply_controller),
    INT (SET_BALANCED_PCT,      SV, "Balanced centre/foot", "Balanceret midt/fod", 0, 100, 10, 50, "%", "balanced", apply_controller),
    INT (SET_VALVE_TOL_PCT,     SV, "Position tolerance", "Positionstolerance", 1, 10, 1, 2, "%", "v_tol", apply_valves),
    INT (SET_VALVE_HYST_PCT,    SV, "Restart hysteresis", "Genstart-hysterese", 0, 10, 1, 2, "%", "v_hyst", apply_valves),
    INT (SET_VALVE_STALL_S,     SV, "Stall timeout", "Stall-tid", 2, 20, 1, 6, "s", "v_stall_s", apply_valves),
    INT (SET_VALVE_MOVE_MAX_S,  SV, "Max move time", "Maks bevægelsestid", 5, 60, 5, 20, "s", "v_move_s", apply_valves),
    INT (SET_VALVE_STALL_RETRIES, SV, "Stall retries", "Stall-forsøg", 1, 10, 1, 3, NULL, "v_retries", apply_valves),
    INT (SET_VALVE_RETRY_S,     SV, "Stall retry wait", "Stall-ventetid", 5, 60, 5, 10, "s", "v_retry_s", apply_valves),
    INT (SET_VALVE_MIN_PWM,     SV, "Butterfly min speed", "Spjæld min. fart", 10, 150, 5, 30, NULL, "v_min_pwm", apply_valves),
    INT (SET_VALVE_RAMP_PCT,    SV, "Butterfly slow band", "Spjæld bremsebånd", 5, 60, 5, 40, "%", "v_ramp", apply_valves),
    INT (SET_VALVE_FILTER,      SV, "Feedback filter", "Positionsfilter", 32, 255, 16, 128, NULL, "v_filter", apply_valves),
    INT (SET_COOLANT_MIN_PWM,   SV, "Coolant min speed", "Kølevand min. fart", 10, 255, 5, 100, NULL, "c_min_pwm", apply_valves),
    INT (SET_COOLANT_RAMP_PCT,  SV, "Coolant slow band", "Kølevand bremsebånd", 5, 60, 5, 20, "%", "c_ramp", apply_valves),
    INT (SET_JOG_PWM,           SV, "Jog speed", "Jog-fart", 20, 255, 5, 40, NULL, "jog_pwm", apply_valves),
    INT (SET_LEARN_MS,          SV, "Learn-direction move", "Lær-retning bevægelse", 300, 3000, 100, 800, "ms", "learn_ms", apply_valves),
    INT (SET_CAL_SPEED_PWM,     SV, "Calibration speed", "Kalibreringsfart", 20, 255, 5, 50, NULL, "cal_pwm", apply_valves),
    INT (SET_CAL_TIMEOUT_S,     SV, "Calibration timeout", "Kalibreringstimeout", 30, 300, 10, 120, "s", "cal_timeout_s", apply_valves),

    /* A/C: defaults = the clutch as before; electric rows are hidden unless electric (k_visible) */
    ENUM(SET_AC_COMPRESSOR,     AC, "Compressor type", "Kompressortype", 2, SET_AC_CLUTCH, k_comp_en, k_comp_da, "compressor_type", apply_controller),
    ENUM(SET_AC_OUT_TYPE,       AC, "Speed output", "Hastighedsudgang", 3, SET_AC_OUT_PWM, k_acout_en, k_acout_en, "ac_out_type", apply_controller),
    ENUM(SET_AC_OUT_PIN,        AC, "Output pin (node)", "Udgangsben (node)", 1, 0, k_acpin_en, k_acpin_en, "ac_out_pin", apply_controller),
    INT (SET_AC_OUT_FREQ_HZ,    AC, "PWM frequency", "PWM-frekvens", 100, 20000, 100, 1000, "Hz", "ac_out_freq_hz", apply_controller),
    /* Inverted, the node's safe state (duty 0 / DAC 0) reads as FULL speed: only the enable
     * relay (GPIO6) stops the compressor then, hence the label */
    BOOL(SET_AC_OUT_INVERT,     AC, "Invert (needs enable)", "Vend (kræver enable)", 0, "ac_out_invert", apply_controller),
    INT (SET_AC_OUT_MIN_PCT,    AC, "Output at min speed", "Udgang ved min. fart", 0, 100, 1, 10, "%", "ac_out_min_pct", apply_controller),
    INT (SET_AC_OUT_MAX_PCT,    AC, "Output at max speed", "Udgang ved maks. fart", 0, 100, 1, 100, "%", "ac_out_max_pct", apply_controller),
    ENUM(SET_AC_OUT_FULL_SCALE, AC, "DAC full scale", "DAC fuldt udslag", 2, SET_AC_FS_3V3, k_acfs_en, k_acfs_da, "ac_out_fs_v", apply_controller),
    ENUM(SET_AC_DAC_ADDR,       AC, "DAC address", "DAC-adresse", 2, SET_AC_DAC_0X60, k_dac_en, k_dac_en, "ac_dac_addr", apply_controller),
    INT (SET_AC_RAMP_PCT_S,     AC, "Speed ramp", "Fartrampe", 1, 100, 1, 20, "%/s", "ac_ramp_pct_s", apply_controller),
    INT (SET_AC_MIN_SPEED_PCT,  AC, "Minimum speed", "Minimum fart", 0, 100, 5, 20, "%", "ac_min_speed", apply_controller),
    INT (SET_AC_IDLE_OFF_S,     AC, "Off after no demand", "Fra efter intet behov", 0, 600, 5, 30, "s", "ac_idle_off_s", apply_controller),
    FLT (SET_EVAP_TARGET,       AC, "Evaporator target", "Fordamper mål", -2, 10, 0.5f, 4, "°C", "evap_target", apply_controller),
    FLT (SET_EVAP_OFF,          AC, "Evaporator cut-out", "Fordamper stop", -2, 8, 0.5f, 1, "°C", "evap_off", apply_controller),
    FLT (SET_EVAP_ON,           AC, "Evaporator cut-in", "Fordamper start", 0, 10, 0.5f, 4, "°C", "evap_on", apply_controller),
    /* ADVANCED: 1 degC above target asks 10 % more speed; Ti = Kp/Ki = 20 s, about the evaporator's time constant */
    FLT (SET_AC_EVAP_KP,        AD, "Evaporator loop Kp", "Fordampersløjfe Kp", 0, 50, 1, 10, "%/°C", "ac_evap_kp", apply_controller),
    FLT (SET_AC_EVAP_KI,        AD, "Evaporator loop Ki", "Fordampersløjfe Ki", 0, 5, 0.1f, 0.5f, "%/°C/s", "ac_evap_ki", apply_controller),
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

/* A valve speed override is 0 (built-in default) or at least the stiction floor (the min-speed
 * setting): below it the driver lowers its ramp minimum to the override and the valve may not move. */
const setting_floor_t g_settings_floors[] = {
    { SET_VALVE_SPEED_0, SET_VALVE_MIN_PWM },
    { SET_VALVE_SPEED_1, SET_VALVE_MIN_PWM },
    { SET_VALVE_SPEED_2, SET_VALVE_MIN_PWM },
    { SET_VALVE_SPEED_3, SET_VALVE_MIN_PWM },
    { SET_VALVE_SPEED_4, SET_COOLANT_MIN_PWM },
};
const size_t g_settings_floor_count = sizeof(g_settings_floors) / sizeof(g_settings_floors[0]);

static const setting_operand_t k_humidity[] = { SETTING(SET_HUMIDITY_OFF), SETTING(SET_HUMIDITY_ON) };
/* min = max made the cascade hold its last output forever (cascade.c) */
static const setting_operand_t k_duct[] = { SETTING(SET_DUCT_MIN), SETTING(SET_DUCT_MAX) };
static const setting_operand_t k_heat_gain[] = { SETTING(SET_HEAT_GAIN_MIN), SETTING(SET_HEAT_GAIN), SETTING(SET_HEAT_GAIN_MAX) };
/* warm-up that ends at or below its own trigger would restart on the next AUTO entry */
static const setting_operand_t k_cold_start[] = { SETTING(SET_COLD_START_BELOW), SETTING(SET_COLD_START_END_C) };
/* calibration below the stiction floor stalls and fails */
static const setting_operand_t k_cal_speed[] = { SETTING(SET_VALVE_MIN_PWM), SETTING(SET_CAL_SPEED_PWM) };
/* min = max would make the electric compressor's speed range a single output level */
static const setting_operand_t k_ac_out[] = { SETTING(SET_AC_OUT_MIN_PCT), SETTING(SET_AC_OUT_MAX_PCT) };
/* cut-in at or below the cut-out would restart the compressor straight into the freeze cut */
static const setting_operand_t k_evap[] = { SETTING(SET_EVAP_OFF), SETTING(SET_EVAP_ON) };

const setting_chain_t g_settings_chains[] = {
    { g_fan_levels, SETTINGS_FAN_LEVELS, 0 },
    { k_humidity, 2, SETTINGS_HUMIDITY_GAP },
    { k_duct, 2, SETTINGS_DUCT_GAP },
    { k_heat_gain, 3, 0 },
    { k_cold_start, 2, SETTINGS_COLD_START_GAP },
    { k_cal_speed, 2, 0 },
    { k_ac_out, 2, SETTINGS_AC_OUT_GAP },
    { k_evap, 2, SETTINGS_EVAP_GAP },
};
const size_t g_settings_chain_count = sizeof(g_settings_chains) / sizeof(g_settings_chains[0]);

/* ---- Row visibility (see settings.h): rows without a predicate are always shown ---- */

static bool electric(const float v[SET_COUNT]) { return v[SET_AC_COMPRESSOR] == SET_AC_ELECTRIC; }
static bool electric_pin(const float v[SET_COUNT]) { return electric(v) && v[SET_AC_OUT_TYPE] != SET_AC_OUT_DAC_MCP4725; }
static bool electric_pwm(const float v[SET_COUNT]) { return electric(v) && v[SET_AC_OUT_TYPE] == SET_AC_OUT_PWM; }
static bool electric_dac(const float v[SET_COUNT]) { return electric(v) && v[SET_AC_OUT_TYPE] == SET_AC_OUT_DAC_MCP4725; }

static const struct {
    setting_id_t id;
    bool (*visible)(const float v[SET_COUNT]);
} k_visible[] = {
    { SET_AC_OUT_TYPE, electric },
    { SET_AC_OUT_PIN, electric_pin },
    { SET_AC_OUT_FREQ_HZ, electric_pwm },      /* analog RC runs at SETTINGS_AC_RC_FREQ_HZ */
    { SET_AC_OUT_INVERT, electric },
    { SET_AC_OUT_MIN_PCT, electric },
    { SET_AC_OUT_MAX_PCT, electric },
    { SET_AC_OUT_FULL_SCALE, electric_dac },
    { SET_AC_DAC_ADDR, electric_dac },
    { SET_AC_RAMP_PCT_S, electric },
    { SET_AC_MIN_SPEED_PCT, electric },
    { SET_AC_IDLE_OFF_S, electric },
    { SET_EVAP_TARGET, electric },
    { SET_AC_EVAP_KP, electric },
    { SET_AC_EVAP_KI, electric },
};

bool settings_rules_visible(setting_id_t id, const float vals[SET_COUNT])
{
    for (size_t i = 0; i < sizeof(k_visible) / sizeof(k_visible[0]); i++) {
        if (k_visible[i].id == id) return k_visible[i].visible(vals);
    }
    return true;
}

/* Rows moved into the A/C group (older ids): shown after its last row, in this order. */
static const setting_id_t k_ac_moved[] = { SET_AUTO_REQUESTS_AC, SET_HUMIDITY_ON, SET_HUMIDITY_OFF, SET_DEF_RESTORES_AC };
#define AC_LAST_ROW SET_EVAP_ON

void settings_display_order(uint16_t out[SET_COUNT])
{
    size_t n = 0, nm = sizeof(k_ac_moved) / sizeof(k_ac_moved[0]);
    for (int i = 0; i < SET_COUNT; i++) {
        bool moved = false;
        for (size_t m = 0; m < nm; m++) moved |= k_ac_moved[m] == (setting_id_t)i;
        if (moved) continue;
        out[n++] = (uint16_t)i;
        if (i == AC_LAST_ROW) {
            for (size_t m = 0; m < nm; m++) out[n++] = (uint16_t)k_ac_moved[m];
        }
    }
}

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
        float floor = vals[fl->floor_id];
        if (fl->floor_id == id) {
            /* the floor itself: not above an override in use (that valve could not reach it) */
            if (vals[fl->id] != 0.0f && v > vals[fl->id]) {
                v = vals[fl->id];
                h = (setting_rule_hit_t){ .floor = fl, .other_above = true };
            }
            continue;
        }
        if (fl->id != id || v == 0.0f || v >= floor) continue;
        /* nearest of 0 and the floor; one that is the current value goes the other way, so a
         * -/+ step from the floor reaches 0 and a step from 0 reaches the floor */
        float near = (v < floor / 2.0f) ? 0.0f : floor;
        if (near == vals[id]) near = floor - near;
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
        float v = vals[fl->id], floor = vals[fl->floor_id];
        if (v != 0.0f && v < floor) {
            vals[fl->id] = (v < floor / 2.0f) ? 0.0f : floor;
            changed++;
        }
    }
    return changed;
}

bool settings_rules_broken(const float vals[SET_COUNT], setting_rule_hit_t *hit, setting_id_t *id)
{
    setting_rule_hit_t h = { 0 };
    setting_id_t who = SET_COUNT;
    for (size_t c = 0; c < g_settings_chain_count && who == SET_COUNT; c++) {
        const setting_chain_t *ch = &g_settings_chains[c];
        for (int i = 0; i + 1 < ch->n; i++) {
            const setting_operand_t *lo = &ch->ops[i], *hi = &ch->ops[i + 1];
            if (op_val(lo, vals) + ch->gap <= op_val(hi, vals)) continue;
            bool lo_is_setting = lo->id < SET_COUNT;
            who = lo_is_setting ? lo->id : hi->id;
            h = (setting_rule_hit_t){ .chain = ch, .other = lo_is_setting ? hi : lo, .other_above = lo_is_setting };
            break;
        }
    }
    for (size_t f = 0; f < g_settings_floor_count && who == SET_COUNT; f++) {
        const setting_floor_t *fl = &g_settings_floors[f];
        if (vals[fl->id] != 0.0f && vals[fl->id] < vals[fl->floor_id]) {
            who = fl->id;
            h = (setting_rule_hit_t){ .floor = fl };
        }
    }
    if (who == SET_COUNT) return false;
    if (hit) *hit = h;
    if (id) *id = who;
    return true;
}
