/**
 * @file web_req.h
 * @brief Pure parsers for the phone page's small JSON POST bodies (host-tested in test/host).
 *
 * Flat objects only ({"name":value,...}); no nesting is needed by any request. Each parser
 * returns NULL when the body is valid, else a short reason for a 400 reply. Value ranges are
 * the firmware's enums, repeated here so this file needs no IDF header; web_server.c asserts
 * they match.
 */
#ifndef WEB_REQ_H
#define WEB_REQ_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WEB_MODES          7    /* HVAC_MODE_MAX: OFF .. AUTO */
#define WEB_FANS           7    /* HVAC_FAN_MAX: OFF .. HIGH, AUTO */
#define WEB_PRESETS        5    /* HVAC_PRESET_COUNT (0 = NONE is not applicable) */
#define WEB_VALVES         5    /* HVAC_VALVE_MAX */
#define WEB_ROLES          5    /* SENSOR_ROLE_COUNT */
#define WEB_SETPOINT_MIN   16   /* SETTINGS_SETPOINT_MIN_C */
#define WEB_SETPOINT_MAX   30   /* SETTINGS_SETPOINT_MAX_C */

/** Pointer to the value after "name": (whitespace skipped), or NULL. */
const char *json_field(const char *body, const char *name);
/** Parse a JSON string at p (simple escapes only) into out; false if not a string or too long. */
bool json_str(const char *p, char *out, size_t n);

/* POST /api/control: exactly one of these fields */
typedef enum {
    WCTL_MODE,          /* "mode": 0..6 */
    WCTL_SETPOINT,      /* "setpoint": 16..30 degC */
    WCTL_SETPOINT_STEP, /* "setpoint_step": -1 | 1 */
    WCTL_FAN,           /* "fan": 0..6 (6 = AUTO) */
    WCTL_FAN_STEP,      /* "fan_step": -1 | 1 */
    WCTL_AC,            /* "ac": true | false */
    WCTL_RECIRC,        /* "recirc": true | false */
    WCTL_PRESET,        /* "preset": 1..4 */
    WCTL_QUICK_AUTO,    /* "quick": "auto" */
    WCTL_QUICK_DEF,     /* "quick": "def" */
} web_ctl_t;

typedef struct {
    web_ctl_t what;
    float value;        /* number, step, or 1/0 for the booleans */
} web_ctl_req_t;

const char *web_parse_control(const char *body, web_ctl_req_t *out);

/* POST /api/service/valve {"valve":0..4,"action":"..."} */
typedef enum { WVALVE_JOG_PLUS, WVALVE_JOG_MINUS, WVALVE_STOP, WVALVE_RECAL, WVALVE_LEARN } web_valve_act_t;
const char *web_parse_valve(const char *body, int *valve, web_valve_act_t *act);

/* POST /api/service/sensors {"action":"search"} | {"action":"assign","rom":"<16 hex>","role":0..4} */
const char *web_parse_sensor(const char *body, bool *search, uint8_t rom[8], int *role);

/* POST /api/service/compressor {"stop":true} | {"demand":0..100,"blower":bool} */
const char *web_parse_compressor(const char *body, bool *stop, int *demand, bool *blower);

/* POST /api/action {"action":"log_clear"|"thermal_reset"|"thermal_save"} | {"action":"setting","id":N} */
typedef enum { WACT_LOG_CLEAR, WACT_THERMAL_RESET, WACT_THERMAL_SAVE, WACT_SETTING } web_act_t;
const char *web_parse_action(const char *body, web_act_t *act, int *id);

#endif /* WEB_REQ_H */
