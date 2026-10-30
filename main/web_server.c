/**
 * @file web_server.c
 * @brief Phone page and JSON API, served while the AP is on (WP-G).
 *
 * Handlers run in the httpd task and only use the thread-safe getters/setters.
 * Lists are sent as chunked responses, one small stack buffer per item, so no
 * response is bounded by a single big buffer.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <math.h>
#include <stdatomic.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "psa/crypto.h"
#include "web_server.h"
#include "wifi_ap.h"
#include "ota_display.h"
#include "ota_image.h"
#include "settings.h"
#include "hvac_controller.h"
#include "remote_io.h"
#include "event_log.h"
#include "selftest.h"
#include "sensor_roles.h"
#include "tb6612fng_driver.h"
#include "hvac_ui.h"
#include "esp_timer.h"
#include "web_req.h"

static const char *TAG = "WEB";

#define SETTINGS_BODY_MAX   256         /* also every other small JSON POST */
#define EXPORT_MAX          8192        /* about 4 KB today */
#define IMPORT_BODY_MAX     (16 * 1024)
#define NODE_IMAGE_MAX      NODE_OTA_SLOT_SIZE  /* one source with the node's OTA slot (remote_io.h) */
#define RECV_CHUNK          4096
#define RECV_RETRIES        3

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");

static httpd_handle_t s_server;

/* One update at a time, display or node. */
static atomic_bool s_ota_busy;

typedef struct {
    const char *state;      /* "idle", "receiving", "flashing", "done", "error" */
    volatile uint32_t done, total;
    char msg[64];
} ota_status_t;

static ota_status_t s_disp = { .state = "idle" };
static ota_status_t s_node = { .state = "idle" };

static uint8_t *s_node_img;
static size_t s_node_len;
static uint8_t s_node_sha[32];

/* ---- JSON helpers ---- */

