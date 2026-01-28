/**
 * @file hvac_ui.c
 * @brief LVGL-based HVAC User Interface Implementation
 */

#include "hvac_ui.h"
#include "hvac_controller.h"
#include "hvac_hw_config.h"
#include "hvac_sensors.h"

#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>


#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"


static const char *TAG = "HVAC_UI";

/*******************************************************************************
 * UI Theme Colors
 ******************************************************************************/
#define UI_COLOR_BG             lv_color_hex(0x1a1a2e)
#define UI_COLOR_PANEL          lv_color_hex(0x16213e)
#define UI_COLOR_ACCENT         lv_color_hex(0x0f4c75)
#define UI_COLOR_HIGHLIGHT      lv_color_hex(0x3282b8)
#define UI_COLOR_TEXT           lv_color_hex(0xeeeeee)
#define UI_COLOR_TEXT_DIM       lv_color_hex(0x888888)
#define UI_COLOR_HEAT           lv_color_hex(0xff6b35)
#define UI_COLOR_COOL           lv_color_hex(0x00b4d8)
#define UI_COLOR_OFF            lv_color_hex(0x404040)
#define UI_COLOR_ON             lv_color_hex(0x00ff88)
#define UI_COLOR_WARNING        lv_color_hex(0xffc107)

/*******************************************************************************
 * UI Objects
 ******************************************************************************/
static lv_obj_t *scr_main = NULL;
static lv_obj_t *tabview = NULL;
static lv_obj_t *tab_main = NULL;
static lv_obj_t *tab_debug = NULL;

// Temperature display (center)
static lv_obj_t *lbl_cabin_temp = NULL;
static lv_obj_t *lbl_target_temp = NULL;
static lv_obj_t *lbl_humidity = NULL;

// Mode buttons (right side)
static lv_obj_t *btn_mode_auto = NULL;
static lv_obj_t *btn_mode_defrost = NULL;
static lv_obj_t *btn_mode_face = NULL;
static lv_obj_t *btn_mode_foot = NULL;

// Fan control (bottom)
static lv_obj_t *slider_fan = NULL;
static lv_obj_t *lbl_fan_speed = NULL;

// Temperature control (+/- buttons in center)
static lv_obj_t *btn_temp_up = NULL;
static lv_obj_t *btn_temp_down = NULL;

// Feature buttons (left side)
static lv_obj_t *btn_ac = NULL;
static lv_obj_t *btn_recirc = NULL;

// Status indicators
static lv_obj_t *lbl_duct_temp = NULL;

// Valve position indicators
static lv_obj_t *lbl_center_valve_pos = NULL;
static lv_obj_t *lbl_center2_valve_pos = NULL;
static lv_obj_t *lbl_foot_valve_pos = NULL;
static lv_obj_t *lbl_windscreen_valve_pos = NULL;
static lv_obj_t *lbl_coolant_valve_pos = NULL;

// Debug screen objects
static lv_obj_t *lbl_thermal_status = NULL;
static lv_obj_t *lbl_raw_sensors = NULL;
static lv_obj_t *lbl_system_info = NULL;
static lv_obj_t *btn_reset_cal = NULL;
static lv_obj_t *btn_save_cal = NULL;
static lv_obj_t *btn_onewire_read = NULL;

// Message box
static lv_obj_t *msg_box = NULL;

// Fault tracking
static bool last_sensor_fault = false;
static bool last_valve_fault = false;

/*******************************************************************************
 * Private Functions - Event Handlers
 ******************************************************************************/

// Message box close event handler - clears the global pointer when box is closed
static void msgbox_close_handler(lv_event_t *e)
{
    msg_box = NULL;
}

static void btn_mode_face_handler(lv_event_t *e)
{
    hvac_controller_set_mode(HVAC_MODE_FACE);
}

static void btn_mode_foot_handler(lv_event_t *e)
{
    hvac_controller_set_mode(HVAC_MODE_FOOT);
}

static void btn_mode_defrost_handler(lv_event_t *e)
{
    hvac_controller_set_mode(HVAC_MODE_DEFROST);
}

static void btn_mode_auto_handler(lv_event_t *e)
{
    hvac_controller_set_mode(HVAC_MODE_AUTO);
}

static void btn_onewire_read_handler(lv_event_t *e)
{
    float temp_c = NAN;
    esp_err_t ret = hvac_sensors_debug_read_onewire(&temp_c);
    if (ret == ESP_OK && !isnan(temp_c)) {
        char msg[48];
        snprintf(msg, sizeof(msg), "1-Wire duct: %.2f C", temp_c);
        hvac_ui_show_info(msg);
    } else {
        hvac_ui_show_error("1-Wire read failed");
    }
}

static void btn_temp_up_handler(lv_event_t *e)
{
    hvac_controller_temp_up();
}

static void btn_temp_down_handler(lv_event_t *e)
{
    hvac_controller_temp_down();
}

