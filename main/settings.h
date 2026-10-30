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
    SET_HUMIDITY_OFF,           /* float %RH, 40..85, default 58 (kept < ON by the setter) */
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
    SET_DUCT_MIN,               /* float degC, 0..30, default 10 */
    SET_DUCT_MAX,               /* float degC, 30..80, default 60 */

    /* Valves (index = hvac_valve_id_t) */
    SET_VALVE_SPEED_0,          /* int PWM, 0 = built-in default, else 30..255 (coolant 100..255); one per valve; caps every move */
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

    SET_COUNT
} setting_id_t;

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

/* A value that is either 0 (= built-in default) or at least floor. */
typedef struct {
    setting_id_t id;
    float floor;
} setting_floor_t;

typedef struct {                    /* chain and floor both NULL: no rule changed the value */
    const setting_chain_t *chain;
    const setting_operand_t *other; /* chain: the operand that bounds the value */
    bool other_above;               /* chain: true = the value had to stay below other */
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
