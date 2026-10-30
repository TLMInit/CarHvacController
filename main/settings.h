/**
 * @file settings.h
 * @brief User settings: one table, persisted in NVS namespace "hvac_cfg".
 *
 * INTERFACE (lead). Implemented by WP-A in settings.c (framework) and
 * settings_table.c (the table). Everyone else reads values through the
 * getters below and never touches NVS for these keys.
 *
 * Rules
 * - Every setting has an id below, one row in the table, one NVS key (<= 15 chars).
 * - Values are clamped to [min, max] and snapped to step on set.
 * - Namespace "hvac_cfg" carries a u16 key "schema". Unknown keys are ignored,
 *   missing keys read as the default, an older schema is migrated on init.
 *   The same scheme (settings_ns_open) is used for "hvac_cal" and "hvac_thermal".
 * - Setters are thread-safe and callable from any task. A change is written to
 *   NVS (debounced inside settings.c, at most once per 2 s per key) and then the
 *   row's apply_cb runs in the caller's context. apply_cb must not block for long
 *   and must not call LVGL.
 * - SET_TYPE_ACTION rows hold no value: settings_set_int(id, 1) runs apply_cb.
 */

#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "nvs.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SETTINGS_NAMESPACE      "hvac_cfg"
#define SETTINGS_SCHEMA         1
#define SETTINGS_STR_MAX        33      /* longest string value incl. NUL (AP password: 8..32 chars) */
#define SETTINGS_HUMIDITY_GAP   2.0f    /* %RH: Humidity A/C off stays at least this far below on (a chain rule) */
#define SETTINGS_DUCT_GAP       5.0f    /* degC: duct minimum stays at least this far below the maximum (a chain rule) */
#define SETTINGS_COLD_START_GAP 1.0f    /* degC: cold start ends at least this far above where it starts (a chain rule) */

typedef enum {
    SET_TYPE_BOOL = 0,
    SET_TYPE_INT,
    SET_TYPE_ENUM,
    SET_TYPE_FLOAT,
    SET_TYPE_STRING,    /* extension to the brief: the AP password */
    SET_TYPE_ACTION,    /* extension to the brief: a button row, no stored value */
} setting_type_t;

typedef enum {
    SET_GROUP_DISPLAY = 0,
    SET_GROUP_CLIMATE,
    SET_GROUP_FAN,
    SET_GROUP_COMFORT,
    SET_GROUP_CONTROL,
    SET_GROUP_VALVES,
    SET_GROUP_CONNECT,
    SET_GROUP_SYSTEM,
    SET_GROUP_SERVICE,          /* valve service page only, editable with mode OFF */
    SET_GROUP_ADVANCED,         /* phone page only */
    SET_GROUP_AIR,              /* air distribution grid: a sub-page of Valves on the display */
    SET_GROUP_COUNT
} setting_group_t;

typedef enum {
    SET_LANG_EN = 0,
    SET_LANG_DA,
} setting_lang_t;

/* Enum value sets used by the table (the stored value is the enum's integer). */
typedef enum { SET_UNIT_C = 0, SET_UNIT_F } set_unit_t;
typedef enum { SET_POWERUP_RESTORE = 0, SET_POWERUP_AUTO, SET_POWERUP_OFF } set_powerup_t;
typedef enum { SET_CURVE_ECO = 0, SET_CURVE_NORMAL, SET_CURVE_MAX } set_curve_t;
typedef enum { SET_VDIR_AUTO = 0, SET_VDIR_NORMAL, SET_VDIR_INVERTED } set_valve_dir_t;
typedef enum { SET_AP_OFF = 0, SET_AP_10MIN, SET_AP_ALWAYS } set_ap_mode_t;
typedef enum { SET_CONTROL_CLASSIC = 0, SET_CONTROL_CASCADE } set_control_mode_t;