/* Escape s into out as JSON string contents (no quotes); truncates safely. */
static const char *jesc(char *out, size_t n, const char *s)
{
    size_t o = 0;
    for (; s && *s && o + 7 < n; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c < 0x20) {
            o += snprintf(out + o, n - o, "\\u%04x", c);
        } else {
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
    return out;
}

/* Number or null (NaN sensors). */
static const char *jnum(char *out, size_t n, float v, int prec)
{
    if (isfinite(v)) {
        snprintf(out, n, "%.*f", prec, v);
    } else {
        snprintf(out, n, "null");
    }
    return out;
}

static esp_err_t chunkf(httpd_req_t *req, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static esp_err_t chunkf(httpd_req_t *req, const char *fmt, ...)
{
    char buf[768];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return httpd_resp_sendstr_chunk(req, buf);
}

static esp_err_t reply(httpd_req_t *req, const char *status, bool ok, const char *msg)
{
    char esc[160], buf[200];
    snprintf(buf, sizeof(buf), "{\"ok\":%s,\"msg\":\"%s\"}", ok ? "true" : "false", jesc(esc, sizeof(esc), msg));
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

/* Receive up to len bytes, tolerating a few socket timeouts. Returns bytes or <= 0 on failure. */
static int recv_some(httpd_req_t *req, char *buf, size_t len)
{
    for (int i = 0; i < RECV_RETRIES; i++) {
        int r = httpd_req_recv(req, buf, len);
        if (r != HTTPD_SOCK_ERR_TIMEOUT) {
            return r;
        }
    }
    return -1;
}

/* ---- GET / ---- */

static esp_err_t index_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, (const char *)index_html_start, index_html_end - index_html_start);
}

/* ---- GET /api/state ---- */

static esp_err_t state_get(httpd_req_t *req)
{
    hvac_state_t s;
    if (!hvac_controller_get_state(&s)) {
        return reply(req, "503 Service Unavailable", false, "Controller busy, retry");
    }
    remote_io_health_t h;
    remote_io_get_health(&h);
    char why[64] = "";
    bool ota_ok = ota_allowed(why, sizeof(why));

    char sp[12], cab[12], duct[12], out[12], cool[12], hum[12], dew[12], evap[12];
    char e_reason[80], e_dv[48], e_nv[100], e_why[100], e_ssid[24];
    char buf[1500];
    snprintf(buf, sizeof(buf),
             "{\"mode\":\"%s\",\"mode_id\":%d,\"fan\":\"%s\",\"fan_id\":%d,\"blower_pct\":%u,"
             "\"setpoint\":%s,\"cabin\":%s,\"duct\":%s,\"outside\":%s,\"coolant\":%s,"
             "\"humidity\":%s,\"dew_point\":%s,\"ac\":%s,\"ac_on\":%s,\"ac_reason\":\"%s\","
             "\"compressor_type\":\"%s\",\"ac_demand\":%.0f,\"ac_output\":%.0f,\"ac_enable\":%s,\"evap_c\":%s,"
             "\"recirc\":%s,\"preset\":\"%s\",\"defog\":%s,\"heater_delay\":%s,"
             "\"sensor_fault\":%s,\"valve_fault\":%s,\"link\":%s,"
             "\"ver_display\":\"%s\",\"ver_node\":\"%s\",\"ota_pending\":%s,"
             "\"ota_allowed\":%s,\"ota_why\":\"%s\",\"ssid\":\"%s\",\"clients\":%d,\"ap_left_s\":%u,"
             "\"preset_id\":%d,\"recirc_fitted\":%s,\"night\":%s,\"bright_day\":%d,\"bright_night\":%d,"
             "\"valve_fault_mask\":%u,\"heating\":%s,\"cooling\":%s,\"reached\":%s,\"ac_test\":%s}",
             hvac_controller_get_mode_name(s.mode), (int)s.mode,
             hvac_controller_get_fan_level_name(s.fan_level), (int)s.fan_level, s.blower_speed,
             jnum(sp, sizeof(sp), s.target_temp_c, 1), jnum(cab, sizeof(cab), s.cabin_temp_c, 1),
             jnum(duct, sizeof(duct), s.duct_temp_c, 1), jnum(out, sizeof(out), s.outside_temp_c, 1),
             jnum(cool, sizeof(cool), s.coolant_temp_c, 1), jnum(hum, sizeof(hum), s.cabin_humidity, 0),
             jnum(dew, sizeof(dew), s.dew_point_c, 1),
             s.ac_enabled ? "true" : "false", s.ac_compressor_on ? "true" : "false",
             jesc(e_reason, sizeof(e_reason), s.ac_reason),
             s.compressor_type == SET_AC_ELECTRIC ? "electric" : "clutch", (double)s.ac_demand_pct,
             (double)s.ac_output_pct, s.ac_enable ? "true" : "false", jnum(evap, sizeof(evap), s.evap_c, 1),
             s.recirculate ? "true" : "false", hvac_controller_preset_name(s.preset),
             s.defog_active ? "true" : "false", s.heater_delay_active ? "true" : "false",
             s.sensor_fault ? "true" : "false", s.valve_fault ? "true" : "false",
             remote_io_is_connected() ? "true" : "false",
             jesc(e_dv, sizeof(e_dv), esp_app_get_description()->version),
             jesc(e_nv, sizeof(e_nv), h.node_version),
             ota_display_pending_validation() ? "true" : "false",
             ota_ok ? "true" : "false", jesc(e_why, sizeof(e_why), why),
             jesc(e_ssid, sizeof(e_ssid), wifi_ap_ssid()), wifi_ap_client_count(),
             (unsigned)wifi_ap_remaining_s(),
             (int)s.preset, settings_get_bool(SET_RECIRC_FITTED) ? "true" : "false",
             settings_get_bool(SET_NIGHT_MODE) ? "true" : "false",
             (int)settings_get_int(SET_BRIGHT_DAY), (int)settings_get_int(SET_BRIGHT_NIGHT),
             (unsigned)s.valve_fault_mask, s.is_heating ? "true" : "false", s.is_cooling ? "true" : "false",
             s.temp_reached ? "true" : "false", s.ac_test ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

/* ---- GET /api/settings ---- */

typedef struct {
    httpd_req_t *req;
    int n;
} list_ctx_t;

static const char *const k_type_name[] = { "bool", "int", "enum", "float", "string", "action" };

static bool settings_row_cb(const setting_def_t *d, void *ctx)
{
    list_ctx_t *c = ctx;
    httpd_req_t *req = c->req;
    char label[100], key[24], fmt_raw[48], fmt[100], unit[24], val[72];
    settings_format(d->id, fmt_raw, sizeof(fmt_raw));
    if (d->type == SET_TYPE_STRING) {
        char s[SETTINGS_STR_MAX], e[SETTINGS_STR_MAX * 2 + 8];
        settings_get_str(d->id, s, sizeof(s));
        snprintf(val, sizeof(val), "\"%s\"", jesc(e, sizeof(e), s));
    } else if (d->type == SET_TYPE_ACTION) {
        snprintf(val, sizeof(val), "null");
    } else {
        snprintf(val, sizeof(val), "%g", (double)settings_get_float(d->id));
    }
    if (chunkf(req, "%s{\"id\":%d,\"key\":%s%s%s,\"group\":%d,\"label\":\"%s\",\"type\":\"%s\","
               "\"min\":%g,\"max\":%g,\"step\":%g,\"unit\":\"%s\",\"value\":%s,\"formatted\":\"%s\",\"visible\":%s",
               c->n++ ? "," : "", (int)d->id,
               d->nvs_key ? "\"" : "", d->nvs_key ? jesc(key, sizeof(key), d->nvs_key) : "null", d->nvs_key ? "\"" : "",
               (int)d->group, jesc(label, sizeof(label), settings_label(d->id)), k_type_name[d->type],
               (double)d->min, (double)d->max, (double)d->step,
               jesc(unit, sizeof(unit), d->unit ? d->unit : ""), val, jesc(fmt, sizeof(fmt), fmt_raw),
               settings_visible(d->id) ? "true" : "false") != ESP_OK) {
        return false;
    }
    if (d->type == SET_TYPE_ENUM) {
        bool da = settings_get_int(SET_LANGUAGE) == SET_LANG_DA;
        const char *const *labels = da ? d->enum_da : d->enum_en;
        httpd_resp_sendstr_chunk(req, ",\"enum\":[");
        for (int i = 0; labels && i <= (int)d->max; i++) {
            chunkf(req, "%s\"%s\"", i ? "," : "", jesc(label, sizeof(label), labels[i]));
        }
        httpd_resp_sendstr_chunk(req, "]");
    }
    return httpd_resp_sendstr_chunk(req, "}") == ESP_OK;
}

static esp_err_t settings_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    list_ctx_t c = { .req = req };
    httpd_resp_sendstr_chunk(req, "[");
    settings_iterate(settings_row_cb, &c);
    httpd_resp_sendstr_chunk(req, "]");
    return httpd_resp_sendstr_chunk(req, NULL);
}

/* ---- POST /api/settings {"key":"...","value":...} ---- */

/* Receive a small JSON body (1..SETTINGS_BODY_MAX bytes), NUL-terminated; on failure the
 * 4xx reply is sent and false returned. */
static bool recv_body(httpd_req_t *req, char body[SETTINGS_BODY_MAX + 1])
{
    if (req->content_len == 0 || req->content_len > SETTINGS_BODY_MAX) {
        reply(req, "413 Content Too Large", false, "Body must be 1..256 bytes");
        return false;
    }
    size_t got = 0;
    while (got < req->content_len) {
        int r = recv_some(req, body + got, req->content_len - got);
        if (r <= 0) {
            reply(req, "400 Bad Request", false, "Body not received");
            return false;
        }
        got += r;
    }
    body[got] = '\0';
    return true;
}

static esp_err_t settings_post(httpd_req_t *req)
{
    char body[SETTINGS_BODY_MAX + 1];
    if (!recv_body(req, body)) {
        return ESP_OK;
    }
    char key[16];
    if (!json_str(json_field(body, "key"), key, sizeof(key))) {
        return reply(req, "400 Bad Request", false, "Missing \"key\"");
    }
    /* ACTION rows have no nvs_key: they run through POST /api/action, after the page's confirmation. */
    const setting_def_t *d = NULL;
    for (int i = 0; i < SET_COUNT; i++) {
        const setting_def_t *r = settings_def((setting_id_t)i);
        if (r->nvs_key && r->type != SET_TYPE_ACTION && strcmp(r->nvs_key, key) == 0) {
            d = r;
            break;
        }
    }
    if (!d) {
        return reply(req, "404 Not Found", false, "Unknown setting");
    }
    if (d->group == SET_GROUP_SERVICE && hvac_controller_get_mode() != HVAC_MODE_OFF) {
        return reply(req, "409 Conflict", false, "Service settings change only with mode OFF");
    }
    const char *v = json_field(body, "value");
    if (!v) {
        return reply(req, "400 Bad Request", false, "Missing \"value\"");
    }

    esp_err_t err;
    const char *note = "Saved";
    if (d->type == SET_TYPE_STRING) {
        char s[SETTINGS_STR_MAX];
        if (!json_str(v, s, sizeof(s)) || strlen(s) < (size_t)d->min || strlen(s) > (size_t)d->max) {
            return reply(req, "400 Bad Request", false, "Password must be 8..32 characters");
        }
        err = settings_set_str(d->id, s);
        if (d->id == SET_AP_PASSWORD) {
            note = "Saved. The new password is used the next time the access point starts "
                   "(set Phone Wi-Fi off and on, or restart)";
        }
    } else {
        float f;
        if (strncmp(v, "true", 4) == 0) {
            f = 1;
        } else if (strncmp(v, "false", 5) == 0) {
            f = 0;
        } else {
            char *end;
            f = strtof(v, &end);
            if (end == v || !isfinite(f)) {
                return reply(req, "400 Bad Request", false, "Value must be a number");
            }
        }
        err = d->type == SET_TYPE_FLOAT ? settings_set_float(d->id, f) : settings_set_int(d->id, lroundf(f));
    }
    if (err != ESP_OK) {
        return reply(req, "400 Bad Request", false, esp_err_to_name(err));
    }
    char fmt[48], rule[SETTINGS_NOTE_MAX], msg[200 + SETTINGS_NOTE_MAX];
    settings_format(d->id, fmt, sizeof(fmt));
    if (settings_last_note(d->id, rule, sizeof(rule))) {
        snprintf(msg, sizeof(msg), "Saved. %s", rule);     /* a cross-field rule limited the value */
    } else {
        snprintf(msg, sizeof(msg), "%s: %s", note, d->type == SET_TYPE_STRING ? "********" : fmt);
    }
    return reply(req, "200 OK", true, msg);
}

/* ---- GET /api/settings/export[?include_password=1] ---- */

static esp_err_t settings_export_get(httpd_req_t *req)
{
    char q[48], v[4];
    bool pw = httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
              httpd_query_key_value(q, "include_password", v, sizeof(v)) == ESP_OK && strcmp(v, "1") == 0;
    const char *ver = esp_app_get_description()->version;
    char *buf = malloc(EXPORT_MAX);
    int n = buf ? settings_export_json(buf, EXPORT_MAX, ver, pw) : -1;
    if (n < 0) {
        const char *msg = buf ? "Export larger than its buffer" : "Out of memory";
        free(buf);
        return reply(req, "500 Internal Server Error", false, msg);
    }
    char name[32], cd[80];
    size_t i = 0;
    for (; ver[i] && i + 1 < sizeof(name); i++) {      /* header-safe file name */
        char c = ver[i];
        name[i] = (c == '.' || c == '-' || c == '_' || (c >= '0' && c <= '9') || ((c | 0x20) >= 'a' && (c | 0x20) <= 'z')) ? c : '_';
    }
    name[i] = '\0';
    snprintf(cd, sizeof(cd), "attachment; filename=\"hvac-settings-%s.json\"", name);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Content-Disposition", cd);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, buf, n);
    free(buf);
    return err;
}

/* ---- POST /api/settings/import (body = an export file) ---- */

static esp_err_t settings_import_post(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len > IMPORT_BODY_MAX) {
        return reply(req, "413 Content Too Large", false, "Settings file must be 1 byte .. 16 KB");
    }
    char *body = malloc(req->content_len + 1);
    if (!body) {
        return reply(req, "500 Internal Server Error", false, "Out of memory");
    }
    size_t got = 0;
    while (got < req->content_len) {
        int r = recv_some(req, body + got, req->content_len - got);
        if (r <= 0) {
            free(body);
            return reply(req, "400 Bad Request", false, "File not received");
        }
        got += r;
    }
    body[got] = '\0';
    char why[160];
    int changed = 0;
    esp_err_t err = settings_import_json(body, hvac_controller_get_mode() == HVAC_MODE_OFF, &changed, why, sizeof(why));
    free(body);
    if (err == ESP_OK) {
        snprintf(why, sizeof(why), "Imported: %d setting%s changed", changed, changed == 1 ? "" : "s");
        event_log_add(EVT_INFO, "Settings imported, %d changed", changed);
    }
    return reply(req, err == ESP_OK ? "200 OK" : err == ESP_ERR_INVALID_STATE ? "409 Conflict"
                 : err == ESP_ERR_NO_MEM ? "500 Internal Server Error" : "400 Bad Request", err == ESP_OK, why);
}

/* ---- GET /api/log ---- */

static bool log_row_cb(const event_t *e, void *ctx)
{
    list_ctx_t *c = ctx;
    char text[EVENT_TEXT_LEN * 2 + 8];
    return chunkf(c->req, "%s{\"t\":%lu,\"cls\":\"%s\",\"text\":\"%s\"}", c->n++ ? "," : "",
                  (unsigned long)e->uptime_ms, event_class_name(e->cls), jesc(text, sizeof(text), e->text)) == ESP_OK;
}

static esp_err_t log_get(httpd_req_t *req)
{
    list_ctx_t c = { .req = req };
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "[");
    event_log_iterate(log_row_cb, &c);      /* newest first */
    httpd_resp_sendstr_chunk(req, "]");
    return httpd_resp_sendstr_chunk(req, NULL);
}

