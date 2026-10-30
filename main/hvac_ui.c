/**
 * @file hvac_ui.c
 * @brief OEM-minimal, touch-first HVAC UI for the 320x240 landscape panel.
 *
 * All UI calls are made by the LVGL task while it owns lvgl_mutex. Periodic
 * rendering uses persistent label buffers and lv_label_set_text_static(),
 * avoiding heap allocation in hvac_ui_update().
 */
#include "hvac_ui_priv.h"
#include "hvac_controller.h"
#include "hvac_hw_config.h"
#include "hvac_sensors.h"
#include "remote_io.h"
#include "tb6612fng_driver.h"
#include "wifi_ap.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <math.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "HVAC_UI";

#define UI_DIAG_REFRESH_MS   750U
#define UI_STATS_LOG_MS      5000U
#define UI_TAP_SLOP          10
/* Main + debug + modal cells + the largest settings view (air distribution grid: 41) with margin. */
#define UI_BUTTON_CONTEXTS   96U

#define UI_MAIN_TOP_H        48
#define UI_MAIN_STATUS_H     24
#define UI_TEMP_ROW_H        72
#define UI_INFO_ROW_H        48
#define UI_FAN_ROW_H         48

static lv_obj_t *scr_main;
static lv_obj_t *tabview;
static lv_obj_t *tab_main;
static lv_obj_t *tab_debug;
static lv_obj_t *tab_settings;
static lv_obj_t *modal;
static bool s_lang_da;
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

typedef struct {
    bool in_use;
    lv_obj_t *obj;
    lv_event_cb_t action_cb;
    void *action_user_data;
    lv_event_cb_t long_press_cb;
    lv_event_cb_t repeat_cb;
    lv_point_t press_point;
    bool press_valid;
    bool press_invalid;
    bool click_handled;
    bool long_press_fired;
} ui_button_context_t;

/* Fixed contexts keep button dispatch allocation-free, including modal cells. */
static ui_button_context_t s_button_contexts[UI_BUTTON_CONTEXTS];

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
static ui_label_t lbl_diag_rows[14];

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

void ui_get_state(hvac_state_t *state)
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

void ui_label_set(ui_label_t *label, const char *text)
{
    if (!label || !label->obj || !text || strcmp(label->text, text) == 0) {
        return;
    }
    snprintf(label->text, sizeof(label->text), "%s", text);
    lv_label_set_text_static(label->obj, label->text);
}

void ui_label_format(ui_label_t *label, const char *fmt, ...)
{
    char tmp[sizeof(label->text)];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    ui_label_set(label, tmp);
}

void ui_label_set_color(ui_label_t *label, uint32_t rgb)
{
    if (label && label->obj && (!label->color_valid || label->color != rgb)) {
        label->color = rgb;
        label->color_valid = true;
        lv_obj_set_style_text_color(label->obj, lv_color_hex(rgb), 0);
    }
}

