/**
 * @file hvac_ui.c
 * @brief OEM-minimal, touch-first HVAC UI for the 320x240 landscape panel.
 *
 * All UI calls are made by the LVGL task while it owns lvgl_mutex. Periodic
 * rendering uses persistent label buffers and lv_label_set_text_static(),
 * avoiding heap allocation in hvac_ui_update().
 */
#include "hvac_ui.h"
#include "hvac_controller.h"
#include "hvac_hw_config.h"
#include "hvac_sensors.h"
#include "remote_io.h"
#include "tb6612fng_driver.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "HVAC_UI";

/* Theme tokens (RGB888; LVGL converts to the configured RGB565 format). */
#define UI_COLOR_BG          0x101419
#define UI_COLOR_SURFACE     0x1D242C
#define UI_COLOR_SURFACE_2   0x252E38
#define UI_COLOR_TEXT        0xF5F7FA
#define UI_COLOR_TEXT_DIM    0xB7C1CC
#define UI_COLOR_DIVIDER     0x37414B
#define UI_COLOR_ACCENT      0x52D3CC
#define UI_COLOR_WARNING     0xFFC857
#define UI_COLOR_FAULT       0xFF5B61
#define UI_COLOR_HEAT        0xFF9A52
#define UI_COLOR_COOL        0x65C8F2

#define UI_DAY_BRIGHTNESS    80U
#define UI_NIGHT_BRIGHTNESS  18U
#define UI_NVS_NAMESPACE     "hvac"
#define UI_NVS_KEY_NIGHT     "night_mode"
#define UI_DIAG_REFRESH_MS   750U
#define UI_STATS_LOG_MS      5000U

#define UI_W                 320
#define UI_H                 240
#define UI_MAIN_TOP_H        48
#define UI_MAIN_STATUS_H     24
#define UI_TEMP_ROW_H        72
#define UI_INFO_ROW_H        48
#define UI_FAN_ROW_H         48

/* Fixed storage is used by label setters so periodic updates don't allocate. */
typedef struct {
    lv_obj_t *obj;
    char text[128];
    uint32_t color;
    bool color_valid;
} ui_label_t;

static lv_obj_t *scr_main;
static lv_obj_t *tabview;
static lv_obj_t *tab_main;
static lv_obj_t *tab_debug;
static lv_obj_t *modal;
static lv_obj_t *msg_box;

static lv_obj_t *btn_mode;
static lv_obj_t *btn_auto;
static lv_obj_t *btn_def;
static lv_obj_t *btn_ac;
static lv_obj_t *btn_recirc;
static lv_obj_t *btn_temp_down;
static lv_obj_t *btn_temp_up;
static lv_obj_t *btn_fan_down;
static lv_obj_t *btn_fan_up;
static lv_obj_t *btn_fan_auto;
static lv_obj_t *btn_night;

static ui_label_t lbl_operating;
static ui_label_t lbl_fault_badge;
static ui_label_t lbl_ac_button;
static ui_label_t lbl_recirc_button;
static ui_label_t lbl_target_temp;
static ui_label_t lbl_cabin_temp;
static ui_label_t lbl_humidity;
static ui_label_t lbl_compressor;
static ui_label_t lbl_fan;
static ui_label_t lbl_night;
static ui_label_t lbl_diag_fault;
static ui_label_t lbl_diag_rows[13];

static bool s_night_mode;
static bool s_night_loaded;
static uint32_t s_last_diag_refresh_ms;
static uint32_t s_last_stats_log_ms;
static uint32_t s_update_calls;
static uint32_t s_update_total_us;
static uint32_t s_update_max_us;

/* hvac_controller_get_state() is void and leaves its output untouched if its
 * state mutex times out. Always seed the destination from this last snapshot
 * so a timeout cannot turn an uninitialised stack struct into UI state. */
static hvac_state_t s_last_good_state;
static bool s_last_good_state_initialized;

static void get_state_cached(hvac_state_t *state)
{
    if (!state) return;
    if (s_last_good_state_initialized) {
        *state = s_last_good_state;
    } else {
        /* Safe display defaults if even the first mutex acquisition times out. */
        memset(state, 0, sizeof(*state));
        state->mode = HVAC_MODE_OFF;
        state->fan_level = HVAC_FAN_AUTO;
        state->temp_unit = TEMP_UNIT_CELSIUS;
        state->target_temp_c = NAN;
        state->cabin_temp_c = NAN;
        state->cabin_humidity = NAN;
        state->duct_temp_c = NAN;
    }
    hvac_controller_get_state(state);
    s_last_good_state = *state;
    s_last_good_state_initialized = true;
}

static const hvac_valve_id_t s_valves[HVAC_VALVE_MAX] = {
    HVAC_VALVE_CENTER_1, HVAC_VALVE_CENTER_2, HVAC_VALVE_FOOT,
    HVAC_VALVE_WINDSCREEN, HVAC_VALVE_COOLANT
};
static const char *s_valve_names[HVAC_VALVE_MAX] = {
    "Center 1", "Center 2", "Foot", "Windscreen", "Coolant"
};

static void label_set(ui_label_t *label, const char *text)
{
    if (!label || !label->obj || !text || strcmp(label->text, text) == 0) {
        return;
    }
    snprintf(label->text, sizeof(label->text), "%s", text);
    lv_label_set_text_static(label->obj, label->text);
}

static void label_format(ui_label_t *label, const char *fmt, ...)
{
    char tmp[sizeof(label->text)];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    label_set(label, tmp);
}

static void label_set_color(ui_label_t *label, uint32_t rgb)
{
    if (label && label->obj && (!label->color_valid || label->color != rgb)) {
        label->color = rgb;
        label->color_valid = true;
        lv_obj_set_style_text_color(label->obj, lv_color_hex(rgb), 0);
    }
}