static void slider_fan_handler(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    int32_t value = lv_slider_get_value(slider);
    
    hvac_fan_speed_t level;
    if (value == 0) {
        level = HVAC_FAN_OFF;
    } else if (value == 1) {
        level = HVAC_FAN_LOW;
    } else if (value == 2) {
        level = HVAC_FAN_MEDIUM_LOW;
    } else if (value == 3) {
        level = HVAC_FAN_MEDIUM;
    } else if (value == 4) {
        level = HVAC_FAN_MEDIUM_HIGH;
    } else {
        level = HVAC_FAN_HIGH;
    }
    
    hvac_controller_set_fan_level(level);
}

static void btn_ac_handler(lv_event_t *e)
{
    bool current = hvac_controller_get_ac();
    hvac_controller_set_ac(!current);
}

static void btn_recirc_handler(lv_event_t *e)
{
    bool current = hvac_controller_get_recirculate();
    hvac_controller_set_recirculate(!current);
}

static void btn_reset_cal_handler(lv_event_t *e)
{
    esp_err_t ret = hvac_controller_reset_thermal_cal();
    if (ret == ESP_OK) {
        hvac_ui_show_info("Thermal calibration reset");
    } else {
        hvac_ui_show_error("Failed to reset thermal calibration");
    }
}

static void btn_save_cal_handler(lv_event_t *e)
{
    esp_err_t ret = hvac_controller_save_thermal_cal();
    if (ret == ESP_OK) {
        hvac_ui_show_info("Thermal calibration saved");
    } else {
        hvac_ui_show_error("Failed to save thermal calibration");
    }
}

/*******************************************************************************
 * Private Functions - UI Creation (Landscape 320x240, Driver-Friendly)
 ******************************************************************************/

// New layout: Center = temp display, Left = A/C + Recirc, Right = Mode buttons, Bottom = Fan slider

