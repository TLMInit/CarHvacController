/**
 * @file hvac_ui_priv.h
 * @brief Helpers shared by hvac_ui.c and the settings/diagnostics/service pages.
 *
 * Everything runs in the LVGL task under lvgl_mutex. Pages build their widgets
 * when opened and delete them when closed; periodic update() hooks only touch
 * persistent label buffers and only when a value changed.
 */
#ifndef HVAC_UI_PRIV_H
#define HVAC_UI_PRIV_H

#include "hvac_ui.h"
#include "hvac_controller.h"
#include "settings.h"

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

#define UI_W                 320
#define UI_H                 240
#define UI_HDR_H             48
#define UI_BODY_H            (UI_H - UI_HDR_H)

/* Generated Montserrat subsets with ae/oe/aa and the symbols the pages use. */
extern const lv_font_t font_da_14;
extern const lv_font_t font_da_16;
#define UI_FONT_14 (&font_da_14)
#define UI_FONT_16 (&font_da_16)

typedef struct {
    lv_obj_t *obj;
    char text[128];
    uint32_t color;
    bool color_valid;
} ui_label_t;

/* Page contract: open() fills hdr (48 px bar, back button already there) and
 * body (320x192 scroll area); update() runs every 100 ms tick while open and
 * must rate-limit itself; close() releases timers/jogs (objects are deleted by
 * the caller). */
typedef struct {
    const char *title_en;
    const char *title_da;
    void (*open)(lv_obj_t *hdr, lv_obj_t *body);
    void (*update)(void);
    void (*close)(void);
} ui_page_t;

extern const ui_page_t ui_page_events, ui_page_link, ui_page_selftest, ui_page_chart;
extern const ui_page_t ui_page_valves, ui_page_sensors;

/* hvac_ui.c */
void ui_get_state(hvac_state_t *state);
void ui_label_set(ui_label_t *label, const char *text);
void ui_label_format(ui_label_t *label, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void ui_label_set_color(ui_label_t *label, uint32_t rgb);
lv_obj_t *ui_make_label(lv_obj_t *parent, ui_label_t *store, const char *text,
                        lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h,
                        const lv_font_t *font, uint32_t color, lv_text_align_t align);
/* Button without a text child; tap slop / SHORT_CLICKED dispatch as everywhere. */
lv_obj_t *ui_make_button_blank(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                               lv_coord_t w, lv_coord_t h, lv_event_cb_t cb,
                               uint32_t normal, uint32_t checked);
lv_obj_t *ui_make_button(lv_obj_t *parent, const char *text, lv_coord_t x, lv_coord_t y,
                         lv_coord_t w, lv_coord_t h, const lv_font_t *font,
                         lv_event_cb_t cb, uint32_t normal, uint32_t checked);
void ui_button_set_action(lv_obj_t *btn, lv_event_cb_t cb, void *user_data);
void ui_button_set_repeat(lv_obj_t *btn, lv_event_cb_t cb);
void ui_button_set_long_press(lv_obj_t *btn, lv_event_cb_t cb);
void *ui_button_user_data(lv_event_t *e);          /* inside an action/repeat callback */
bool ui_press_within_slop(lv_event_t *e);          /* for hold-controls (jog) */
void ui_set_checked(lv_obj_t *obj, bool checked);
void ui_set_enabled(lv_obj_t *obj, bool enabled);
lv_obj_t *ui_make_body(lv_obj_t *parent, lv_coord_t y, lv_coord_t h);

lv_obj_t *ui_modal_open(uint32_t border_color, lv_coord_t w, lv_coord_t h);
void ui_modal_close(void);
typedef void (*ui_confirm_cb_t)(void *data);
void ui_modal_confirm(const char *title, const char *desc, const char *ok_text,
                      uint32_t ok_color, ui_confirm_cb_t cb, void *data);
void ui_open_presets(void);
bool ui_lang_da(void);

/* hvac_ui_settings.c */
void ui_settings_init(lv_obj_t *tab);
void ui_settings_update(void);                     /* tick while the settings tab is shown */
bool ui_settings_at_root(void);
void ui_settings_reset_view(void);

/* hvac_ui_diag.c */
void ui_diag_init(void);                           /* starts the 10 s chart feed timer */

#endif
