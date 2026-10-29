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
#include "esp_mac.h"
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
        /* shortcut: derived from the public MAC, so guessable by anyone who sees the BSSID; users should set their own */
        uint8_t mac[6] = {0};
        esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
        snprintf(tmp, sizeof(tmp), "hvac-%02x%02x%02x", mac[3], mac[4], mac[5]);
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
    bool changed, other_changed = false;
    setting_id_t other = id;
    taskENTER_CRITICAL(&s_lock);
    /* Humidity hysteresis: OFF stays at least 2 %RH below ON. */
    if (id == SET_HUMIDITY_OFF && v > s_val[SET_HUMIDITY_ON] - 2.0f) {
        v = s_val[SET_HUMIDITY_ON] - 2.0f;
    }
    changed = store_locked(id, v);
    if (id == SET_HUMIDITY_ON && s_val[SET_HUMIDITY_OFF] > v - 2.0f) {
        other = SET_HUMIDITY_OFF;
        other_changed = store_locked(other, sanitize(settings_def(other), v - 2.0f));
    }
    taskEXIT_CRITICAL(&s_lock);

    if (changed) {
        after_change(id);
    }
    if (other_changed) {
        after_change(other);
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
    /* Id order resets HUMIDITY_ON before HUMIDITY_OFF, so the default pair is never clamped. */
    for (int i = 0; i < SET_COUNT; i++) {
        settings_reset((setting_id_t)i);
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
        if (e == ESP_OK) {
            s_val[i] = sanitize(d, v);     /* a range or step changed by a newer firmware is clamped, not dropped */
            loaded++;
        } else if (e != ESP_ERR_NVS_NOT_FOUND) {
            rejected++;         /* e.g. type changed between firmware versions */
        }
    }
    if (s_val[SET_HUMIDITY_OFF] > s_val[SET_HUMIDITY_ON] - 2.0f) {
        s_val[SET_HUMIDITY_OFF] = s_val[SET_HUMIDITY_ON] - 2.0f;
    }
    s_seq++;

    if (xTaskCreate(persist_task, "settings_nvs", 3072, NULL, 1, &s_persist_task) != pdPASS) {
        ESP_LOGE(TAG, "persist task not created: changes are not saved");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "%s schema %u: %d keys loaded, %d rejected, %d rows total", SETTINGS_NAMESPACE, SETTINGS_SCHEMA,
             loaded, rejected, SET_COUNT);
    return ESP_OK;
}
