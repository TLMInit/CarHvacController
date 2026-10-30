/**
 * @file settings.c
 * @brief Settings framework: RAM cache, clamping, debounced NVS persistence.
 *
 * Tasks:
 * - Getters: any task (control loops every 100 ms, UI, web). Numeric reads are
 *   lock-free (aligned 32-bit float); string reads copy under a spinlock.
 * - Setters: any task. They update the cache under a spinlock, wake the persist
 *   task and run apply_cb in the caller's context. They never touch flash.
 * - "settings_nvs" task (priority 1): writes dirty keys 2 s after their first
 *   unwritten change, so a key is written at most once per 2 s.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_random.h"
#include "bootloader_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"
#include "settings.h"

static const char *TAG = "SETTINGS";

extern const setting_def_t g_settings_table[SET_COUNT];
void settings_factory_reset_cb(setting_id_t id);

#define PERSIST_DELAY_US    (2 * 1000 * 1000)
#define STR_SLOTS           1   /* string rows in the table; checked at init */

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static float s_val[SET_COUNT];
static char s_str[STR_SLOTS][SETTINGS_STR_MAX];
static int8_t s_str_slot[SET_COUNT];
static bool s_dirty[SET_COUNT];
static int64_t s_dirty_since[SET_COUNT];
static volatile uint32_t s_seq;
static volatile bool s_halt;            /* factory reset in progress: no more writes */
static nvs_handle_t s_nvs;
static TaskHandle_t s_persist_task;
static char s_ap_default[11];   /* AP password used while the user has set none; written once by settings_init */
static setting_id_t s_note_id = SET_COUNT;     /* setting the note belongs to */
static char s_note[SETTINGS_NOTE_MAX];          /* written by the setter, read by settings_last_note (both under s_lock) */

const setting_def_t *settings_def(setting_id_t id)
{
    return &g_settings_table[id < SET_COUNT ? id : 0];
}

void settings_iterate(bool (*cb)(const setting_def_t *def, void *ctx), void *ctx)
{
    for (int i = 0; i < SET_COUNT; i++) {
        if (!cb(&g_settings_table[i], ctx)) {
            return;
        }
    }
}

uint32_t settings_seq(void)
{
    return s_seq;
}

static bool lang_da(void)
{
    return lroundf(s_val[SET_LANGUAGE]) == SET_LANG_DA;
}

const char *settings_label(setting_id_t id)
{
    const setting_def_t *d = settings_def(id);
    return lang_da() ? d->label_da : d->label_en;
}

/* ---- getters ---- */

float settings_operand_value(const setting_operand_t *op)
{
    return op->id < SET_COUNT ? s_val[op->id] : op->k;
}

bool settings_last_note(setting_id_t id, char *buf, size_t len)
{
    if (!buf || len == 0) {
        return false;
    }
    taskENTER_CRITICAL(&s_lock);
    bool have = s_note_id == id && s_note[0];
    snprintf(buf, len, "%s", have ? s_note : "");
    taskEXIT_CRITICAL(&s_lock);
    return have;
}

int32_t settings_get_int(setting_id_t id)
{
    return id < SET_COUNT ? (int32_t)lroundf(s_val[id]) : 0;
}

float settings_get_float(setting_id_t id)
{
    return id < SET_COUNT ? s_val[id] : 0.0f;
}

bool settings_get_bool(setting_id_t id)
{
    return settings_get_int(id) != 0;
}

void settings_get_str(setting_id_t id, char *buf, size_t len)
{
    if (!buf || len == 0) {
        return;
    }
    buf[0] = '\0';
    if (id >= SET_COUNT || s_str_slot[id] < 0) {
        return;
    }
    char tmp[SETTINGS_STR_MAX];
    taskENTER_CRITICAL(&s_lock);
    memcpy(tmp, s_str[s_str_slot[id]], sizeof(tmp));
    taskEXIT_CRITICAL(&s_lock);
    if (tmp[0] == '\0' && id == SET_AP_PASSWORD) {
        snprintf(tmp, sizeof(tmp), "%s", s_ap_default);
    }
    snprintf(buf, len, "%s", tmp);
}

/* ---- setters ---- */