/* ---- GET /api/diag ---- */

static esp_err_t diag_get(httpd_req_t *req)
{
    static const char *const k_res[] = { "pending", "pass", "warn", "fail" };
    remote_io_health_t h;
    remote_io_get_health(&h);
    char nv[100], sum[160], esum[200];
    selftest_summary(sum, sizeof(sum));
    float th_heat = 0, th_cool = 0, th_gain = 0;
    uint32_t th_n = 0;
    bool th_cal = false;
    (void)hvac_controller_get_thermal_status(&th_heat, &th_cool, &th_gain, &th_n, &th_cal);
    httpd_resp_set_type(req, "application/json");
    chunkf(req, "{\"link\":{\"connected\":%s,\"rtt_last_us\":%lu,\"rtt_p95_us\":%lu,\"timeouts_last_min\":%lu,"
           "\"tx\":%lu,\"rx\":%lu,\"crc_errors\":%lu,\"timeouts\":%lu,\"mismatches\":%lu,"
           "\"node_proto\":%u,\"node_caps\":%u,\"node_version\":\"%s\",\"node_uptime_s\":%lu,\"node_restarts\":%lu},"
           "\"heap_internal\":%u,\"heap_psram\":%u,\"uptime_s\":%lu,"
           "\"thermal\":{\"heat_rate\":%.2f,\"cool_rate\":%.2f,\"gain\":%.2f,\"samples\":%lu,\"calibrated\":%s},"
           "\"selftest_done\":%s,\"selftest_summary\":\"%s\",\"selftest\":[",
           h.connected ? "true" : "false", (unsigned long)h.rtt_last_us, (unsigned long)h.rtt_p95_us,
           (unsigned long)h.timeouts_last_min, (unsigned long)h.tx_frames, (unsigned long)h.rx_frames,
           (unsigned long)h.crc_errors, (unsigned long)h.timeouts, (unsigned long)h.mismatches,
           h.node_proto, h.node_caps, jesc(nv, sizeof(nv), h.node_version),
           (unsigned long)h.node_uptime_s, (unsigned long)h.node_restarts,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
           (unsigned long)(esp_timer_get_time() / 1000000), (double)th_heat, (double)th_cool, (double)th_gain,
           (unsigned long)th_n, th_cal ? "true" : "false", selftest_done() ? "true" : "false", jesc(esum, sizeof(esum), sum));
    selftest_item_t items[SELFTEST_MAX_ITEMS];
    size_t n = selftest_get_items(items, SELFTEST_MAX_ITEMS);
    for (size_t i = 0; i < n; i++) {
        char name[64], det[SELFTEST_DETAIL_LEN * 2 + 8];
        chunkf(req, "%s{\"name\":\"%s\",\"result\":\"%s\",\"detail\":\"%s\"}", i ? "," : "",
               jesc(name, sizeof(name), items[i].name ? items[i].name : "?"),
               k_res[items[i].result <= SELFTEST_FAIL ? items[i].result : 0],
               jesc(det, sizeof(det), items[i].detail));
    }
    httpd_resp_sendstr_chunk(req, "]}");
    return httpd_resp_sendstr_chunk(req, NULL);
}