/* Every setting. Order is the display order within a group. Append only; never renumber. */
typedef enum {
    /* Display */
    SET_TEMP_UNIT = 0,          /* enum set_unit_t, default C */
    SET_LANGUAGE,               /* enum setting_lang_t, default EN */
    SET_SHOW_BLOWER_PCT,        /* bool, default yes */
    SET_NIGHT_MODE,             /* bool, the DAY/NIGHT choice on the main screen, default DAY */
    SET_BRIGHT_DAY,             /* int %, 10..100, default 80 */
    SET_BRIGHT_NIGHT,           /* int %, 1..100, default 20 */
    SET_AUTO_DIM_S,             /* int s, 0 = off .. 600, default 0: dim to night level after idle */
    SET_NIGHT_SCREEN_OFF,       /* bool, default no: at night, backlight off after idle, wake on touch */

    /* Climate */
    SET_SETPOINT_STEP,          /* float degC, 0.1..1.0 step 0.1, default 0.5 */
    SET_DEFAULT_SETPOINT,       /* float degC, 16..30 step 0.5, default 22 */
    SET_POWER_UP_STATE,         /* enum set_powerup_t, default RESTORE */
    SET_AUTO_REQUESTS_AC,       /* bool, default no: AUTO asks for A/C when cooling (interlocks still apply) */
    SET_HUMIDITY_ON,            /* float %RH, 50..90, default 65 */
    SET_HUMIDITY_OFF,           /* float %RH, 40..85, default 58 (<= ON - SETTINGS_HUMIDITY_GAP, a chain rule) */
    SET_RECIRC_FITTED,          /* bool, default no: no recirc flap -> button hidden, presets leave it alone */
    SET_RECIRC_AUTO_CANCEL_MIN, /* int min, 0 = off .. 60, default 0 */
    SET_DEF_RESTORES_AC,        /* bool, default yes: leaving DEF restores the A/C choice from before DEF */

    /* Fan */
    SET_FAN_MIN_DUTY,           /* int %, 5..100, default 30: manual LOW and the AUTO floor */
    SET_FAN_MAX_DUTY,           /* int %, 30..100, default 100 */
    SET_AUTO_CURVE,             /* enum set_curve_t, default NORMAL */
    SET_QUIET_NIGHT_CAP,        /* int %, 0 = off .. 100, default 0: fan cap while NIGHT is selected */
    SET_SOFT_START_MS,          /* int ms, 0..3000 step 100, default 500: blower ramp */

    /* Comfort */
    SET_HEATER_DELAY,           /* bool, default no */
    SET_HEATER_DELAY_TEMP,      /* float degC, 20..70, default 40: coolant (or duct) temperature that ends it */
    SET_HEATER_DELAY_FAN,       /* enum hvac_fan_speed_t OFF..HIGH, default LOW */
    SET_HEATER_DELAY_MAX_MIN,   /* int min, 1..20, default 8 */
    SET_DEFOG,                  /* bool, default no: dew-point defog */
    SET_DEFOG_MARGIN,           /* float degC, 0..10, default 3 */
    SET_DEFOG_USES_AC,          /* bool, default yes */
    SET_DEFOG_GLASS_OFFSET,     /* float degC, -5..10, default 2: glass = outside + offset */

    /* Control */
    SET_CONTROL_MODE,           /* enum set_control_mode_t, default CLASSIC */
    SET_CASCADE_OUTER_KP,       /* float degC duct per degC cabin error, 0..20, default 6 */
    SET_CASCADE_OUTER_KI,       /* float per minute, 0..5, default 0.5 */
    SET_CASCADE_INNER_KP,       /* float % valve per degC duct error, 0..20, default 3 */
    SET_CASCADE_INNER_KI,       /* float per minute, 0..10, default 1 */
    SET_DUCT_MIN,               /* float degC, 0..30, default 10; <= DUCT_MAX - SETTINGS_DUCT_GAP (a chain rule) */
    SET_DUCT_MAX,               /* float degC, 30..80, default 60 */

    /* Valves (index = hvac_valve_id_t) */
    SET_VALVE_SPEED_0,          /* int PWM, 0 = built-in default, else >= SET_VALVE_MIN_PWM (coolant SET_COOLANT_MIN_PWM) .. 255; one per valve; caps every move */
    SET_VALVE_SPEED_1,
    SET_VALVE_SPEED_2,
    SET_VALVE_SPEED_3,
    SET_VALVE_SPEED_4,
    SET_VALVE_DIR_0,            /* enum set_valve_dir_t, default AUTO (= calibrated); one per valve */
    SET_VALVE_DIR_1,
    SET_VALVE_DIR_2,
    SET_VALVE_DIR_3,
    SET_VALVE_DIR_4,
    SET_SAFE_MARGIN_PCT,        /* int %, 0..10, default 1: distance kept from calibrated ends */
    SET_CAL_VALVE_0,            /* action: calibrate this valve now; one per valve */
    SET_CAL_VALVE_1,
    SET_CAL_VALVE_2,
    SET_CAL_VALVE_3,
    SET_CAL_VALVE_4,
    SET_RECAL_ALL_NEXT_BOOT,    /* bool, default no; cleared by the valve driver after use */

    /* Connectivity */
    SET_WIFI_AP_MODE,           /* enum set_ap_mode_t, default OFF */
    SET_AP_PASSWORD,            /* string 8..32; empty = random per-device password made at first boot */

    /* System */
    SET_FACTORY_RESET,          /* action: erase hvac_cfg, hvac_cal, hvac_thermal, hvac; reboot */
    SET_NODE_REBOOT,            /* action: MSG_RESET to the IO node */

    /* User rows that were constants (INVENTORY.md section 2); each group in the table */
    SET_TEMP_DEADBAND,          /* float degC, 0.2..2.0 step 0.1, default 0.5: no heat/cool band, AUTO fan lower knee */
    SET_FEET_FIRST_BELOW,       /* float degC, 10..25 step 0.5, default 18: AUTO heating sends air to the feet */
    SET_AUTO_WS_RH,             /* int %RH, 50..95 step 5, default 70: AUTO adds windscreen air above */
    SET_SENSOR_FALLBACK_HEAT,   /* int %, 0..100 step 5, default 50: coolant valve without a cabin reading */
    SET_PRESET_HEAT_C,          /* float degC, 25..30 step 0.5, default 30: Max heat preset setpoint */
    SET_PRESET_COOL_C,          /* float degC, 16..22 step 0.5, default 16: Max cool preset setpoint */
    SET_COLD_START_BELOW,       /* float degC, 0..20 step 1, default 15: AUTO warm-up starts below */
    SET_COLD_START_MIN_S,       /* int s, 0..600 step 30, default 60 */
    SET_COLD_START_MAX_MIN,     /* int min, 1..15 step 1, default 5 */
    SET_COLD_START_END_C,       /* float degC, 10..25 step 0.5, default 18; >= SET_COLD_START_BELOW + SETTINGS_COLD_START_GAP (a chain rule) */
    SET_COLD_START_END_RH,      /* int %RH, 30..90 step 5, default 60 */
    SET_COLD_START_DUCT_C,      /* float degC, 20..70 step 1, default 40 */
    SET_DEFOG_RELEASE_C,        /* float degC, 0.5..5 step 0.5, default 2 */
    SET_DEFOG_WS_PCT,           /* int %, 30..100 step 10, default 50 */
    SET_ECO_FAN_CAP,            /* int %, 30..100 step 5, default 60: AUTO fan cap under the Eco preset */
    SET_AUTO_FAN_FULL_C,        /* float degC, 2..10 step 0.5, default 5: cabin error for 100 % AUTO fan */
    SET_FAN_LEVEL_MLOW,         /* int %, 10..100 step 5, default 45; fan chain: min <= Med-low <= Medium <= Med-high <= max */
    SET_FAN_LEVEL_MED,          /* int %, 10..100 step 5, default 60 */
    SET_FAN_LEVEL_MHIGH,        /* int %, 10..100 step 5, default 80 */
    SET_AUTO_FAN_NO_SENSOR,     /* int %, 0..100 step 5, default 50: AUTO fan without a cabin reading */
    SET_COOLANT_IDLE_PCT,       /* int %, 0..30 step 1, default 10: classic law at the setpoint */
    SET_HOLD_DELAY_MS,          /* int ms, 200..1000 step 50, default 400: LVGL long press */
    SET_HOLD_REPEAT_MS,         /* int ms, 50..500 step 10, default 100: LVGL long-press repeat */
    SET_NIGHT_OFF_S,            /* int s, 10..600 step 10, default 30: night screen-off when auto-dim is 0 */
    SET_CHART_INTERVAL_S,       /* int s, 5..60 step 5, default 10: trend chart point (60 points) */
    SET_AP_IDLE_MIN,            /* int min, 1..60 step 1, default 10: Wi-Fi auto-off without a phone */

    /* Advanced (phone page only) */
    SET_HEAT_GAIN,              /* float %/degC, 5..50 step 1, default 20: classic law base gain */
    SET_HEAT_GAIN_MIN,          /* float %/degC, 5..50 step 1, default 10; chain: minimum <= SET_HEAT_GAIN <= maximum */
    SET_HEAT_GAIN_MAX,          /* float %/degC, 10..100 step 1, default 50 */
    SET_AC_MIN_OFF_S,           /* int s, 5..60 step 5, default 10 */
    SET_AC_MIN_ON_S,            /* int s, 10..300 step 10, default 30: humidity-only clutch */
    SET_AP_CHANNEL,             /* int, 1..13, default 1: used at the next AP start */

    /* Air distribution: valve % per manual mode (rows FACE..DEFROST_FOOT, columns centre 1,
     * centre 2, foot, windscreen = hvac_valve_id_t 0..3); int 0..100 step 5. Use SETTINGS_AIR_ID. */
    SET_AIR_FACE_C1, SET_AIR_FACE_C2, SET_AIR_FACE_FOOT, SET_AIR_FACE_WS,
    SET_AIR_FOOT_C1, SET_AIR_FOOT_C2, SET_AIR_FOOT_FOOT, SET_AIR_FOOT_WS,
    SET_AIR_FF_C1, SET_AIR_FF_C2, SET_AIR_FF_FOOT, SET_AIR_FF_WS,
    SET_AIR_DEF_C1, SET_AIR_DEF_C2, SET_AIR_DEF_FOOT, SET_AIR_DEF_WS,
    SET_AIR_DF_C1, SET_AIR_DF_C2, SET_AIR_DF_FOOT, SET_AIR_DF_WS,

    /* Service (valve service page, editable with mode OFF); defaults = the old constants */
    SET_VALVE_FITTED_0,         /* bool, default yes; one per valve (hvac_valve_id_t): not fitted = never driven, calibrated or self-tested */
    SET_VALVE_FITTED_1,
    SET_VALVE_FITTED_2,
    SET_VALVE_FITTED_3,
    SET_VALVE_FITTED_4,
    SET_WARMUP_FOOT_PCT,        /* int %, 0..100 step 10, default 70: AUTO cold-start foot valve */
    SET_WARMUP_CENTRE_PCT,      /* int %, 0..100 step 10, default 30: AUTO cold-start centre valves */
    SET_COLD_HEAT_CENTRE_PCT,   /* int %, 0..100 step 10, default 30: AUTO heating a cold cabin, centres */
    SET_COLD_HEAT_WS_PCT,       /* int %, 0..100 step 10, default 30: same, windscreen */
    SET_COOLING_FOOT_PCT,       /* int %, 0..100 step 10, default 30: AUTO cooling, foot */
    SET_BALANCED_PCT,           /* int %, 0..100 step 10, default 50: AUTO at setpoint and on entry, centres and foot */
    SET_VALVE_TOL_PCT,          /* int %, 1..10, default 2: "at target" window */
    SET_VALVE_HYST_PCT,         /* int %, 0..10, default 2: extra error before a stopped valve restarts */
    SET_VALVE_STALL_S,          /* int s, 2..20, default 6: no movement while driving = stall */
    SET_VALVE_MOVE_MAX_S,       /* int s, 5..60 step 5, default 20: one move longer than this = stall */
    SET_VALVE_STALL_RETRIES,    /* int, 1..10, default 3 (at least 1: a rule) */
    SET_VALVE_RETRY_S,          /* int s, 5..60 step 5, default 10: back-off step between retries */
    SET_VALVE_MIN_PWM,          /* int PWM, 10..150 step 5, default 30: butterfly ramp floor; floor of SET_VALVE_SPEED_0..3 */
    SET_VALVE_RAMP_PCT,         /* int %, 5..60 step 5, default 40: butterfly slows below this error */
    SET_VALVE_FILTER,           /* int /256, 32..255 step 16, default 128: position EMA weight of a new reading */
    SET_COOLANT_MIN_PWM,        /* int PWM, 10..255 step 5, default 100: coolant ramp floor; floor of SET_VALVE_SPEED_4 */
    SET_COOLANT_RAMP_PCT,       /* int %, 5..60 step 5, default 20 */
    SET_JOG_PWM,                /* int PWM, 20..255 step 5, default 40: service jog */
    SET_LEARN_MS,               /* int ms, 300..3000 step 100, default 800: learn-direction move */
    SET_CAL_SPEED_PWM,          /* int PWM, 20..255 step 5, default 50; >= SET_VALVE_MIN_PWM (a chain rule) */
    SET_CAL_TIMEOUT_S,          /* int s, 30..300 step 10, default 120: per calibration phase */

    SET_COUNT
} setting_id_t;

