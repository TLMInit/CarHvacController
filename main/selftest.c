/**
 * @file selftest.c
 * @brief Boot self-test: checks link, sensors, valves and blower once, after calibration.
 */

#include "selftest.h"
#include "event_log.h"
#include "remote_io.h"
#include "sensor_roles.h"
#include "hvac_sensors.h"
#include "tb6612fng_driver.h"
#include "bts7960_driver.h"
#include "hvac_hw_config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "SELFTEST";

#define SELFTEST_TASK_STACK     3072
#define SELFTEST_TASK_PRIO      2
#define SELFTEST_SETTLE_MS      3000    // sensors need a few cycles before their status means anything
#define SELFTEST_MAX_WAIT_MS    60000
#define SELFTEST_POLL_MS        250

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static selftest_item_t s_items[SELFTEST_MAX_ITEMS];
static size_t s_count;
static bool s_done;
static uint32_t s_seq;
static bool s_started;

// Only the self-test task touches this; published by copying under s_lock.
static selftest_item_t s_work[SELFTEST_MAX_ITEMS];
static size_t s_work_count;

static const struct {
    hvac_valve_id_t id;
    bool enabled;
    const char *name;
} s_valves[] = {
    { HVAC_VALVE_CENTER_1,   VALVE_CENTER_1_ENABLED,   "Valve C1" },
    { HVAC_VALVE_CENTER_2,   VALVE_CENTER_2_ENABLED,   "Valve C2" },
    { HVAC_VALVE_FOOT,       VALVE_FOOT_ENABLED,       "Valve Foot" },
    { HVAC_VALVE_WINDSCREEN, VALVE_WINDSCREEN_ENABLED, "Valve WS" },
    { HVAC_VALVE_COOLANT,    VALVE_COOLANT_ENABLED,    "Valve Coolant" },
};