static float sanitize(const setting_def_t *d, float v)
{
    if (!isfinite(v)) {
        v = d->def;
    }
    if (d->step > 0.0f) {
        v = roundf(v / d->step) * d->step;     /* snap to the grid through 0, so 1..100 step 5 still reaches 5, 10, ... */
    }
    if (v < d->min) {
        v = d->min;
    }
    if (v > d->max) {
        v = d->max;
    }
    return v;
}

static void mark_dirty_locked(setting_id_t id)
{
    if (!s_dirty[id]) {
        s_dirty[id] = true;
        s_dirty_since[id] = esp_timer_get_time();
    }
    s_seq++;
}

/* Store a sanitized value; returns true if it changed. Caller holds s_lock. */
static bool store_locked(setting_id_t id, float v)
{
    if (s_val[id] == v) {
        return false;
    }
    s_val[id] = v;
    mark_dirty_locked(id);
    return true;
}

static void after_change(setting_id_t id)
{
    if (s_persist_task) {
        xTaskNotifyGive(s_persist_task);
    }
    const setting_def_t *d = settings_def(id);
    if (d->apply_cb) {
        d->apply_cb(id);
    }
}

static bool fahrenheit(const setting_def_t *d)
{
    return d->unit && strcmp(d->unit, "°C") == 0 && lroundf(s_val[SET_TEMP_UNIT]) == SET_UNIT_F;
}

/* Number with the row's unit, in the display unit; delta = a difference (no +32 offset). */
static void fmt_amount(const setting_def_t *d, float v, bool delta, char *buf, size_t len)
{
    const char *unit = d->unit;
    if (fahrenheit(d)) {
        unit = "°F";
        v = delta ? v * 1.8f : v * 1.8f + 32.0f;
    }
    snprintf(buf, len, "%g%s%s", (double)(roundf(v * 10.0f) / 10.0f), unit ? " " : "", unit ? unit : "");
}

/* "Set to 63 %RH: must be at least 2 %RH below Humidity A/C on (65 %RH)" in the current language. */
static void make_note(setting_id_t id, const setting_rule_hit_t *hit, char *buf, size_t len)
{
    const setting_def_t *d = settings_def(id);
    const setting_operand_t *o = hit->other;
    bool da = lang_da();
    char val[24], other_val[24], gap[24], rel[64];
    settings_format(id, val, sizeof(val));
    if (hit->floor) {
        snprintf(buf, len, da ? "Sat til %s: 0 = standard, ellers mindst %g (ventilen kan sidde fast)"
                              : "Set to %s: 0 = default, else at least %g (the valve may not move)",
                 val, (double)hit->floor->floor);
        return;
    }
    fmt_amount(d, settings_operand_value(o), false, other_val, sizeof(other_val));
    const char *other = o->id < SET_COUNT ? settings_label(o->id) : (da ? o->name_da : o->name_en);
    if (hit->chain->gap > 0.0f) {
        fmt_amount(d, hit->chain->gap, true, gap, sizeof(gap));
        snprintf(rel, sizeof(rel), da ? "skal være mindst %s %s" : "must be at least %s %s", gap,
                 hit->other_above ? (da ? "under" : "below") : (da ? "over" : "above"));
    } else {
        snprintf(rel, sizeof(rel), "%s", hit->other_above ? (da ? "må ikke være over" : "must not be above")
                                                          : (da ? "må ikke være under" : "must not be below"));
    }
    snprintf(buf, len, "%s %s: %s %s (%s)", da ? "Sat til" : "Set to", val, rel, other, other_val);
}

