/**
 * @file hvac_ui_service.c
 * @brief Service sub-pages: valve service (jog, recalibrate, learn direction)
 *        and 1-Wire sensor role assignment.
 */
#include "hvac_ui_priv.h"
#include "sensor_roles.h"
#include "tb6612fng_driver.h"

#include "hvac_ui_common.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static void set_hidden(lv_obj_t *obj, bool hidden)
{
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN) == hidden) return;
    if (hidden) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

/* ================= valve service ================= */

#define JOG_REPEAT_MS    150U   /* the driver's dead-man stops a jog after 300 ms */
#define VALVE_REFRESH_MS 250U

static const char *const k_valve_names[HVAC_VALVE_MAX] = {
    "Center 1", "Center 2", "Foot", "Windscreen", "Coolant"
};
static const hvac_valve_id_t k_valve_ids[HVAC_VALVE_MAX] = {
    HVAC_VALVE_CENTER_1, HVAC_VALVE_CENTER_2, HVAC_VALVE_FOOT,
    HVAC_VALVE_WINDSCREEN, HVAC_VALVE_COOLANT
};
static const char *const k_cal_src[] = { "DEFAULT", "STORED", "CALIBRATED", "FIXED" };

static int s_valve;
static int s_jog_dir;
static bool s_jog_refused;
static lv_timer_t *s_jog_timer;
static uint32_t s_valve_last;
static ui_label_t s_v_name, s_v_info[3];
static lv_obj_t *s_btn_jog_minus, *s_btn_jog_plus, *s_btn_recal, *s_btn_learn;

static void jog_stop(void)
{
    if (s_jog_timer) lv_timer_pause(s_jog_timer);
    if (s_jog_dir != 0) {
        s_jog_dir = 0;
        (void)tb6612fng_service_stop(k_valve_ids[s_valve]);
    }
}

static bool service_allowed(const hvac_state_t *st, const tb6612_valve_info_t *vi)
{
    return st->mode == HVAC_MODE_OFF && !vi->calibrating && !tb6612fng_is_calibrating();
}

static void jog_send(void)
{
    hvac_state_t st;
    tb6612_valve_info_t vi;
    ui_get_state(&st);
    tb6612fng_get_valve_info(k_valve_ids[s_valve], &vi);
    if (!service_allowed(&st, &vi) ||
        tb6612fng_service_jog(k_valve_ids[s_valve], s_jog_dir) != ESP_OK) {
        s_jog_refused = true;
        jog_stop();
    }
}

static void jog_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (s_jog_dir != 0) jog_send();
}

static void jog_pressed(lv_event_t *e)
{
    if (s_jog_dir != 0) return;
    s_jog_refused = false;
    s_jog_dir = lv_event_get_target(e) == s_btn_jog_plus ? 1 : -1;
    jog_send();
    if (s_jog_dir != 0 && s_jog_timer) lv_timer_resume(s_jog_timer);
}

static void jog_released(lv_event_t *e)
{
    (void)e;
    jog_stop();
}

/* Dragging off the button (a swipe or scroll) must stop the motor as well. */
static void jog_pressing(lv_event_t *e)
{
    if (!ui_press_within_slop(e)) jog_stop();
}