/* Air distribution grid: m = mode - HVAC_MODE_FACE (0..4), v = hvac_valve_id_t (0..3). */
#define SETTINGS_AIR_MODES      5
#define SETTINGS_AIR_VALVES     4
#define SETTINGS_AIR_ID(m, v)   ((setting_id_t)(SET_AIR_FACE_C1 + (m) * SETTINGS_AIR_VALVES + (v)))

typedef void (*setting_apply_cb_t)(setting_id_t id);

typedef struct {
    setting_id_t id;
    setting_group_t group;
    const char *label_en;
    const char *label_da;
    setting_type_t type;
    float min;                      /* BOOL 0/1, ENUM 0..n-1, STRING: min length */
    float max;                      /* STRING: max length */
    float step;
    float def;                      /* STRING: unused (see def_str) */
    const char *def_str;            /* STRING default, else NULL */
    const char *const *enum_en;     /* ENUM labels, n = max + 1; else NULL */
    const char *const *enum_da;
    const char *unit;               /* "%", "°C", "s", ... or NULL */
    const char *nvs_key;            /* <= 15 chars; NULL for ACTION */
    setting_apply_cb_t apply_cb;    /* may be NULL */
} setting_def_t;

/** Load namespace hvac_cfg (migrating if needed). Call after nvs_flash_init(), before anything reads a setting. */
esp_err_t settings_init(void);