static lv_obj_t *make_label(lv_obj_t *parent, ui_label_t *store,
                            const char *text, lv_coord_t x, lv_coord_t y,
                            lv_coord_t w, lv_coord_t h, const lv_font_t *font,
                            uint32_t color, lv_text_align_t align)
{
    store->obj = lv_label_create(parent);
    lv_obj_set_pos(store->obj, x, y);
    lv_obj_set_size(store->obj, w, h);
    lv_label_set_long_mode(store->obj, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(store->obj, font, 0);
    lv_obj_set_style_text_color(store->obj, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(store->obj, align, 0);
    lv_obj_set_style_pad_all(store->obj, 0, 0);
    lv_obj_add_flag(store->obj, LV_OBJ_FLAG_GESTURE_BUBBLE);
    store->color = color;
    store->color_valid = true;
    store->text[0] = '\0';
    label_set(store, text);
    return store->obj;
}

static void style_button(lv_obj_t *btn, uint32_t normal, uint32_t checked)
{
    lv_obj_set_style_bg_color(btn, lv_color_hex(normal), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(btn, lv_color_hex(checked), LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(btn, lv_color_hex(UI_COLOR_SURFACE_2), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 1, LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(btn, lv_color_hex(UI_COLOR_DIVIDER), LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(btn, 2, LV_STATE_CHECKED);
    lv_obj_set_style_border_color(btn, lv_color_hex(UI_COLOR_ACCENT), LV_STATE_CHECKED);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_GESTURE_BUBBLE);
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *text,
                             lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h,
                             const lv_font_t *font, lv_event_cb_t cb,
                             uint32_t normal, uint32_t checked)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, h);
    style_button(btn, normal, checked);
    if (cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(lbl, LV_OBJ_FLAG_GESTURE_BUBBLE);
    /* RECIRC must fit its fixed 56 px target while retaining the required 16 px font. */
    if (strncmp(text, "RECIRC", 6) == 0) {
        lv_obj_set_style_text_letter_space(lbl, -2, 0);
    }
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(lbl, w);
    lv_obj_center(lbl);
    return btn;
}

static void set_checked(lv_obj_t *obj, bool checked)
{
    if (!obj) return;
    bool current = lv_obj_has_state(obj, LV_STATE_CHECKED);
    if (current == checked) return;
    if (checked) lv_obj_add_state(obj, LV_STATE_CHECKED);
    else lv_obj_clear_state(obj, LV_STATE_CHECKED);
}

static void cancel_modal_handler(lv_event_t *e);

static void msgbox_delete_handler(lv_event_t *e)
{
    (void)e;
    msg_box = NULL;
}

static void modal_close(void)
{
    if (modal) {
        lv_obj_del(modal);
        modal = NULL;
    }
}

static void enter_debug(void)
{
    if (tabview && lv_tabview_get_tab_act(tabview) != 1) {
        lv_tabview_set_act(tabview, 1, LV_ANIM_OFF);
    }
}

static void enter_main(void)
{
    if (tabview && lv_tabview_get_tab_act(tabview) != 0) {
        lv_tabview_set_act(tabview, 0, LV_ANIM_OFF);
    }
}

static void gesture_handler(lv_event_t *e)
{
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
    if (lv_tabview_get_tab_act(tabview) == 0 && dir == LV_DIR_LEFT) {
        enter_debug();
    } else if (lv_tabview_get_tab_act(tabview) == 1 && dir == LV_DIR_RIGHT) {
        enter_main();
    }
    /* A drag/gesture must not release as a click on the object under the finger. */
    lv_indev_t *indev = lv_indev_get_act();
    if (indev) lv_indev_wait_release(indev);
    (void)e;
}

static void mode_choice_handler(lv_event_t *e)
{
    intptr_t mode = (intptr_t)lv_event_get_user_data(e);
    modal_close();
    if (mode >= 0 && mode < HVAC_MODE_MAX) {
        esp_err_t ret = hvac_controller_set_mode((hvac_mode_t)mode);
        if (ret != ESP_OK) hvac_ui_show_error("Mode change failed");
    }
}

static void cancel_modal_handler(lv_event_t *e)
{
    (void)e;
    modal_close();
}

static void open_mode_picker(lv_event_t *e)
{
    (void)e;
    if (modal) return;
    modal = lv_obj_create(scr_main);
    lv_obj_set_size(modal, UI_W, 140);
    lv_obj_center(modal);
    lv_obj_set_style_bg_color(modal, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_border_color(modal, lv_color_hex(UI_COLOR_ACCENT), 0);
    lv_obj_set_style_border_width(modal, 2, 0);
    lv_obj_set_style_radius(modal, 8, 0);
    lv_obj_set_style_pad_all(modal, 0, 0);
    lv_obj_clear_flag(modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(modal, LV_OBJ_FLAG_GESTURE_BUBBLE);

    lv_obj_t *title = lv_label_create(modal);
    lv_label_set_text(title, "AIRFLOW MODE");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 5);

    static const struct { const char *text; hvac_mode_t mode; } opts[] = {
        {"OFF", HVAC_MODE_OFF}, {"FACE", HVAC_MODE_FACE}, {"FOOT", HVAC_MODE_FOOT},
        {"F+F", HVAC_MODE_FACE_FOOT}, {"DEF", HVAC_MODE_DEFROST},
        {"D+F", HVAC_MODE_DEFROST_FOOT}, {"AUTO", HVAC_MODE_AUTO}
    };
    for (size_t i = 0; i < sizeof(opts) / sizeof(opts[0]); ++i) {
        lv_coord_t x = 4 + (lv_coord_t)(i % 4) * 80;
        lv_coord_t y = 28 + (lv_coord_t)(i / 4) * 56;
        lv_obj_t *btn = make_button(modal, opts[i].text, x, y, 72, 48,
                                    &lv_font_montserrat_16, NULL,
                                    UI_COLOR_SURFACE, UI_COLOR_SURFACE_2);
        lv_obj_add_event_cb(btn, mode_choice_handler, LV_EVENT_CLICKED,
                            (void *)(intptr_t)opts[i].mode);
    }
    /* Cancel occupies the eighth 72x48 cell, preserving the minimum target. */
    (void)make_button(modal, "CANCEL", 244, 84, 72, 48,
                      &lv_font_montserrat_16, cancel_modal_handler,
                      UI_COLOR_SURFACE, UI_COLOR_SURFACE_2);
}

static void mode_long_press_handler(lv_event_t *e)
{
    (void)e;
    enter_debug();
}

static void auto_handler(lv_event_t *e)
{
    (void)e;
    esp_err_t ret = hvac_controller_set_mode(HVAC_MODE_AUTO);
    if (ret != ESP_OK) hvac_ui_show_error("AUTO mode change failed");
}

static void def_handler(lv_event_t *e)
{
    (void)e;
    hvac_controller_quick_defrost();
}

static void ac_handler(lv_event_t *e)
{
    hvac_state_t state;
    (void)e;
    get_state_cached(&state);
    hvac_controller_set_ac(!state.ac_enabled);
}

static void recirc_handler(lv_event_t *e)
{
    hvac_state_t state;
    (void)e;
    get_state_cached(&state);
    hvac_controller_set_recirculate(!state.recirculate);
}

static void temp_down_handler(lv_event_t *e)
{
    (void)e;
    hvac_controller_temp_down();
}

static void temp_up_handler(lv_event_t *e)
{
    (void)e;
    hvac_controller_temp_up();
}

static const char *main_mode_short_name(hvac_mode_t mode)
{
    /* Keep the status line glanceable even for the longest airflow mode. */
    switch (mode) {
    case HVAC_MODE_OFF:          return "OFF";
    case HVAC_MODE_FACE:         return "FACE";
    case HVAC_MODE_FOOT:         return "FOOT";
    case HVAC_MODE_FACE_FOOT:    return "FACE+FOOT";
    case HVAC_MODE_DEFROST:      return "DEF";
    case HVAC_MODE_DEFROST_FOOT: return "D+F";
    case HVAC_MODE_AUTO:         return "AUTO";
    default:                     return "UNKNOWN";
    }
}

static const char *fan_level_short_name(hvac_fan_speed_t level)
{
    /* The central fan button must fit the level and the live blower percentage. */
    switch (level) {
    case HVAC_FAN_OFF:         return "OFF";
    case HVAC_FAN_LOW:         return "LOW";
    case HVAC_FAN_MEDIUM_LOW:  return "M-LOW";
    case HVAC_FAN_MEDIUM:      return "MED";
    case HVAC_FAN_MEDIUM_HIGH: return "M-HI";
    case HVAC_FAN_HIGH:        return "HIGH";
    case HVAC_FAN_AUTO:        return "AUTO";
    default:                   return "?";
    }
}

static int fan_step(hvac_fan_speed_t level, int step)
{
    int current = (int)level;
    /* AUTO is outside the manual Off..High sequence: +/- exits AUTO at an
     * unambiguous adjacent manual endpoint (Off for minus, Low for plus). */
    if (current == HVAC_FAN_AUTO) return step < 0 ? HVAC_FAN_OFF : HVAC_FAN_LOW;
    current += step;
    if (current < HVAC_FAN_OFF) current = HVAC_FAN_OFF;
    if (current > HVAC_FAN_HIGH) current = HVAC_FAN_HIGH;
    return current;
}

static void fan_down_handler(lv_event_t *e)
{
    hvac_state_t state;
    (void)e;
    get_state_cached(&state);
    (void)hvac_controller_set_fan_level((hvac_fan_speed_t)fan_step(state.fan_level, -1));
}

static void fan_up_handler(lv_event_t *e)
{
    hvac_state_t state;
    (void)e;
    get_state_cached(&state);
    (void)hvac_controller_set_fan_level((hvac_fan_speed_t)fan_step(state.fan_level, +1));
}

static void fan_auto_handler(lv_event_t *e)
{
    (void)e;
    (void)hvac_controller_set_fan_level(HVAC_FAN_AUTO);
}

static void add_repeat_handler(lv_obj_t *btn, lv_event_cb_t cb)
{
    lv_obj_add_event_cb(btn, cb, LV_EVENT_LONG_PRESSED_REPEAT, NULL);
}

static bool persist_night_mode(bool night)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(UI_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS open for brightness: %s", esp_err_to_name(err));
        return false;
    }
    err = nvs_set_u8(h, UI_NVS_KEY_NIGHT, night ? 1 : 0);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) ESP_LOGW(TAG, "NVS save brightness mode: %s", esp_err_to_name(err));
    return err == ESP_OK;
}

static void load_night_mode(void)
{
    nvs_handle_t h;
    uint8_t stored = 0;
    s_night_mode = false;
    if (nvs_open(UI_NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_u8(h, UI_NVS_KEY_NIGHT, &stored) == ESP_OK) s_night_mode = stored != 0;
        nvs_close(h);
    }
    s_night_loaded = true;
}

static void night_handler(lv_event_t *e)
{
    (void)e;
    bool new_night = !s_night_mode;
    if (!persist_night_mode(new_night)) {
        hvac_ui_show_error("Brightness setting was not saved");
        return;
    }
    s_night_mode = new_night;
    label_set(&lbl_night, s_night_mode ? "NIGHT" : "DAY");
    hvac_ui_set_brightness(s_night_mode ? UI_NIGHT_BRIGHTNESS : UI_DAY_BRIGHTNESS);
}

static void reset_cal_execute(lv_event_t *e)
{
    (void)e;
    modal_close();
    esp_err_t ret = hvac_controller_reset_thermal_cal();
    if (ret == ESP_OK) hvac_ui_show_info("Thermal calibration reset");
    else hvac_ui_show_error("Failed to reset thermal calibration");
}

static void reset_cal_confirm_handler(lv_event_t *e)
{
    (void)e;
    if (modal) return;
    modal = lv_obj_create(scr_main);
    lv_obj_set_size(modal, 304, 148);
    lv_obj_center(modal);
    lv_obj_set_style_bg_color(modal, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_border_color(modal, lv_color_hex(UI_COLOR_WARNING), 0);
    lv_obj_set_style_border_width(modal, 2, 0);
    lv_obj_set_style_radius(modal, 8, 0);
    lv_obj_set_style_pad_all(modal, 0, 0);
    lv_obj_clear_flag(modal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(modal);
    lv_label_set_text(title, "RESET THERMAL CAL?");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);
    lv_obj_t *desc = lv_label_create(modal);
    lv_label_set_text(desc, "Clears the learned model");
    lv_obj_set_style_text_font(desc, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(desc, lv_color_hex(UI_COLOR_TEXT_DIM), 0);
    lv_obj_align(desc, LV_ALIGN_TOP_MID, 0, 38);
    lv_obj_t *keep = make_button(modal, "KEEP MODEL", 4, 84, 144, 48,
                                 &lv_font_montserrat_16, cancel_modal_handler,
                                 UI_COLOR_SURFACE, UI_COLOR_SURFACE_2);
    (void)keep;
    lv_obj_t *reset = make_button(modal, "RESET CAL", 156, 84, 144, 48,
                                  &lv_font_montserrat_16, reset_cal_execute,
                                  UI_COLOR_FAULT, UI_COLOR_FAULT);
    (void)reset;
}

static void onewire_read_handler(lv_event_t *e)
{
    float temp_c = NAN;
    (void)e;
    esp_err_t ret = hvac_sensors_debug_read_onewire(&temp_c);
    if (ret == ESP_OK && !isnan(temp_c)) {
        char msg[64];
        snprintf(msg, sizeof(msg), "1-Wire duct: %.2f C", temp_c);
        hvac_ui_show_info(msg);
    } else {
        hvac_ui_show_error("1-Wire read failed");
    }
}

static void save_cal_handler(lv_event_t *e)
{
    (void)e;
    esp_err_t ret = hvac_controller_save_thermal_cal();
    if (ret == ESP_OK) hvac_ui_show_info("Thermal calibration saved");
    else hvac_ui_show_error("Failed to save thermal calibration");
}

static void make_main_screen(void)
{
    static const struct { const char *text; lv_event_cb_t handler; } top[] = {
        {"MODE", NULL}, {"AUTO", auto_handler}, {"DEF", def_handler},
        {"A/C\nOFF", ac_handler}, {"RECIRC\nOFF", recirc_handler}
    };
    for (size_t i = 0; i < sizeof(top) / sizeof(top[0]); ++i) {
        /* MODE tap is SHORT_CLICKED-only; a long press goes to diagnostics. */
        lv_event_cb_t tap_handler = top[i].handler;
        lv_obj_t *btn = make_button(tab_main, top[i].text, 4 + (lv_coord_t)i * 64, 24,
                                    56, UI_MAIN_TOP_H, &lv_font_montserrat_16,
                                    tap_handler, UI_COLOR_SURFACE, UI_COLOR_SURFACE_2);
        if (i == 0) {
            btn_mode = btn;
            lv_obj_add_event_cb(btn_mode, open_mode_picker, LV_EVENT_SHORT_CLICKED, NULL);
            lv_obj_add_event_cb(btn_mode, mode_long_press_handler, LV_EVENT_LONG_PRESSED, NULL);
        } else if (i == 1) btn_auto = btn;
        else if (i == 2) btn_def = btn;
        else if (i == 3) btn_ac = btn;
        else btn_recirc = btn;
    }

    /* Explicit ON/OFF text ensures toggle state is not conveyed by colour alone. */
    lbl_ac_button.obj = lv_obj_get_child(btn_ac, 0);
    lbl_ac_button.text[0] = '\0';
    label_set(&lbl_ac_button, "A/C\nOFF");
    lbl_recirc_button.obj = lv_obj_get_child(btn_recirc, 0);
    lbl_recirc_button.text[0] = '\0';
    label_set(&lbl_recirc_button, "RECIRC\nOFF");

    make_label(tab_main, &lbl_operating, "OFF | READY", 4, 0, 196, UI_MAIN_STATUS_H,
               &lv_font_montserrat_16, UI_COLOR_TEXT, LV_TEXT_ALIGN_LEFT);
    make_label(tab_main, &lbl_fault_badge, "", 204, 0, 112, UI_MAIN_STATUS_H,
               &lv_font_montserrat_16, UI_COLOR_FAULT, LV_TEXT_ALIGN_RIGHT);

    btn_temp_down = make_button(tab_main, LV_SYMBOL_MINUS, 4, 72, 64, UI_TEMP_ROW_H,
                                &lv_font_montserrat_22, NULL,
                                UI_COLOR_SURFACE, UI_COLOR_SURFACE_2);
    btn_temp_up = make_button(tab_main, LV_SYMBOL_PLUS, 252, 72, 64, UI_TEMP_ROW_H,
                              &lv_font_montserrat_22, NULL,
                              UI_COLOR_SURFACE, UI_COLOR_SURFACE_2);
    lv_obj_add_event_cb(btn_temp_down, temp_down_handler, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_add_event_cb(btn_temp_up, temp_up_handler, LV_EVENT_SHORT_CLICKED, NULL);
    add_repeat_handler(btn_temp_down, temp_down_handler);
    add_repeat_handler(btn_temp_up, temp_up_handler);
    make_label(tab_main, &lbl_target_temp, "22.0°C", 76, 80, 168, 56,
               &lv_font_montserrat_44, UI_COLOR_TEXT, LV_TEXT_ALIGN_CENTER);

    make_label(tab_main, &lbl_cabin_temp, "CABIN --.-°C", 4, 144, 140, UI_INFO_ROW_H,
               &lv_font_montserrat_16, UI_COLOR_TEXT, LV_TEXT_ALIGN_LEFT);
    make_label(tab_main, &lbl_humidity, "RH --%", 144, 144, 72, UI_INFO_ROW_H,
               &lv_font_montserrat_16, UI_COLOR_TEXT_DIM, LV_TEXT_ALIGN_CENTER);
    make_label(tab_main, &lbl_compressor, "COMP OFF", 216, 144, 100, UI_INFO_ROW_H,
               &lv_font_montserrat_16, UI_COLOR_TEXT, LV_TEXT_ALIGN_RIGHT);

    btn_fan_down = make_button(tab_main, LV_SYMBOL_MINUS, 4, 192, 56, UI_FAN_ROW_H,
                               &lv_font_montserrat_22, NULL,
                               UI_COLOR_SURFACE, UI_COLOR_SURFACE_2);
    btn_fan_up = make_button(tab_main, LV_SYMBOL_PLUS, 260, 192, 56, UI_FAN_ROW_H,
                             &lv_font_montserrat_22, NULL,
                             UI_COLOR_SURFACE, UI_COLOR_SURFACE_2);
    lv_obj_add_event_cb(btn_fan_down, fan_down_handler, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_add_event_cb(btn_fan_up, fan_up_handler, LV_EVENT_SHORT_CLICKED, NULL);
    add_repeat_handler(btn_fan_down, fan_down_handler);
    add_repeat_handler(btn_fan_up, fan_up_handler);
    btn_fan_auto = make_button(tab_main, "FAN AUTO | 0%", 68, 192, 184, UI_FAN_ROW_H,
                               &lv_font_montserrat_16, fan_auto_handler,
                               UI_COLOR_SURFACE, UI_COLOR_SURFACE_2);
    /* Bind the label's retained buffer so periodic updates never allocate text. */
    lbl_fan.obj = lv_obj_get_child(btn_fan_auto, 0);
    lbl_fan.text[0] = '\0';
    label_set(&lbl_fan, "FAN AUTO | 0%");
    /* The centre fan label doubles as the one-tap AUTO selector. */

    /* Main-screen gestures are bubbled to the tabview; screen changes are instant. */
    lv_obj_add_flag(tab_main, LV_OBJ_FLAG_GESTURE_BUBBLE);
}

static void make_debug_screen(void)
{
    lv_obj_t *diag_title = lv_label_create(tab_debug);
    lv_label_set_text_static(diag_title, "DIAGNOSTICS");
    lv_obj_set_pos(diag_title, 4, 0);
    lv_obj_set_size(diag_title, 208, 48);
    lv_obj_set_style_text_font(diag_title, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(diag_title, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_add_flag(diag_title, LV_OBJ_FLAG_GESTURE_BUBBLE);
    btn_night = make_button(tab_debug, "DAY", 216, 0, 100, 48,
                            &lv_font_montserrat_16, night_handler,
                            UI_COLOR_SURFACE, UI_COLOR_SURFACE_2);
    lbl_night.obj = lv_obj_get_child(btn_night, 0);
    lbl_night.text[0] = '\0';
    label_set(&lbl_night, "DAY");

    make_label(tab_debug, &lbl_diag_fault, "NO ACTIVE FAULT", 4, 48, 312, 36,
               &lv_font_montserrat_14, UI_COLOR_TEXT_DIM, LV_TEXT_ALIGN_LEFT);
    lv_label_set_long_mode(lbl_diag_fault.obj, LV_LABEL_LONG_WRAP);

    lv_obj_t *body = lv_obj_create(tab_debug);
    lv_obj_set_pos(body, 4, 84);
    lv_obj_set_size(body, 312, 108);
    lv_obj_set_style_bg_color(body, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_pad_all(body, 0, 0);
    lv_obj_set_style_pad_row(body, 0, 0);
    lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_AUTO);

    static const char *initial[13] = {
        "CABIN --.-C  RH --%  DUCT --.-C",
        "MODE OFF  FAN AUTO  BLOWER --%",
        "A/C REQ OFF  COMP OFF  RECIRC OFF",
        "Center 1     ACT --%  TARGET --%",
        "Center 2     ACT --%  TARGET --%",
        "Foot          ACT --%  TARGET --%",
        "Windscreen    ACT --%  TARGET --%",
        "Coolant       ACT --%  TARGET --%",
        "HEAT OFF  COOL OFF  REACHED OFF",
        "THERMAL: --",
        "UPTIME --  HEAP --KB",
        "REMOTE TX -- RX -- CRC -- TO --",
        "LINK --"
    };
    for (size_t i = 0; i < 13; ++i) {
        make_label(body, &lbl_diag_rows[i], initial[i], 0, (lv_coord_t)i * 18,
                   308, 18, &lv_font_montserrat_14, UI_COLOR_TEXT, LV_TEXT_ALIGN_LEFT);
    }

    (void)make_button(tab_debug, "READ 1W", 8, 192, 96, 48,
                      &lv_font_montserrat_14, onewire_read_handler,
                      UI_COLOR_SURFACE, UI_COLOR_SURFACE_2);
    (void)make_button(tab_debug, "RESET CAL", 112, 192, 96, 48,
                      &lv_font_montserrat_14, reset_cal_confirm_handler,
                      UI_COLOR_WARNING, UI_COLOR_WARNING);
    (void)make_button(tab_debug, "SAVE CAL", 216, 192, 96, 48,
                      &lv_font_montserrat_14, save_cal_handler,
                      UI_COLOR_SURFACE, UI_COLOR_SURFACE_2);
    lv_obj_add_flag(tab_debug, LV_OBJ_FLAG_GESTURE_BUBBLE);
}

static const char *fault_short_text(const hvac_state_t *s, bool link_ok)
{
    if (!link_ok) return "LINK LOST";
    if (s->sensor_fault && (s->valve_fault || s->valve_fault_mask)) return "FAULT";
    if (s->sensor_fault) return "SENSOR";
    if (s->valve_fault || s->valve_fault_mask) return "VALVE";
    return "";
}

static void fault_append(char *dst, size_t capacity, size_t *used, const char *fmt, ...)
{
    if (*used >= capacity) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(dst + *used, capacity - *used, fmt, ap);
    va_end(ap);
    if (n > 0) {
        size_t remaining = capacity - *used;
        *used += (size_t)n < remaining ? (size_t)n : remaining - 1;
    }
}

static void update_fault_diagnostics(const hvac_state_t *state, bool link_ok)
{
    char line[128] = {0};
    size_t used = 0;
    bool fault = false;

    if (!link_ok) {
        fault_append(line, sizeof(line), &used, "LINK LOST");
        fault = true;
    }
    if (state->sensor_fault) {
        fault_append(line, sizeof(line), &used, "%sSensor fault", used ? "; " : "");
        fault = true;
    }
    if (state->valve_fault || state->valve_fault_mask) {
        fault_append(line, sizeof(line), &used, "%sValve fault:", used ? "; " : "");
        fault = true;
        bool any_valve = false;
        for (int i = 0; i < HVAC_VALVE_MAX; ++i) {
            if (state->valve_fault_mask & (1U << i)) {
                fault_append(line, sizeof(line), &used, "%s%s", any_valve ? "," : " ",
                             s_valve_names[i]);
                any_valve = true;
            }
        }
        if (!any_valve) fault_append(line, sizeof(line), &used, " detected");
    }

    label_set(&lbl_diag_fault, fault ? line : "NO ACTIVE FAULT");
    label_set_color(&lbl_diag_fault, fault ? UI_COLOR_FAULT : UI_COLOR_TEXT_DIM);
}

static void update_diagnostics(const hvac_state_t *state, bool link_ok)
{
    char buf[128];
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000U);
    if ((uint32_t)(now_ms - s_last_diag_refresh_ms) < UI_DIAG_REFRESH_MS) return;
    s_last_diag_refresh_ms = now_ms;

    const float cabin = state->temp_unit == TEMP_UNIT_CELSIUS
                            ? state->cabin_temp_c : hvac_controller_c_to_f(state->cabin_temp_c);
    const float duct = state->temp_unit == TEMP_UNIT_CELSIUS
                           ? state->duct_temp_c : hvac_controller_c_to_f(state->duct_temp_c);
    const char *unit = state->temp_unit == TEMP_UNIT_CELSIUS ? "C" : "F";
    char cabin_text[20], humidity_text[12], duct_text[20];
    if (isnan(cabin)) snprintf(cabin_text, sizeof(cabin_text), "--.-");
    else snprintf(cabin_text, sizeof(cabin_text), "%.1f%s", cabin, unit);
    if (isnan(state->cabin_humidity)) snprintf(humidity_text, sizeof(humidity_text), "--");
    else snprintf(humidity_text, sizeof(humidity_text), "%.0f%%", state->cabin_humidity);
    if (isnan(duct)) snprintf(duct_text, sizeof(duct_text), "--.-");
    else snprintf(duct_text, sizeof(duct_text), "%.1f%s", duct, unit);
    snprintf(buf, sizeof(buf), "CABIN %s  RH %s  DUCT %s",
             cabin_text, humidity_text, duct_text);
    label_set(&lbl_diag_rows[0], buf);
    label_format(&lbl_diag_rows[1], "MODE %s  FAN %s  BLOWER %u%%",
                 hvac_controller_get_mode_name(state->mode),
                 hvac_controller_get_fan_level_name(state->fan_level),
                 (unsigned)state->blower_speed);
    label_format(&lbl_diag_rows[2], "A/C REQ %s  COMP %s  RECIRC %s",
                 state->ac_enabled ? "ON" : "OFF",
                 state->ac_compressor_on ? "ON" : "OFF",
                 state->recirculate ? "ON" : "OFF");

    const uint8_t actuals[HVAC_VALVE_MAX] = {
        state->center_valve_pos, state->center2_valve_pos, state->foot_valve_pos,
        state->windscreen_valve_pos, state->coolant_valve_pos
    };
    for (int i = 0; i < HVAC_VALVE_MAX; ++i) {
        uint8_t actual = actuals[i];
        uint8_t target = tb6612fng_get_valve_target(s_valves[i]);
        label_format(&lbl_diag_rows[3 + i], "%-11s ACT %3u%% TARGET %3u%%",
                     s_valve_names[i], (unsigned)actual, (unsigned)target);
        if (state->valve_fault_mask & (1U << i)) {
            label_set_color(&lbl_diag_rows[3 + i], UI_COLOR_FAULT);
        } else {
            label_set_color(&lbl_diag_rows[3 + i], UI_COLOR_TEXT);
        }
    }

    float heating_rate = 0.0f, cooling_rate = 0.0f, adaptive_gain = 0.0f;
    uint32_t sample_count = 0;
    bool calibrated = false;
    (void)hvac_controller_get_thermal_status(&heating_rate, &cooling_rate,
                                              &adaptive_gain, &sample_count, &calibrated);
    label_format(&lbl_diag_rows[8], "HEAT %s  COOL %s  REACHED %s",
                 state->is_heating ? "ON" : "OFF", state->is_cooling ? "ON" : "OFF",
                 state->temp_reached ? "ON" : "OFF");
    label_format(&lbl_diag_rows[9], "TH H %.2f C %.2f G %.1f N %lu %s",
                 heating_rate, cooling_rate, adaptive_gain, (unsigned long)sample_count,
                 calibrated ? "CAL" : "LEARN");
    uint32_t uptime_sec = xTaskGetTickCount() * portTICK_PERIOD_MS / 1000U;
    label_format(&lbl_diag_rows[10], "UP %luh%lum  HEAP %luKB",
                 (unsigned long)(uptime_sec / 3600U),
                 (unsigned long)((uptime_sec % 3600U) / 60U),
                 (unsigned long)(esp_get_free_heap_size() / 1024U));
#if USE_REMOTE_IO
    uint32_t tx = 0, rx = 0, crc = 0, timeout = 0;
    remote_io_get_stats(&tx, &rx, &crc, &timeout);
    label_format(&lbl_diag_rows[11], "REMOTE TX %lu RX %lu CRC %lu TO %lu",
                 (unsigned long)tx, (unsigned long)rx,
                 (unsigned long)crc, (unsigned long)timeout);
    label_set(&lbl_diag_rows[12], link_ok ? "LINK CONNECTED" : "LINK LOST");
    label_set_color(&lbl_diag_rows[12], link_ok ? UI_COLOR_ACCENT : UI_COLOR_FAULT);
#else
    label_set(&lbl_diag_rows[11], "REMOTE IO DISABLED");
    label_set(&lbl_diag_rows[12], "LOCAL IO");
#endif
}

esp_err_t hvac_ui_init(void)
{
    ESP_LOGI(TAG, "Initializing OEM-minimal HVAC UI");
    load_night_mode();
    hvac_state_t initial_state;
    get_state_cached(&initial_state);

    scr_main = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_main, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_border_width(scr_main, 0, 0);
    lv_obj_set_style_pad_all(scr_main, 0, 0);
    lv_obj_clear_flag(scr_main, LV_OBJ_FLAG_SCROLLABLE);

    /* Hidden tab bar; all programmatic screen changes explicitly disable animation. */
    tabview = lv_tabview_create(scr_main, LV_DIR_BOTTOM, 0);
    lv_obj_set_pos(tabview, 0, 0);
    lv_obj_set_size(tabview, UI_W, UI_H);
    lv_obj_set_style_bg_color(tabview, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_border_width(tabview, 0, 0);
    lv_obj_set_style_pad_all(tabview, 0, 0);
    lv_obj_set_scrollbar_mode(tabview, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(tabview, LV_OBJ_FLAG_SCROLLABLE);
    tab_main = lv_tabview_add_tab(tabview, "Main");
    tab_debug = lv_tabview_add_tab(tabview, "Diagnostics");
    for (size_t i = 0; i < 2; ++i) {
        lv_obj_t *tab = i == 0 ? tab_main : tab_debug;
        lv_obj_set_style_bg_color(tab, lv_color_hex(UI_COLOR_BG), 0);
        lv_obj_set_style_border_width(tab, 0, 0);
        lv_obj_set_style_pad_all(tab, 0, 0);
        lv_obj_clear_flag(tab, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(tab, LV_OBJ_FLAG_GESTURE_BUBBLE);
    }
    /* The tabview's content object owns horizontal scrolling/snapping in LVGL 8.4.
     * Clear its scrollable flag after tab creation so swipes reach gesture_handler. */
    lv_obj_clear_flag(lv_tabview_get_content(tabview), LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(tabview, gesture_handler, LV_EVENT_GESTURE, NULL);

    make_main_screen();
    make_debug_screen();
    label_set(&lbl_night, s_night_mode ? "NIGHT" : "DAY");

    lv_tabview_set_act(tabview, 0, LV_ANIM_OFF);
    lv_scr_load(scr_main);

    lv_mem_monitor_t mem;
    lv_mem_monitor(&mem);
    ESP_LOGI(TAG, "LVGL heap after UI init: total=%lu free=%lu used=%u%% frag=%u%% largest=%lu",
             (unsigned long)mem.total_size, (unsigned long)mem.free_size,
             (unsigned)mem.used_pct, (unsigned)mem.frag_pct,
             (unsigned long)mem.free_biggest_size);
    ESP_LOGI(TAG, "UI ready; Montserrat 44 setpoint font enabled; night=%s",
             s_night_mode ? "yes" : "no");
    return ESP_OK;
}

void hvac_ui_update(void)
{
    const uint64_t started_us = esp_timer_get_time();
    hvac_state_t state;
    get_state_cached(&state);
#if USE_REMOTE_IO
    const bool link_ok = remote_io_is_connected();
#else
    const bool link_ok = true;
#endif

    if (!isnan(state.target_temp_c)) {
        float target = state.temp_unit == TEMP_UNIT_CELSIUS
                           ? state.target_temp_c : hvac_controller_c_to_f(state.target_temp_c);
        label_format(&lbl_target_temp, "%.1f°%c", target,
                     state.temp_unit == TEMP_UNIT_CELSIUS ? 'C' : 'F');
    } else {
        label_set(&lbl_target_temp, "--.-°");
    }
    if (state.is_heating) label_set_color(&lbl_target_temp, UI_COLOR_HEAT);
    else if (state.is_cooling) label_set_color(&lbl_target_temp, UI_COLOR_COOL);
    else label_set_color(&lbl_target_temp, UI_COLOR_TEXT);

    const float cabin = state.temp_unit == TEMP_UNIT_CELSIUS
                            ? state.cabin_temp_c : hvac_controller_c_to_f(state.cabin_temp_c);
    if (isnan(cabin)) label_set(&lbl_cabin_temp, "CABIN --.-°");
    else label_format(&lbl_cabin_temp, "CABIN %.1f°%c", cabin,
                      state.temp_unit == TEMP_UNIT_CELSIUS ? 'C' : 'F');
    if (isnan(state.cabin_humidity)) label_set(&lbl_humidity, "RH --%");
    else label_format(&lbl_humidity, "RH %.0f%%", state.cabin_humidity);
    label_format(&lbl_compressor, "COMP %s", state.ac_compressor_on ? "ON" : "OFF");
    label_set_color(&lbl_compressor, state.ac_compressor_on ? UI_COLOR_ACCENT : UI_COLOR_TEXT);

    const char *thermal = state.is_heating ? "HEATING" : state.is_cooling ? "COOLING" :
                          state.temp_reached ? "REACHED" : "READY";
    label_format(&lbl_operating, "%s | %s", main_mode_short_name(state.mode), thermal);
    const char *badge = fault_short_text(&state, link_ok);
    label_set(&lbl_fault_badge, badge);
    label_set_color(&lbl_fault_badge, badge[0] ? UI_COLOR_FAULT : UI_COLOR_TEXT_DIM);

    set_checked(btn_auto, state.mode == HVAC_MODE_AUTO);
    set_checked(btn_def, state.mode == HVAC_MODE_DEFROST || state.mode == HVAC_MODE_DEFROST_FOOT);
    set_checked(btn_ac, state.ac_enabled);
    set_checked(btn_recirc, state.recirculate);
    label_set(&lbl_ac_button, state.ac_enabled ? "A/C\nON" : "A/C\nOFF");
    label_set(&lbl_recirc_button, state.recirculate ? "RECIRC\nON" : "RECIRC\nOFF");

    label_format(&lbl_fan, "FAN %s | %u%%", fan_level_short_name(state.fan_level),
                 (unsigned)state.blower_speed);
    set_checked(btn_fan_auto, state.fan_level == HVAC_FAN_AUTO);

    update_fault_diagnostics(&state, link_ok);
    if (lv_tabview_get_tab_act(tabview) == 1) update_diagnostics(&state, link_ok);

    uint64_t elapsed = esp_timer_get_time() - started_us;
    s_update_calls++;
    s_update_total_us += (uint32_t)(elapsed > UINT32_MAX ? UINT32_MAX : elapsed);
    if (elapsed > s_update_max_us) s_update_max_us = (uint32_t)elapsed;
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000U);
    if ((uint32_t)(now_ms - s_last_stats_log_ms) >= UI_STATS_LOG_MS) {
        if (s_update_calls) {
            ESP_LOGI(TAG, "hvac_ui_update avg=%lu us max=%lu us over %lu calls",
                     (unsigned long)(s_update_total_us / s_update_calls),
                     (unsigned long)s_update_max_us, (unsigned long)s_update_calls);
        }
        s_last_stats_log_ms = now_ms;
        s_update_calls = 0;
        s_update_total_us = 0;
        s_update_max_us = 0;
    }
}

lv_obj_t *hvac_ui_get_screen(void)
{
    return scr_main;
}

void hvac_ui_set_brightness(uint8_t brightness)
{
    if (brightness > 100) brightness = 100;
    hvac_hw_backlight_set(brightness);
}

void hvac_ui_apply_saved_brightness(void)
{
    if (!s_night_loaded) load_night_mode();
    hvac_ui_set_brightness(s_night_mode ? UI_NIGHT_BRIGHTNESS : UI_DAY_BRIGHTNESS);
}

void hvac_ui_show_error(const char *message)
{
    if (!message) return;
    if (msg_box) {
        lv_msgbox_close(msg_box);
        msg_box = NULL;
    }
    msg_box = lv_msgbox_create(NULL, "Error", message, NULL, true);
    lv_obj_set_style_bg_color(msg_box, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_text_color(msg_box, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(msg_box, &lv_font_montserrat_16, 0);
    lv_obj_center(msg_box);
    lv_obj_add_event_cb(msg_box, msgbox_delete_handler, LV_EVENT_DELETE, NULL);
}

void hvac_ui_show_info(const char *message)
{
    if (!message) return;
    if (msg_box) {
        lv_msgbox_close(msg_box);
        msg_box = NULL;
    }
    msg_box = lv_msgbox_create(NULL, "Info", message, NULL, true);
    lv_obj_set_style_bg_color(msg_box, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_text_color(msg_box, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(msg_box, &lv_font_montserrat_16, 0);
    lv_obj_center(msg_box);
    lv_obj_add_event_cb(msg_box, msgbox_delete_handler, LV_EVENT_DELETE, NULL);
}

void hvac_ui_hide_message(void)
{
    if (msg_box) {
        lv_msgbox_close(msg_box);
        msg_box = NULL;
    }
}
