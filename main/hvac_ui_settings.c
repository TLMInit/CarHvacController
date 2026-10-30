/**
 * @file hvac_ui_settings.c
 * @brief Settings page (third swipe page) and the navigation of its sub-pages.
 *
 * Page order: main <-> diagnostics <-> settings. The settings page is one
 * 320x240 container that is cleared and rebuilt whenever the view changes
 * (home list, a settings group, a diagnostics/service sub-page). Rows are built
 * once per view; later only changed values are written (settings_seq()).
 */
#include "hvac_ui_priv.h"
#include "wifi_ap.h"

#include <stdio.h>
#include <string.h>

#define MAX_ROWS        20      /* Valves group: 17 rows; air distribution grid: 20 cells */
#define ROW_H           48
#define INFO_ROW_H      24
#define WIFI_REFRESH_MS 500U
/* Air distribution grid: a column header line, then per mode a name line, a value line and
 * the -/+ buttons of each valve (37 x 40 px). Five modes scroll vertically. */
#define AIR_COL_W       78
#define AIR_HEAD_H      20
#define AIR_BLOCK_H     84

typedef enum {
    V_HOME, V_GROUP, V_DIAG_MENU, V_SERVICE_MENU, V_PAGE, V_AIR
} view_t;

typedef struct { const char *en, *da; } name_t;

static const name_t k_groups[SET_GROUP_COUNT] = {
    {"Display", "Visning"}, {"Climate", "Klima"}, {"Fan", "Bl\xC3\xA6ser"},
    {"Comfort", "Komfort"}, {"Control", "Regulering"}, {"Valves", "Ventiler"},
    {"Connect", "Forbindelse"}, {"System", "System"},
    {"Service", "Service"}, {"Advanced", "Avanceret"}, {"Air distribution", "Luftfordeling"},
};

static const name_t k_air_modes[SETTINGS_AIR_MODES] = {
    {"Face", "Ansigt"}, {"Foot", "Fod"}, {"Face+Foot", "Ansigt+fod"},
    {"Defrost", "Afrim"}, {"Defrost+Foot", "Afrim+fod"},
};
static const name_t k_air_valves[SETTINGS_AIR_VALVES] = {
    {"Center 1", "Midt 1"}, {"Center 2", "Midt 2"}, {"Foot", "Fod"}, {"Screen", "Rude"},
};

typedef struct {
    const char *en, *da;
    const ui_page_t *page;
} menu_item_t;

static const menu_item_t k_diag_menu[] = {
    {"Event log", "H\xC3\xA6ndelseslog", &ui_page_events},
    {"Link health", "Linkstatus", &ui_page_link},
    {"Self-test", "Selvtest", &ui_page_selftest},
    {"Trend chart", "Kurve", &ui_page_chart},
};
static const menu_item_t k_service_menu[] = {
    {"Valve service", "Ventilservice", &ui_page_valves},
    {"Sensor roles", "Sensorroller", &ui_page_sensors},
};

/* Home list entries: >= 0 is a settings group; negative values are the others. */
#define HOME_PRESETS (-1)
#define HOME_DIAG    (-2)
#define HOME_SERVICE (-3)

typedef struct {
    setting_id_t id;
    lv_obj_t *name;
    ui_label_t value;
} row_t;

static lv_obj_t *s_root;
static lv_obj_t *s_hdr;
static lv_obj_t *s_body;
static view_t s_view;
static int s_group;
static const ui_page_t *s_page;
static bool s_lang_da;
static uint32_t s_seq = UINT32_MAX;
static uint32_t s_last_wifi_ms;

static row_t s_rows[MAX_ROWS];
static size_t s_nrows;
static ui_label_t s_wifi_state, s_wifi_ssid;
static bool s_wifi_rows;

/* Editor modal (INT / FLOAT). */
static setting_id_t s_ed_id = SET_COUNT;
static ui_label_t s_ed_value;
static ui_label_t s_ed_note;     /* why a cross-field rule limited the value, else empty */

static const char *tr(const char *en, const char *da)
{
    return s_lang_da ? da : en;
}

static void open_view(view_t view, int group, const ui_page_t *page);