/** Table row for an id (never NULL for id < SET_COUNT). */
const setting_def_t *settings_def(setting_id_t id);

/** Visit every row in table order. Stop early by returning false. */
void settings_iterate(bool (*cb)(const setting_def_t *def, void *ctx), void *ctx);

int32_t settings_get_int(setting_id_t id);      /* BOOL/INT/ENUM; FLOAT rounded */
float   settings_get_float(setting_id_t id);    /* any numeric type */
bool    settings_get_bool(setting_id_t id);
void    settings_get_str(setting_id_t id, char *buf, size_t len);

esp_err_t settings_set_int(setting_id_t id, int32_t value);
esp_err_t settings_set_float(setting_id_t id, float value);
esp_err_t settings_set_str(setting_id_t id, const char *value);
esp_err_t settings_step(setting_id_t id, int direction);   /* +1 / -1 by the row's step (UI - / + buttons) */
esp_err_t settings_reset(setting_id_t id);                 /* back to the default */
esp_err_t settings_reset_all(void);

/** Value as text for the UI/web, e.g. "22.0 °C", "Normal", "On", in the current language. */
void settings_format(setting_id_t id, char *buf, size_t len);

/** Label in the current language. */
const char *settings_label(setting_id_t id);

/* ---- Cross-field rules (data and checker in settings_table.c, host-tested) ----
 * A chain lists operands, each a setting or a fixed value, with op[i] + gap <= op[i+1];
 * a floor keeps a value at 0 or at least the floor (to the nearer of the two at load).
 * A set that would break a chain is clamped to the nearest legal value; the other setting
 * is never moved. The setter leaves a note (settings_last_note). settings_init applies the
 * chains to stored values: the lower setting moves down, or the upper one up when the lower
 * one is fixed or would leave its range. */