static esp_err_t set_value(setting_id_t id, float value)
{
    if (id >= SET_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    const setting_def_t *d = settings_def(id);
    if (d->type == SET_TYPE_ACTION) {
        if (value != 0.0f && d->apply_cb) {
            d->apply_cb(id);
        }
        return ESP_OK;
    }
    if (d->type == SET_TYPE_STRING) {
        return ESP_ERR_INVALID_ARG;
    }

    float v = sanitize(d, value);
    setting_rule_hit_t hit;
    taskENTER_CRITICAL(&s_lock);
    v = settings_rules_clamp(id, v, s_val, &hit);
    bool changed = store_locked(id, v);
    taskEXIT_CRITICAL(&s_lock);

    char note[SETTINGS_NOTE_MAX] = "";
    if (hit.chain || hit.floor) {
        make_note(id, &hit, note, sizeof(note));
        ESP_LOGI(TAG, "%s: %s", d->nvs_key, note);
    }
    taskENTER_CRITICAL(&s_lock);
    s_note_id = id;
    memcpy(s_note, note, sizeof(s_note));
    taskEXIT_CRITICAL(&s_lock);

    if (changed) {
        after_change(id);
    }
    return ESP_OK;
}

esp_err_t settings_set_int(setting_id_t id, int32_t value)
{
    return set_value(id, (float)value);
}

esp_err_t settings_set_float(setting_id_t id, float value)
{
    return set_value(id, value);
}

esp_err_t settings_set_str(setting_id_t id, const char *value)
{
    if (id >= SET_COUNT || s_str_slot[id] < 0 || !value) {
        return ESP_ERR_INVALID_ARG;
    }
    const setting_def_t *d = settings_def(id);
    size_t n = strlen(value);
    if (n != 0 && (n < (size_t)d->min || n > (size_t)d->max)) {
        return ESP_ERR_INVALID_SIZE;
    }
    for (size_t i = 0; i < n; i++) {
        if (value[i] < 0x20 || value[i] > 0x7e) {
            return ESP_ERR_INVALID_ARG;     /* WPA2 passphrases are printable ASCII */
        }
    }
    bool changed = false;
    taskENTER_CRITICAL(&s_lock);
    char *dst = s_str[s_str_slot[id]];
    if (strcmp(dst, value) != 0) {
        memcpy(dst, value, n + 1);
        mark_dirty_locked(id);
        changed = true;
    }
    taskEXIT_CRITICAL(&s_lock);
    if (changed) {
        after_change(id);
    }
    return ESP_OK;
}

esp_err_t settings_step(setting_id_t id, int direction)
{
    if (id >= SET_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    const setting_def_t *d = settings_def(id);
    if (d->type == SET_TYPE_STRING || d->type == SET_TYPE_ACTION) {
        return ESP_ERR_INVALID_ARG;
    }
    return set_value(id, s_val[id] + (direction > 0 ? d->step : -d->step));
}

esp_err_t settings_reset(setting_id_t id)
{
    if (id >= SET_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    const setting_def_t *d = settings_def(id);
    switch (d->type) {
    case SET_TYPE_ACTION:
        return ESP_OK;
    case SET_TYPE_STRING:
        return settings_set_str(id, d->def_str ? d->def_str : "");
    default:
        return set_value(id, d->def);
    }
}

esp_err_t settings_reset_all(void)
{
    /* Twice: a rule can clamp a default against a partner not yet reset (e.g. ON back to 65
     * while OFF is still 80); the second pass then lands on the defaults, which obey every rule. */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < SET_COUNT; i++) {
            settings_reset((setting_id_t)i);
        }
    }
    return ESP_OK;
}

/* ---- formatting ---- */

static bool is_temp_delta(setting_id_t id)
{
    return id == SET_SETPOINT_STEP || id == SET_DEFOG_MARGIN || id == SET_DEFOG_GLASS_OFFSET;
}

void settings_format(setting_id_t id, char *buf, size_t len)
{
    if (!buf || len == 0) {
        return;
    }
    buf[0] = '\0';
    if (id >= SET_COUNT) {
        return;
    }
    const setting_def_t *d = settings_def(id);
    bool da = lang_da();
    float v = s_val[id];
    int iv = (int)lroundf(v);

    switch (d->type) {
    case SET_TYPE_ACTION:
        return;
    case SET_TYPE_STRING:
        settings_get_str(id, buf, len);
        return;
    case SET_TYPE_BOOL:
        snprintf(buf, len, "%s", iv ? (da ? "Til" : "On") : (da ? "Fra" : "Off"));
        return;
    case SET_TYPE_ENUM: {
        const char *const *labels = da ? d->enum_da : d->enum_en;
        snprintf(buf, len, "%s", (labels && iv >= 0 && iv <= (int)d->max) ? labels[iv] : "?");
        return;
    }
    default:
        break;
    }

    /* 0 means "not used" for these rows */
    if (iv == 0 && (id == SET_AUTO_DIM_S || id == SET_RECIRC_AUTO_CANCEL_MIN || id == SET_QUIET_NIGHT_CAP)) {
        snprintf(buf, len, "%s", da ? "Fra" : "Off");
        return;
    }
    if (iv == 0 && id >= SET_VALVE_SPEED_0 && id <= SET_VALVE_SPEED_4) {
        snprintf(buf, len, "%s", "Standard");
        return;
    }

    const char *unit = d->unit;
    float step = d->step;
    if (unit && strcmp(unit, "°C") == 0 && settings_get_int(SET_TEMP_UNIT) == SET_UNIT_F) {
        unit = "°F";
        step *= 1.8f;
        v = is_temp_delta(id) ? v * 1.8f : v * 1.8f + 32.0f;
    }
    int decimals = (d->type == SET_TYPE_INT || step >= 0.999f) ? 0 : (step >= 0.0999f ? 1 : 2);
    snprintf(buf, len, "%.*f%s%s", decimals, (double)v, unit ? " " : "", unit ? unit : "");
}

/* ---- persistence ---- */

static void write_key(setting_id_t id)
{
    const setting_def_t *d = settings_def(id);
    char str[SETTINGS_STR_MAX] = "";
    float v;

    taskENTER_CRITICAL(&s_lock);
    s_dirty[id] = false;
    v = s_val[id];
    if (s_str_slot[id] >= 0) {
        memcpy(str, s_str[s_str_slot[id]], sizeof(str));
    }
    taskEXIT_CRITICAL(&s_lock);

    esp_err_t err;
    bool is_default = (d->type == SET_TYPE_STRING) ? strcmp(str, d->def_str ? d->def_str : "") == 0 : v == d->def;
    if (is_default) {
        /* A missing key reads as the default, so a reset also follows future default changes. */
        err = nvs_erase_key(s_nvs, d->nvs_key);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
    } else if (d->type == SET_TYPE_STRING) {
        err = nvs_set_str(s_nvs, d->nvs_key, str);
    } else if (d->type == SET_TYPE_FLOAT) {
        uint32_t bits;
        memcpy(&bits, &v, sizeof(bits));
        err = nvs_set_u32(s_nvs, d->nvs_key, bits);
    } else {
        err = nvs_set_i32(s_nvs, d->nvs_key, (int32_t)lroundf(v));
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "write %s failed: %s (retry in 2 s)", d->nvs_key, esp_err_to_name(err));
        taskENTER_CRITICAL(&s_lock);
        if (!s_dirty[id]) {
            s_dirty[id] = true;
            s_dirty_since[id] = esp_timer_get_time();
        }
        taskEXIT_CRITICAL(&s_lock);
    }
}