static void create_center_temp_display(lv_obj_t *parent)
{
    // Center panel for temperature display (setpoint large, actual small below)
    lv_obj_t *temp_panel = lv_obj_create(parent);
    lv_obj_set_size(temp_panel, 150, 195);
    lv_obj_align(temp_panel, LV_ALIGN_TOP_MID, -20, 0);
    lv_obj_set_style_bg_color(temp_panel, UI_COLOR_PANEL, 0);
    lv_obj_set_style_border_width(temp_panel, 1, 0);
    lv_obj_set_style_border_color(temp_panel, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_radius(temp_panel, 8, 0);
    lv_obj_set_style_pad_all(temp_panel, 5, 0);
    lv_obj_clear_flag(temp_panel, LV_OBJ_FLAG_SCROLLABLE);
    
    // Large setpoint temperature (main focus) - larger font with decimal
    lbl_target_temp = lv_label_create(temp_panel);
    lv_obj_align(lbl_target_temp, LV_ALIGN_TOP_MID, 0, 5);
    lv_obj_set_style_text_font(lbl_target_temp, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(lbl_target_temp, UI_COLOR_HIGHLIGHT, 0);
    lv_label_set_text(lbl_target_temp, "22.0°C");
    
    // +/- buttons for temperature adjustment - moved down
    btn_temp_down = lv_btn_create(temp_panel);
    lv_obj_set_size(btn_temp_down, 65, 55);
    lv_obj_align(btn_temp_down, LV_ALIGN_TOP_LEFT, 0, 40);
    lv_obj_set_style_bg_color(btn_temp_down, UI_COLOR_COOL, 0);
    lv_obj_add_event_cb(btn_temp_down, btn_temp_down_handler, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_minus = lv_label_create(btn_temp_down);
    lv_label_set_text(lbl_minus, LV_SYMBOL_MINUS);
    lv_obj_set_style_text_font(lbl_minus, &lv_font_montserrat_22, 0);
    lv_obj_center(lbl_minus);
    
    btn_temp_up = lv_btn_create(temp_panel);
    lv_obj_set_size(btn_temp_up, 65, 55);
    lv_obj_align(btn_temp_up, LV_ALIGN_TOP_RIGHT, 0, 40);
    lv_obj_set_style_bg_color(btn_temp_up, UI_COLOR_HEAT, 0);
    lv_obj_add_event_cb(btn_temp_up, btn_temp_up_handler, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_plus = lv_label_create(btn_temp_up);
    lv_label_set_text(lbl_plus, LV_SYMBOL_PLUS);
    lv_obj_set_style_text_font(lbl_plus, &lv_font_montserrat_22, 0);
    lv_obj_center(lbl_plus);
    
    // Smaller actual cabin temperature - moved down
    lbl_cabin_temp = lv_label_create(temp_panel);
    lv_obj_align(lbl_cabin_temp, LV_ALIGN_TOP_MID, 0, 105);
    lv_obj_set_style_text_font(lbl_cabin_temp, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_cabin_temp, UI_COLOR_TEXT, 0);
    lv_label_set_text(lbl_cabin_temp, "Cabin: --.-°C");
    
    // Humidity below cabin temp
    lbl_humidity = lv_label_create(temp_panel);
    lv_obj_align(lbl_humidity, LV_ALIGN_TOP_MID, 0, 125);
    lv_obj_set_style_text_color(lbl_humidity, UI_COLOR_TEXT_DIM, 0);
    lv_label_set_text(lbl_humidity, "-- %RH");
}

static lv_obj_t* create_mode_button(lv_obj_t *parent, const char *text, 
                                    lv_event_cb_t handler, lv_coord_t x, lv_coord_t y,
                                    lv_coord_t w, lv_coord_t h)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_style_bg_color(btn, UI_COLOR_PANEL, 0);
    lv_obj_set_style_bg_color(btn, UI_COLOR_ACCENT, LV_STATE_CHECKED);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CHECKABLE);
    lv_obj_add_event_cb(btn, handler, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(lbl);
    
    return btn;
}

static void create_left_buttons(lv_obj_t *parent)
{
    // Left side: A/C and Recirc buttons (stacked vertically, same size as mode buttons)
    lv_coord_t btn_w = 48;
    lv_coord_t btn_h = 80;
    lv_coord_t gap = 1;
    
    // A/C button
    btn_ac = lv_btn_create(parent);
    lv_obj_set_size(btn_ac, btn_w, btn_h);
    lv_obj_align(btn_ac, LV_ALIGN_TOP_LEFT, -5, 5);
    lv_obj_set_style_bg_color(btn_ac, UI_COLOR_PANEL, 0);
    lv_obj_set_style_bg_color(btn_ac, UI_COLOR_COOL, LV_STATE_CHECKED);
    lv_obj_add_flag(btn_ac, LV_OBJ_FLAG_CHECKABLE);
    lv_obj_add_event_cb(btn_ac, btn_ac_handler, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t *lbl_ac = lv_label_create(btn_ac);
    lv_label_set_text(lbl_ac, "A/C");
    lv_obj_set_style_text_font(lbl_ac, &lv_font_montserrat_14, 0);
    lv_obj_center(lbl_ac);
    
    // Recirculation button
    btn_recirc = lv_btn_create(parent);
    lv_obj_set_size(btn_recirc, btn_w, btn_h);
    lv_obj_align(btn_recirc, LV_ALIGN_TOP_LEFT, -5, 5 + btn_h + gap);
    lv_obj_set_style_bg_color(btn_recirc, UI_COLOR_PANEL, 0);
    lv_obj_set_style_bg_color(btn_recirc, UI_COLOR_ON, LV_STATE_CHECKED);
    lv_obj_add_flag(btn_recirc, LV_OBJ_FLAG_CHECKABLE);
    lv_obj_add_event_cb(btn_recirc, btn_recirc_handler, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t *lbl_recirc = lv_label_create(btn_recirc);
    lv_label_set_text(lbl_recirc, LV_SYMBOL_LOOP);
    lv_obj_set_style_text_font(lbl_recirc, &lv_font_montserrat_16, 0);
    lv_obj_center(lbl_recirc);
}

static void create_right_mode_buttons(lv_obj_t *parent)
{
    // Right side: Auto, Defrost, Center (face), Legs (foot) - 2x2 grid
    lv_coord_t x_start = 210;
    lv_coord_t y_start = 5;
    lv_coord_t btn_w = 48;
    lv_coord_t btn_h = 70;
    lv_coord_t gap = 4;
    
    // Row 1: AUTO, DEF
    btn_mode_auto = create_mode_button(parent, "AUTO", btn_mode_auto_handler, 
                                       x_start, y_start, btn_w, btn_h);
    btn_mode_defrost = create_mode_button(parent, "DEF", btn_mode_defrost_handler, 
                                          x_start + btn_w + gap, y_start, btn_w, btn_h);
    
    // Row 2: CENTER (face), LEGS (foot)
    btn_mode_face = create_mode_button(parent, LV_SYMBOL_UP, btn_mode_face_handler, 
                                       x_start, y_start + btn_h + gap, btn_w, btn_h);
    btn_mode_foot = create_mode_button(parent, LV_SYMBOL_DOWN, btn_mode_foot_handler, 
                                       x_start + btn_w + gap, y_start + btn_h + gap, btn_w, btn_h);
    
    // Set AUTO as initially checked
    lv_obj_add_state(btn_mode_auto, LV_STATE_CHECKED);
}

static void create_fan_control(lv_obj_t *parent)
{
    // Fan control panel - full width at very bottom
    lv_obj_t *fan_panel = lv_obj_create(parent);
    lv_obj_set_size(fan_panel, 320, 38);
    lv_obj_align(fan_panel, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(fan_panel, UI_COLOR_PANEL, 0);
    lv_obj_set_style_border_width(fan_panel, 0, 0);
    lv_obj_set_style_radius(fan_panel, 0, 0);
    lv_obj_set_style_pad_all(fan_panel, 5, 0);
    lv_obj_clear_flag(fan_panel, LV_OBJ_FLAG_SCROLLABLE);
    
    // Fan icon
    lv_obj_t *lbl_fan_icon = lv_label_create(fan_panel);
    lv_obj_align(lbl_fan_icon, LV_ALIGN_LEFT_MID, 0, 0);
    lv_label_set_text(lbl_fan_icon, LV_SYMBOL_REFRESH);
    lv_obj_set_style_text_color(lbl_fan_icon, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(lbl_fan_icon, &lv_font_montserrat_16, 0);
    
    // Fan slider - wider for easier touch
    slider_fan = lv_slider_create(fan_panel);
    lv_obj_set_size(slider_fan, 220, 22);
    lv_obj_align(slider_fan, LV_ALIGN_LEFT_MID, 25, 0);
    lv_slider_set_range(slider_fan, 0, 5);  // 0-5 for OFF, Low, Med-Lo, Med, Med-Hi, High (no auto)
    lv_slider_set_value(slider_fan, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider_fan, lv_color_hex(0x2a2a4e), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider_fan, UI_COLOR_HIGHLIGHT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider_fan, UI_COLOR_TEXT, LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider_fan, 8, LV_PART_KNOB);  // Larger knob
    lv_obj_add_event_cb(slider_fan, slider_fan_handler, LV_EVENT_VALUE_CHANGED, NULL);
    
    // Fan speed label
    lbl_fan_speed = lv_label_create(fan_panel);
    lv_obj_align(lbl_fan_speed, LV_ALIGN_RIGHT_MID, -5, 0);
    lv_obj_set_style_text_color(lbl_fan_speed, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(lbl_fan_speed, &lv_font_montserrat_14, 0);
    lv_label_set_text(lbl_fan_speed, "OFF");
}

static void create_hidden_labels(lv_obj_t *parent)
{
    // Create hidden dummy labels for data that's shown in debug screen
    // This avoids NULL pointer crashes in hvac_ui_update when debug tab not visible
    lbl_duct_temp = lv_label_create(parent);
    lv_obj_add_flag(lbl_duct_temp, LV_OBJ_FLAG_HIDDEN);
    // Note: Valve position labels are created in debug screen now
}

/*******************************************************************************
 * Private Functions - UI Update
 ******************************************************************************/

static void update_mode_buttons(hvac_mode_t mode)
{
    // Clear all checked states
    lv_obj_clear_state(btn_mode_face, LV_STATE_CHECKED);
    lv_obj_clear_state(btn_mode_foot, LV_STATE_CHECKED);
    lv_obj_clear_state(btn_mode_defrost, LV_STATE_CHECKED);
    lv_obj_clear_state(btn_mode_auto, LV_STATE_CHECKED);
    
    // Set the active mode
    switch (mode) {
        case HVAC_MODE_OFF:
            // No button checked when OFF
            break;
        case HVAC_MODE_FACE:
        case HVAC_MODE_FACE_FOOT:
            lv_obj_add_state(btn_mode_face, LV_STATE_CHECKED);
            break;
        case HVAC_MODE_FOOT:
            lv_obj_add_state(btn_mode_foot, LV_STATE_CHECKED);
            break;
        case HVAC_MODE_DEFROST:
        case HVAC_MODE_DEFROST_FOOT:
            lv_obj_add_state(btn_mode_defrost, LV_STATE_CHECKED);
            break;
        case HVAC_MODE_AUTO:
            lv_obj_add_state(btn_mode_auto, LV_STATE_CHECKED);
            break;
        default:
            break;
    }
    
    // Disable/enable fan slider based on auto mode
    if (mode == HVAC_MODE_AUTO) {
        lv_obj_add_state(slider_fan, LV_STATE_DISABLED);
        lv_obj_set_style_opa(slider_fan, LV_OPA_50, 0);
        lv_label_set_text(lbl_fan_speed, "Auto");
    } else {
        lv_obj_clear_state(slider_fan, LV_STATE_DISABLED);
        lv_obj_set_style_opa(slider_fan, LV_OPA_COVER, 0);
    }
}

static void update_fan_slider(hvac_fan_speed_t level)
{
    int32_t slider_val = 0;
    const char *text = "OFF";
    
    switch (level) {
        case HVAC_FAN_OFF:
            slider_val = 0;
            text = "OFF";
            break;
        case HVAC_FAN_LOW:
            slider_val = 1;
            text = "Low";
            break;
        case HVAC_FAN_MEDIUM_LOW:
            slider_val = 2;
            text = "M-Lo";
            break;
        case HVAC_FAN_MEDIUM:
            slider_val = 3;
            text = "Med";
            break;
        case HVAC_FAN_MEDIUM_HIGH:
            slider_val = 4;
            text = "M-Hi";
            break;
        case HVAC_FAN_HIGH:
            slider_val = 5;
            text = "High";
            break;
        case HVAC_FAN_AUTO:
            // Don't update slider in auto mode - it's disabled
            text = "Auto";
            lv_label_set_text(lbl_fan_speed, text);
            return;
        default:
            break;
    }
    
    lv_slider_set_value(slider_fan, slider_val, LV_ANIM_OFF);
    lv_label_set_text(lbl_fan_speed, text);
}

static void create_debug_screen(lv_obj_t *parent)
{
    // Debug screen layout for landscape (320x~200 usable)
    // Clean layout: Sensors left, Valves center, System right, Buttons bottom
    
    // === LEFT COLUMN: Temperatures ===
    lv_obj_t *panel_temps = lv_obj_create(parent);
    lv_obj_set_size(panel_temps, 100, 120);
    lv_obj_align(panel_temps, LV_ALIGN_TOP_LEFT, 2, 2);
    lv_obj_set_style_bg_color(panel_temps, UI_COLOR_PANEL, 0);
    lv_obj_set_style_border_width(panel_temps, 1, 0);
    lv_obj_set_style_border_color(panel_temps, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_radius(panel_temps, 6, 0);
    lv_obj_set_style_pad_all(panel_temps, 5, 0);
    lv_obj_clear_flag(panel_temps, LV_OBJ_FLAG_SCROLLABLE);
    
    lv_obj_t *lbl_temps_title = lv_label_create(panel_temps);
    lv_obj_align(lbl_temps_title, LV_ALIGN_TOP_MID, 0, 0);
    lv_label_set_text(lbl_temps_title, "Temps");
    lv_obj_set_style_text_color(lbl_temps_title, UI_COLOR_HIGHLIGHT, 0);
    lv_obj_set_style_text_font(lbl_temps_title, &lv_font_montserrat_14, 0);
    
    lbl_raw_sensors = lv_label_create(panel_temps);
    lv_obj_align(lbl_raw_sensors, LV_ALIGN_TOP_LEFT, 0, 20);
    lv_label_set_text(lbl_raw_sensors, "Cabin: --\nCoolant: --\nDuct: --\nHumid: --");
    lv_obj_set_style_text_color(lbl_raw_sensors, UI_COLOR_TEXT, 0);
    
    // === CENTER COLUMN: Valves ===
    lv_obj_t *panel_valves = lv_obj_create(parent);
    lv_obj_set_size(panel_valves, 100, 120);
    lv_obj_align(panel_valves, LV_ALIGN_TOP_MID, 0, 2);
    lv_obj_set_style_bg_color(panel_valves, UI_COLOR_PANEL, 0);
    lv_obj_set_style_border_width(panel_valves, 1, 0);
    lv_obj_set_style_border_color(panel_valves, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_radius(panel_valves, 6, 0);
    lv_obj_set_style_pad_all(panel_valves, 5, 0);
    lv_obj_clear_flag(panel_valves, LV_OBJ_FLAG_SCROLLABLE);
    
    lv_obj_t *lbl_valves_title = lv_label_create(panel_valves);
    lv_obj_align(lbl_valves_title, LV_ALIGN_TOP_MID, 0, 0);
    lv_label_set_text(lbl_valves_title, "Valves");
    lv_obj_set_style_text_color(lbl_valves_title, UI_COLOR_HIGHLIGHT, 0);
    lv_obj_set_style_text_font(lbl_valves_title, &lv_font_montserrat_14, 0);
    
    // Valve labels with positions (5 valves, 18px spacing)
    lv_obj_t *lbl_v1 = lv_label_create(panel_valves);
    lv_obj_align(lbl_v1, LV_ALIGN_TOP_LEFT, 0, 18);
    lv_label_set_text(lbl_v1, "C1:");
    lv_obj_set_style_text_color(lbl_v1, UI_COLOR_TEXT_DIM, 0);
    
    lbl_center_valve_pos = lv_label_create(panel_valves);
    lv_obj_align(lbl_center_valve_pos, LV_ALIGN_TOP_RIGHT, 0, 18);
    lv_label_set_text(lbl_center_valve_pos, "--%");
    lv_obj_set_style_text_color(lbl_center_valve_pos, UI_COLOR_TEXT, 0);
    
    lv_obj_t *lbl_v1b = lv_label_create(panel_valves);
    lv_obj_align(lbl_v1b, LV_ALIGN_TOP_LEFT, 0, 36);
    lv_label_set_text(lbl_v1b, "C2:");
    lv_obj_set_style_text_color(lbl_v1b, UI_COLOR_TEXT_DIM, 0);
    
    lbl_center2_valve_pos = lv_label_create(panel_valves);
    lv_obj_align(lbl_center2_valve_pos, LV_ALIGN_TOP_RIGHT, 0, 36);
    lv_label_set_text(lbl_center2_valve_pos, "--%");
    lv_obj_set_style_text_color(lbl_center2_valve_pos, UI_COLOR_TEXT, 0);
    
    lv_obj_t *lbl_v2 = lv_label_create(panel_valves);
    lv_obj_align(lbl_v2, LV_ALIGN_TOP_LEFT, 0, 54);
    lv_label_set_text(lbl_v2, "Foot:");
    lv_obj_set_style_text_color(lbl_v2, UI_COLOR_TEXT_DIM, 0);
    
    lbl_foot_valve_pos = lv_label_create(panel_valves);
    lv_obj_align(lbl_foot_valve_pos, LV_ALIGN_TOP_RIGHT, 0, 54);
    lv_label_set_text(lbl_foot_valve_pos, "--%");
    lv_obj_set_style_text_color(lbl_foot_valve_pos, UI_COLOR_TEXT, 0);
    
    lv_obj_t *lbl_v3 = lv_label_create(panel_valves);
    lv_obj_align(lbl_v3, LV_ALIGN_TOP_LEFT, 0, 72);
    lv_label_set_text(lbl_v3, "WS:");
    lv_obj_set_style_text_color(lbl_v3, UI_COLOR_TEXT_DIM, 0);
    
    lbl_windscreen_valve_pos = lv_label_create(panel_valves);
    lv_obj_align(lbl_windscreen_valve_pos, LV_ALIGN_TOP_RIGHT, 0, 72);
    lv_label_set_text(lbl_windscreen_valve_pos, "--%");
    lv_obj_set_style_text_color(lbl_windscreen_valve_pos, UI_COLOR_TEXT, 0);
    
    lv_obj_t *lbl_v4 = lv_label_create(panel_valves);
    lv_obj_align(lbl_v4, LV_ALIGN_TOP_LEFT, 0, 90);
    lv_label_set_text(lbl_v4, "Cool:");
    lv_obj_set_style_text_color(lbl_v4, UI_COLOR_TEXT_DIM, 0);
    
    lbl_coolant_valve_pos = lv_label_create(panel_valves);
    lv_obj_align(lbl_coolant_valve_pos, LV_ALIGN_TOP_RIGHT, 0, 90);
    lv_label_set_text(lbl_coolant_valve_pos, "--%");
    lv_obj_set_style_text_color(lbl_coolant_valve_pos, UI_COLOR_TEXT, 0);
    
    // === RIGHT COLUMN: System & Thermal ===
    lv_obj_t *panel_system = lv_obj_create(parent);
    lv_obj_set_size(panel_system, 100, 120);
    lv_obj_align(panel_system, LV_ALIGN_TOP_RIGHT, -2, 2);
    lv_obj_set_style_bg_color(panel_system, UI_COLOR_PANEL, 0);
    lv_obj_set_style_border_width(panel_system, 1, 0);
    lv_obj_set_style_border_color(panel_system, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_radius(panel_system, 6, 0);
    lv_obj_set_style_pad_all(panel_system, 5, 0);
    lv_obj_clear_flag(panel_system, LV_OBJ_FLAG_SCROLLABLE);
    
    lv_obj_t *lbl_system_title = lv_label_create(panel_system);
    lv_obj_align(lbl_system_title, LV_ALIGN_TOP_MID, 0, 0);
    lv_label_set_text(lbl_system_title, "System");
    lv_obj_set_style_text_color(lbl_system_title, UI_COLOR_HIGHLIGHT, 0);
    lv_obj_set_style_text_font(lbl_system_title, &lv_font_montserrat_14, 0);
    
    lbl_system_info = lv_label_create(panel_system);
    lv_obj_align(lbl_system_info, LV_ALIGN_TOP_LEFT, 0, 20);
    lv_label_set_text(lbl_system_info, "Uptime: --\nHeap: --");
    lv_obj_set_style_text_color(lbl_system_info, UI_COLOR_TEXT, 0);
    
    lbl_thermal_status = lv_label_create(panel_system);
    lv_obj_align(lbl_thermal_status, LV_ALIGN_TOP_LEFT, 0, 55);
    lv_label_set_text(lbl_thermal_status, "Thermal:\n--");
    lv_obj_set_style_text_color(lbl_thermal_status, UI_COLOR_TEXT_DIM, 0);
    
    // === BOTTOM: Control Buttons ===
    btn_onewire_read = lv_btn_create(parent);
    lv_obj_set_size(btn_onewire_read, 95, 45);
    lv_obj_align(btn_onewire_read, LV_ALIGN_BOTTOM_MID, 0, -5);
    lv_obj_set_style_bg_color(btn_onewire_read, UI_COLOR_ACCENT, 0);
    lv_obj_add_event_cb(btn_onewire_read, btn_onewire_read_handler, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_onewire = lv_label_create(btn_onewire_read);
    lv_label_set_text(lbl_onewire, "Read 1W");
    lv_obj_set_style_text_font(lbl_onewire, &lv_font_montserrat_14, 0);
    lv_obj_center(lbl_onewire);

    btn_reset_cal = lv_btn_create(parent);
    lv_obj_set_size(btn_reset_cal, 95, 45);
    lv_obj_align(btn_reset_cal, LV_ALIGN_BOTTOM_LEFT, 5, -5);
    lv_obj_set_style_bg_color(btn_reset_cal, UI_COLOR_WARNING, 0);
    lv_obj_add_event_cb(btn_reset_cal, btn_reset_cal_handler, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_reset = lv_label_create(btn_reset_cal);
    lv_label_set_text(lbl_reset, "Reset Cal");
    lv_obj_set_style_text_font(lbl_reset, &lv_font_montserrat_14, 0);
    lv_obj_center(lbl_reset);
    
    btn_save_cal = lv_btn_create(parent);
    lv_obj_set_size(btn_save_cal, 95, 45);
    lv_obj_align(btn_save_cal, LV_ALIGN_BOTTOM_RIGHT, -5, -5);
    lv_obj_set_style_bg_color(btn_save_cal, UI_COLOR_ON, 0);
    lv_obj_add_event_cb(btn_save_cal, btn_save_cal_handler, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_save = lv_label_create(btn_save_cal);
    lv_label_set_text(lbl_save, "Save Cal");
    lv_obj_set_style_text_font(lbl_save, &lv_font_montserrat_14, 0);
    lv_obj_center(lbl_save);
}

/*******************************************************************************
 * Public Functions
 ******************************************************************************/

esp_err_t hvac_ui_init(void)
{
    ESP_LOGI(TAG, "Initializing HVAC UI");
    
    // Create main screen
    scr_main = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_main, UI_COLOR_BG, 0);
    
    // Create tabview with hidden tabs (swipe to navigate)
    tabview = lv_tabview_create(scr_main, LV_DIR_BOTTOM, 0);  // 0 height hides tab bar
    lv_obj_set_style_bg_color(tabview, UI_COLOR_BG, 0);
    
    // Create tabs
    tab_main = lv_tabview_add_tab(tabview, "Main");
    tab_debug = lv_tabview_add_tab(tabview, "Debug");
    
    // Disable scrolling on tabs (but allow swipe between tabs)
    lv_obj_clear_flag(tab_main, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(tab_debug, LV_OBJ_FLAG_SCROLLABLE);
    
    // Create main tab UI components
    // Layout: Left=A/C+Recirc, Center=Temp, Right=Modes, Bottom=Fan
    create_left_buttons(tab_main);
    create_center_temp_display(tab_main);
    create_right_mode_buttons(tab_main);
    create_fan_control(tab_main);
    create_hidden_labels(tab_main);
    
    // Create debug tab UI
    create_debug_screen(tab_debug);
    
    // Load the screen
    lv_scr_load(scr_main);
    
    ESP_LOGI(TAG, "HVAC UI initialized");
    return ESP_OK;
}

void hvac_ui_update(void)
{
    static hvac_state_t last_state = {0};
    hvac_state_t state;
    
    hvac_controller_get_state(&state);
    
    char buf[128];
    
    // Update cabin temperature
    if (!isnan(state.cabin_temp_c)) {
        if (state.temp_unit == TEMP_UNIT_CELSIUS) {
            snprintf(buf, sizeof(buf), "Cabin: %.1f°C", state.cabin_temp_c);
        } else {
            snprintf(buf, sizeof(buf), "Cabin: %.1f°F", hvac_controller_c_to_f(state.cabin_temp_c));
        }
        lv_label_set_text(lbl_cabin_temp, buf);
    } else {
        lv_label_set_text(lbl_cabin_temp, "Cabin: --.-°C");
    }
    
    // Update humidity
    if (!isnan(state.cabin_humidity)) {
        snprintf(buf, sizeof(buf), "%.0f %%RH", state.cabin_humidity);
        lv_label_set_text(lbl_humidity, buf);
    } else {
        lv_label_set_text(lbl_humidity, "-- %RH");
    }
    
    // Update target temperature (large setpoint display with decimal)
    if (state.temp_unit == TEMP_UNIT_CELSIUS) {
        snprintf(buf, sizeof(buf), "%.1f°C", state.target_temp_c);
    } else {
        snprintf(buf, sizeof(buf), "%.1f°F", hvac_controller_c_to_f(state.target_temp_c));
    }
    lv_label_set_text(lbl_target_temp, buf);
    
    // Color setpoint based on heating/cooling state
    if (state.is_heating) {
        lv_obj_set_style_text_color(lbl_target_temp, UI_COLOR_HEAT, 0);
    } else if (state.is_cooling) {
        lv_obj_set_style_text_color(lbl_target_temp, UI_COLOR_COOL, 0);
    } else {
        lv_obj_set_style_text_color(lbl_target_temp, UI_COLOR_HIGHLIGHT, 0);
    }
    
    // Update mode buttons only if changed
    if (state.mode != last_state.mode) {
        update_mode_buttons(state.mode);
    }
    
    // Update fan slider only if changed
    if (state.fan_level != last_state.fan_level) {
        update_fan_slider(state.fan_level);
    }
    
    // Update A/C button - show CHECKED (blue) when compressor is actually running
    // This reflects automatic A/C (defrost, warmup, high humidity) not just user toggle
    if (state.ac_compressor_on) {
        lv_obj_add_state(btn_ac, LV_STATE_CHECKED);
    } else {
        lv_obj_clear_state(btn_ac, LV_STATE_CHECKED);
    }
    
    // Update recirculation button
    if (state.recirculate) {
        lv_obj_add_state(btn_recirc, LV_STATE_CHECKED);
    } else {
        lv_obj_clear_state(btn_recirc, LV_STATE_CHECKED);
    }
    
    // Update duct temperature
    if (!isnan(state.duct_temp_c)) {
        snprintf(buf, sizeof(buf), "%.0f°C", state.duct_temp_c);
        lv_label_set_text(lbl_duct_temp, buf);
    } else {
        lv_label_set_text(lbl_duct_temp, "--°C");
    }
    
    // Update valve positions
    snprintf(buf, sizeof(buf), "%d%%", state.center_valve_pos);
    lv_label_set_text(lbl_center_valve_pos, buf);
    
    snprintf(buf, sizeof(buf), "%d%%", state.center2_valve_pos);
    lv_label_set_text(lbl_center2_valve_pos, buf);
    
    snprintf(buf, sizeof(buf), "%d%%", state.foot_valve_pos);
    lv_label_set_text(lbl_foot_valve_pos, buf);
    
    snprintf(buf, sizeof(buf), "%d%%", state.windscreen_valve_pos);
    lv_label_set_text(lbl_windscreen_valve_pos, buf);
    
    snprintf(buf, sizeof(buf), "%d%%", state.coolant_valve_pos);
    lv_label_set_text(lbl_coolant_valve_pos, buf);

    if (state.sensor_fault && !last_sensor_fault) {
        hvac_ui_show_error("Sensor fault detected. Check sensor wiring.");
    } else if (!state.sensor_fault && last_sensor_fault) {
        hvac_ui_hide_message();
    }
    if (state.valve_fault && !last_valve_fault && !state.sensor_fault) {
        hvac_ui_show_error("Valve stall detected. Inspect actuators.");
    } else if (!state.valve_fault && last_valve_fault && !state.sensor_fault) {
        hvac_ui_hide_message();
    }
    last_sensor_fault = state.sensor_fault;
    last_valve_fault = state.valve_fault;
    
    // Update debug screen (only when visible)
    if (lv_tabview_get_tab_act(tabview) == 1) {
        // Temps panel - raw sensor readings
        snprintf(buf, sizeof(buf),
            "Cabin: %.1f°C\nDuct: %.1f°C\nHumid: %.0f%%",
            state.cabin_temp_c,
            state.duct_temp_c, state.cabin_humidity);
        lv_label_set_text(lbl_raw_sensors, buf);
        
        // Valve positions are already updated above (always visible labels now)
        
        // System info
        uint32_t uptime_sec = xTaskGetTickCount() * portTICK_PERIOD_MS / 1000;
        uint32_t free_heap = esp_get_free_heap_size();
        snprintf(buf, sizeof(buf),
                "Up: %luh%lum\nHeap: %luKB",
                uptime_sec / 3600, (uptime_sec % 3600) / 60,
                free_heap / 1024);
        lv_label_set_text(lbl_system_info, buf);
        
        // Thermal adaptation status
        float heating_rate, cooling_rate, adaptive_gain;
        uint32_t sample_count;
        bool calibrated;
        hvac_controller_get_thermal_status(&heating_rate, &cooling_rate, &adaptive_gain, 
                                          &sample_count, &calibrated);
        
        snprintf(buf, sizeof(buf), 
                "Thermal:\n%.2f/%.2f %s",
                heating_rate, cooling_rate,
                calibrated ? "CAL" : "LRN");
        lv_label_set_text(lbl_thermal_status, buf);
    }
    
    // Save state for comparison
    last_state = state;
}

lv_obj_t* hvac_ui_get_screen(void)
{
    return scr_main;
}

void hvac_ui_set_brightness(uint8_t brightness)
{
    hvac_hw_backlight_set(brightness);
}

void hvac_ui_show_error(const char *message)
{
    if (msg_box != NULL) {
        lv_msgbox_close(msg_box);
        msg_box = NULL;
    }
    
    msg_box = lv_msgbox_create(NULL, "Error", message, NULL, true);
    lv_obj_center(msg_box);
    // Register handler to clear msg_box when user closes it
    lv_obj_add_event_cb(msg_box, msgbox_close_handler, LV_EVENT_DELETE, NULL);
}

void hvac_ui_show_info(const char *message)
{
    if (msg_box != NULL) {
        lv_msgbox_close(msg_box);
        msg_box = NULL;
    }
    
    msg_box = lv_msgbox_create(NULL, "Info", message, NULL, true);
    lv_obj_center(msg_box);
    // Register handler to clear msg_box when user closes it
    lv_obj_add_event_cb(msg_box, msgbox_close_handler, LV_EVENT_DELETE, NULL);
}

void hvac_ui_hide_message(void)
{
    if (msg_box != NULL) {
        lv_msgbox_close(msg_box);
        msg_box = NULL;
    }
}