typedef struct {
    setting_id_t id;                /* SET_COUNT: the fixed value k */
    float k;
    const char *name_en, *name_da;  /* fixed values only */
} setting_operand_t;

typedef struct {
    const setting_operand_t *ops;
    uint8_t n;
    float gap;
} setting_chain_t;

/* A value that is either 0 (= built-in default) or at least the floor setting's value. A set
 * of the floor setting is clamped to stay at or below a non-zero value; at load the value moves. */
typedef struct {
    setting_id_t id;
    setting_id_t floor_id;
} setting_floor_t;

typedef struct {                    /* chain and floor both NULL: no rule changed the value */
    const setting_chain_t *chain;
    const setting_operand_t *other; /* chain: the operand that bounds the value */
    bool other_above;               /* chain: true = the value had to stay below other;
                                     * floor: true = the floor setting had to stay at or below floor->id */
    const setting_floor_t *floor;
} setting_rule_hit_t;

extern const setting_chain_t g_settings_chains[];
extern const size_t g_settings_chain_count;
extern const setting_floor_t g_settings_floors[];
extern const size_t g_settings_floor_count;

/* Manual fan levels Low..High in %: the one source for the controller and the fan chain. */
#define SETTINGS_FAN_LEVELS 5
extern const setting_operand_t g_fan_levels[SETTINGS_FAN_LEVELS];