static void persist_task(void *arg)
{
    (void)arg;
    for (;;) {
        TickType_t wait = portMAX_DELAY;
        bool wrote = false;
        int64_t now = esp_timer_get_time();
        for (int i = 0; i < SET_COUNT && !s_halt; i++) {
            taskENTER_CRITICAL(&s_lock);
            bool dirty = s_dirty[i];
            int64_t due = s_dirty_since[i] + PERSIST_DELAY_US;
            taskEXIT_CRITICAL(&s_lock);
            if (!dirty) {
                continue;
            }
            if (now >= due) {
                write_key((setting_id_t)i);
                wrote = true;
            } else {
                TickType_t t = pdMS_TO_TICKS((due - now) / 1000) + 1;
                if (t < wait) {
                    wait = t;
                }
            }
        }
        if (wrote) {
            esp_err_t err = nvs_commit(s_nvs);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "commit failed: %s", esp_err_to_name(err));
            }
            wait = 0;   /* rescan: a failed write re-marked a key dirty, or a key changed meanwhile */
        }
        if (wait > 0) {
            ulTaskNotifyTake(pdTRUE, wait);
        }
    }
}

void settings_factory_reset_cb(setting_id_t id)
{
    (void)id;
    static const char *const k_ns[] = { SETTINGS_NAMESPACE, "hvac_cal", "hvac_thermal", "hvac" };
    s_halt = true;
    ESP_LOGW(TAG, "factory reset: erasing settings, calibration and state, then rebooting");
    for (size_t i = 0; i < sizeof(k_ns) / sizeof(k_ns[0]); i++) {
        nvs_handle_t h;
        if (nvs_open(k_ns[i], NVS_READWRITE, &h) == ESP_OK) {
            nvs_erase_all(h);
            nvs_commit(h);
            nvs_close(h);
        }
    }
    esp_restart();
}

