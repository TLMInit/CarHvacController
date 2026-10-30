/**
 * @file web_req.c
 * @brief Pure parsers for the phone page's POST bodies (see web_req.h).
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "web_req.h"

static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}

const char *json_field(const char *body, const char *name)
{
    char pat[24];
    snprintf(pat, sizeof(pat), "\"%s\"", name);
    const char *p = strstr(body, pat);
    if (!p) {
        return NULL;
    }
    p = skip_ws(p + strlen(pat));
    if (*p++ != ':') {
        return NULL;
    }
    return skip_ws(p);
}

bool json_str(const char *p, char *out, size_t n)
{
    if (!p || *p++ != '"') {
        return false;
    }
    size_t o = 0;
    for (; *p && *p != '"'; p++) {
        if (*p == '\\') {
            p++;
            if (*p != '"' && *p != '\\' && *p != '/') {
                return false;
            }
        }
        if (o + 1 >= n) {
            return false;
        }
        out[o++] = *p;
    }
    out[o] = '\0';
    return *p == '"';
}

/* A number that ends where a JSON value ends. */
static bool json_num(const char *p, float *v)
{
    if (!p) {
        return false;
    }
    char *end;
    *v = strtof(p, &end);
    end = (char *)skip_ws(end);
    return end != p && isfinite(*v) && (*end == ',' || *end == '}' || *end == '\0');
}

static bool json_int(const char *p, int lo, int hi, int *v)
{
    float f;
    if (!json_num(p, &f) || f != floorf(f) || f < lo || f > hi) {
        return false;
    }
    *v = (int)f;
    return true;
}

static bool json_bool(const char *p, bool *v)
{
    if (p && strncmp(p, "true", 4) == 0) {
        *v = true;
    } else if (p && strncmp(p, "false", 5) == 0) {
        *v = false;
    } else {
        return false;
    }
    return true;
}

const char *web_parse_control(const char *body, web_ctl_req_t *out)
{
    static const char *const k_names[] = { "mode", "setpoint", "setpoint_step", "fan", "fan_step",
                                           "ac", "recirc", "preset", "quick" };
    const char *p = NULL;
    int which = -1, n = 0;
    for (int i = 0; i < (int)(sizeof(k_names) / sizeof(k_names[0])); i++) {
        const char *f = json_field(body, k_names[i]);
        if (f) {
            p = f;
            which = i;
            n++;
        }
    }
    if (n != 1) {
        return n ? "Send one control at a time" : "No known control in the request";
    }
    int iv;
    bool b;
    char s[8];
    switch (which) {
    case 0:
        if (!json_int(p, 0, WEB_MODES - 1, &iv)) return "mode must be 0..6";
        *out = (web_ctl_req_t){ WCTL_MODE, (float)iv };
        return NULL;
    case 1:
        if (!json_num(p, &out->value) || out->value < WEB_SETPOINT_MIN || out->value > WEB_SETPOINT_MAX) {
            return "setpoint must be 16..30";
        }
        out->what = WCTL_SETPOINT;
        return NULL;
    case 2:
    case 4:
        if (!json_int(p, -1, 1, &iv) || iv == 0) return "step must be -1 or 1";
        *out = (web_ctl_req_t){ which == 2 ? WCTL_SETPOINT_STEP : WCTL_FAN_STEP, (float)iv };
        return NULL;
    case 3:
        if (!json_int(p, 0, WEB_FANS - 1, &iv)) return "fan must be 0..6";
        *out = (web_ctl_req_t){ WCTL_FAN, (float)iv };
        return NULL;
    case 5:
    case 6:
        if (!json_bool(p, &b)) return "ac/recirc must be true or false";
        *out = (web_ctl_req_t){ which == 5 ? WCTL_AC : WCTL_RECIRC, b ? 1.0f : 0.0f };
        return NULL;
    case 7:
        if (!json_int(p, 1, WEB_PRESETS - 1, &iv)) return "preset must be 1..4";
        *out = (web_ctl_req_t){ WCTL_PRESET, (float)iv };
        return NULL;
    default:
        if (json_str(p, s, sizeof(s)) && strcmp(s, "auto") == 0) {
            *out = (web_ctl_req_t){ WCTL_QUICK_AUTO, 0 };
        } else if (json_str(p, s, sizeof(s)) && strcmp(s, "def") == 0) {
            *out = (web_ctl_req_t){ WCTL_QUICK_DEF, 0 };
        } else {
            return "quick must be \"auto\" or \"def\"";
        }
        return NULL;
    }
}

const char *web_parse_valve(const char *body, int *valve, web_valve_act_t *act)
{
    static const char *const k_act[] = { "jog_plus", "jog_minus", "stop", "recalibrate", "learn" };
    char s[16];
    if (!json_int(json_field(body, "valve"), 0, WEB_VALVES - 1, valve)) {
        return "valve must be 0..4";
    }
    if (json_str(json_field(body, "action"), s, sizeof(s))) {
        for (int i = 0; i < (int)(sizeof(k_act) / sizeof(k_act[0])); i++) {
            if (strcmp(s, k_act[i]) == 0) {
                *act = (web_valve_act_t)i;
                return NULL;
            }
        }
    }
    return "action must be jog_plus, jog_minus, stop, recalibrate or learn";
}

static int hex(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c |= 0x20;
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

const char *web_parse_sensor(const char *body, bool *search, uint8_t rom[8], int *role)
{
    char s[20];
    if (!json_str(json_field(body, "action"), s, sizeof(s))) {
        return "Missing \"action\"";
    }
    *search = strcmp(s, "search") == 0;
    if (*search) {
        return NULL;
    }
    if (strcmp(s, "assign") != 0) {
        return "action must be search or assign";
    }
    if (!json_str(json_field(body, "rom"), s, sizeof(s)) || strlen(s) != 16) {
        return "rom must be 16 hex digits";
    }
    for (int i = 0; i < 8; i++) {
        int hi = hex(s[2 * i]), lo = hex(s[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return "rom must be 16 hex digits";
        }
        rom[i] = (uint8_t)(hi << 4 | lo);
    }
    if (!json_int(json_field(body, "role"), 0, WEB_ROLES - 1, role)) {
        return "role must be 0..4";
    }
    return NULL;
}

const char *web_parse_compressor(const char *body, bool *stop, int *demand, bool *blower)
{
    *stop = false;
    const char *p = json_field(body, "stop");
    if (p) {
        return json_bool(p, stop) && *stop ? NULL : "stop must be true";
    }
    if (!json_int(json_field(body, "demand"), 0, 100, demand)) {
        return "demand must be 0..100";
    }
    if (!json_bool(json_field(body, "blower"), blower)) {
        return "blower must be true or false";
    }
    return NULL;
}

const char *web_parse_action(const char *body, web_act_t *act, int *id)
{
    char s[16];
    if (!json_str(json_field(body, "action"), s, sizeof(s))) {
        return "Missing \"action\"";
    }
    if (strcmp(s, "log_clear") == 0) {
        *act = WACT_LOG_CLEAR;
    } else if (strcmp(s, "thermal_reset") == 0) {
        *act = WACT_THERMAL_RESET;
    } else if (strcmp(s, "thermal_save") == 0) {
        *act = WACT_THERMAL_SAVE;
    } else if (strcmp(s, "setting") == 0) {
        *act = WACT_SETTING;
        if (!json_int(json_field(body, "id"), 0, 9999, id)) {
            return "id must be a setting id";
        }
    } else {
        return "Unknown action";
    }
    return NULL;
}