static selftest_item_t *add_item(const char *name, selftest_result_t result, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static selftest_item_t *add_item(const char *name, selftest_result_t result, const char *fmt, ...)
{
    if (s_work_count >= SELFTEST_MAX_ITEMS) {
        ESP_LOGE(TAG, "Too many items, dropped %s", name);
        return NULL;
    }
    selftest_item_t *it = &s_work[s_work_count++];
    it->name = name;
    it->result = result;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(it->detail, sizeof(it->detail), fmt, ap);
    va_end(ap);
    return it;
}

static void check_io_node(void)
{
    remote_io_health_t h;
    remote_io_get_health(&h);
    if (!h.connected && !remote_io_is_connected()) {
        add_item("IO node", SELFTEST_FAIL, "no link");
        return;
    }
    const char *version = h.node_version;
    if (version[0] == '\0' || strcmp(version, "?") == 0) {
        remote_io_get_node_info(&version, NULL, NULL);
    }
    if (h.node_proto == 1) {
        add_item("IO node", SELFTEST_WARN, "%s, no HELLO (proto 1)", version);
    } else if (h.node_proto == 0) {
        add_item("IO node", SELFTEST_PASS, "%s, proto ?", version);
    } else {
        add_item("IO node", SELFTEST_PASS, "%s, proto %u", version, h.node_proto);
    }
}

static void check_cabin(void)
{
    if (hvac_sensors_get_status(SENSOR_CABIN_TEMP) == SENSOR_STATUS_OK) {
        add_item("Cabin sensor", SELFTEST_PASS, "%.1f C", (double)hvac_sensors_get_cabin_temp());
    } else {
        add_item("Cabin sensor", SELFTEST_FAIL, "BME280 not answering");
    }
}

static bool role_assigned(const sensor_rom_info_t *roms, size_t n, sensor_role_t role)
{
    for (size_t i = 0; i < n; i++) {
        if (roms[i].role == role) {
            return true;
        }
    }
    return false;
}

static void check_roles(void)
{
    sensor_rom_info_t roms[SENSOR_ROLES_MAX_ROMS];
    size_t n = sensor_roles_get_found(roms, SENSOR_ROLES_MAX_ROMS);

    float duct = sensor_roles_get_temp(SENSOR_ROLE_DUCT);
    if (!isnan(duct)) {
        add_item("Duct sensor", SELFTEST_PASS, "%.1f C", (double)duct);
    } else {
        add_item("Duct sensor", SELFTEST_WARN,
                 role_assigned(roms, n, SENSOR_ROLE_DUCT) ? "no reading" : "not found");
    }

    static const struct { sensor_role_t role; const char *name; } optional[] = {
        { SENSOR_ROLE_COOLANT, "Coolant sensor" },
        { SENSOR_ROLE_OUTSIDE, "Outside sensor" },
    };
    for (size_t i = 0; i < sizeof(optional) / sizeof(optional[0]); i++) {
        float t = sensor_roles_get_temp(optional[i].role);
        if (!isnan(t)) {
            add_item(optional[i].name, SELFTEST_PASS, "%.1f C", (double)t);
        } else if (role_assigned(roms, n, optional[i].role)) {
            add_item(optional[i].name, SELFTEST_WARN, "assigned, no reading");
        } else {
            add_item(optional[i].name, SELFTEST_PASS, "not assigned");
        }
    }
}

static void check_valves(void)
{
    for (size_t i = 0; i < sizeof(s_valves) / sizeof(s_valves[0]); i++) {
        if (!s_valves[i].enabled) {
            continue;
        }
        tb6612_valve_info_t info;
        tb6612fng_get_valve_info(s_valves[i].id, &info);
        const char *name = s_valves[i].name;

        if (info.feedback_fault) {
            add_item(name, SELFTEST_FAIL, "feedback lost");
        } else if (info.stalled || tb6612fng_valve_has_fault(s_valves[i].id)) {
            add_item(name, SELFTEST_FAIL, "stalled");
        } else {
            switch (info.cal_source) {
            case VALVE_CAL_SRC_STORED:     add_item(name, SELFTEST_PASS, "stored cal"); break;
            case VALVE_CAL_SRC_CALIBRATED: add_item(name, SELFTEST_PASS, "calibrated"); break;
            case VALVE_CAL_SRC_FIXED:      add_item(name, SELFTEST_PASS, "fixed (linear)"); break;
            default:                       add_item(name, SELFTEST_WARN, "uncalibrated"); break;
            }
        }
    }
}

static void check_blower(void)
{
    if (bts7960_last_command_ok()) {
        add_item("Blower", SELFTEST_PASS, "ack");
    } else {
        add_item("Blower", SELFTEST_FAIL, "no ack");
    }
}

static const char *result_name(selftest_result_t r)
{
    switch (r) {
    case SELFTEST_PASS: return "PASS";
    case SELFTEST_WARN: return "WARN";
    case SELFTEST_FAIL: return "FAIL";
    default:            return "PENDING";
    }
}

static void selftest_task(void *arg)
{
    (void)arg;
    int64_t start_ms = esp_timer_get_time() / 1000;
    for (;;) {
        int64_t elapsed = esp_timer_get_time() / 1000 - start_ms;
        if (elapsed >= SELFTEST_MAX_WAIT_MS) {
            ESP_LOGW(TAG, "Calibration still running after %d s, testing anyway", SELFTEST_MAX_WAIT_MS / 1000);
            break;
        }
        if (elapsed >= SELFTEST_SETTLE_MS && !tb6612fng_is_calibrating()) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(SELFTEST_POLL_MS));
    }

    s_work_count = 0;
    check_io_node();
    check_cabin();
    check_roles();
    check_valves();
    check_blower();

    portENTER_CRITICAL(&s_lock);
    memcpy(s_items, s_work, sizeof(s_items));
    s_count = s_work_count;
    s_done = true;
    s_seq++;
    portEXIT_CRITICAL(&s_lock);

    for (size_t i = 0; i < s_work_count; i++) {
        const selftest_item_t *it = &s_work[i];
        ESP_LOGI(TAG, "%-4s %-14s %s", result_name(it->result), it->name, it->detail);
        if (it->result == SELFTEST_FAIL) {
            event_log_add(EVT_FAULT, "%s: %s", it->name, it->detail);
        } else if (it->result == SELFTEST_WARN) {
            event_log_add(EVT_INFO, "%s: %s", it->name, it->detail);
        }
    }
    char summary[EVENT_TEXT_LEN];
    selftest_summary(summary, sizeof(summary));
    event_log_add(EVT_INFO, "%s", summary);

    vTaskDelete(NULL);
}