esp_err_t settings_ns_open(const char *ns, uint16_t schema, settings_migrate_fn migrate, nvs_handle_t *out)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "open %s: %s", ns, esp_err_to_name(err));
        return err;
    }
    uint16_t from = 0;     /* no "schema" key = data from before the scheme (or an empty namespace) */
    if (nvs_get_u16(h, "schema", &from) != ESP_OK) {
        from = 0;
    }
    if (from < schema) {
        err = migrate ? migrate(h, from, schema) : nvs_erase_all(h);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "%s: migrate %u->%u failed (%s), erasing", ns, from, schema, esp_err_to_name(err));
            nvs_erase_all(h);
        } else if (from > 0 || migrate) {
            ESP_LOGI(TAG, "%s: schema %u -> %u", ns, from, schema);
        }
        err = nvs_set_u16(h, "schema", schema);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        if (err != ESP_OK) {
            nvs_close(h);
            return err;
        }
    } else if (from > schema) {
        ESP_LOGW(TAG, "%s: schema %u is newer than %u, reading what is known", ns, from, schema);
    }
    *out = h;
    return ESP_OK;
}

/* Table sanity: ids in order, keys short and unique, defaults in range, enum labels present. */
static bool table_ok(void)
{
    bool ok = true;
    int slots = 0;
    for (int i = 0; i < SET_COUNT; i++) {
        const setting_def_t *d = &g_settings_table[i];
        s_str_slot[i] = -1;
        if (d->id != (setting_id_t)i || !d->label_en || !d->label_da) {
            ESP_LOGE(TAG, "table row %d missing or out of order", i);
            ok = false;
            continue;
        }
        if (d->type == SET_TYPE_STRING) {
            if (slots >= STR_SLOTS) {
                ESP_LOGE(TAG, "row %d: raise STR_SLOTS", i);
                ok = false;
                continue;
            }
            s_str_slot[i] = (int8_t)slots++;
        }
        if (d->type == SET_TYPE_ACTION) {
            continue;
        }
        if (!d->nvs_key || strlen(d->nvs_key) > 15 || strcmp(d->nvs_key, "schema") == 0) {
            ESP_LOGE(TAG, "row %d: bad nvs key", i);
            ok = false;
        }
        for (int j = 0; j < i && d->nvs_key; j++) {
            if (g_settings_table[j].nvs_key && strcmp(g_settings_table[j].nvs_key, d->nvs_key) == 0) {
                ESP_LOGE(TAG, "rows %d and %d share key %s", j, i, d->nvs_key);
                ok = false;
            }
        }
        if (d->type != SET_TYPE_STRING && (d->def < d->min || d->def > d->max)) {
            ESP_LOGE(TAG, "row %d: default out of range", i);
            ok = false;
        }
        if (d->type == SET_TYPE_ENUM && (!d->enum_en || !d->enum_da)) {
            ESP_LOGE(TAG, "row %d: enum labels missing", i);
            ok = false;
        }
    }
    return ok;
}

/* Random per-device default AP password, generated on first boot (and again after a
 * factory reset) and kept in hvac_cfg. Not derived from the MAC: the BSSID broadcasts it. */
static void load_ap_default(void)
{
    size_t n = sizeof(s_ap_default);
    if (nvs_get_str(s_nvs, "ap_pass_gen", s_ap_default, &n) == ESP_OK && strlen(s_ap_default) == 10) {
        return;
    }
    static const char alphabet[] = "abcdefghjkmnpqrstuvwxyz23456789";   /* no 0/o, 1/l/i */
    uint8_t rnd[10];
    /* Wi-Fi is not up yet, so the RNG needs the SAR-ADC entropy source to be truly random */
    bootloader_random_enable();
    esp_fill_random(rnd, sizeof(rnd));
    bootloader_random_disable();
    for (size_t i = 0; i < sizeof(rnd); i++) {
        s_ap_default[i] = alphabet[rnd[i] % (sizeof(alphabet) - 1)];
    }
    s_ap_default[10] = '\0';
    if (nvs_set_str(s_nvs, "ap_pass_gen", s_ap_default) != ESP_OK || nvs_commit(s_nvs) != ESP_OK) {
        ESP_LOGW(TAG, "AP default password not saved: a new one is made next boot");
    }
}

