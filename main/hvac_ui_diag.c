/**
 * @file hvac_ui_diag.c
 * @brief Diagnostics sub-pages of the settings page: event log, link health,
 *        self-test and the trend chart (60 points, one every SET_CHART_INTERVAL_S).
 *
 * The chart ring is filled by an lv_timer that starts at UI init, so the
 * history exists before the page is first opened. It is never fed from the
 * 100 ms update.
 */
#include "hvac_ui_priv.h"
#include "event_log.h"
#include "remote_io.h"
#include "selftest.h"

#include "hvac_ui_common.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define PAGE_REFRESH_MS 750U

/* ================= event log ================= */

#define EV_TEXT_MAX (EVENT_LOG_SIZE * 112)

static char s_ev_text[EV_TEXT_MAX];
static lv_obj_t *s_ev_label;
static uint32_t s_ev_seq, s_ev_last;

static void format_age(char *buf, size_t len, uint32_t age_s)
{
    if (age_s < 60) snprintf(buf, len, "%us", (unsigned)age_s);
    else if (age_s < 3600) snprintf(buf, len, "%um", (unsigned)(age_s / 60));
    else snprintf(buf, len, "%uh%02um", (unsigned)(age_s / 3600), (unsigned)((age_s % 3600) / 60));
}

static uint32_t class_color(event_class_t cls)
{
    switch (cls) {
    case EVT_TIMEOUT: case EVT_STALL: case EVT_FAULT: return UI_COLOR_FAULT;
    case EVT_NODE_RESET: return UI_COLOR_WARNING;
    default: return UI_COLOR_ACCENT;
    }
}

typedef struct { size_t used; uint32_t now; } ev_fmt_t;

static bool ev_format_cb(const event_t *e, void *ctx)
{
    ev_fmt_t *f = ctx;
    char age[12], text[EVENT_TEXT_LEN];
    format_age(age, sizeof(age), (f->now - e->uptime_ms) / 1000U);
    /* '#' would open a recolor command inside the label. */
    snprintf(text, sizeof(text), "%s", e->text);
    for (char *p = text; *p; ++p) if (*p == '#') *p = '*';
    int n = snprintf(s_ev_text + f->used, EV_TEXT_MAX - f->used, "#%06X %s#  %s\n   %s\n",
                     (unsigned)class_color(e->cls), event_class_name(e->cls), age, text);
    if (n < 0 || (size_t)n >= EV_TEXT_MAX - f->used) return false;
    f->used += (size_t)n;
    return true;
}

static void ev_rebuild(void)
{
    static uint32_t previous_hash;
    ev_fmt_t f = { .used = 0, .now = now_ms() };
    s_ev_text[0] = '\0';
    event_log_iterate(ev_format_cb, &f);
    if (f.used == 0) snprintf(s_ev_text, EV_TEXT_MAX, "No events");
    /* Only relayout when the text really changed (ages tick slowly). */
    uint32_t hash = 2166136261u;                    /* FNV-1a */
    for (const char *p = s_ev_text; *p; ++p) hash = (hash ^ (uint8_t)*p) * 16777619u;
    if (hash != previous_hash || lv_label_get_text(s_ev_label) != s_ev_text) {
        previous_hash = hash;
        lv_label_set_text_static(s_ev_label, s_ev_text);
    }
}

static void ev_clear_confirmed(void *data)
{
    (void)data;
    event_log_clear();
}

static void ev_clear_tap(lv_event_t *e)
{
    (void)e;
    ui_modal_confirm("Clear event log?", "All stored events are removed.", "CLEAR",
                     UI_COLOR_FAULT, ev_clear_confirmed, NULL);
}