/* ---- v2.5 phone parity: control, service, actions, trend ----
 * Each handler calls the same public API the touch screen calls, under the same gate. */

_Static_assert(WEB_MODES == HVAC_MODE_MAX && WEB_FANS == HVAC_FAN_MAX && WEB_PRESETS == HVAC_PRESET_COUNT &&
               WEB_VALVES == HVAC_VALVE_MAX && WEB_ROLES == SENSOR_ROLE_COUNT &&
               WEB_SETPOINT_MIN == SETTINGS_SETPOINT_MIN_C && WEB_SETPOINT_MAX == SETTINGS_SETPOINT_MAX_C,
               "web_req.h ranges = the firmware's enums");

static const char *const k_valve_names[WEB_VALVES] = { "Center 1", "Center 2", "Foot", "Windscreen", "Coolant" };
static const char *const k_cal_src[] = { "DEFAULT", "STORED", "CALIBRATED", "FIXED" };

static uint32_t ms_now(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* POST /api/control: one control per request, as one tap on the main screen */
static esp_err_t control_post(httpd_req_t *req)
{
    char body[SETTINGS_BODY_MAX + 1];
    if (!recv_body(req, body)) {
        return ESP_OK;
    }
    web_ctl_req_t c;
    const char *why = web_parse_control(body, &c);
    if (why) {
        return reply(req, "400 Bad Request", false, why);
    }
    hvac_state_t s;
    if (!hvac_controller_get_state(&s)) {
        return reply(req, "503 Service Unavailable", false, "Controller busy, retry");
    }
    esp_err_t err = ESP_OK;
    switch (c.what) {
    case WCTL_MODE:          err = hvac_controller_set_mode((hvac_mode_t)c.value); break;
    case WCTL_SETPOINT:      err = hvac_controller_set_target_temp(c.value); break;
    case WCTL_SETPOINT_STEP: c.value > 0 ? hvac_controller_temp_up() : hvac_controller_temp_down(); break;
    case WCTL_FAN:           err = hvac_controller_set_fan_level((hvac_fan_speed_t)c.value); break;
    case WCTL_FAN_STEP:      err = hvac_controller_set_fan_level(hvac_controller_fan_step(s.fan_level, (int)c.value)); break;
    case WCTL_AC:            hvac_controller_set_ac(c.value != 0); break;
    case WCTL_RECIRC:
        if (!settings_get_bool(SET_RECIRC_FITTED)) {    /* the screen hides the button */
            return reply(req, "409 Conflict", false, "No recirc flap fitted (Settings > Climate)");
        }
        hvac_controller_set_recirculate(c.value != 0);
        break;
    case WCTL_PRESET:        err = hvac_controller_apply_preset((hvac_preset_t)c.value); break;
    case WCTL_QUICK_AUTO:    err = hvac_controller_set_mode(HVAC_MODE_AUTO); break;
    case WCTL_QUICK_DEF:     hvac_controller_quick_defrost(); break;
    }
    if (err != ESP_OK) {
        return reply(req, "409 Conflict", false, esp_err_to_name(err));
    }
    return reply(req, "200 OK", true, "OK");
}

/* NULL = the valve may be jogged, recalibrated or learned: the display's valve service gate
 * (hvac_ui_service.c service_allowed), worded as its status line. */
static const char *valve_why(const hvac_state_t *st, int v, const tb6612_valve_info_t *vi)
{
    if (!tb6612fng_valve_fitted((hvac_valve_id_t)v)) return "Not fitted (see Settings > Valves)";
    if (st->mode != HVAC_MODE_OFF) return "Set airflow mode OFF to jog or service";
    if (vi->calibrating || tb6612fng_is_calibrating()) return "Busy: calibrating";
    return NULL;
}

/* The compressor test page's gate (hvac_ui_service.c comp_allowed). */
static const char *comp_why(const hvac_state_t *st)
{
    if (st->compressor_type != SET_AC_ELECTRIC) return "Clutch compressor: no test";
    if (st->mode != HVAC_MODE_OFF) return "Set airflow mode OFF to test";
    return NULL;
}

/* GET /api/service: valves, sensor roles and the compressor test in one poll */
static esp_err_t service_get(httpd_req_t *req)
{
    hvac_state_t st;
    if (!hvac_controller_get_state(&st)) {
        return reply(req, "503 Service Unavailable", false, "Controller busy, retry");
    }
    char e1[80], e2[80], t[12];
    httpd_resp_set_type(req, "application/json");
    chunkf(req, "{\"mode\":\"%s\",\"mode_off\":%s,\"jog_timeout_ms\":%d,\"valves\":[",
           hvac_controller_get_mode_name(st.mode), st.mode == HVAC_MODE_OFF ? "true" : "false", SERVICE_JOG_TIMEOUT_MS);
    for (int v = 0; v < WEB_VALVES; v++) {
        tb6612_valve_info_t vi;
        tb6612fng_get_valve_info((hvac_valve_id_t)v, &vi);
        const char *why = valve_why(&st, v, &vi);
        const char *fault = vi.stalled ? "stalled" : vi.feedback_fault ? "no feedback"
                            : (st.valve_fault_mask & (1U << v)) ? "valve fault" : "";
        chunkf(req, "%s{\"name\":\"%s\",\"fitted\":%s,\"pos\":%u,\"target\":%u,\"adc\":%u,\"cal\":\"%s\","
               "\"inverted\":%s,\"pwm\":%u,\"moving\":%s,\"busy\":%s,\"fault\":\"%s\",\"can\":%s,\"why\":\"%s\"}",
               v ? "," : "", k_valve_names[v], tb6612fng_valve_fitted((hvac_valve_id_t)v) ? "true" : "false",
               vi.position, vi.target, vi.adc_pos, vi.cal_source < 4 ? k_cal_src[vi.cal_source] : "?",
               vi.is_inverted ? "true" : "false", vi.speed, vi.moving ? "true" : "false",
               vi.service_active || vi.calibrating ? "true" : "false", fault,
               why ? "false" : "true", why ? why : "");
    }
    sensor_rom_info_t found[SENSOR_ROLES_MAX_ROMS];
    size_t n = sensor_roles_get_found(found, SENSOR_ROLES_MAX_ROMS);
    chunkf(req, "],\"search_pending\":%s,\"sensors\":[", sensor_roles_search_pending() ? "true" : "false");
    for (size_t i = 0; i < n && i < SENSOR_ROLES_MAX_ROMS; i++) {
        const uint8_t *r = found[i].rom;
        chunkf(req, "%s{\"rom\":\"%02X%02X%02X%02X%02X%02X%02X%02X\",\"role\":%d,\"present\":%s,\"temp\":%s}",
               i ? "," : "", r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], (int)found[i].role,
               found[i].present ? "true" : "false", jnum(t, sizeof(t), found[i].temp_c, 1));
    }
    httpd_resp_sendstr_chunk(req, "],\"roles\":[");
    for (int i = 0; i < SENSOR_ROLE_COUNT; i++) {
        chunkf(req, "%s\"%s\"", i ? "," : "", sensor_role_name((sensor_role_t)i));
    }
    const char *cwhy = comp_why(&st);
    chunkf(req, "],\"comp\":{\"electric\":%s,\"test\":%s,\"blower\":%s,\"demand\":%.0f,\"output\":%.0f,"
           "\"enable\":%s,\"evap\":%s,\"blower_pct\":%u,\"reason\":\"%s\",\"can\":%s,\"why\":\"%s\"}}",
           st.compressor_type == SET_AC_ELECTRIC ? "true" : "false", st.ac_test ? "true" : "false",
           st.ac_test_blower ? "true" : "false", (double)st.ac_demand_pct, (double)st.ac_output_pct,
           st.ac_enable ? "true" : "false", jnum(t, sizeof(t), st.evap_c, 1), st.blower_speed,
           jesc(e1, sizeof(e1), st.ac_reason), cwhy ? "false" : "true", jesc(e2, sizeof(e2), cwhy ? cwhy : ""));
    return httpd_resp_sendstr_chunk(req, NULL);
}