lv_obj_t *ui_make_label(lv_obj_t *parent, ui_label_t *store,
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
    ui_label_set(store, text);
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

static ui_button_context_t *button_context_find(lv_obj_t *obj)
{
    for (size_t i = 0; i < UI_BUTTON_CONTEXTS; ++i) {
        if (s_button_contexts[i].in_use && s_button_contexts[i].obj == obj) {
            return &s_button_contexts[i];
        }
    }
    return NULL;
}

static ui_button_context_t *button_context_allocate(lv_obj_t *obj)
{
    for (size_t i = 0; i < UI_BUTTON_CONTEXTS; ++i) {
        ui_button_context_t *ctx = &s_button_contexts[i];
        if (!ctx->in_use) {
            memset(ctx, 0, sizeof(*ctx));
            ctx->in_use = true;
            ctx->obj = obj;
            return ctx;
        }
    }
    ESP_LOGE(TAG, "Button context pool exhausted (%u)", (unsigned)UI_BUTTON_CONTEXTS);
    return NULL;
}

static void button_context_delete_handler(lv_event_t *e)
{
    ui_button_context_t *ctx = lv_event_get_user_data(e);
    if (ctx) {
        ctx->obj = NULL;
        ctx->in_use = false;
    }
}

static void button_press_handler(lv_event_t *e)
{
    ui_button_context_t *ctx = lv_event_get_user_data(e);
    lv_indev_t *indev = lv_event_get_indev(e);
    if (!ctx) return;

    ctx->press_valid = indev != NULL;
    ctx->press_invalid = false;
    ctx->click_handled = false;
    ctx->long_press_fired = false;
    if (indev) lv_indev_get_point(indev, &ctx->press_point);
}

/* Invalidate a press as soon as it drifts outside the slop; it cannot re-arm
 * by moving back to the origin. The release dispatcher repeats this check. */
static bool button_press_within_slop(lv_event_t *e, ui_button_context_t *ctx)
{
    if (!ctx || !ctx->press_valid || ctx->press_invalid) return false;
    lv_indev_t *indev = lv_event_get_indev(e);
    if (!indev) {
        ctx->press_invalid = true;
        return false;
    }

    lv_point_t point;
    lv_indev_get_point(indev, &point);
    int dx = abs((int)point.x - (int)ctx->press_point.x);
    int dy = abs((int)point.y - (int)ctx->press_point.y);
    if (dx > UI_TAP_SLOP || dy > UI_TAP_SLOP) {
        ctx->press_invalid = true;
        return false;
    }
    return true;
}

static void button_pressing_handler(lv_event_t *e)
{
    ui_button_context_t *ctx = lv_event_get_user_data(e);
    if (ctx) (void)button_press_within_slop(e, ctx);
}

static void button_press_lost_handler(lv_event_t *e)
{
    ui_button_context_t *ctx = lv_event_get_user_data(e);
    if (ctx) {
        ctx->press_invalid = true;
        ctx->click_handled = true;
    }
}

/* LVGL 8.4 sends SHORT_CLICKED and then CLICKED for one tap. Share this
 * dispatcher and consume the sequence once, so a tap can never run twice. */
static void button_click_dispatcher(lv_event_t *e)
{
    ui_button_context_t *ctx = lv_event_get_user_data(e);
    if (!ctx || ctx->click_handled) return;

    ctx->click_handled = true;
    if (ctx->long_press_fired || !button_press_within_slop(e, ctx)) return;

    lv_event_cb_t action = ctx->action_cb;
    if (action) action(e);
}

static void button_long_press_dispatcher(lv_event_t *e)
{
    ui_button_context_t *ctx = lv_event_get_user_data(e);
    if (!ctx) return;
    ctx->long_press_fired = true;
    if (!button_press_within_slop(e, ctx)) return;

    lv_event_cb_t action = ctx->long_press_cb;
    if (action) action(e);
}

static void button_repeat_dispatcher(lv_event_t *e)
{
    ui_button_context_t *ctx = lv_event_get_user_data(e);
    if (!ctx || !button_press_within_slop(e, ctx)) return;

    lv_event_cb_t action = ctx->repeat_cb;
    if (action) action(e);
}

void ui_button_set_action(lv_obj_t *btn, lv_event_cb_t cb, void *user_data)
{
    ui_button_context_t *ctx = button_context_find(btn);
    if (!ctx) return;
    ctx->action_cb = cb;
    ctx->action_user_data = user_data;
}

void ui_button_set_long_press(lv_obj_t *btn, lv_event_cb_t cb)
{
    ui_button_context_t *ctx = button_context_find(btn);
    if (ctx) ctx->long_press_cb = cb;
}

lv_obj_t *ui_make_button_blank(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                               lv_coord_t w, lv_coord_t h, lv_event_cb_t cb,
                               uint32_t normal, uint32_t checked)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, h);
    style_button(btn, normal, checked);
    lv_obj_set_style_opa(btn, LV_OPA_50, LV_STATE_DISABLED);

    ui_button_context_t *ctx = button_context_allocate(btn);
    if (ctx) {
        ctx->action_cb = cb;
        lv_obj_add_event_cb(btn, button_press_handler, LV_EVENT_PRESSED, ctx);
        lv_obj_add_event_cb(btn, button_pressing_handler, LV_EVENT_PRESSING, ctx);
        lv_obj_add_event_cb(btn, button_press_lost_handler, LV_EVENT_PRESS_LOST, ctx);
        lv_obj_add_event_cb(btn, button_click_dispatcher, LV_EVENT_SHORT_CLICKED, ctx);
        lv_obj_add_event_cb(btn, button_click_dispatcher, LV_EVENT_CLICKED, ctx);
        lv_obj_add_event_cb(btn, button_long_press_dispatcher, LV_EVENT_LONG_PRESSED, ctx);
        lv_obj_add_event_cb(btn, button_repeat_dispatcher, LV_EVENT_LONG_PRESSED_REPEAT, ctx);
        lv_obj_add_event_cb(btn, button_context_delete_handler, LV_EVENT_DELETE, ctx);
    }
    return btn;
}

void *ui_button_user_data(lv_event_t *e)
{
    ui_button_context_t *ctx = lv_event_get_user_data(e);
    return ctx ? ctx->action_user_data : NULL;
}

bool ui_press_within_slop(lv_event_t *e)
{
    ui_button_context_t *ctx = button_context_find(lv_event_get_target(e));
    return ctx && button_press_within_slop(e, ctx);
}

void ui_set_enabled(lv_obj_t *obj, bool enabled)
{
    if (!obj || lv_obj_has_state(obj, LV_STATE_DISABLED) == !enabled) return;
    if (enabled) lv_obj_clear_state(obj, LV_STATE_DISABLED);
    else lv_obj_add_state(obj, LV_STATE_DISABLED);
}

lv_obj_t *ui_make_body(lv_obj_t *parent, lv_coord_t y, lv_coord_t h)
{
    lv_obj_t *body = lv_obj_create(parent);
    lv_obj_set_pos(body, 0, y);
    lv_obj_set_size(body, UI_W, h);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_pad_all(body, 0, 0);
    lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_AUTO);
    return body;
}