void selftest_start(void)
{
    if (s_started) {
        return;
    }
    s_started = true;
    if (xTaskCreate(selftest_task, "selftest", SELFTEST_TASK_STACK, NULL, SELFTEST_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create self-test task");
    }
}

bool selftest_done(void)
{
    portENTER_CRITICAL(&s_lock);
    bool done = s_done;
    portEXIT_CRITICAL(&s_lock);
    return done;
}

static selftest_result_t overall_locked(void)
{
    if (!s_done) {
        return SELFTEST_PENDING;
    }
    selftest_result_t worst = SELFTEST_PASS;
    for (size_t i = 0; i < s_count; i++) {
        if (s_items[i].result > worst) {
            worst = s_items[i].result;
        }
    }
    return worst;
}

selftest_result_t selftest_overall(void)
{
    portENTER_CRITICAL(&s_lock);
    selftest_result_t r = overall_locked();
    portEXIT_CRITICAL(&s_lock);
    return r;
}

size_t selftest_get_items(selftest_item_t *out, size_t max)
{
    if (out == NULL) {
        return 0;
    }
    portENTER_CRITICAL(&s_lock);
    size_t n = s_count < max ? s_count : max;
    memcpy(out, s_items, n * sizeof(*out));
    portEXIT_CRITICAL(&s_lock);
    return n;
}

void selftest_summary(char *buf, size_t len)
{
    if (buf == NULL || len == 0) {
        return;
    }
    int warns = 0, fails = 0;
    const char *first[2] = { NULL, NULL };
    portENTER_CRITICAL(&s_lock);
    bool done = s_done;
    for (size_t i = 0; i < s_count; i++) {
        if (s_items[i].result == SELFTEST_PASS) {
            continue;
        }
        if (s_items[i].result == SELFTEST_FAIL) {
            fails++;
        } else {
            warns++;
        }
        if (first[0] == NULL) {
            first[0] = s_items[i].name;
        } else if (first[1] == NULL) {
            first[1] = s_items[i].name;
        }
    }
    portEXIT_CRITICAL(&s_lock);

    if (!done) {
        snprintf(buf, len, "Self-test running");
        return;
    }
    if (warns + fails == 0) {
        snprintf(buf, len, "Self-test OK");
        return;
    }
    // Counts that are zero are left out so the line stays near 40 chars
    char counts[32] = "";
    int pos = 0;
    if (warns) {
        pos += snprintf(counts + pos, sizeof(counts) - pos, "%d warning%s", warns, warns == 1 ? "" : "s");
    }
    if (fails) {
        snprintf(counts + pos, sizeof(counts) - pos, "%s%d failure%s", warns ? ", " : "", fails, fails == 1 ? "" : "s");
    }
    if (first[1]) {
        snprintf(buf, len, "Self-test: %s - %s, %s", counts, first[0], first[1]);
    } else {
        snprintf(buf, len, "Self-test: %s - %s", counts, first[0]);
    }
}

uint32_t selftest_seq(void)
{
    portENTER_CRITICAL(&s_lock);
    uint32_t seq = s_seq;
    portEXIT_CRITICAL(&s_lock);
    return seq;
}