/* POST /api/service/valve. A jog runs for SERVICE_JOG_TIMEOUT_MS (300 ms) unless repeated: the
 * page repeats it every 150 ms while the button is held, so a lost phone or Wi-Fi stops the motor
 * through the driver's own dead-man. Jogs closer than JOG_MIN_GAP_MS are refused (429). */
#define JOG_MIN_GAP_MS 50U
static uint32_t s_jog_ms[WEB_VALVES];     /* httpd task only */

static esp_err_t service_valve_post(httpd_req_t *req)
{
    char body[SETTINGS_BODY_MAX + 1];
    if (!recv_body(req, body)) {
        return ESP_OK;
    }
    int v;
    web_valve_act_t act;
    const char *why = web_parse_valve(body, &v, &act);
    if (why) {
        return reply(req, "400 Bad Request", false, why);
    }
    const hvac_valve_id_t id = (hvac_valve_id_t)v;
    if (act == WVALVE_STOP) {                   /* never gated */
        (void)tb6612fng_service_stop(id);
        return reply(req, "200 OK", true, "Stopped");
    }
    hvac_state_t st;
    tb6612_valve_info_t vi;
    if (!hvac_controller_get_state(&st)) {
        return reply(req, "503 Service Unavailable", false, "Controller busy, retry");
    }
    tb6612fng_get_valve_info(id, &vi);
    const bool jog = act == WVALVE_JOG_PLUS || act == WVALVE_JOG_MINUS;
    if ((why = valve_why(&st, v, &vi)) != NULL) {
        if (jog) {
            (void)tb6612fng_service_stop(id);   /* e.g. mode left OFF mid-jog: stop now, as the screen does */
        }
        return reply(req, "409 Conflict", false, why);
    }
    esp_err_t err;
    if (jog) {
        uint32_t now = ms_now();
        if (now - s_jog_ms[v] < JOG_MIN_GAP_MS) {
            return reply(req, "429 Too Many Requests", false, "Jog repeats closer than 50 ms are ignored");
        }
        s_jog_ms[v] = now;
        if (tb6612fng_service_jog(id, act == WVALVE_JOG_PLUS ? 1 : -1) != ESP_OK) {
            (void)tb6612fng_service_stop(id);
            return reply(req, "409 Conflict", false, "Jog refused by the valve driver");
        }
        return reply(req, "200 OK", true, "Jogging");
    }
    err = act == WVALVE_RECAL ? tb6612fng_service_recalibrate(id) : tb6612fng_service_learn_direction(id);
    if (err != ESP_OK) {
        return reply(req, "409 Conflict", false, act == WVALVE_RECAL ? "Recalibrate refused" : "Learn direction refused");
    }
    return reply(req, "200 OK", true, act == WVALVE_RECAL ? "Recalibrating: the valve drives to both end stops"
                                                         : "Learning the direction");
}

