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

static const char *TAG = "WEB";

#define SETTINGS_BODY_MAX   256
#define NODE_IMAGE_MAX      (1024 * 1024)
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

    char sp[12], cab[12], duct[12], out[12], cool[12], hum[12], dew[12];
    char e_reason[80], e_dv[48], e_nv[100], e_why[100], e_ssid[24];
    char buf[1100];
    snprintf(buf, sizeof(buf),
             "{\"mode\":\"%s\",\"mode_id\":%d,\"fan\":\"%s\",\"fan_id\":%d,\"blower_pct\":%u,"
             "\"setpoint\":%s,\"cabin\":%s,\"duct\":%s,\"outside\":%s,\"coolant\":%s,"
             "\"humidity\":%s,\"dew_point\":%s,\"ac\":%s,\"ac_on\":%s,\"ac_reason\":\"%s\","
             "\"recirc\":%s,\"preset\":\"%s\",\"defog\":%s,\"heater_delay\":%s,"
             "\"sensor_fault\":%s,\"valve_fault\":%s,\"link\":%s,"
             "\"ver_display\":\"%s\",\"ver_node\":\"%s\",\"ota_pending\":%s,"
             "\"ota_allowed\":%s,\"ota_why\":\"%s\",\"ssid\":\"%s\",\"clients\":%d,\"ap_left_s\":%u}",
             hvac_controller_get_mode_name(s.mode), (int)s.mode,
             hvac_controller_get_fan_level_name(s.fan_level), (int)s.fan_level, s.blower_speed,
             jnum(sp, sizeof(sp), s.target_temp_c, 1), jnum(cab, sizeof(cab), s.cabin_temp_c, 1),
             jnum(duct, sizeof(duct), s.duct_temp_c, 1), jnum(out, sizeof(out), s.outside_temp_c, 1),
             jnum(cool, sizeof(cool), s.coolant_temp_c, 1), jnum(hum, sizeof(hum), s.cabin_humidity, 0),
             jnum(dew, sizeof(dew), s.dew_point_c, 1),
             s.ac_enabled ? "true" : "false", s.ac_compressor_on ? "true" : "false",
             jesc(e_reason, sizeof(e_reason), s.ac_reason),
             s.recirculate ? "true" : "false", hvac_controller_preset_name(s.preset),
             s.defog_active ? "true" : "false", s.heater_delay_active ? "true" : "false",
             s.sensor_fault ? "true" : "false", s.valve_fault ? "true" : "false",
             remote_io_is_connected() ? "true" : "false",
             jesc(e_dv, sizeof(e_dv), esp_app_get_description()->version),
             jesc(e_nv, sizeof(e_nv), h.node_version),
             ota_display_pending_validation() ? "true" : "false",
             ota_ok ? "true" : "false", jesc(e_why, sizeof(e_why), why),
             jesc(e_ssid, sizeof(e_ssid), wifi_ap_ssid()), wifi_ap_client_count(),
             (unsigned)wifi_ap_remaining_s());
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
               "\"min\":%g,\"max\":%g,\"step\":%g,\"unit\":\"%s\",\"value\":%s,\"formatted\":\"%s\"",
               c->n++ ? "," : "", (int)d->id,
               d->nvs_key ? "\"" : "", d->nvs_key ? jesc(key, sizeof(key), d->nvs_key) : "null", d->nvs_key ? "\"" : "",
               (int)d->group, jesc(label, sizeof(label), settings_label(d->id)), k_type_name[d->type],
               (double)d->min, (double)d->max, (double)d->step,
               jesc(unit, sizeof(unit), d->unit ? d->unit : ""), val, jesc(fmt, sizeof(fmt), fmt_raw)) != ESP_OK) {
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

/* Pointer to the value after "name": (whitespace skipped), or NULL. */
static const char *json_field(const char *body, const char *name)
{
    char pat[24];
    snprintf(pat, sizeof(pat), "\"%s\"", name);
    const char *p = strstr(body, pat);
    if (!p) {
        return NULL;
    }
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p++ != ':') {
        return NULL;
    }
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}

/* Parse a JSON string at p (simple escapes only). */
static bool json_str(const char *p, char *out, size_t n)
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

static esp_err_t settings_post(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len > SETTINGS_BODY_MAX) {
        return reply(req, "413 Content Too Large", false, "Body must be 1..256 bytes");
    }
    char body[SETTINGS_BODY_MAX + 1];
    size_t got = 0;
    while (got < req->content_len) {
        int r = recv_some(req, body + got, req->content_len - got);
        if (r <= 0) {
            return reply(req, "400 Bad Request", false, "Body not received");
        }
        got += r;
    }
    body[got] = '\0';

    char key[16];
    if (!json_str(json_field(body, "key"), key, sizeof(key))) {
        return reply(req, "400 Bad Request", false, "Missing \"key\"");
    }
    /* ACTION rows have no nvs_key, so they cannot be reached from here (display only). */
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
    httpd_resp_set_type(req, "application/json");
    chunkf(req, "{\"link\":{\"connected\":%s,\"rtt_last_us\":%lu,\"rtt_p95_us\":%lu,\"timeouts_last_min\":%lu,"
           "\"tx\":%lu,\"rx\":%lu,\"crc_errors\":%lu,\"timeouts\":%lu,\"mismatches\":%lu,"
           "\"node_proto\":%u,\"node_caps\":%u,\"node_version\":\"%s\",\"node_uptime_s\":%lu,\"node_restarts\":%lu},"
           "\"heap_internal\":%u,\"heap_psram\":%u,\"selftest_done\":%s,\"selftest_summary\":\"%s\",\"selftest\":[",
           h.connected ? "true" : "false", (unsigned long)h.rtt_last_us, (unsigned long)h.rtt_p95_us,
           (unsigned long)h.timeouts_last_min, (unsigned long)h.tx_frames, (unsigned long)h.rx_frames,
           (unsigned long)h.crc_errors, (unsigned long)h.timeouts, (unsigned long)h.mismatches,
           h.node_proto, h.node_caps, jesc(nv, sizeof(nv), h.node_version),
           (unsigned long)h.node_uptime_s, (unsigned long)h.node_restarts,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
           selftest_done() ? "true" : "false", jesc(esum, sizeof(esum), sum));
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
        return reply(req, "413 Content Too Large", false, "Node image must be 1 byte .. 1 MB");
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
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 6144;
    cfg.max_open_sockets = 4;
    cfg.max_uri_handlers = 10;
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 10;
    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(err));
        s_server = NULL;
        return err;
    }
    static const httpd_uri_t uris[] = {
        { .uri = "/",                .method = HTTP_GET,  .handler = index_get },
        { .uri = "/api/state",       .method = HTTP_GET,  .handler = state_get },
        { .uri = "/api/settings",    .method = HTTP_GET,  .handler = settings_get },
        { .uri = "/api/settings",    .method = HTTP_POST, .handler = settings_post },
        { .uri = "/api/log",         .method = HTTP_GET,  .handler = log_get },
        { .uri = "/api/diag",        .method = HTTP_GET,  .handler = diag_get },
        { .uri = "/api/ota/display", .method = HTTP_POST, .handler = ota_display_post },
        { .uri = "/api/ota/node",    .method = HTTP_POST, .handler = ota_node_post },
        { .uri = "/api/ota/status",  .method = HTTP_GET,  .handler = ota_status_get },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(s_server, &uris[i]);
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