esp_err_t settings_init(void)
{
    /* A broken table is a build mistake: stop loudly on the first bench boot. */
    ESP_ERROR_CHECK(table_ok() ? ESP_OK : ESP_ERR_INVALID_STATE);

    for (int i = 0; i < SET_COUNT; i++) {
        const setting_def_t *d = &g_settings_table[i];
        if (s_str_slot[i] >= 0) {
            snprintf(s_str[s_str_slot[i]], SETTINGS_STR_MAX, "%s", d->def_str ? d->def_str : "");
        } else {
            s_val[i] = d->def;
        }
    }

    esp_err_t err = settings_ns_open(SETTINGS_NAMESPACE, SETTINGS_SCHEMA, NULL, &s_nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s unavailable (%s): running on defaults, changes are not saved", SETTINGS_NAMESPACE, esp_err_to_name(err));
        return err;
    }

    int loaded = 0, rejected = 0;
    for (int i = 0; i < SET_COUNT; i++) {
        const setting_def_t *d = &g_settings_table[i];
        if (d->type == SET_TYPE_ACTION) {
            continue;
        }
        esp_err_t e;
        float v = d->def;
        if (d->type == SET_TYPE_STRING) {
            char str[SETTINGS_STR_MAX];
            size_t n = sizeof(str);
            e = nvs_get_str(s_nvs, d->nvs_key, str, &n);
            if (e == ESP_OK) {
                size_t sl = strlen(str);
                if (sl >= (size_t)d->min && sl <= (size_t)d->max) {
                    memcpy(s_str[s_str_slot[i]], str, sl + 1);
                    loaded++;
                } else {
                    rejected++;
                }
            } else if (e != ESP_ERR_NVS_NOT_FOUND) {
                rejected++;
            }
            continue;
        }
        if (d->type == SET_TYPE_FLOAT) {
            uint32_t bits;
            e = nvs_get_u32(s_nvs, d->nvs_key, &bits);
            if (e == ESP_OK) {
                memcpy(&v, &bits, sizeof(v));
            }
        } else {
            int32_t iv;
            e = nvs_get_i32(s_nvs, d->nvs_key, &iv);
            if (e == ESP_OK) {
                v = (float)iv;
            }
        }
        if (e == ESP_OK && i == SET_FAN_MIN_DUTY && v == 0.0f) {
            v = d->def;     /* 0 is no longer allowed; older firmware ran a stored 0 as the default 30 */
            ESP_LOGW(TAG, "%s: stored 0 read as %g", d->nvs_key, (double)v);
            mark_dirty_locked((setting_id_t)i);
        }
        if (e == ESP_OK) {
            s_val[i] = sanitize(d, v);     /* a range or step changed by a newer firmware is clamped, not dropped */
            loaded++;
        } else if (e != ESP_ERR_NVS_NOT_FOUND) {
            rejected++;         /* e.g. type changed between firmware versions */
        }
    }
    /* Values stored by older firmware may break a cross-field rule: fix, log and write back. */
    float before[SET_COUNT];
    memcpy(before, s_val, sizeof(before));
    if (settings_rules_fix(s_val) > 0) {
        for (int i = 0; i < SET_COUNT; i++) {
            if (s_val[i] != before[i]) {
                ESP_LOGW(TAG, "%s: stored %g breaks a rule, using %g", g_settings_table[i].nvs_key,
                         (double)before[i], (double)s_val[i]);
                mark_dirty_locked((setting_id_t)i);
            }
        }
    }
    load_ap_default();
    s_seq++;

    if (xTaskCreate(persist_task, "settings_nvs", 3072, NULL, 1, &s_persist_task) != pdPASS) {
        ESP_LOGE(TAG, "persist task not created: changes are not saved");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "%s schema %u: %d keys loaded, %d rejected, %d rows total", SETTINGS_NAMESPACE, SETTINGS_SCHEMA,
             loaded, rejected, SET_COUNT);
    return ESP_OK;
}