/* POST /api/service/sensors */
static esp_err_t service_sensors_post(httpd_req_t *req)
{
    char body[SETTINGS_BODY_MAX + 1];
    if (!recv_body(req, body)) {
        return ESP_OK;
    }
    bool search;
    uint8_t rom[8];
    int role;
    const char *why = web_parse_sensor(body, &search, rom, &role);
    if (why) {
        return reply(req, "400 Bad Request", false, why);
    }
    if (search) {
        sensor_roles_request_search();
        return reply(req, "200 OK", true, "Searching the 1-Wire bus");
    }
    esp_err_t err = sensor_roles_assign(rom, (sensor_role_t)role);
    if (err != ESP_OK) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Role was not saved: %s", esp_err_to_name(err));
        return reply(req, "409 Conflict", false, msg);
    }
    return reply(req, "200 OK", true, "Role saved");
}

/* POST /api/service/compressor. The controller ends the test 60 s after the last call, on a mode
 * change or on stop; the page resends while its Service tab is open and visible. */
static esp_err_t service_compressor_post(httpd_req_t *req)
{
    char body[SETTINGS_BODY_MAX + 1];
    if (!recv_body(req, body)) {
        return ESP_OK;
    }
    bool stop, blower;
    int demand;
    const char *why = web_parse_compressor(body, &stop, &demand, &blower);
    if (why) {
        return reply(req, "400 Bad Request", false, why);
    }
    if (stop) {
        hvac_controller_ac_test_end();
        return reply(req, "200 OK", true, "Test ended, demand 0");
    }
    hvac_state_t st;
    if (!hvac_controller_get_state(&st)) {
        return reply(req, "503 Service Unavailable", false, "Controller busy, retry");
    }
    if ((why = comp_why(&st)) != NULL) {
        return reply(req, "409 Conflict", false, why);
    }
    if (hvac_controller_ac_test((float)demand, blower) != ESP_OK) {
        return reply(req, "409 Conflict", false, "Refused by the controller");
    }
    return reply(req, "200 OK", true, "Test running");
}