static void add_jog_handlers(lv_obj_t *btn)
{
    lv_obj_add_event_cb(btn, jog_pressed, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(btn, jog_pressing, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(btn, jog_released, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(btn, jog_released, LV_EVENT_PRESS_LOST, NULL);
}

static void stop_tap(lv_event_t *e)
{
    (void)e;
    jog_stop();
    (void)tb6612fng_service_stop(k_valve_ids[s_valve]);
}

static void valve_select(lv_event_t *e)
{
    intptr_t d = (intptr_t)ui_button_user_data(e);
    jog_stop();
    s_jog_refused = false;
    s_valve = (s_valve + (int)d + HVAC_VALVE_MAX) % HVAC_VALVE_MAX;
    s_valve_last -= VALVE_REFRESH_MS;
}

static void service_run(void *data)
{
    bool recal = data != NULL;
    esp_err_t err = recal ? tb6612fng_service_recalibrate(k_valve_ids[s_valve])
                          : tb6612fng_service_learn_direction(k_valve_ids[s_valve]);
    if (err != ESP_OK) hvac_ui_show_error(recal ? "Recalibrate refused" : "Learn direction refused");
}

static void recal_tap(lv_event_t *e)
{
    (void)e;
    char title[40];
    snprintf(title, sizeof(title), "Recalibrate %s?", k_valve_names[s_valve]);
    ui_modal_confirm(title, "The valve drives to both end stops.", "START",
                     UI_COLOR_WARNING, service_run, (void *)1);
}

static void learn_tap(lv_event_t *e)
{
    (void)e;
    service_run(NULL);
}

static void valves_open(lv_obj_t *hdr, lv_obj_t *body)
{
    (void)hdr;
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    s_jog_dir = 0;
    s_jog_refused = false;

    lv_obj_t *prev = ui_make_button(body, LV_SYMBOL_LEFT, 4, 0, 56, 48, UI_FONT_16, valve_select,
                                    UI_COLOR_SURFACE, UI_COLOR_SURFACE_2);
    lv_obj_t *next = ui_make_button(body, LV_SYMBOL_RIGHT, 260, 0, 56, 48, UI_FONT_16, valve_select,
                                    UI_COLOR_SURFACE, UI_COLOR_SURFACE_2);
    ui_button_set_action(prev, valve_select, (void *)(intptr_t)-1);
    ui_button_set_action(next, valve_select, (void *)(intptr_t)1);
    (void)ui_make_label(body, &s_v_name, "", 64, 12, 192, 24, UI_FONT_16, UI_COLOR_ACCENT,
                        LV_TEXT_ALIGN_CENTER);
    for (int i = 0; i < 3; ++i) {
        (void)ui_make_label(body, &s_v_info[i], "", 8, 50 + i * 15, 304, 15, UI_FONT_14,
                            UI_COLOR_TEXT, LV_TEXT_ALIGN_LEFT);
    }

    s_btn_jog_minus = ui_make_button(body, LV_SYMBOL_MINUS, 4, 96, 100, 48, UI_FONT_16, NULL,
                                     UI_COLOR_SURFACE, UI_COLOR_SURFACE_2);
    lv_obj_t *stop = ui_make_button(body, "STOP", 108, 96, 100, 48, UI_FONT_16, stop_tap,
                                    UI_COLOR_FAULT, UI_COLOR_FAULT);
    s_btn_jog_plus = ui_make_button(body, LV_SYMBOL_PLUS, 212, 96, 100, 48, UI_FONT_16, NULL,
                                    UI_COLOR_SURFACE, UI_COLOR_SURFACE_2);
    (void)stop;
    add_jog_handlers(s_btn_jog_minus);
    add_jog_handlers(s_btn_jog_plus);
    s_btn_recal = ui_make_button(body, "RECAL", 4, 144, 152, 48, UI_FONT_16, recal_tap,
                                 UI_COLOR_SURFACE, UI_COLOR_SURFACE_2);
    s_btn_learn = ui_make_button(body, "LEARN DIR", 160, 144, 152, 48, UI_FONT_16, learn_tap,
                                 UI_COLOR_SURFACE, UI_COLOR_SURFACE_2);

    s_jog_timer = lv_timer_create(jog_timer_cb, JOG_REPEAT_MS, NULL);
    lv_timer_pause(s_jog_timer);
    s_valve_last = now_ms() - VALVE_REFRESH_MS;
    ui_page_valves.update();
}

static void valves_update(void)
{
    if (!refresh_due(&s_valve_last, VALVE_REFRESH_MS)) return;
    hvac_state_t st;
    tb6612_valve_info_t vi;
    ui_get_state(&st);
    tb6612fng_get_valve_info(k_valve_ids[s_valve], &vi);

    ui_label_set(&s_v_name, k_valve_names[s_valve]);
    ui_label_format(&s_v_info[0], "Pos %u%%  Target %u%%  ADC %u%%",
                    (unsigned)vi.position, (unsigned)vi.target, (unsigned)vi.adc_pos);
    ui_label_format(&s_v_info[1], "Cal %s  Inverted %s  PWM %u",
                    vi.cal_source < 4 ? k_cal_src[vi.cal_source] : "?",
                    vi.is_inverted ? "yes" : "no", (unsigned)vi.speed);

    const bool fault = vi.stalled || vi.feedback_fault || (st.valve_fault_mask & (1U << s_valve));
    const bool allowed = service_allowed(&st, &vi);
    if (fault) {
        ui_label_format(&s_v_info[2], "FAULT: %s", vi.stalled ? "stalled" :
                        vi.feedback_fault ? "no feedback" : "valve fault");
        ui_label_set_color(&s_v_info[2], UI_COLOR_FAULT);
    } else if (st.mode != HVAC_MODE_OFF) {
        ui_label_set(&s_v_info[2], "Set airflow mode OFF to jog or service");
        ui_label_set_color(&s_v_info[2], UI_COLOR_WARNING);
    } else if (vi.calibrating || (vi.service_active && s_jog_dir == 0)) {
        ui_label_set(&s_v_info[2], "Busy: calibrating / service move");
        ui_label_set_color(&s_v_info[2], UI_COLOR_WARNING);
    } else if (s_jog_refused) {
        ui_label_set(&s_v_info[2], "Jog refused by the valve driver");
        ui_label_set_color(&s_v_info[2], UI_COLOR_WARNING);
    } else {
        ui_label_set(&s_v_info[2], "Hold - or + to jog");
        ui_label_set_color(&s_v_info[2], UI_COLOR_TEXT_DIM);
    }
    ui_set_enabled(s_btn_jog_minus, allowed);
    ui_set_enabled(s_btn_jog_plus, allowed);
    ui_set_enabled(s_btn_recal, allowed);
    ui_set_enabled(s_btn_learn, allowed);
    /* Mode changed away from OFF during a jog: the timer would refuse anyway, stop now. */
    if (!allowed) jog_stop();
}

static void valves_close(void)
{
    jog_stop();
    if (s_jog_timer) {
        lv_timer_del(s_jog_timer);
        s_jog_timer = NULL;
    }
}

const ui_page_t ui_page_valves = { "Valve service", "Ventilservice", valves_open, valves_update, valves_close };

/* ================= sensor roles ================= */

#define SENSOR_REFRESH_MS 500U

typedef struct {
    lv_obj_t *info_obj, *btn;
    ui_label_t info, role;
    uint8_t rom[8];
    sensor_role_t cur;
} sensor_slot_t;

static sensor_slot_t s_slots[SENSOR_ROLES_MAX_ROMS];
static ui_label_t s_search_label, s_sensor_hint;
static lv_obj_t *s_search_btn;
static uint32_t s_sensor_last;

static const char *role_text(sensor_role_t r)
{
    static const char *const en[SENSOR_ROLE_COUNT] = { "Unused", "Duct", "Coolant", "Outside" };
    static const char *const da[SENSOR_ROLE_COUNT] = { "Ubrugt", "Kanal", "K\xC3\xB8lev\xC3\xA6ske", "Ude" };
    if ((unsigned)r >= SENSOR_ROLE_COUNT) return "?";
    return ui_lang_da() ? da[r] : en[r];
}

static void search_tap(lv_event_t *e)
{
    (void)e;
    sensor_roles_request_search();
    s_sensor_last -= SENSOR_REFRESH_MS;
}

static void role_tap(lv_event_t *e)
{
    sensor_slot_t *slot = ui_button_user_data(e);
    if (!slot) return;
    sensor_role_t next = (sensor_role_t)((slot->cur + 1) % SENSOR_ROLE_COUNT);
    if (sensor_roles_assign(slot->rom, next) != ESP_OK) hvac_ui_show_error("Role was not saved");
    s_sensor_last -= SENSOR_REFRESH_MS;
}

static void sensors_open(lv_obj_t *hdr, lv_obj_t *body)
{
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    s_search_btn = ui_make_button(hdr, "SEARCH", 180, 0, 136, UI_HDR_H, UI_FONT_16, search_tap,
                                  UI_COLOR_SURFACE, UI_COLOR_SURFACE_2);
    s_search_label.obj = lv_obj_get_child(s_search_btn, 0);
    s_search_label.text[0] = '\0';
    ui_label_set(&s_search_label, "SEARCH");

    (void)ui_make_label(body, &s_sensor_hint, "", 8, 8, 304, 40, UI_FONT_14, UI_COLOR_TEXT_DIM,
                        LV_TEXT_ALIGN_LEFT);
    lv_label_set_long_mode(s_sensor_hint.obj, LV_LABEL_LONG_WRAP);
    for (size_t i = 0; i < SENSOR_ROLES_MAX_ROMS; ++i) {
        sensor_slot_t *s = &s_slots[i];
        memset(s->rom, 0, sizeof(s->rom));
        s->cur = SENSOR_ROLE_UNUSED;
        s->info_obj = ui_make_label(body, &s->info, "", 8, (lv_coord_t)i * 48 + 14, 190, 20,
                                    UI_FONT_16, UI_COLOR_TEXT, LV_TEXT_ALIGN_LEFT);
        s->btn = ui_make_button(body, "", 204, (lv_coord_t)i * 48 + 2, 108, 44, UI_FONT_16,
                                role_tap, UI_COLOR_SURFACE, UI_COLOR_SURFACE_2);
        ui_button_set_action(s->btn, role_tap, s);
        s->role.obj = lv_obj_get_child(s->btn, 0);
        s->role.text[0] = '\0';
        set_hidden(s->info_obj, true);
        set_hidden(s->btn, true);
    }
    s_sensor_last = now_ms() - SENSOR_REFRESH_MS;
    ui_page_sensors.update();
}

static void sensors_update(void)
{
    if (!refresh_due(&s_sensor_last, SENSOR_REFRESH_MS)) return;
    sensor_rom_info_t found[SENSOR_ROLES_MAX_ROMS];
    size_t n = sensor_roles_get_found(found, SENSOR_ROLES_MAX_ROMS);
    if (n > SENSOR_ROLES_MAX_ROMS) n = SENSOR_ROLES_MAX_ROMS;
    const bool pending = sensor_roles_search_pending();

    ui_label_set(&s_search_label, pending ? "SEARCHING" : "SEARCH");
    ui_set_enabled(s_search_btn, !pending);
    if (n == 0) {
        ui_label_set(&s_sensor_hint, pending ? "Searching the 1-Wire bus..."
                                             : "No sensors known. Tap SEARCH to scan the bus.");
    } else {
        ui_label_set(&s_sensor_hint, "");
    }

    for (size_t i = 0; i < SENSOR_ROLES_MAX_ROMS; ++i) {
        sensor_slot_t *s = &s_slots[i];
        const bool used = i < n;
        set_hidden(s->info_obj, !used);
        set_hidden(s->btn, !used);
        if (!used) continue;
        memcpy(s->rom, found[i].rom, sizeof(s->rom));
        s->cur = found[i].role;
        /* rom[1..4] is the low end of the serial: the part that differs between sensors. */
        if (!found[i].present) {
            ui_label_format(&s->info, "%02X%02X%02X%02X  missing", found[i].rom[1], found[i].rom[2],
                            found[i].rom[3], found[i].rom[4]);
            ui_label_set_color(&s->info, UI_COLOR_WARNING);
        } else if (isnan(found[i].temp_c)) {
            ui_label_format(&s->info, "%02X%02X%02X%02X  --", found[i].rom[1], found[i].rom[2],
                            found[i].rom[3], found[i].rom[4]);
            ui_label_set_color(&s->info, UI_COLOR_TEXT);
        } else {
            ui_label_format(&s->info, "%02X%02X%02X%02X  %.1f\xC2\xB0""C", found[i].rom[1], found[i].rom[2],
                            found[i].rom[3], found[i].rom[4], (double)found[i].temp_c);
            ui_label_set_color(&s->info, UI_COLOR_TEXT);
        }
        ui_label_set(&s->role, role_text(found[i].role));
    }
}

const ui_page_t ui_page_sensors = { "Sensor roles", "Sensorroller", sensors_open, sensors_update, NULL };