/** Pure: v (already range-clamped) for id, clamped so every chain holds against vals[]. */
float settings_rules_clamp(setting_id_t id, float v, const float vals[SET_COUNT], setting_rule_hit_t *hit);

/** Pure: make stored values obey every chain (see above). Returns the number of values changed. */
int settings_rules_fix(float vals[SET_COUNT]);

/** Operand value: the setting's current value or the fixed value. */
float settings_operand_value(const setting_operand_t *op);

#define SETTINGS_NOTE_MAX 128
/** Note left by the last set/step/reset of id when a rule changed the value, e.g.
 * "Set to 63 %RH: must be at least 2 %RH below Humidity A/C on (65 %RH)". Returns false
 * (buf = "") when the last change of any setting was not this id or was not clamped by a rule. */
bool settings_last_note(setting_id_t id, char *buf, size_t len);

/** Pure: the first chain pair or floor that vals[] breaks. *id = the setting to name in a message,
 * hit as for settings_rules_clamp (hit->other = the operand it conflicts with). False if none. */
bool settings_rules_broken(const float vals[SET_COUNT], setting_rule_hit_t *hit, setting_id_t *id);

/* ---- Export / import (phone page) ---- */
#define SETTINGS_EXPORT_FORMAT "hvac-settings"

/** {"format":"hvac-settings","schema":N,"firmware":"...","settings":{"<nvs_key>":value,...}} with every
 * non-ACTION row; the AP password only when include_password. Returns the length, or -1 if len is too small. */
int settings_export_json(char *buf, size_t len, const char *firmware, bool include_password);

/** Check a whole export file, then apply all of it or nothing. Refuses (nothing changed, reason in why):
 * not the format, another schema, unknown or repeated keys, a value of the wrong type, out of range or
 * off the step grid, a SERVICE change while !service_ok, a cross-field rule broken by the resulting set.
 * Keys missing from the file keep their value. ESP_OK: *changed = settings changed.
 * Errors: ESP_ERR_INVALID_VERSION (schema), ESP_ERR_INVALID_STATE (service / changed meanwhile),
 * ESP_ERR_NO_MEM, ESP_ERR_INVALID_ARG (everything else). */
esp_err_t settings_import_json(const char *json, bool service_ok, int *changed, char *why, size_t why_len);

/** Increments on every change; lets readers redraw only on change. */
uint32_t settings_seq(void);

/**
 * Open a versioned namespace (hvac_cfg, hvac_cal, hvac_thermal): reads u16 "schema",
 * calls migrate(h, from, to) when it is older, then writes the current schema.
 * migrate may be NULL (an older schema is then erased). Caller closes the handle.
 */
typedef esp_err_t (*settings_migrate_fn)(nvs_handle_t h, uint16_t from, uint16_t to);
esp_err_t settings_ns_open(const char *ns, uint16_t schema, settings_migrate_fn migrate, nvs_handle_t *out);

#ifdef __cplusplus
}
#endif

#endif /* SETTINGS_H */