/* POST /api/action: buttons the page confirms first (factory reset and IO node restart twice) */
static esp_err_t action_post(httpd_req_t *req)
{
    char body[SETTINGS_BODY_MAX + 1];
    if (!recv_body(req, body)) {
        return ESP_OK;
    }
    web_act_t act;
    int id = 0;
    const char *why = web_parse_action(body, &act, &id);
    if (why) {
        return reply(req, "400 Bad Request", false, why);
    }
    switch (act) {
    case WACT_LOG_CLEAR:
        event_log_clear();
        return reply(req, "200 OK", true, "Event log cleared");
    case WACT_THERMAL_RESET:
        return hvac_controller_reset_thermal_cal() == ESP_OK
               ? reply(req, "200 OK", true, "Thermal calibration reset")
               : reply(req, "500 Internal Server Error", false, "Failed to reset thermal calibration");
    case WACT_THERMAL_SAVE:
        return hvac_controller_save_thermal_cal() == ESP_OK
               ? reply(req, "200 OK", true, "Thermal calibration saved")
               : reply(req, "500 Internal Server Error", false, "Failed to save thermal calibration");
    case WACT_SETTING:
        break;
    }
    if (id >= SET_COUNT || settings_def((setting_id_t)id)->type != SET_TYPE_ACTION) {
        return reply(req, "404 Not Found", false, "Not an action row");
    }
    const setting_def_t *d = settings_def((setting_id_t)id);
    if (d->group == SET_GROUP_SERVICE && hvac_controller_get_mode() != HVAC_MODE_OFF) {
        return reply(req, "409 Conflict", false, "Service settings change only with mode OFF");
    }
    char msg[100];
    if (d->id == SET_FACTORY_RESET) {           /* restarts inside the call: answer first */
        reply(req, "200 OK", true, "Factory reset: erasing and restarting. Reconnect to the Wi-Fi in about 20 s");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    esp_err_t err = settings_set_int(d->id, 1);
    snprintf(msg, sizeof(msg), "%s: %s", settings_label(d->id), err == ESP_OK ? "started" : esp_err_to_name(err));
    return reply(req, err == ESP_OK ? "200 OK" : "409 Conflict", err == ESP_OK, msg);
}

/* GET /api/trend: the display's chart ring (oldest first), null = no reading */
static esp_err_t trend_get(httpd_req_t *req)
{
    static lv_coord_t ring[HVAC_UI_TREND_SERIES][HVAC_UI_TREND_POINTS];    /* httpd task only, off its stack */
    static const char *const k_series[HVAC_UI_TREND_SERIES] = { "cabin", "duct", "setpoint" };
    uint32_t interval_s;
    bool ok = hvac_ui_trend_copy(ring, &interval_s);
    httpd_resp_set_type(req, "application/json");
    chunkf(req, "{\"interval_s\":%lu,\"points\":%d", (unsigned long)interval_s, HVAC_UI_TREND_POINTS);
    for (int s = 0; s < HVAC_UI_TREND_SERIES; s++) {
        char line[HVAC_UI_TREND_POINTS * 7 + 24];    /* "-123.4," per point */
        size_t o = snprintf(line, sizeof(line), ",\"%s\":[", k_series[s]);
        for (int i = 0; i < HVAC_UI_TREND_POINTS && o < sizeof(line); i++) {
            lv_coord_t v = ring[s][i];
            o += (!ok || v == LV_CHART_POINT_NONE)
                 ? snprintf(line + o, sizeof(line) - o, "%snull", i ? "," : "")
                 : snprintf(line + o, sizeof(line) - o, "%s%.1f", i ? "," : "", (double)v / 10.0);
        }
        if (o < sizeof(line)) {
            snprintf(line + o, sizeof(line) - o, "]");
        }
        httpd_resp_sendstr_chunk(req, line);
    }
    httpd_resp_sendstr_chunk(req, "}");
    return httpd_resp_sendstr_chunk(req, NULL);
}

/* ---- OTA ---- */

static void set_status(ota_status_t *st, const char *state, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void set_status(ota_status_t *st, const char *state, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(st->msg, sizeof(st->msg), fmt, ap);
    va_end(ap);
    st->state = state;
}

/* Claims the single OTA slot; on refusal sends 409 with the reason and returns false. */
static bool ota_claim(httpd_req_t *req)
{
    char why[64];
    if (atomic_exchange(&s_ota_busy, true)) {
        reply(req, "409 Conflict", false, "Another update is in progress");
        return false;
    }
    if (!ota_allowed(why, sizeof(why))) {
        atomic_store(&s_ota_busy, false);
        reply(req, "409 Conflict", false, why);
        return false;
    }
    return true;
}

static esp_err_t ota_display_post(httpd_req_t *req)
{
    if (req->content_len == 0) {
        return reply(req, "411 Length Required", false, "Send the .bin with a Content-Length");
    }
    if (!ota_claim(req)) {
        return ESP_OK;
    }
    s_disp.done = 0;
    s_disp.total = req->content_len;
    esp_err_t err = ota_display_begin(req->content_len);
    if (err != ESP_OK) {
        set_status(&s_disp, "error", "Begin failed: %s", esp_err_to_name(err));
        atomic_store(&s_ota_busy, false);
        return reply(req, err == ESP_ERR_INVALID_SIZE ? "413 Content Too Large" : "500 Internal Server Error",
                     false, s_disp.msg);
    }
    set_status(&s_disp, "receiving", "Writing");
    char *buf = heap_caps_malloc(RECV_CHUNK, MALLOC_CAP_INTERNAL);
    bool no_mem = !buf;
    while (buf && s_disp.done < s_disp.total) {
        int r = recv_some(req, buf, RECV_CHUNK);
        if (r <= 0 || (err = ota_display_write(buf, r)) != ESP_OK) {
            break;
        }
        s_disp.done += r;
    }
    free(buf);
    if (s_disp.done < s_disp.total) {
        ota_display_abort();
        if (err == ESP_ERR_INVALID_VERSION) {
            set_status(&s_disp, "error", "%s", ota_display_reject_reason());
        } else {
            set_status(&s_disp, "error", no_mem ? "Out of memory" : "Upload interrupted (%s)", esp_err_to_name(err));
        }
        atomic_store(&s_ota_busy, false);
        return reply(req, "400 Bad Request", false, s_disp.msg);
    }
    set_status(&s_disp, "flashing", "Verifying");
    err = ota_display_end();
    if (err != ESP_OK) {
        if (err == ESP_ERR_INVALID_VERSION) {
            set_status(&s_disp, "error", "%s", ota_display_reject_reason());
        } else {
            set_status(&s_disp, "error", "Image rejected: %s", esp_err_to_name(err));
        }
        atomic_store(&s_ota_busy, false);
        return reply(req, "400 Bad Request", false, s_disp.msg);
    }
    set_status(&s_disp, "done", "Restarting");
    reply(req, "200 OK", true, "Update stored. Restarting; reconnect to the Wi-Fi in about 20 s");
    vTaskDelay(pdMS_TO_TICKS(1000));    /* let the response leave before the restart */
    esp_restart();
    return ESP_OK;
}

static void node_progress(uint32_t done, uint32_t total, void *ctx)
{
    (void)ctx;
    s_node.done = done;
    s_node.total = total;
}

static void node_ota_task(void *arg)
{
    (void)arg;
    event_log_add(EVT_OTA, "Node OTA begin, %u bytes", (unsigned)s_node_len);
    set_status(&s_node, "flashing", "Sending to the node");
    s_node.done = 0;
    esp_err_t err = remote_io_ota_node(s_node_img, s_node_len, s_node_sha, node_progress, NULL);
    heap_caps_free(s_node_img);
    s_node_img = NULL;
    if (err == ESP_OK) {
        set_status(&s_node, "done", "Node updated and answering");
        event_log_add(EVT_OTA, "Node OTA OK");
    } else {
        set_status(&s_node, "error", "Node update failed: %s", err == ESP_ERR_INVALID_VERSION
                   ? "the node refused the image (not a CarHvacIoNode build)" : esp_err_to_name(err));
        event_log_add(EVT_OTA, "Node OTA failed: %s", esp_err_to_name(err));
    }
    atomic_store(&s_ota_busy, false);
    vTaskDelete(NULL);
}

static esp_err_t ota_node_post(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len > NODE_IMAGE_MAX) {
        return reply(req, "413 Content Too Large", false, "Node image must be 1 byte .. 960 KB (the node's OTA slot)");
    }
    if (!ota_claim(req)) {
        return ESP_OK;
    }
    s_node.done = 0;
    s_node.total = req->content_len;
    s_node_len = req->content_len;
    s_node_img = heap_caps_malloc(s_node_len, MALLOC_CAP_SPIRAM);
    char *buf = heap_caps_malloc(RECV_CHUNK, MALLOC_CAP_INTERNAL);
    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    bool ok = s_node_img && buf && psa_crypto_init() == PSA_SUCCESS &&
              psa_hash_setup(&op, PSA_ALG_SHA_256) == PSA_SUCCESS;
    set_status(&s_node, "receiving", "Receiving");
    size_t got = 0;
    while (ok && got < s_node_len) {
        int r = recv_some(req, buf, RECV_CHUNK < s_node_len - got ? RECV_CHUNK : s_node_len - got);
        /* hashed from the internal chunk buffer, not from PSRAM */
        ok = r > 0 && psa_hash_update(&op, (const uint8_t *)buf, r) == PSA_SUCCESS;
        if (ok) {
            memcpy(s_node_img + got, buf, r);
            got += r;
            s_node.done = got;
        }
    }
    size_t hlen = 0;
    ok = ok && psa_hash_finish(&op, s_node_sha, sizeof(s_node_sha), &hlen) == PSA_SUCCESS;
    psa_hash_abort(&op);
    free(buf);
    char why[96];
    if (ok && !ota_image_check((const uint8_t *)s_node_img, s_node_len, NODE_PROJECT_NAME, why, sizeof(why))) {
        heap_caps_free(s_node_img);
        s_node_img = NULL;
        set_status(&s_node, "error", "%s", why);
        ESP_LOGE(TAG, "Node image refused: %s", why);
        event_log_add(EVT_OTA, "Node OTA refused: %s", why);
        atomic_store(&s_ota_busy, false);
        return reply(req, "400 Bad Request", false, s_node.msg);
    }
    if (!ok || xTaskCreate(node_ota_task, "node_ota", 4096, NULL, 4, NULL) != pdPASS) {
        heap_caps_free(s_node_img);
        s_node_img = NULL;
        set_status(&s_node, "error", "%s", got < s_node_len ? "Upload interrupted or out of memory" : "Could not start");
        event_log_add(EVT_OTA, "Node OTA failed: %s", s_node.msg);
        atomic_store(&s_ota_busy, false);
        return reply(req, "400 Bad Request", false, s_node.msg);
    }
    return reply(req, "202 Accepted", true, "Image received; sending it to the node");
}

static esp_err_t ota_status_get(httpd_req_t *req)
{
    char dm[140], nm[140], buf[400];
    snprintf(buf, sizeof(buf),
             "{\"display\":{\"state\":\"%s\",\"done\":%lu,\"total\":%lu,\"msg\":\"%s\"},"
             "\"node\":{\"state\":\"%s\",\"done\":%lu,\"total\":%lu,\"msg\":\"%s\"}}",
             s_disp.state, (unsigned long)s_disp.done, (unsigned long)s_disp.total, jesc(dm, sizeof(dm), s_disp.msg),
             s_node.state, (unsigned long)s_node.done, (unsigned long)s_node.total, jesc(nm, sizeof(nm), s_node.msg));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

/* ---- start / stop ---- */

esp_err_t web_server_start(void)
{
    if (s_server) {
        return ESP_OK;
    }
    static const httpd_uri_t uris[] = {
        { .uri = "/",                .method = HTTP_GET,  .handler = index_get },
        { .uri = "/api/state",       .method = HTTP_GET,  .handler = state_get },
        { .uri = "/api/settings",    .method = HTTP_GET,  .handler = settings_get },
        { .uri = "/api/settings",    .method = HTTP_POST, .handler = settings_post },
        { .uri = "/api/settings/export", .method = HTTP_GET, .handler = settings_export_get },
        { .uri = "/api/settings/import", .method = HTTP_POST, .handler = settings_import_post },
        { .uri = "/api/log",         .method = HTTP_GET,  .handler = log_get },
        { .uri = "/api/diag",        .method = HTTP_GET,  .handler = diag_get },
        { .uri = "/api/ota/display", .method = HTTP_POST, .handler = ota_display_post },
        { .uri = "/api/ota/node",    .method = HTTP_POST, .handler = ota_node_post },
        { .uri = "/api/ota/status",  .method = HTTP_GET,  .handler = ota_status_get },
        { .uri = "/api/control",     .method = HTTP_POST, .handler = control_post },
        { .uri = "/api/service",     .method = HTTP_GET,  .handler = service_get },
        { .uri = "/api/service/valve", .method = HTTP_POST, .handler = service_valve_post },
        { .uri = "/api/service/sensors", .method = HTTP_POST, .handler = service_sensors_post },
        { .uri = "/api/service/compressor", .method = HTTP_POST, .handler = service_compressor_post },
        { .uri = "/api/action",      .method = HTTP_POST, .handler = action_post },
        { .uri = "/api/trend",       .method = HTTP_GET,  .handler = trend_get },
    };
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 6144;
    cfg.max_open_sockets = 4;
    cfg.max_uri_handlers = sizeof(uris) / sizeof(uris[0]);     /* every route above, no more */
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 10;
    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(err));
        s_server = NULL;
        return err;
    }
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        if (httpd_register_uri_handler(s_server, &uris[i]) != ESP_OK) {
            ESP_LOGE(TAG, "Route %s not registered", uris[i].uri);
        }
    }
    ESP_LOGI(TAG, "Web server on port %d", cfg.server_port);
    return ESP_OK;
}

void web_server_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
}