static void go_back(lv_event_t *e)
{
    (void)e;
    switch (s_view) {
    case V_PAGE:
        open_view(s_page == &ui_page_valves || s_page == &ui_page_sensors ? V_SERVICE_MENU : V_DIAG_MENU, 0, NULL);
        break;
    case V_GROUP: case V_DIAG_MENU: case V_SERVICE_MENU:
        open_view(V_HOME, 0, NULL);
        break;
    case V_AIR:
        open_view(V_GROUP, SET_GROUP_VALVES, NULL);
        break;
    default:
        break;
    }
}

static void make_header(const char *title)
{
    s_hdr = lv_obj_create(s_root);
    lv_obj_set_pos(s_hdr, 0, 0);
    lv_obj_set_size(s_hdr, UI_W, UI_HDR_H);
    lv_obj_set_style_bg_opa(s_hdr, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_hdr, 0, 0);
    lv_obj_set_style_pad_all(s_hdr, 0, 0);
    lv_obj_clear_flag(s_hdr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_hdr, LV_OBJ_FLAG_GESTURE_BUBBLE);

    lv_coord_t x = 4;
    if (s_view != V_HOME) {
        (void)ui_make_button(s_hdr, LV_SYMBOL_LEFT, 4, 0, 64, UI_HDR_H, UI_FONT_16,
                             go_back, UI_COLOR_SURFACE, UI_COLOR_SURFACE_2);
        x = 76;
    }
    lv_obj_t *t = lv_label_create(s_hdr);
    lv_label_set_text(t, title);
    lv_label_set_long_mode(t, LV_LABEL_LONG_CLIP);
    lv_obj_set_pos(t, x, 14);
    lv_obj_set_width(t, 150);
    lv_obj_set_style_text_font(t, UI_FONT_16, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_add_flag(t, LV_OBJ_FLAG_GESTURE_BUBBLE);
}

/* ---- row actions ---- */

static void chk(esp_err_t err)
{
    if (err != ESP_OK) hvac_ui_show_error("Setting was not saved");
}

static void action_run(void *data)
{
    chk(settings_set_int((setting_id_t)(intptr_t)data, 1));
}

static void string_reset(void *data)
{
    chk(settings_reset((setting_id_t)(intptr_t)data));
}

static void editor_closed(lv_event_t *e)
{
    (void)e;
    s_ed_id = SET_COUNT;
    s_ed_value.obj = NULL;
    s_ed_note.obj = NULL;
}

static void editor_note(void)
{
    char note[SETTINGS_NOTE_MAX];
    (void)settings_last_note(s_ed_id, note, sizeof(note));
    if (s_ed_note.obj) ui_label_set(&s_ed_note, note);
}

static void editor_step(lv_event_t *e)
{
    intptr_t dir = (intptr_t)ui_button_user_data(e);
    if (s_ed_id < SET_COUNT) {
        chk(settings_step(s_ed_id, (int)dir));
        editor_note();
    }
}

static void editor_default(lv_event_t *e)
{
    (void)e;
    if (s_ed_id < SET_COUNT) {
        chk(settings_reset(s_ed_id));
        editor_note();
    }
}

static void editor_ok(lv_event_t *e)
{
    (void)e;
    ui_modal_close();
}

static void open_editor(setting_id_t id)
{
    lv_obj_t *m = ui_modal_open(UI_COLOR_ACCENT, 312, 184);   /* 148 + two lines for the rule note */
    if (!m) return;
    s_ed_id = id;

    lv_obj_t *title = lv_label_create(m);
    lv_label_set_text(title, settings_label(id));
    lv_label_set_long_mode(title, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(title, 296);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(title, UI_FONT_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(UI_COLOR_TEXT_DIM), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

    (void)ui_make_label(m, &s_ed_value, "", 4, 32, 296, 40, &lv_font_montserrat_28,
                        UI_COLOR_ACCENT, LV_TEXT_ALIGN_CENTER);
    lv_obj_add_event_cb(s_ed_value.obj, editor_closed, LV_EVENT_DELETE, NULL);
    char buf[40];
    settings_format(id, buf, sizeof(buf));
    ui_label_set(&s_ed_value, buf);

    lv_obj_t *minus = ui_make_button(m, LV_SYMBOL_MINUS, 4, 84, 72, 48, UI_FONT_16, NULL,
                                     UI_COLOR_SURFACE, UI_COLOR_SURFACE_2);
    lv_obj_t *plus = ui_make_button(m, LV_SYMBOL_PLUS, 80, 84, 72, 48, UI_FONT_16, NULL,
                                    UI_COLOR_SURFACE, UI_COLOR_SURFACE_2);
    ui_button_set_action(minus, editor_step, (void *)(intptr_t)-1);
    ui_button_set_action(plus, editor_step, (void *)(intptr_t)1);
    ui_button_set_repeat(minus, editor_step);
    ui_button_set_repeat(plus, editor_step);
    (void)ui_make_button(m, tr("DEFAULT", "STANDARD"), 156, 84, 72, 48, UI_FONT_14,
                         editor_default, UI_COLOR_SURFACE, UI_COLOR_SURFACE_2);
    (void)ui_make_button(m, LV_SYMBOL_OK, 232, 84, 72, 48, UI_FONT_16, editor_ok,
                         UI_COLOR_ACCENT, UI_COLOR_ACCENT);
    (void)ui_make_label(m, &s_ed_note, "", 8, 138, 296, 38, UI_FONT_14, UI_COLOR_WARNING, LV_TEXT_ALIGN_CENTER);
    lv_label_set_long_mode(s_ed_note.obj, LV_LABEL_LONG_WRAP);
}

static void row_tap(lv_event_t *e)
{
    setting_id_t id = (setting_id_t)(intptr_t)ui_button_user_data(e);
    const setting_def_t *d = settings_def(id);
    switch (d->type) {
    case SET_TYPE_BOOL:
        chk(settings_set_int(id, settings_get_bool(id) ? 0 : 1));
        break;
    case SET_TYPE_ENUM: {
        int32_t v = settings_get_int(id) + 1;
        chk(settings_set_int(id, v > (int32_t)d->max ? (int32_t)d->min : v));
        break;
    }
    case SET_TYPE_INT:
    case SET_TYPE_FLOAT:
        open_editor(id);
        break;
    case SET_TYPE_STRING:
        ui_modal_confirm(settings_label(id), tr("Reset to the default value?", "Nulstil til standardv\xC3\xA6rdi?"),
                         tr("RESET", "NULSTIL"), UI_COLOR_FAULT, string_reset, (void *)(intptr_t)id);
        break;
    case SET_TYPE_ACTION:
        ui_modal_confirm(settings_label(id),
                         id == SET_FACTORY_RESET
                             ? tr("Erases ALL settings and restarts.", "Sletter ALLE indstillinger og genstarter.")
                             : tr("Run this action now?", "K\xC3\xB8r handlingen nu?"),
                         tr("RUN", "K\xC3\x98R"), UI_COLOR_FAULT, action_run, (void *)(intptr_t)id);
        break;
    }
}

/* ---- views ---- */

typedef struct {
    int group;
    lv_coord_t y;
} build_ctx_t;

static bool build_row(const setting_def_t *d, void *ctx)
{
    build_ctx_t *b = ctx;
    if ((int)d->group != b->group || s_nrows >= MAX_ROWS) return true;

    row_t *r = &s_rows[s_nrows++];
    memset(r, 0, sizeof(*r));
    r->id = d->id;
    lv_obj_t *btn = ui_make_button_blank(s_body, 4, b->y, 312, ROW_H, row_tap,
                                         UI_COLOR_SURFACE, UI_COLOR_SURFACE_2);
    ui_button_set_action(btn, row_tap, (void *)(intptr_t)d->id);
    b->y += ROW_H;

    r->name = lv_label_create(btn);
    lv_label_set_long_mode(r->name, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(r->name, UI_FONT_16, 0);
    lv_obj_set_style_text_color(r->name, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_add_flag(r->name, LV_OBJ_FLAG_GESTURE_BUBBLE);
    if (d->type == SET_TYPE_STRING) {
        /* Long value (the AP password) goes on its own line. */
        lv_obj_set_pos(r->name, 8, 4);
        lv_obj_set_width(r->name, 292);
        (void)ui_make_label(btn, &r->value, "", 8, 26, 292, 18, UI_FONT_14,
                            UI_COLOR_ACCENT, LV_TEXT_ALIGN_LEFT);
    } else {
        lv_obj_set_pos(r->name, 8, 14);
        lv_obj_set_width(r->name, 166);
        (void)ui_make_label(btn, &r->value, "", 178, 14, 122, 20, UI_FONT_16,
                            UI_COLOR_ACCENT, LV_TEXT_ALIGN_RIGHT);
    }
    return true;
}

static void refresh_rows(void)
{
    char buf[40];
    for (size_t i = 0; i < s_nrows; ++i) {
        row_t *r = &s_rows[i];
        const char *name = settings_label(r->id);
        if (r->name && lv_label_get_text(r->name) != name) lv_label_set_text_static(r->name, name);
        if (settings_def(r->id)->type == SET_TYPE_ACTION) {
            ui_label_set(&r->value, LV_SYMBOL_RIGHT);
        } else {
            settings_format(r->id, buf, sizeof(buf));
            ui_label_set(&r->value, buf);
        }
    }
    if (s_ed_id < SET_COUNT && s_ed_value.obj) {
        settings_format(s_ed_id, buf, sizeof(buf));
        ui_label_set(&s_ed_value, buf);
    }
}

static void refresh_wifi(void)
{
    if (!s_wifi_rows) return;
    uint32_t now = (uint32_t)(lv_tick_get());
    if ((uint32_t)(now - s_last_wifi_ms) < WIFI_REFRESH_MS) return;
    s_last_wifi_ms = now;

    if (wifi_ap_is_on()) {
        uint32_t left = wifi_ap_remaining_s();
        if (left) {
            ui_label_format(&s_wifi_state, "AP ON  %d client(s)  %u:%02u left",
                            wifi_ap_client_count(), (unsigned)(left / 60), (unsigned)(left % 60));
        } else {
            ui_label_format(&s_wifi_state, "AP ON  %d client(s)", wifi_ap_client_count());
        }
        ui_label_set_color(&s_wifi_state, UI_COLOR_ACCENT);
    } else {
        ui_label_set(&s_wifi_state, "AP OFF");
        ui_label_set_color(&s_wifi_state, UI_COLOR_TEXT_DIM);
    }
    ui_label_format(&s_wifi_ssid, "SSID %s", wifi_ap_ssid());
}

static void home_tap(lv_event_t *e)
{
    int code = (int)(intptr_t)ui_button_user_data(e);
    if (code == HOME_PRESETS) ui_open_presets();
    else if (code == HOME_DIAG) open_view(V_DIAG_MENU, 0, NULL);
    else if (code == HOME_SERVICE) open_view(V_SERVICE_MENU, 0, NULL);
    else open_view(V_GROUP, code, NULL);
}

static void air_tap(lv_event_t *e)
{
    (void)e;
    open_view(V_AIR, 0, NULL);
}

/* user data = id * 2 + (1 for +, 0 for -) */
static void air_step(lv_event_t *e)
{
    intptr_t d = (intptr_t)ui_button_user_data(e);
    chk(settings_step((setting_id_t)(d / 2), (d & 1) ? 1 : -1));
}

static void air_label(const char *text, lv_coord_t x, lv_coord_t y, lv_coord_t w, uint32_t color,
                      lv_text_align_t align)
{
    lv_obj_t *l = lv_label_create(s_body);
    lv_label_set_text_static(l, text);
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_width(l, w);
    lv_obj_set_style_text_font(l, UI_FONT_14, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(l, align, 0);
    lv_obj_add_flag(l, LV_OBJ_FLAG_GESTURE_BUBBLE);
}

/* 5 x 4 grid: rows = manual modes, columns = air valves. Values refresh on settings_seq(). */
static void build_air_grid(void)
{
    for (int v = 0; v < SETTINGS_AIR_VALVES; ++v) {
        air_label(tr(k_air_valves[v].en, k_air_valves[v].da), 4 + v * AIR_COL_W, 2, AIR_COL_W - 2,
                  UI_COLOR_TEXT_DIM, LV_TEXT_ALIGN_CENTER);
    }
    for (int m = 0; m < SETTINGS_AIR_MODES; ++m) {
        lv_coord_t y = AIR_HEAD_H + m * AIR_BLOCK_H;
        air_label(tr(k_air_modes[m].en, k_air_modes[m].da), 8, y, 300, UI_COLOR_TEXT, LV_TEXT_ALIGN_LEFT);
        for (int v = 0; v < SETTINGS_AIR_VALVES && s_nrows < MAX_ROWS; ++v) {
            setting_id_t id = SETTINGS_AIR_ID(m, v);
            lv_coord_t x = 4 + v * AIR_COL_W;
            row_t *r = &s_rows[s_nrows++];
            memset(r, 0, sizeof(*r));
            r->id = id;
            (void)ui_make_label(s_body, &r->value, "", x, y + 18, AIR_COL_W - 2, 20, UI_FONT_16,
                                UI_COLOR_ACCENT, LV_TEXT_ALIGN_CENTER);
            lv_obj_t *minus = ui_make_button(s_body, LV_SYMBOL_MINUS, x, y + 40, 37, 40, UI_FONT_16, NULL,
                                             UI_COLOR_SURFACE, UI_COLOR_SURFACE_2);
            lv_obj_t *plus = ui_make_button(s_body, LV_SYMBOL_PLUS, x + 39, y + 40, 37, 40, UI_FONT_16, NULL,
                                            UI_COLOR_SURFACE, UI_COLOR_SURFACE_2);
            ui_button_set_action(minus, air_step, (void *)(intptr_t)(id * 2));
            ui_button_set_action(plus, air_step, (void *)(intptr_t)(id * 2 + 1));
            ui_button_set_repeat(minus, air_step);
            ui_button_set_repeat(plus, air_step);
        }
    }
    refresh_rows();
}

static void menu_tap(lv_event_t *e)
{
    open_view(V_PAGE, 0, (const ui_page_t *)ui_button_user_data(e));
}

static void add_nav_row(lv_coord_t y, const char *text, lv_event_cb_t cb, void *data)
{
    lv_obj_t *btn = ui_make_button_blank(s_body, 4, y, 312, ROW_H, cb,
                                         UI_COLOR_SURFACE, UI_COLOR_SURFACE_2);
    ui_button_set_action(btn, cb, data);
    lv_obj_t *l = lv_label_create(btn);
    lv_label_set_text(l, text);
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    lv_obj_set_pos(l, 8, 14);
    lv_obj_set_width(l, 260);
    lv_obj_set_style_text_font(l, UI_FONT_16, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_add_flag(l, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_t *arrow = lv_label_create(btn);
    lv_label_set_text_static(arrow, LV_SYMBOL_RIGHT);
    lv_obj_set_pos(arrow, 284, 14);
    lv_obj_set_style_text_font(arrow, UI_FONT_16, 0);
    lv_obj_set_style_text_color(arrow, lv_color_hex(UI_COLOR_TEXT_DIM), 0);
    lv_obj_add_flag(arrow, LV_OBJ_FLAG_GESTURE_BUBBLE);
}

static void open_view(view_t view, int group, const ui_page_t *page)
{
    ui_modal_close();
    if (s_page && s_page->close) s_page->close();
    s_page = NULL;
    lv_obj_clean(s_root);
    s_hdr = s_body = NULL;
    s_nrows = 0;
    s_wifi_rows = false;
    s_ed_id = SET_COUNT;
    s_view = view;
    s_group = group;
    s_page = page;
    s_lang_da = settings_get_int(SET_LANGUAGE) == SET_LANG_DA;

    const char *title = "";
    switch (view) {
    case V_HOME: title = tr("SETTINGS", "INDSTILLINGER"); break;
    case V_GROUP: title = tr(k_groups[group].en, k_groups[group].da); break;
    case V_DIAG_MENU: title = tr("Diagnostics", "Diagnostik"); break;
    case V_SERVICE_MENU: title = tr("Service", "Service"); break;
    case V_PAGE: title = tr(page->title_en, page->title_da); break;
    case V_AIR: title = tr(k_groups[SET_GROUP_AIR].en, k_groups[SET_GROUP_AIR].da); break;
    }
    make_header(title);
    s_body = ui_make_body(s_root, UI_HDR_H, UI_BODY_H);

    switch (view) {
    case V_HOME: {
        lv_coord_t y = 0;
        add_nav_row(y, tr("Presets", "Forindstillinger"), home_tap, (void *)(intptr_t)HOME_PRESETS);
        y += ROW_H;
        /* SERVICE goes on the valve service page, ADVANCED is on the phone page only,
         * AIR is the grid under Valves */
        for (int g = 0; g < SET_GROUP_SERVICE; ++g, y += ROW_H) {
            add_nav_row(y, tr(k_groups[g].en, k_groups[g].da), home_tap, (void *)(intptr_t)g);
        }
        add_nav_row(y, tr("Diagnostics", "Diagnostik"), home_tap, (void *)(intptr_t)HOME_DIAG);
        y += ROW_H;
        add_nav_row(y, tr("Service", "Service"), home_tap, (void *)(intptr_t)HOME_SERVICE);
        break;
    }
    case V_GROUP: {
        build_ctx_t b = { .group = group, .y = 0 };
        if (group == SET_GROUP_CONNECT) {
            (void)ui_make_label(s_body, &s_wifi_state, "", 8, 3, 304, 18, UI_FONT_14,
                                UI_COLOR_TEXT_DIM, LV_TEXT_ALIGN_LEFT);
            (void)ui_make_label(s_body, &s_wifi_ssid, "", 8, 3 + INFO_ROW_H, 304, 18, UI_FONT_14,
                                UI_COLOR_TEXT, LV_TEXT_ALIGN_LEFT);
            b.y = 2 * INFO_ROW_H;
            s_wifi_rows = true;
            s_last_wifi_ms = lv_tick_get() - WIFI_REFRESH_MS;
            refresh_wifi();
        } else if (group == SET_GROUP_VALVES) {
            add_nav_row(0, tr(k_groups[SET_GROUP_AIR].en, k_groups[SET_GROUP_AIR].da), air_tap, NULL);
            b.y = ROW_H;
        }
        settings_iterate(build_row, &b);
        refresh_rows();
        break;
    }
    case V_DIAG_MENU:
        for (size_t i = 0; i < sizeof(k_diag_menu) / sizeof(k_diag_menu[0]); ++i) {
            add_nav_row((lv_coord_t)i * ROW_H, tr(k_diag_menu[i].en, k_diag_menu[i].da),
                        menu_tap, (void *)k_diag_menu[i].page);
        }
        break;
    case V_SERVICE_MENU:
        for (size_t i = 0; i < sizeof(k_service_menu) / sizeof(k_service_menu[0]); ++i) {
            add_nav_row((lv_coord_t)i * ROW_H, tr(k_service_menu[i].en, k_service_menu[i].da),
                        menu_tap, (void *)k_service_menu[i].page);
        }
        break;
    case V_PAGE:
        if (page->open) page->open(s_hdr, s_body);
        break;
    case V_AIR:
        build_air_grid();
        break;
    }
    s_seq = settings_seq();
}

void ui_settings_init(lv_obj_t *tab)
{
    s_root = lv_obj_create(tab);
    lv_obj_set_pos(s_root, 0, 0);
    lv_obj_set_size(s_root, UI_W, UI_H);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_root, 0, 0);
    lv_obj_set_style_pad_all(s_root, 0, 0);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_GESTURE_BUBBLE);
    open_view(V_HOME, 0, NULL);
}

void ui_settings_reset_view(void)
{
    if (s_view != V_HOME) open_view(V_HOME, 0, NULL);
}

bool ui_settings_at_root(void)
{
    return s_view == V_HOME;
}

void ui_settings_update(void)
{
    bool da = settings_get_int(SET_LANGUAGE) == SET_LANG_DA;
    if (da != s_lang_da) {
        /* Titles are baked into widgets; rebuild the view in the new language. */
        open_view(s_view, s_group, s_page);
        return;
    }
    uint32_t seq = settings_seq();
    if (seq != s_seq) {
        s_seq = seq;
        if (s_view == V_GROUP || s_view == V_AIR) refresh_rows();
    }
    refresh_wifi();
    if (s_page && s_page->update) s_page->update();
}