lv_obj_t *ui_make_button(lv_obj_t *parent, const char *text,
                         lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h,
                         const lv_font_t *font, lv_event_cb_t cb,
                         uint32_t normal, uint32_t checked)
{
    lv_obj_t *btn = ui_make_button_blank(parent, x, y, w, h, cb, normal, checked);

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

void ui_set_checked(lv_obj_t *obj, bool checked)
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

void ui_modal_close(void)
{
    if (modal) {
        lv_obj_del(modal);
        modal = NULL;
    }
}

bool ui_lang_da(void)
{
    return s_lang_da;
}

lv_obj_t *ui_modal_open(uint32_t border_color, lv_coord_t w, lv_coord_t h)
{
    if (modal) return NULL;
    modal = lv_obj_create(scr_main);
    lv_obj_set_size(modal, w, h);
    lv_obj_center(modal);
    lv_obj_set_style_bg_color(modal, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_border_color(modal, lv_color_hex(border_color), 0);
    lv_obj_set_style_border_width(modal, 2, 0);
    lv_obj_set_style_radius(modal, 8, 0);
    lv_obj_set_style_pad_all(modal, 0, 0);
    lv_obj_clear_flag(modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(modal, LV_OBJ_FLAG_GESTURE_BUBBLE);
    return modal;
}

static ui_confirm_cb_t s_confirm_cb;
static void *s_confirm_data;

static void confirm_ok_handler(lv_event_t *e)
{
    (void)e;
    ui_confirm_cb_t cb = s_confirm_cb;
    void *data = s_confirm_data;
    ui_modal_close();
    if (cb) cb(data);
}

void ui_modal_confirm(const char *title, const char *desc, const char *ok_text,
                      uint32_t ok_color, ui_confirm_cb_t cb, void *data)
{
    if (!ui_modal_open(UI_COLOR_WARNING, 304, 148)) return;
    s_confirm_cb = cb;
    s_confirm_data = data;
    lv_obj_t *t = lv_label_create(modal);
    lv_label_set_text(t, title);
    lv_label_set_long_mode(t, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(t, 288);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(t, UI_FONT_16, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 8);
    lv_obj_t *d = lv_label_create(modal);
    lv_label_set_text(d, desc);
    lv_label_set_long_mode(d, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(d, 288);
    lv_obj_set_style_text_align(d, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(d, UI_FONT_14, 0);
    lv_obj_set_style_text_color(d, lv_color_hex(UI_COLOR_TEXT_DIM), 0);
    lv_obj_align(d, LV_ALIGN_TOP_MID, 0, 34);
    (void)ui_make_button(modal, s_lang_da ? "ANNULLER" : "CANCEL", 4, 84, 144, 48, UI_FONT_16,
                         cancel_modal_handler, UI_COLOR_SURFACE, UI_COLOR_SURFACE_2);
    (void)ui_make_button(modal, ok_text, 152, 84, 144, 48, UI_FONT_16,
                         confirm_ok_handler, ok_color, ok_color);
}

static void enter_tab(uint32_t idx)
{
    if (tabview && lv_tabview_get_tab_act(tabview) != idx) {
        lv_tabview_set_act(tabview, idx, LV_ANIM_OFF);
    }
}

static void enter_debug(void)
{
    enter_tab(1);
}

static void enter_main(void)
{
    enter_tab(0);
}

static void gesture_handler(lv_event_t *e)
{
    /* Gestures bubble to scr_main. Capture the direction and suppress release
     * activation even when an overlay makes the swipe a no-op. Pages are
     * main <-> diagnostics <-> settings; inside a settings sub-page a swipe
     * does nothing (BACK is explicit, and a jog must never be interrupted). */
    lv_indev_t *indev = lv_event_get_indev(e);
    if (!indev) return;
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    lv_indev_wait_release(indev);

    if (modal || msg_box) return;
    uint32_t act = lv_tabview_get_tab_act(tabview);
    if (act == 0 && dir == LV_DIR_LEFT) {
        enter_debug();
    } else if (act == 1 && dir == LV_DIR_LEFT) {
        ui_settings_reset_view();
        enter_tab(2);
    } else if (act == 1 && dir == LV_DIR_RIGHT) {
        enter_main();
    } else if (act == 2 && dir == LV_DIR_RIGHT && ui_settings_at_root()) {
        enter_debug();
    }
}

static void mode_choice_handler(lv_event_t *e)
{
    ui_button_context_t *ctx = lv_event_get_user_data(e);
    intptr_t mode = ctx ? (intptr_t)ctx->action_user_data : -1;
    ui_modal_close();
    if (mode >= 0 && mode < HVAC_MODE_MAX) {
        esp_err_t ret = hvac_controller_set_mode((hvac_mode_t)mode);
        if (ret != ESP_OK) hvac_ui_show_error("Mode change failed");
    }
}

static void cancel_modal_handler(lv_event_t *e)
{
    (void)e;
    ui_modal_close();
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
        lv_obj_t *btn = ui_make_button(modal, opts[i].text, x, y, 72, 48,
                                    &lv_font_montserrat_16, NULL,
                                    UI_COLOR_SURFACE, UI_COLOR_SURFACE_2);
        ui_button_set_action(btn, mode_choice_handler, (void *)(intptr_t)opts[i].mode);
    }
    /* Cancel occupies the eighth 72x48 cell, preserving the minimum target. */
    (void)ui_make_button(modal, "CANCEL", 244, 84, 72, 48,
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

static void preset_choice_handler(lv_event_t *e)
{
    intptr_t preset = (intptr_t)ui_button_user_data(e);
    ui_modal_close();
    if (preset > HVAC_PRESET_NONE && preset < HVAC_PRESET_COUNT &&
        hvac_controller_apply_preset((hvac_preset_t)preset) != ESP_OK) {
        hvac_ui_show_error("Preset failed");
    }
}

void ui_open_presets(void)
{
    static const struct { const char *en, *da; hvac_preset_t preset; } opts[] = {
        {"DEFOG", "AFROS", HVAC_PRESET_DEFOG}, {"MAX HEAT", "MAX VARME", HVAC_PRESET_MAX_HEAT},
        {"MAX COOL", "MAX KOLD", HVAC_PRESET_MAX_COOL}, {"ECO", "ECO", HVAC_PRESET_ECO}
    };
    if (!ui_modal_open(UI_COLOR_ACCENT, 312, 196)) return;
    lv_obj_t *title = lv_label_create(modal);
    lv_label_set_text(title, s_lang_da ? "FORINDSTILLINGER" : "PRESETS");
    lv_obj_set_style_text_font(title, UI_FONT_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);
    const hvac_preset_t active = hvac_controller_get_preset();
    for (size_t i = 0; i < 4; ++i) {
        lv_obj_t *btn = ui_make_button(modal, s_lang_da ? opts[i].da : opts[i].en,
                                       4 + (lv_coord_t)(i % 2) * 150, 30 + (lv_coord_t)(i / 2) * 52,
                                       146, 48, UI_FONT_16, preset_choice_handler,
                                       UI_COLOR_SURFACE, UI_COLOR_ACCENT);
        ui_button_set_action(btn, preset_choice_handler, (void *)(intptr_t)opts[i].preset);
        ui_set_checked(btn, active == opts[i].preset);
    }
    (void)ui_make_button(modal, s_lang_da ? "LUK" : "CLOSE", 4, 136, 296, 48, UI_FONT_16,
                         cancel_modal_handler, UI_COLOR_SURFACE, UI_COLOR_SURFACE_2);
}

static void auto_long_press_handler(lv_event_t *e)
{
    (void)e;
    ui_open_presets();
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
    ui_get_state(&state);
    hvac_controller_set_ac(!state.ac_enabled);
}

static void recirc_handler(lv_event_t *e)
{
    hvac_state_t state;
    (void)e;
    ui_get_state(&state);
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
    ui_get_state(&state);
    (void)hvac_controller_set_fan_level((hvac_fan_speed_t)fan_step(state.fan_level, -1));
}

static void fan_up_handler(lv_event_t *e)
{
    hvac_state_t state;
    (void)e;
    ui_get_state(&state);
    (void)hvac_controller_set_fan_level((hvac_fan_speed_t)fan_step(state.fan_level, +1));
}

static void fan_auto_handler(lv_event_t *e)
{
    (void)e;
    (void)hvac_controller_set_fan_level(HVAC_FAN_AUTO);
}

void ui_button_set_repeat(lv_obj_t *btn, lv_event_cb_t cb)
{
    ui_button_context_t *ctx = button_context_find(btn);
    if (ctx) ctx->repeat_cb = cb;
}

static void night_handler(lv_event_t *e)
{
    (void)e;
    /* Night mode lives in the settings table (the controller's night fan cap reads it too). */
    if (settings_set_int(SET_NIGHT_MODE, settings_get_bool(SET_NIGHT_MODE) ? 0 : 1) != ESP_OK) {
        hvac_ui_show_error("Brightness setting was not saved");
    }
}

/* ---- display settings: brightness, auto-dim, night screen-off ---- */

typedef struct {
    bool night, screen_off, show_blower, recirc_fitted;
    int day, night_level, dim_s, night_off_s;
} display_cfg_t;

static display_cfg_t s_cfg;
static uint32_t s_cfg_seq = UINT32_MAX;
static int s_backlight = -1;
static lv_obj_t *s_wake_overlay;

static void cfg_reload(void)
{
    s_cfg.night = settings_get_bool(SET_NIGHT_MODE);
    s_cfg.screen_off = settings_get_bool(SET_NIGHT_SCREEN_OFF);
    s_cfg.show_blower = settings_get_bool(SET_SHOW_BLOWER_PCT);
    s_cfg.recirc_fitted = settings_get_bool(SET_RECIRC_FITTED);
    s_cfg.day = settings_get_int(SET_BRIGHT_DAY);
    s_cfg.night_level = settings_get_int(SET_BRIGHT_NIGHT);
    s_cfg.dim_s = settings_get_int(SET_AUTO_DIM_S);
    s_cfg.night_off_s = settings_get_int(SET_NIGHT_OFF_S);     /* "Night: screen off" without auto-dim */
    s_lang_da = settings_get_int(SET_LANGUAGE) == SET_LANG_DA;
}

static void wake_overlay_pressed(lv_event_t *e)
{
    /* The waking touch must not press whatever is under it. */
    lv_indev_t *indev = lv_event_get_indev(e);
    if (indev) lv_indev_wait_release(indev);
}

/* LVGL task only: hold delay / repeat of every input device, and the chart period. */
static void apply_lvgl_settings(void)
{
    for (lv_indev_t *in = lv_indev_get_next(NULL); in; in = lv_indev_get_next(in)) {
        in->driver->long_press_time = (uint16_t)settings_get_int(SET_HOLD_DELAY_MS);
        in->driver->long_press_repeat_time = (uint16_t)settings_get_int(SET_HOLD_REPEAT_MS);
    }
    ui_diag_apply_settings();
}

static void apply_display(void)
{
    uint32_t seq = settings_seq();
    if (seq != s_cfg_seq) {
        s_cfg_seq = seq;
        cfg_reload();
        apply_lvgl_settings();
    }
    const uint32_t idle_ms = lv_disp_get_inactive_time(NULL);
    int level = s_cfg.night ? s_cfg.night_level : s_cfg.day;
    if (s_cfg.dim_s > 0 && idle_ms > (uint32_t)s_cfg.dim_s * 1000U && s_cfg.night_level < level) {
        level = s_cfg.night_level;
    }
    const uint32_t off_after_s = (uint32_t)(s_cfg.dim_s > 0 ? s_cfg.dim_s : s_cfg.night_off_s);
    const bool off = s_cfg.night && s_cfg.screen_off && idle_ms > off_after_s * 1000U;
    if (off) level = 0;

    if (off && !s_wake_overlay) {
        s_wake_overlay = lv_obj_create(lv_layer_top());
        lv_obj_set_size(s_wake_overlay, UI_W, UI_H);
        lv_obj_set_style_bg_opa(s_wake_overlay, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_wake_overlay, 0, 0);
        lv_obj_clear_flag(s_wake_overlay, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(s_wake_overlay, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_wake_overlay, wake_overlay_pressed, LV_EVENT_PRESSED, NULL);
    } else if (!off && s_wake_overlay) {
        lv_obj_del(s_wake_overlay);
        s_wake_overlay = NULL;
    }
    if (level != s_backlight) {
        s_backlight = level;
        hvac_ui_set_brightness((uint8_t)level);
    }
}

static void reset_cal_execute(lv_event_t *e)
{
    (void)e;
    ui_modal_close();
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
    lv_obj_t *keep = ui_make_button(modal, "KEEP MODEL", 4, 84, 144, 48,
                                 &lv_font_montserrat_16, cancel_modal_handler,
                                 UI_COLOR_SURFACE, UI_COLOR_SURFACE_2);
    (void)keep;
    lv_obj_t *reset = ui_make_button(modal, "RESET CAL", 156, 84, 144, 48,
                                  &lv_font_montserrat_16, reset_cal_execute,
                                  UI_COLOR_FAULT, UI_COLOR_FAULT);
    (void)reset;
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
        {"MODE", open_mode_picker}, {"AUTO", auto_handler}, {"DEF", def_handler},
        {"A/C\nOFF", ac_handler}, {"RECIRC\nOFF", recirc_handler}
    };
    for (size_t i = 0; i < sizeof(top) / sizeof(top[0]); ++i) {
        /* MODE tap is SHORT_CLICKED-only; a long press goes to diagnostics. */
        lv_event_cb_t tap_handler = top[i].handler;
        lv_obj_t *btn = ui_make_button(tab_main, top[i].text, 4 + (lv_coord_t)i * 64, 24,
                                    56, UI_MAIN_TOP_H, &lv_font_montserrat_16,
                                    tap_handler, UI_COLOR_SURFACE, UI_COLOR_SURFACE_2);
        if (i == 0) {
            btn_mode = btn;
            ui_button_set_long_press(btn_mode, mode_long_press_handler);
        } else if (i == 1) {
            btn_auto = btn;
            /* Presets live behind a long press so the main page stays uncrowded. */
            ui_button_set_long_press(btn_auto, auto_long_press_handler);
        }
        else if (i == 2) btn_def = btn;
        else if (i == 3) btn_ac = btn;
        else btn_recirc = btn;
    }

    /* Explicit ON/OFF text ensures toggle state is not conveyed by colour alone. */
    lbl_ac_button.obj = lv_obj_get_child(btn_ac, 0);
    lbl_ac_button.text[0] = '\0';
    ui_label_set(&lbl_ac_button, "A/C\nOFF");
    lbl_recirc_button.obj = lv_obj_get_child(btn_recirc, 0);
    lbl_recirc_button.text[0] = '\0';
    ui_label_set(&lbl_recirc_button, "RECIRC\nOFF");

    ui_make_label(tab_main, &lbl_operating, "OFF | READY", 4, 0, 196, UI_MAIN_STATUS_H,
               &lv_font_montserrat_16, UI_COLOR_TEXT, LV_TEXT_ALIGN_LEFT);
    ui_make_label(tab_main, &lbl_fault_badge, "", 204, 0, 112, UI_MAIN_STATUS_H,
               &lv_font_montserrat_16, UI_COLOR_FAULT, LV_TEXT_ALIGN_RIGHT);

    btn_temp_down = ui_make_button(tab_main, LV_SYMBOL_MINUS, 4, 72, 64, UI_TEMP_ROW_H,
                                &lv_font_montserrat_22, NULL,
                                UI_COLOR_SURFACE, UI_COLOR_SURFACE_2);
    btn_temp_up = ui_make_button(tab_main, LV_SYMBOL_PLUS, 252, 72, 64, UI_TEMP_ROW_H,
                              &lv_font_montserrat_22, NULL,
                              UI_COLOR_SURFACE, UI_COLOR_SURFACE_2);
    ui_button_set_action(btn_temp_down, temp_down_handler, NULL);
    ui_button_set_action(btn_temp_up, temp_up_handler, NULL);
    ui_button_set_repeat(btn_temp_down, temp_down_handler);
    ui_button_set_repeat(btn_temp_up, temp_up_handler);
    ui_make_label(tab_main, &lbl_target_temp, "22.0°C", 76, 80, 168, 56,
               &lv_font_montserrat_44, UI_COLOR_TEXT, LV_TEXT_ALIGN_CENTER);

    ui_make_label(tab_main, &lbl_cabin_temp, "CABIN --.-°C", 4, 144, 140, UI_INFO_ROW_H,
               &lv_font_montserrat_16, UI_COLOR_TEXT, LV_TEXT_ALIGN_LEFT);
    ui_make_label(tab_main, &lbl_humidity, "RH --%", 144, 144, 72, UI_INFO_ROW_H,
               &lv_font_montserrat_16, UI_COLOR_TEXT_DIM, LV_TEXT_ALIGN_CENTER);
    ui_make_label(tab_main, &lbl_compressor, "COMP OFF", 216, 144, 100, UI_INFO_ROW_H,
               &lv_font_montserrat_16, UI_COLOR_TEXT, LV_TEXT_ALIGN_RIGHT);

    btn_fan_down = ui_make_button(tab_main, LV_SYMBOL_MINUS, 4, 192, 56, UI_FAN_ROW_H,
                               &lv_font_montserrat_22, NULL,
                               UI_COLOR_SURFACE, UI_COLOR_SURFACE_2);
    btn_fan_up = ui_make_button(tab_main, LV_SYMBOL_PLUS, 260, 192, 56, UI_FAN_ROW_H,
                             &lv_font_montserrat_22, NULL,
                             UI_COLOR_SURFACE, UI_COLOR_SURFACE_2);
    ui_button_set_action(btn_fan_down, fan_down_handler, NULL);
    ui_button_set_action(btn_fan_up, fan_up_handler, NULL);
    ui_button_set_repeat(btn_fan_down, fan_down_handler);
    ui_button_set_repeat(btn_fan_up, fan_up_handler);
    btn_fan_auto = ui_make_button(tab_main, "FAN AUTO | 0%", 68, 192, 184, UI_FAN_ROW_H,
                               &lv_font_montserrat_16, fan_auto_handler,
                               UI_COLOR_SURFACE, UI_COLOR_SURFACE_2);
    /* Bind the label's retained buffer so periodic updates never allocate text. */
    lbl_fan.obj = lv_obj_get_child(btn_fan_auto, 0);
    lbl_fan.text[0] = '\0';
    ui_label_set(&lbl_fan, "FAN AUTO | 0%");
    /* The centre fan label doubles as the one-tap AUTO selector. */

    /* Main-screen gestures bubble to scr_main; page changes are instant. */
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
    btn_night = ui_make_button(tab_debug, "DAY", 216, 0, 100, 48,
                            &lv_font_montserrat_16, night_handler,
                            UI_COLOR_SURFACE, UI_COLOR_SURFACE_2);
    lbl_night.obj = lv_obj_get_child(btn_night, 0);
    lbl_night.text[0] = '\0';
    ui_label_set(&lbl_night, "DAY");

    ui_make_label(tab_debug, &lbl_diag_fault, "NO ACTIVE FAULT", 4, 48, 312, 36,
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

    static const char *initial[14] = {
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
        "LINK --",
        "WI-FI --"
    };
    for (size_t i = 0; i < 14; ++i) {
        ui_make_label(body, &lbl_diag_rows[i], initial[i], 0, (lv_coord_t)i * 18,
                   308, 18, &lv_font_montserrat_14, UI_COLOR_TEXT, LV_TEXT_ALIGN_LEFT);
    }

    (void)ui_make_button(tab_debug, "RESET CAL", 112, 192, 96, 48,
                      &lv_font_montserrat_14, reset_cal_confirm_handler,
                      UI_COLOR_WARNING, UI_COLOR_WARNING);
    (void)ui_make_button(tab_debug, "SAVE CAL", 216, 192, 96, 48,
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

    ui_label_set(&lbl_diag_fault, fault ? line : "NO ACTIVE FAULT");
    ui_label_set_color(&lbl_diag_fault, fault ? UI_COLOR_FAULT : UI_COLOR_TEXT_DIM);
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
    ui_label_set(&lbl_diag_rows[0], buf);
    ui_label_format(&lbl_diag_rows[1], "MODE %s  FAN %s  BLOWER %u%%",
                 hvac_controller_get_mode_name(state->mode),
                 hvac_controller_get_fan_level_name(state->fan_level),
                 (unsigned)state->blower_speed);
    if (state->compressor_type == SET_AC_ELECTRIC) {
        /* worst case "A/C ELEC dem 100% out 100% EN ev -2.0C" is 294 px of the 308 px row */
        char evap_text[12];
        if (isnan(state->evap_c)) snprintf(evap_text, sizeof(evap_text), "--");
        else snprintf(evap_text, sizeof(evap_text), "%.1fC", (double)state->evap_c);
        ui_label_format(&lbl_diag_rows[2], "A/C ELEC dem %.0f%% out %.0f%% %s ev %s",
                        (double)state->ac_demand_pct, (double)state->ac_output_pct,
                        state->ac_enable ? "EN" : "--", evap_text);
    } else {
        ui_label_format(&lbl_diag_rows[2], "A/C REQ %s  COMP %s  RECIRC %s",
                     state->ac_enabled ? "ON" : "OFF",
                     state->ac_compressor_on ? "ON" : "OFF",
                     state->recirculate ? "ON" : "OFF");
    }

    const uint8_t actuals[HVAC_VALVE_MAX] = {
        state->center_valve_pos, state->center2_valve_pos, state->foot_valve_pos,
        state->windscreen_valve_pos, state->coolant_valve_pos
    };
    for (int i = 0; i < HVAC_VALVE_MAX; ++i) {
        uint8_t actual = actuals[i];
        uint8_t target = tb6612fng_get_valve_target(s_valves[i]);
        if (!tb6612fng_valve_fitted(s_valves[i])) {
            ui_label_format(&lbl_diag_rows[3 + i], "%-11s not fitted", s_valve_names[i]);
        } else {
            ui_label_format(&lbl_diag_rows[3 + i], "%-11s ACT %3u%% TARGET %3u%%",
                         s_valve_names[i], (unsigned)actual, (unsigned)target);
        }
        if (state->valve_fault_mask & (1U << i)) {
            ui_label_set_color(&lbl_diag_rows[3 + i], UI_COLOR_FAULT);
        } else {
            ui_label_set_color(&lbl_diag_rows[3 + i], UI_COLOR_TEXT);
        }
    }

    float heating_rate = 0.0f, cooling_rate = 0.0f, adaptive_gain = 0.0f;
    uint32_t sample_count = 0;
    bool calibrated = false;
    (void)hvac_controller_get_thermal_status(&heating_rate, &cooling_rate,
                                              &adaptive_gain, &sample_count, &calibrated);
    ui_label_format(&lbl_diag_rows[8], "HEAT %s  COOL %s  REACHED %s",
                 state->is_heating ? "ON" : "OFF", state->is_cooling ? "ON" : "OFF",
                 state->temp_reached ? "ON" : "OFF");
    ui_label_format(&lbl_diag_rows[9], "TH H %.2f C %.2f G %.1f N %lu %s",
                 heating_rate, cooling_rate, adaptive_gain, (unsigned long)sample_count,
                 calibrated ? "CAL" : "LEARN");
    uint32_t uptime_sec = xTaskGetTickCount() * portTICK_PERIOD_MS / 1000U;
    ui_label_format(&lbl_diag_rows[10], "UP %luh%lum  HEAP %luKB",
                 (unsigned long)(uptime_sec / 3600U),
                 (unsigned long)((uptime_sec % 3600U) / 60U),
                 (unsigned long)(esp_get_free_heap_size() / 1024U));
    uint32_t tx = 0, rx = 0, crc = 0, timeout = 0;
    remote_io_get_stats(&tx, &rx, &crc, &timeout);
    ui_label_format(&lbl_diag_rows[11], "REMOTE TX %lu RX %lu CRC %lu TO %lu",
                 (unsigned long)tx, (unsigned long)rx,
                 (unsigned long)crc, (unsigned long)timeout);
    ui_label_set(&lbl_diag_rows[12], link_ok ? "LINK CONNECTED" : "LINK LOST");
    ui_label_set_color(&lbl_diag_rows[12], link_ok ? UI_COLOR_ACCENT : UI_COLOR_FAULT);

    const char *wifi_err = wifi_ap_last_error();
    if (wifi_err[0]) {
        ui_label_format(&lbl_diag_rows[13], "WI-FI FAILED %s", wifi_err);
    } else if (wifi_ap_is_on()) {
        ui_label_format(&lbl_diag_rows[13], "WI-FI ON %d PHONE(S)", wifi_ap_client_count());
    } else {
        ui_label_set(&lbl_diag_rows[13], "WI-FI OFF");
    }
    ui_label_set_color(&lbl_diag_rows[13], wifi_err[0] ? UI_COLOR_FAULT : UI_COLOR_TEXT);
}

esp_err_t hvac_ui_init(void)
{
    ESP_LOGI(TAG, "Initializing OEM-minimal HVAC UI");
    cfg_reload();
    hvac_state_t initial_state;
    ui_get_state(&initial_state);

    scr_main = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_main, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_border_width(scr_main, 0, 0);
    lv_obj_set_style_pad_all(scr_main, 0, 0);
    lv_obj_clear_flag(scr_main, LV_OBJ_FLAG_SCROLLABLE);
    /* LVGL 8.4 bubbles gestures past gesture-bubbling children to scr_main. */
    lv_obj_add_event_cb(scr_main, gesture_handler, LV_EVENT_GESTURE, NULL);

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
    tab_settings = lv_tabview_add_tab(tabview, "Settings");
    for (size_t i = 0; i < 3; ++i) {
        lv_obj_t *tab = i == 0 ? tab_main : i == 1 ? tab_debug : tab_settings;
        lv_obj_set_style_bg_color(tab, lv_color_hex(UI_COLOR_BG), 0);
        lv_obj_set_style_border_width(tab, 0, 0);
        lv_obj_set_style_pad_all(tab, 0, 0);
        lv_obj_clear_flag(tab, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(tab, LV_OBJ_FLAG_GESTURE_BUBBLE);
    }
    /* The tabview content owns horizontal scrolling/snapping in LVGL 8.4.
     * Disable native scrolling; swipes are handled programmatically on scr_main. */
    lv_obj_clear_flag(lv_tabview_get_content(tabview), LV_OBJ_FLAG_SCROLLABLE);

    make_main_screen();
    make_debug_screen();
    ui_settings_init(tab_settings);
    ui_diag_init();
    apply_lvgl_settings();

    lv_tabview_set_act(tabview, 0, LV_ANIM_OFF);
    lv_scr_load(scr_main);

    lv_mem_monitor_t mem;
    lv_mem_monitor(&mem);
    ESP_LOGI(TAG, "LVGL heap after UI init: total=%lu free=%lu used=%u%% frag=%u%% largest=%lu",
             (unsigned long)mem.total_size, (unsigned long)mem.free_size,
             (unsigned)mem.used_pct, (unsigned)mem.frag_pct,
             (unsigned long)mem.free_biggest_size);
    ESP_LOGI(TAG, "UI ready; night=%s", s_cfg.night ? "yes" : "no");
    return ESP_OK;
}

void hvac_ui_update(void)
{
    const uint64_t started_us = esp_timer_get_time();
    hvac_state_t state;
    ui_get_state(&state);
    const bool link_ok = remote_io_is_connected();

    if (!isnan(state.target_temp_c)) {
        float target = state.temp_unit == TEMP_UNIT_CELSIUS
                           ? state.target_temp_c : hvac_controller_c_to_f(state.target_temp_c);
        ui_label_format(&lbl_target_temp, "%.1f°%c", target,
                     state.temp_unit == TEMP_UNIT_CELSIUS ? 'C' : 'F');
    } else {
        ui_label_set(&lbl_target_temp, "--.-°");
    }
    if (state.is_heating) ui_label_set_color(&lbl_target_temp, UI_COLOR_HEAT);
    else if (state.is_cooling) ui_label_set_color(&lbl_target_temp, UI_COLOR_COOL);
    else ui_label_set_color(&lbl_target_temp, UI_COLOR_TEXT);

    const float cabin = state.temp_unit == TEMP_UNIT_CELSIUS
                            ? state.cabin_temp_c : hvac_controller_c_to_f(state.cabin_temp_c);
    if (isnan(cabin)) ui_label_set(&lbl_cabin_temp, "CABIN --.-°");
    else ui_label_format(&lbl_cabin_temp, "CABIN %.1f°%c", cabin,
                      state.temp_unit == TEMP_UNIT_CELSIUS ? 'C' : 'F');
    if (isnan(state.cabin_humidity)) ui_label_set(&lbl_humidity, "RH --%");
    else ui_label_format(&lbl_humidity, "RH %.0f%%", state.cabin_humidity);
    ui_label_format(&lbl_compressor, "COMP %s", state.ac_compressor_on ? "ON" : "OFF");
    ui_label_set_color(&lbl_compressor, state.ac_compressor_on ? UI_COLOR_ACCENT : UI_COLOR_TEXT);

    const char *thermal = state.is_heating ? "HEATING" : state.is_cooling ? "COOLING" :
                          state.temp_reached ? "REACHED" : "READY";
    ui_label_format(&lbl_operating, "%s | %s", main_mode_short_name(state.mode), thermal);
    const char *badge = fault_short_text(&state, link_ok);
    ui_label_set(&lbl_fault_badge, badge);
    ui_label_set_color(&lbl_fault_badge, badge[0] ? UI_COLOR_FAULT : UI_COLOR_TEXT_DIM);

    ui_set_checked(btn_auto, state.mode == HVAC_MODE_AUTO);
    ui_set_checked(btn_def, state.mode == HVAC_MODE_DEFROST || state.mode == HVAC_MODE_DEFROST_FOOT);
    ui_set_checked(btn_ac, state.ac_enabled);
    ui_set_checked(btn_recirc, state.recirculate);
    ui_label_set(&lbl_ac_button, state.ac_enabled ? "A/C\nON" : "A/C\nOFF");
    ui_label_set(&lbl_recirc_button, state.recirculate ? "RECIRC\nON" : "RECIRC\nOFF");

    if (s_cfg.show_blower) {
        ui_label_format(&lbl_fan, "FAN %s | %u%%", fan_level_short_name(state.fan_level),
                        (unsigned)state.blower_speed);
    } else {
        ui_label_format(&lbl_fan, "FAN %s", fan_level_short_name(state.fan_level));
    }
    ui_set_checked(btn_fan_auto, state.fan_level == HVAC_FAN_AUTO);

    update_fault_diagnostics(&state, link_ok);
    const uint32_t active_tab = lv_tabview_get_tab_act(tabview);
    if (active_tab == 1) update_diagnostics(&state, link_ok);
    else if (active_tab == 2) ui_settings_update();

    apply_display();
    ui_label_set(&lbl_night, s_cfg.night ? "NIGHT" : "DAY");
    /* The flap button only exists on cars that have the recirc flap. */
    if (lv_obj_has_flag(btn_recirc, LV_OBJ_FLAG_HIDDEN) == s_cfg.recirc_fitted) {
        if (s_cfg.recirc_fitted) lv_obj_clear_flag(btn_recirc, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(btn_recirc, LV_OBJ_FLAG_HIDDEN);
    }

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

void hvac_ui_set_brightness(uint8_t brightness)
{
    if (brightness > 100) brightness = 100;
    hvac_hw_backlight_set(brightness);
}

void hvac_ui_apply_saved_brightness(void)
{
    /* May run outside the LVGL task: no LVGL calls here, the first tick handles dim/off. */
    cfg_reload();
    s_cfg_seq = settings_seq();
    s_backlight = s_cfg.night ? s_cfg.night_level : s_cfg.day;
    hvac_ui_set_brightness((uint8_t)s_backlight);
}

static void dismiss_message_handler(lv_event_t *e)
{
    (void)e;
    if (msg_box) lv_obj_del(msg_box);
}

static void show_message(const char *title, const char *message)
{
    if (!message || !scr_main) return;
    if (msg_box) lv_obj_del(msg_box);

    /* Use the same tap-slop dispatcher as every other UI button. LVGL's
     * built-in message-box close button closes on CLICKED before a later
     * guard could reject a drag, so message overlays use our own OK control. */
    msg_box = lv_obj_create(scr_main);
    lv_obj_set_pos(msg_box, 0, 0);
    lv_obj_set_size(msg_box, UI_W, UI_H);
    lv_obj_set_style_bg_color(msg_box, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(msg_box, LV_OPA_70, 0);
    lv_obj_set_style_border_width(msg_box, 0, 0);
    lv_obj_set_style_pad_all(msg_box, 0, 0);
    lv_obj_clear_flag(msg_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(msg_box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(msg_box, msgbox_delete_handler, LV_EVENT_DELETE, NULL);

    lv_obj_t *panel = lv_obj_create(msg_box);
    lv_obj_set_pos(panel, 16, 48);
    lv_obj_set_size(panel, 288, 144);
    lv_obj_set_style_bg_color(panel, lv_color_hex(UI_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(UI_COLOR_ACCENT), 0);
    lv_obj_set_style_border_width(panel, 2, 0);
    lv_obj_set_style_radius(panel, 8, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_GESTURE_BUBBLE);

    lv_obj_t *heading = lv_label_create(panel);
    lv_label_set_text(heading, title);
    lv_obj_set_pos(heading, 12, 8);
    lv_obj_set_size(heading, 264, 24);
    lv_obj_set_style_text_font(heading, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(heading, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_add_flag(heading, LV_OBJ_FLAG_GESTURE_BUBBLE);

    lv_obj_t *body = lv_label_create(panel);
    lv_label_set_text(body, message);
    lv_obj_set_pos(body, 12, 36);
    lv_obj_set_size(body, 264, 48);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(body, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(body, lv_color_hex(UI_COLOR_TEXT_DIM), 0);
    lv_obj_add_flag(body, LV_OBJ_FLAG_GESTURE_BUBBLE);

    (void)ui_make_button(panel, "OK", 96, 88, 96, 48,
                      &lv_font_montserrat_16, dismiss_message_handler,
                      UI_COLOR_SURFACE_2, UI_COLOR_SURFACE_2);
}

void hvac_ui_show_error(const char *message)
{
    show_message("Error", message);
}

void hvac_ui_show_info(const char *message)
{
    show_message("Info", message);
}
