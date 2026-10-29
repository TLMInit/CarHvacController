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
static const char *const k_ap_en[]      = { "Off", "10 min", "Always" };
static const char *const k_ap_da[]      = { "Fra", "10 min", "Altid" };

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
    INT (SET_FAN_MIN_DUTY,     FN, "Fan minimum", "Blæser minimum", 0, 100, 5, 30, "%", "fan_min", apply_controller),
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
};