static void ev_open(lv_obj_t *hdr, lv_obj_t *body)
{
    (void)ui_make_button(hdr, "CLEAR", 228, 0, 88, UI_HDR_H, UI_FONT_16, ev_clear_tap,
                         UI_COLOR_SURFACE, UI_COLOR_SURFACE_2);
    s_ev_label = lv_label_create(body);
    lv_label_set_recolor(s_ev_label, true);
    lv_label_set_long_mode(s_ev_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(s_ev_label, 8, 0);
    lv_obj_set_width(s_ev_label, 296);
    lv_obj_set_style_text_font(s_ev_label, UI_FONT_14, 0);
    lv_obj_set_style_text_color(s_ev_label, lv_color_hex(UI_COLOR_TEXT_DIM), 0);
    lv_obj_add_flag(s_ev_label, LV_OBJ_FLAG_GESTURE_BUBBLE);
    s_ev_seq = event_log_seq();
    s_ev_last = now_ms();
    ev_rebuild();
}

static void ev_update(void)
{
    uint32_t seq = event_log_seq();
    if (seq != s_ev_seq) {
        s_ev_seq = seq;
        s_ev_last = now_ms();
        ev_rebuild();
    } else if (refresh_due(&s_ev_last, 2000U)) {
        ev_rebuild();
    }
}

const ui_page_t ui_page_events = { "Event log", "H\xC3\xA6ndelseslog", ev_open, ev_update, NULL };

/* ================= link health ================= */

static ui_label_t s_link[8];
static uint32_t s_link_last;

static void link_open(lv_obj_t *hdr, lv_obj_t *body)
{
    (void)hdr;
    for (size_t i = 0; i < 8; ++i) {
        (void)ui_make_label(body, &s_link[i], "", 8, 4 + (lv_coord_t)i * 24, 304, 24,
                            UI_FONT_16, UI_COLOR_TEXT, LV_TEXT_ALIGN_LEFT);
    }
    s_link_last = now_ms() - PAGE_REFRESH_MS;
    ui_page_link.update();
}

static void link_update(void)
{
    if (!refresh_due(&s_link_last, PAGE_REFRESH_MS)) return;
    remote_io_health_t h;
    remote_io_get_health(&h);

    ui_label_set(&s_link[0], h.connected ? "Link: CONNECTED" : "Link: LOST");
    ui_label_set_color(&s_link[0], h.connected ? UI_COLOR_ACCENT : UI_COLOR_FAULT);
    ui_label_format(&s_link[1], "RTT %.1f ms   p95 %.1f ms",
                    (double)h.rtt_last_us / 1000.0, (double)h.rtt_p95_us / 1000.0);
    ui_label_format(&s_link[2], "Timeouts/min: %lu", (unsigned long)h.timeouts_last_min);
    ui_label_set_color(&s_link[2], h.timeouts_last_min ? UI_COLOR_WARNING : UI_COLOR_TEXT);
    ui_label_format(&s_link[3], "TX %lu   RX %lu", (unsigned long)h.tx_frames, (unsigned long)h.rx_frames);
    ui_label_format(&s_link[4], "CRC %lu  TO %lu  Mism %lu", (unsigned long)h.crc_errors,
                    (unsigned long)h.timeouts, (unsigned long)h.mismatches);
    ui_label_format(&s_link[5], "Node %s", h.node_version);
    ui_label_format(&s_link[6], "Up %luh%02lum  Restarts %lu",
                    (unsigned long)(h.node_uptime_s / 3600U),
                    (unsigned long)((h.node_uptime_s % 3600U) / 60U), (unsigned long)h.node_restarts);
    ui_label_set_color(&s_link[6], h.node_restarts ? UI_COLOR_WARNING : UI_COLOR_TEXT);
    ui_label_format(&s_link[7], "Proto %u   caps 0x%02X", (unsigned)h.node_proto, (unsigned)h.node_caps);
}

const ui_page_t ui_page_link = { "Link health", "Linkstatus", link_open, link_update, NULL };

/* ================= self-test ================= */

static lv_obj_t *s_st_body;
static uint32_t s_st_seq;

static uint32_t result_color(selftest_result_t r)
{
    switch (r) {
    case SELFTEST_PASS: return UI_COLOR_ACCENT;
    case SELFTEST_WARN: return UI_COLOR_WARNING;
    case SELFTEST_FAIL: return UI_COLOR_FAULT;
    default: return UI_COLOR_TEXT_DIM;
    }
}

static const char *result_text(selftest_result_t r)
{
    switch (r) {
    case SELFTEST_PASS: return "PASS";
    case SELFTEST_WARN: return "WARN";
    case SELFTEST_FAIL: return "FAIL";
    default: return "...";
    }
}

static void st_build(void)
{
    static selftest_item_t items[SELFTEST_MAX_ITEMS];
    char summary[96];
    lv_obj_clean(s_st_body);
    s_st_seq = selftest_seq();
    selftest_summary(summary, sizeof(summary));

    lv_obj_t *sum = lv_label_create(s_st_body);
    lv_label_set_text(sum, summary);
    lv_label_set_long_mode(sum, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(sum, 8, 2);
    lv_obj_set_width(sum, 304);
    lv_obj_set_style_text_font(sum, UI_FONT_14, 0);
    lv_obj_set_style_text_color(sum, lv_color_hex(result_color(selftest_overall())), 0);
    lv_obj_add_flag(sum, LV_OBJ_FLAG_GESTURE_BUBBLE);

    size_t n = selftest_get_items(items, SELFTEST_MAX_ITEMS);
    for (size_t i = 0; i < n; ++i) {
        lv_coord_t y = 40 + (lv_coord_t)i * 24;
        lv_obj_t *tag = lv_label_create(s_st_body);
        lv_label_set_text(tag, result_text(items[i].result));
        lv_obj_set_pos(tag, 8, y);
        lv_obj_set_style_text_font(tag, UI_FONT_16, 0);
        lv_obj_set_style_text_color(tag, lv_color_hex(result_color(items[i].result)), 0);
        lv_obj_add_flag(tag, LV_OBJ_FLAG_GESTURE_BUBBLE);

        char line[SELFTEST_DETAIL_LEN + 40];
        snprintf(line, sizeof(line), "%s: %s", items[i].name, items[i].detail);
        lv_obj_t *txt = lv_label_create(s_st_body);
        lv_label_set_text(txt, line);
        lv_label_set_long_mode(txt, LV_LABEL_LONG_CLIP);
        lv_obj_set_pos(txt, 60, y + 1);
        lv_obj_set_width(txt, 252);
        lv_obj_set_style_text_font(txt, UI_FONT_14, 0);
        lv_obj_set_style_text_color(txt, lv_color_hex(UI_COLOR_TEXT), 0);
        lv_obj_add_flag(txt, LV_OBJ_FLAG_GESTURE_BUBBLE);
    }
}

static void st_open(lv_obj_t *hdr, lv_obj_t *body)
{
    (void)hdr;
    s_st_body = body;
    st_build();
}

static void st_update(void)
{
    if (selftest_seq() != s_st_seq) st_build();
}

const ui_page_t ui_page_selftest = { "Self-test", "Selvtest", st_open, st_update, NULL };

/* ================= trend chart ================= */

#define CHART_POINTS     60

enum { SER_CABIN, SER_DUCT, SER_SETPOINT, SER_COUNT };

/* Newest point is last; values are tenths of degC. */
static lv_coord_t s_ring[SER_COUNT][CHART_POINTS];
static bool s_ring_init;
static lv_obj_t *s_chart;
static lv_chart_series_t *s_series[SER_COUNT];
static ui_label_t s_chart_range[2], s_chart_now;
static lv_timer_t *s_chart_timer;

static uint32_t chart_period_ms(void)
{
    return (uint32_t)settings_get_int(SET_CHART_INTERVAL_S) * 1000U;
}

static void chart_refresh_view(void)
{
    if (!s_chart) return;
    lv_coord_t lo = INT16_MAX, hi = INT16_MIN;
    for (int s = 0; s < SER_COUNT; ++s) {
        for (int i = 0; i < CHART_POINTS; ++i) {
            lv_coord_t v = s_ring[s][i];
            if (v == LV_CHART_POINT_NONE) continue;
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
    }
    if (lo > hi) { lo = 150; hi = 300; }
    lo = (lo / 50 - 1) * 50;
    hi = (hi / 50 + 1) * 50;
    lv_chart_set_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, lo, hi);
    ui_label_format(&s_chart_range[0], "%.0f", (double)hi / 10.0);
    ui_label_format(&s_chart_range[1], "%.0f", (double)lo / 10.0);

    const lv_coord_t c = s_ring[SER_CABIN][CHART_POINTS - 1];
    const lv_coord_t d = s_ring[SER_DUCT][CHART_POINTS - 1];
    const lv_coord_t t = s_ring[SER_SETPOINT][CHART_POINTS - 1];
    char cs[12] = "--", ds[12] = "--", ts[12] = "--";
    if (c != LV_CHART_POINT_NONE) snprintf(cs, sizeof(cs), "%.1f", (double)c / 10.0);
    if (d != LV_CHART_POINT_NONE) snprintf(ds, sizeof(ds), "%.1f", (double)d / 10.0);
    if (t != LV_CHART_POINT_NONE) snprintf(ts, sizeof(ts), "%.1f", (double)t / 10.0);
    ui_label_format(&s_chart_now, "Cabin %s  Duct %s  Set %s", cs, ds, ts);
    lv_chart_refresh(s_chart);
}

static lv_coord_t to_tenths(float c)
{
    return isnan(c) ? LV_CHART_POINT_NONE : (lv_coord_t)lroundf(c * 10.0f);
}

/* LVGL timer: runs in the LVGL task under lvgl_mutex. */
static void chart_feed_timer(lv_timer_t *t)
{
    (void)t;
    hvac_state_t st;
    ui_get_state(&st);
    const lv_coord_t v[SER_COUNT] = {
        to_tenths(st.cabin_temp_c), to_tenths(st.duct_temp_c), to_tenths(st.target_temp_c)
    };
    for (int s = 0; s < SER_COUNT; ++s) {
        memmove(&s_ring[s][0], &s_ring[s][1], (CHART_POINTS - 1) * sizeof(lv_coord_t));
        s_ring[s][CHART_POINTS - 1] = v[s];
    }
    chart_refresh_view();
}

void ui_diag_init(void)
{
    if (!s_ring_init) {
        for (int s = 0; s < SER_COUNT; ++s)
            for (int i = 0; i < CHART_POINTS; ++i) s_ring[s][i] = LV_CHART_POINT_NONE;
        s_ring_init = true;
    }
    s_chart_timer = lv_timer_create(chart_feed_timer, chart_period_ms(), NULL);
}

void ui_diag_apply_settings(void)
{
    if (s_chart_timer && s_chart_timer->period != chart_period_ms()) {
        lv_timer_set_period(s_chart_timer, chart_period_ms());
    }
}

static void chart_open(lv_obj_t *hdr, lv_obj_t *body)
{
    (void)hdr;
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    s_chart = lv_chart_create(body);
    lv_obj_set_pos(s_chart, 36, 4);
    lv_obj_set_size(s_chart, 276, 136);
    lv_obj_set_style_bg_color(s_chart, lv_color_hex(UI_COLOR_SURFACE), 0);
    lv_obj_set_style_border_color(s_chart, lv_color_hex(UI_COLOR_DIVIDER), 0);
    lv_obj_set_style_line_color(s_chart, lv_color_hex(UI_COLOR_DIVIDER), LV_PART_MAIN);
    lv_obj_set_style_size(s_chart, 0, LV_PART_INDICATOR);
    lv_obj_add_flag(s_chart, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_clear_flag(s_chart, LV_OBJ_FLAG_CLICKABLE);
    lv_chart_set_type(s_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(s_chart, CHART_POINTS);
    lv_chart_set_div_line_count(s_chart, 4, 5);

    static const uint32_t colors[SER_COUNT] = { UI_COLOR_TEXT, UI_COLOR_HEAT, UI_COLOR_COOL };
    for (int s = 0; s < SER_COUNT; ++s) {
        s_series[s] = lv_chart_add_series(s_chart, lv_color_hex(colors[s]), LV_CHART_AXIS_PRIMARY_Y);
        lv_chart_set_ext_y_array(s_chart, s_series[s], s_ring[s]);
    }
    (void)ui_make_label(body, &s_chart_range[0], "", 0, 2, 34, 16, UI_FONT_14,
                        UI_COLOR_TEXT_DIM, LV_TEXT_ALIGN_RIGHT);
    (void)ui_make_label(body, &s_chart_range[1], "", 0, 124, 34, 16, UI_FONT_14,
                        UI_COLOR_TEXT_DIM, LV_TEXT_ALIGN_RIGHT);
    (void)ui_make_label(body, &s_chart_now, "", 8, 144, 304, 18, UI_FONT_14,
                        UI_COLOR_TEXT, LV_TEXT_ALIGN_LEFT);
    lv_obj_t *legend = lv_label_create(body);
    lv_label_set_recolor(legend, true);
    /* 60 points x N s = N min */
    lv_label_set_text_fmt(legend, "#F5F7FA Cabin#  #FF9A52 Duct#  #65C8F2 Setpoint#   %d min",
                          (int)settings_get_int(SET_CHART_INTERVAL_S));
    lv_obj_set_pos(legend, 8, 164);
    lv_obj_set_style_text_font(legend, UI_FONT_14, 0);
    lv_obj_set_style_text_color(legend, lv_color_hex(UI_COLOR_TEXT_DIM), 0);
    lv_obj_add_flag(legend, LV_OBJ_FLAG_GESTURE_BUBBLE);
    chart_refresh_view();
}

static void chart_close(void)
{
    s_chart = NULL;     /* the object itself is deleted with the view */
}

const ui_page_t ui_page_chart = { "Trend chart", "Kurve", chart_open, NULL, chart_close };
