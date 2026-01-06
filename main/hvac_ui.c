/**
 * @file hvac_ui.c
 * @brief LVGL-based HVAC User Interface Implementation
 */

#include "hvac_ui.h"
#include "hvac_controller.h"
#include "hvac_hw_config.h"

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

// Temperature display
static lv_obj_t *lbl_cabin_temp = NULL;
static lv_obj_t *lbl_target_temp = NULL;
static lv_obj_t *lbl_humidity = NULL;
static lv_obj_t *arc_temp = NULL;

// Mode buttons
static lv_obj_t *btn_mode_off = NULL;
static lv_obj_t *btn_mode_face = NULL;
static lv_obj_t *btn_mode_foot = NULL;
static lv_obj_t *btn_mode_defrost = NULL;
static lv_obj_t *btn_mode_auto = NULL;

// Fan control
static lv_obj_t *slider_fan = NULL;
static lv_obj_t *lbl_fan_speed = NULL;

// Temperature control
static lv_obj_t *btn_temp_up = NULL;
static lv_obj_t *btn_temp_down = NULL;

// Feature buttons
static lv_obj_t *btn_ac = NULL;
static lv_obj_t *btn_recirc = NULL;

// Status indicators
static lv_obj_t *lbl_coolant_temp = NULL;
static lv_obj_t *lbl_duct_temp = NULL;
static lv_obj_t *lbl_status = NULL;

// Valve position indicators
static lv_obj_t *lbl_center_valve_pos = NULL;
static lv_obj_t *lbl_foot_valve_pos = NULL;
static lv_obj_t *lbl_windscreen_valve_pos = NULL;
static lv_obj_t *lbl_coolant_valve_pos = NULL;

// Debug screen objects
static lv_obj_t *lbl_thermal_status = NULL;
static lv_obj_t *lbl_raw_sensors = NULL;
static lv_obj_t *lbl_system_info = NULL;
static lv_obj_t *btn_reset_cal = NULL;
static lv_obj_t *btn_save_cal = NULL;

// Message box
static lv_obj_t *msg_box = NULL;

// Fault tracking
static bool last_sensor_fault = false;
static bool last_valve_fault = false;

/*******************************************************************************
 * Private Functions - Event Handlers
 ******************************************************************************/

static void btn_mode_off_handler(lv_event_t *e)
{
    hvac_controller_off();
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
    } else if (value == 5) {
        level = HVAC_FAN_HIGH;
    } else {
        level = HVAC_FAN_AUTO;
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
 * Private Functions - UI Creation
 ******************************************************************************/
static void create_temp_display(lv_obj_t *parent)
{
    // Temperature arc background
    arc_temp = lv_arc_create(parent);
    lv_obj_set_size(arc_temp, 140, 140);
    lv_obj_align(arc_temp, LV_ALIGN_TOP_MID, 0, 10);
    lv_arc_set_rotation(arc_temp, 135);
    lv_arc_set_bg_angles(arc_temp, 0, 270);
    lv_arc_set_range(arc_temp, 16, 30);
    lv_arc_set_value(arc_temp, 22);
    lv_obj_remove_style(arc_temp, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(arc_temp, LV_OBJ_FLAG_CLICKABLE);
    
    // Set arc colors
    lv_obj_set_style_arc_color(arc_temp, UI_COLOR_OFF, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc_temp, UI_COLOR_HIGHLIGHT, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(arc_temp, 10, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc_temp, 10, LV_PART_INDICATOR);
    
    // Cabin temperature (large, center)
    lbl_cabin_temp = lv_label_create(parent);
    lv_obj_align(lbl_cabin_temp, LV_ALIGN_TOP_MID, 0, 45);
    lv_obj_set_style_text_font(lbl_cabin_temp, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_cabin_temp, UI_COLOR_TEXT, 0);
    lv_label_set_text(lbl_cabin_temp, "--.-°C");
    
    // "Cabin" label
    lv_obj_t *lbl_cabin = lv_label_create(parent);
    lv_obj_align(lbl_cabin, LV_ALIGN_TOP_MID, 0, 80);
    lv_obj_set_style_text_color(lbl_cabin, UI_COLOR_TEXT_DIM, 0);
    lv_label_set_text(lbl_cabin, "Cabin");
    
    // Humidity
    lbl_humidity = lv_label_create(parent);
    lv_obj_align(lbl_humidity, LV_ALIGN_TOP_MID, 0, 100);
    lv_obj_set_style_text_color(lbl_humidity, UI_COLOR_TEXT_DIM, 0);
    lv_label_set_text(lbl_humidity, "-- %RH");
    
    // Target temperature with +/- buttons
    btn_temp_down = lv_btn_create(parent);
    lv_obj_set_size(btn_temp_down, 40, 40);
    lv_obj_align(btn_temp_down, LV_ALIGN_TOP_LEFT, 10, 45);
    lv_obj_set_style_bg_color(btn_temp_down, UI_COLOR_COOL, 0);
    lv_obj_add_event_cb(btn_temp_down, btn_temp_down_handler, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t *lbl_minus = lv_label_create(btn_temp_down);
    lv_label_set_text(lbl_minus, LV_SYMBOL_MINUS);
    lv_obj_center(lbl_minus);
    
    btn_temp_up = lv_btn_create(parent);
    lv_obj_set_size(btn_temp_up, 40, 40);
    lv_obj_align(btn_temp_up, LV_ALIGN_TOP_RIGHT, -10, 45);
    lv_obj_set_style_bg_color(btn_temp_up, UI_COLOR_HEAT, 0);
    lv_obj_add_event_cb(btn_temp_up, btn_temp_up_handler, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t *lbl_plus = lv_label_create(btn_temp_up);
    lv_label_set_text(lbl_plus, LV_SYMBOL_PLUS);
    lv_obj_center(lbl_plus);
    
    // Target temp label
    lbl_target_temp = lv_label_create(parent);
    lv_obj_align(lbl_target_temp, LV_ALIGN_TOP_MID, 0, 125);
    lv_obj_set_style_text_font(lbl_target_temp, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_target_temp, UI_COLOR_HIGHLIGHT, 0);
    lv_label_set_text(lbl_target_temp, "Set: 22.0°C");
}

static lv_obj_t* create_mode_button(lv_obj_t *parent, const char *text, 
                                    lv_event_cb_t handler, lv_coord_t x, lv_coord_t y)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 44, 36);
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

static void create_mode_buttons(lv_obj_t *parent)
{
    // Mode buttons row
    lv_coord_t y_pos = 155;
    lv_coord_t x_start = 5;
    lv_coord_t x_step = 47;
    
    btn_mode_off = create_mode_button(parent, "OFF", btn_mode_off_handler, x_start, y_pos);
    btn_mode_face = create_mode_button(parent, LV_SYMBOL_UP, btn_mode_face_handler, x_start + x_step, y_pos);
    btn_mode_foot = create_mode_button(parent, LV_SYMBOL_DOWN, btn_mode_foot_handler, x_start + x_step*2, y_pos);
    btn_mode_defrost = create_mode_button(parent, "DEF", btn_mode_defrost_handler, x_start + x_step*3, y_pos);
    btn_mode_auto = create_mode_button(parent, "AUTO", btn_mode_auto_handler, x_start + x_step*4, y_pos);
    
    // Set OFF as initially checked
    lv_obj_add_state(btn_mode_off, LV_STATE_CHECKED);
}

static void create_fan_control(lv_obj_t *parent)
{
    // Fan icon
    lv_obj_t *lbl_fan_icon = lv_label_create(parent);
    lv_obj_align(lbl_fan_icon, LV_ALIGN_BOTTOM_LEFT, 10, -45);
    lv_label_set_text(lbl_fan_icon, LV_SYMBOL_REFRESH);
    lv_obj_set_style_text_color(lbl_fan_icon, UI_COLOR_TEXT, 0);
    
    // Fan slider
    slider_fan = lv_slider_create(parent);
    lv_obj_set_size(slider_fan, 150, 20);
    lv_obj_align(slider_fan, LV_ALIGN_BOTTOM_LEFT, 35, -45);
    lv_slider_set_range(slider_fan, 0, 6);
    lv_slider_set_value(slider_fan, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider_fan, UI_COLOR_PANEL, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider_fan, UI_COLOR_HIGHLIGHT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider_fan, UI_COLOR_TEXT, LV_PART_KNOB);
    lv_obj_add_event_cb(slider_fan, slider_fan_handler, LV_EVENT_VALUE_CHANGED, NULL);
    
    // Fan speed label
    lbl_fan_speed = lv_label_create(parent);
    lv_obj_align(lbl_fan_speed, LV_ALIGN_BOTTOM_RIGHT, -10, -45);
    lv_obj_set_style_text_color(lbl_fan_speed, UI_COLOR_TEXT, 0);
    lv_label_set_text(lbl_fan_speed, "OFF");
}

static void create_feature_buttons(lv_obj_t *parent)
{
    // A/C button
    btn_ac = lv_btn_create(parent);
    lv_obj_set_size(btn_ac, 50, 30);
    lv_obj_align(btn_ac, LV_ALIGN_BOTTOM_LEFT, 10, -10);
    lv_obj_set_style_bg_color(btn_ac, UI_COLOR_PANEL, 0);
    lv_obj_set_style_bg_color(btn_ac, UI_COLOR_COOL, LV_STATE_CHECKED);
    lv_obj_add_flag(btn_ac, LV_OBJ_FLAG_CHECKABLE);
    lv_obj_add_event_cb(btn_ac, btn_ac_handler, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t *lbl_ac = lv_label_create(btn_ac);
    lv_label_set_text(lbl_ac, "A/C");
    lv_obj_center(lbl_ac);
    
    // Recirculation button
    btn_recirc = lv_btn_create(parent);
    lv_obj_set_size(btn_recirc, 50, 30);
    lv_obj_align(btn_recirc, LV_ALIGN_BOTTOM_LEFT, 70, -10);
    lv_obj_set_style_bg_color(btn_recirc, UI_COLOR_PANEL, 0);
    lv_obj_set_style_bg_color(btn_recirc, UI_COLOR_ON, LV_STATE_CHECKED);
    lv_obj_add_flag(btn_recirc, LV_OBJ_FLAG_CHECKABLE);
    lv_obj_add_event_cb(btn_recirc, btn_recirc_handler, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t *lbl_recirc = lv_label_create(btn_recirc);
    lv_label_set_text(lbl_recirc, LV_SYMBOL_LOOP);
    lv_obj_center(lbl_recirc);
}

static void create_status_panel(lv_obj_t *parent)
{
    // Status panel on the right
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_set_size(panel, 100, 60);
    lv_obj_align(panel, LV_ALIGN_BOTTOM_RIGHT, -5, -5);
    lv_obj_set_style_bg_color(panel, UI_COLOR_PANEL, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_radius(panel, 5, 0);
    lv_obj_set_style_pad_all(panel, 5, 0);
    
    // Coolant temp
    lv_obj_t *lbl_coolant_icon = lv_label_create(panel);
    lv_obj_align(lbl_coolant_icon, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_label_set_text(lbl_coolant_icon, LV_SYMBOL_WARNING);
    lv_obj_set_style_text_color(lbl_coolant_icon, UI_COLOR_HEAT, 0);
    lv_obj_set_style_text_font(lbl_coolant_icon, &lv_font_montserrat_14, 0);
    
    lbl_coolant_temp = lv_label_create(panel);
    lv_obj_align(lbl_coolant_temp, LV_ALIGN_TOP_LEFT, 15, 0);
    lv_label_set_text(lbl_coolant_temp, "--°C");
    lv_obj_set_style_text_color(lbl_coolant_temp, UI_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_font(lbl_coolant_temp, &lv_font_montserrat_14, 0);
    
    // Duct temp
    lv_obj_t *lbl_duct_icon = lv_label_create(panel);
    lv_obj_align(lbl_duct_icon, LV_ALIGN_TOP_LEFT, 0, 15);
    lv_label_set_text(lbl_duct_icon, LV_SYMBOL_CHARGE);
    lv_obj_set_style_text_color(lbl_duct_icon, UI_COLOR_COOL, 0);
    lv_obj_set_style_text_font(lbl_duct_icon, &lv_font_montserrat_14, 0);
    
    lbl_duct_temp = lv_label_create(panel);
    lv_obj_align(lbl_duct_temp, LV_ALIGN_TOP_LEFT, 15, 15);
    lv_label_set_text(lbl_duct_temp, "--°C");
    lv_obj_set_style_text_color(lbl_duct_temp, UI_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_font(lbl_duct_temp, &lv_font_montserrat_14, 0);
    
    // Status text
    lbl_status = lv_label_create(panel);
    lv_obj_align(lbl_status, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_label_set_text(lbl_status, "Ready");
    lv_obj_set_style_text_color(lbl_status, UI_COLOR_ON, 0);
    lv_obj_set_style_text_font(lbl_status, &lv_font_montserrat_14, 0);
}

static void create_valve_positions_panel(lv_obj_t *parent)
{
    // Valve positions panel (left bottom)
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_set_size(panel, 150, 65);
    lv_obj_align(panel, LV_ALIGN_BOTTOM_LEFT, 5, -5);
    lv_obj_set_style_bg_color(panel, UI_COLOR_PANEL, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_radius(panel, 5, 0);
    lv_obj_set_style_pad_all(panel, 3, 0);
    
    // Small font for valve positions
    lv_obj_t *lbl_center_icon = lv_label_create(panel);
    lv_obj_align(lbl_center_icon, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_label_set_text(lbl_center_icon, "C:");
    lv_obj_set_style_text_color(lbl_center_icon, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_text_font(lbl_center_icon, &lv_font_montserrat_14, 0);
    
    lbl_center_valve_pos = lv_label_create(panel);
    lv_obj_align(lbl_center_valve_pos, LV_ALIGN_TOP_LEFT, 20, 0);
    lv_label_set_text(lbl_center_valve_pos, "--%%");
    lv_obj_set_style_text_color(lbl_center_valve_pos, UI_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_font(lbl_center_valve_pos, &lv_font_montserrat_14, 0);
    
    // Foot valve
    lv_obj_t *lbl_foot_icon = lv_label_create(panel);
    lv_obj_align(lbl_foot_icon, LV_ALIGN_TOP_LEFT, 0, 15);
    lv_label_set_text(lbl_foot_icon, "F:");
    lv_obj_set_style_text_color(lbl_foot_icon, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_text_font(lbl_foot_icon, &lv_font_montserrat_14, 0);
    
    lbl_foot_valve_pos = lv_label_create(panel);
    lv_obj_align(lbl_foot_valve_pos, LV_ALIGN_TOP_LEFT, 20, 15);
    lv_label_set_text(lbl_foot_valve_pos, "--%%");
    lv_obj_set_style_text_color(lbl_foot_valve_pos, UI_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_font(lbl_foot_valve_pos, &lv_font_montserrat_14, 0);
    
    // Windscreen valve
    lv_obj_t *lbl_windscreen_icon = lv_label_create(panel);
    lv_obj_align(lbl_windscreen_icon, LV_ALIGN_TOP_LEFT, 65, 0);
    lv_label_set_text(lbl_windscreen_icon, "W:");
    lv_obj_set_style_text_color(lbl_windscreen_icon, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_text_font(lbl_windscreen_icon, &lv_font_montserrat_14, 0);
    
    lbl_windscreen_valve_pos = lv_label_create(panel);
    lv_obj_align(lbl_windscreen_valve_pos, LV_ALIGN_TOP_LEFT, 85, 0);
    lv_label_set_text(lbl_windscreen_valve_pos, "--%%");
    lv_obj_set_style_text_color(lbl_windscreen_valve_pos, UI_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_font(lbl_windscreen_valve_pos, &lv_font_montserrat_14, 0);
    
    // Coolant valve
    lv_obj_t *lbl_coolant_icon = lv_label_create(panel);
    lv_obj_align(lbl_coolant_icon, LV_ALIGN_TOP_LEFT, 65, 15);
    lv_label_set_text(lbl_coolant_icon, "A:");
    lv_obj_set_style_text_color(lbl_coolant_icon, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_text_font(lbl_coolant_icon, &lv_font_montserrat_14, 0);
    
    lbl_coolant_valve_pos = lv_label_create(panel);
    lv_obj_align(lbl_coolant_valve_pos, LV_ALIGN_TOP_LEFT, 85, 15);
    lv_label_set_text(lbl_coolant_valve_pos, "--%%");
    lv_obj_set_style_text_color(lbl_coolant_valve_pos, UI_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_font(lbl_coolant_valve_pos, &lv_font_montserrat_14, 0);
}

/*******************************************************************************
 * Private Functions - UI Update
 ******************************************************************************/

static void update_mode_buttons(hvac_mode_t mode)
{
    // Clear all checked states
    lv_obj_clear_state(btn_mode_off, LV_STATE_CHECKED);
    lv_obj_clear_state(btn_mode_face, LV_STATE_CHECKED);
    lv_obj_clear_state(btn_mode_foot, LV_STATE_CHECKED);
    lv_obj_clear_state(btn_mode_defrost, LV_STATE_CHECKED);
    lv_obj_clear_state(btn_mode_auto, LV_STATE_CHECKED);
    
    // Set the active mode
    switch (mode) {
        case HVAC_MODE_OFF:
            lv_obj_add_state(btn_mode_off, LV_STATE_CHECKED);
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
            slider_val = 6;
            text = "Auto";
            break;
        default:
            break;
    }
    
    lv_slider_set_value(slider_fan, slider_val, LV_ANIM_OFF);
    lv_label_set_text(lbl_fan_speed, text);
}

static void create_debug_screen(lv_obj_t *parent)
{
    // Thermal Adaptation Status Panel
    lv_obj_t *panel_thermal = lv_obj_create(parent);
    lv_obj_set_size(panel_thermal, 220, 80);
    lv_obj_align(panel_thermal, LV_ALIGN_TOP_MID, 0, 5);
    lv_obj_set_style_bg_color(panel_thermal, UI_COLOR_PANEL, 0);
    lv_obj_set_style_border_width(panel_thermal, 1, 0);
    lv_obj_set_style_border_color(panel_thermal, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_radius(panel_thermal, 8, 0);
    
    lv_obj_t *lbl_thermal_title = lv_label_create(panel_thermal);
    lv_obj_align(lbl_thermal_title, LV_ALIGN_TOP_LEFT, 5, 3);
    lv_label_set_text(lbl_thermal_title, "Thermal Adaptation");
    lv_obj_set_style_text_color(lbl_thermal_title, UI_COLOR_HIGHLIGHT, 0);
    lv_obj_set_style_text_font(lbl_thermal_title, &lv_font_montserrat_14, 0);
    
    lbl_thermal_status = lv_label_create(panel_thermal);
    lv_obj_align(lbl_thermal_status, LV_ALIGN_TOP_LEFT, 5, 23);
    lv_label_set_text(lbl_thermal_status, "Loading...");
    lv_obj_set_style_text_color(lbl_thermal_status, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(lbl_thermal_status, &lv_font_montserrat_14, 0);
    
    // Raw Sensors Panel
    lv_obj_t *panel_sensors = lv_obj_create(parent);
    lv_obj_set_size(panel_sensors, 220, 85);
    lv_obj_align(panel_sensors, LV_ALIGN_TOP_MID, 0, 90);
    lv_obj_set_style_bg_color(panel_sensors, UI_COLOR_PANEL, 0);
    lv_obj_set_style_border_width(panel_sensors, 1, 0);
    lv_obj_set_style_border_color(panel_sensors, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_radius(panel_sensors, 8, 0);
    
    lv_obj_t *lbl_sensors_title = lv_label_create(panel_sensors);
    lv_obj_align(lbl_sensors_title, LV_ALIGN_TOP_LEFT, 5, 3);
    lv_label_set_text(lbl_sensors_title, "Raw Sensors");
    lv_obj_set_style_text_color(lbl_sensors_title, UI_COLOR_HIGHLIGHT, 0);
    lv_obj_set_style_text_font(lbl_sensors_title, &lv_font_montserrat_14, 0);
    
    lbl_raw_sensors = lv_label_create(panel_sensors);
    lv_obj_align(lbl_raw_sensors, LV_ALIGN_TOP_LEFT, 5, 23);
    lv_label_set_text(lbl_raw_sensors, "Loading...");
    lv_obj_set_style_text_color(lbl_raw_sensors, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(lbl_raw_sensors, &lv_font_montserrat_14, 0);
    
    // System Info Panel
    lv_obj_t *panel_system = lv_obj_create(parent);
    lv_obj_set_size(panel_system, 220, 65);
    lv_obj_align(panel_system, LV_ALIGN_TOP_MID, 0, 180);
    lv_obj_set_style_bg_color(panel_system, UI_COLOR_PANEL, 0);
    lv_obj_set_style_border_width(panel_system, 1, 0);
    lv_obj_set_style_border_color(panel_system, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_radius(panel_system, 8, 0);
    
    lv_obj_t *lbl_system_title = lv_label_create(panel_system);
    lv_obj_align(lbl_system_title, LV_ALIGN_TOP_LEFT, 5, 3);
    lv_label_set_text(lbl_system_title, "System Info");
    lv_obj_set_style_text_color(lbl_system_title, UI_COLOR_HIGHLIGHT, 0);
    lv_obj_set_style_text_font(lbl_system_title, &lv_font_montserrat_14, 0);
    
    lbl_system_info = lv_label_create(panel_system);
    lv_obj_align(lbl_system_info, LV_ALIGN_TOP_LEFT, 5, 23);
    lv_label_set_text(lbl_system_info, "Loading...");
    lv_obj_set_style_text_color(lbl_system_info, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(lbl_system_info, &lv_font_montserrat_14, 0);
    
    // Control Buttons
    btn_reset_cal = lv_btn_create(parent);
    lv_obj_set_size(btn_reset_cal, 100, 35);
    lv_obj_align(btn_reset_cal, LV_ALIGN_BOTTOM_LEFT, 15, -10);
    lv_obj_set_style_bg_color(btn_reset_cal, UI_COLOR_WARNING, 0);
    lv_obj_add_event_cb(btn_reset_cal, btn_reset_cal_handler, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_reset = lv_label_create(btn_reset_cal);
    lv_label_set_text(lbl_reset, "Reset Cal");
    lv_obj_center(lbl_reset);
    
    btn_save_cal = lv_btn_create(parent);
    lv_obj_set_size(btn_save_cal, 100, 35);
    lv_obj_align(btn_save_cal, LV_ALIGN_BOTTOM_RIGHT, -15, -10);
    lv_obj_set_style_bg_color(btn_save_cal, UI_COLOR_ON, 0);
    lv_obj_add_event_cb(btn_save_cal, btn_save_cal_handler, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_save = lv_label_create(btn_save_cal);
    lv_label_set_text(lbl_save, "Save Cal");
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
    
    // Create tabview with bottom tabs
    tabview = lv_tabview_create(scr_main, LV_DIR_BOTTOM, 35);
    lv_obj_set_style_bg_color(tabview, UI_COLOR_BG, 0);
    
    // Create tabs
    tab_main = lv_tabview_add_tab(tabview, "Main");
    tab_debug = lv_tabview_add_tab(tabview, "Debug");
    
    // Set tab button colors
    lv_obj_t *tab_btns = lv_tabview_get_tab_btns(tabview);
    lv_obj_set_style_bg_color(tab_btns, UI_COLOR_PANEL, 0);
    lv_obj_set_style_text_color(tab_btns, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_color(tab_btns, UI_COLOR_HIGHLIGHT, LV_PART_ITEMS | LV_STATE_CHECKED);
    
    // Create main tab UI components
    create_temp_display(tab_main);
    create_mode_buttons(tab_main);
    create_fan_control(tab_main);
    create_feature_buttons(tab_main);
    create_status_panel(tab_main);
    create_valve_positions_panel(tab_main);
    
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
            snprintf(buf, sizeof(buf), "%.1f°C", state.cabin_temp_c);
        } else {
            snprintf(buf, sizeof(buf), "%.1f°F", hvac_controller_c_to_f(state.cabin_temp_c));
        }
        lv_label_set_text(lbl_cabin_temp, buf);
        
        // Update temperature arc
        int arc_val = (int)(state.cabin_temp_c);
        if (arc_val < 16) arc_val = 16;
        if (arc_val > 30) arc_val = 30;
        lv_arc_set_value(arc_temp, arc_val);
        
        // Color based on heating/cooling
        if (state.is_heating) {
            lv_obj_set_style_arc_color(arc_temp, UI_COLOR_HEAT, LV_PART_INDICATOR);
        } else if (state.is_cooling) {
            lv_obj_set_style_arc_color(arc_temp, UI_COLOR_COOL, LV_PART_INDICATOR);
        } else {
            lv_obj_set_style_arc_color(arc_temp, UI_COLOR_HIGHLIGHT, LV_PART_INDICATOR);
        }
    } else {
        lv_label_set_text(lbl_cabin_temp, "--.-°C");
    }
    
    // Update humidity
    if (!isnan(state.cabin_humidity)) {
        snprintf(buf, sizeof(buf), "%.0f %%RH", state.cabin_humidity);
        lv_label_set_text(lbl_humidity, buf);
    } else {
        lv_label_set_text(lbl_humidity, "-- %RH");
    }
    
    // Update target temperature
    if (state.temp_unit == TEMP_UNIT_CELSIUS) {
        snprintf(buf, sizeof(buf), "Set: %.1f°C", state.target_temp_c);
    } else {
        snprintf(buf, sizeof(buf), "Set: %.1f°F", hvac_controller_c_to_f(state.target_temp_c));
    }
    lv_label_set_text(lbl_target_temp, buf);
    
    // Update mode buttons only if changed
    if (state.mode != last_state.mode) {
        update_mode_buttons(state.mode);
    }
    
    // Update fan slider only if changed
    if (state.fan_level != last_state.fan_level) {
        update_fan_slider(state.fan_level);
    }
    
    // Update A/C button
    if (state.ac_enabled) {
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
    
    // Update coolant temperature
    if (!isnan(state.coolant_temp_c)) {
        snprintf(buf, sizeof(buf), "%.0f°C", state.coolant_temp_c);
        lv_label_set_text(lbl_coolant_temp, buf);
    } else {
        lv_label_set_text(lbl_coolant_temp, "--°C");
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
    
    snprintf(buf, sizeof(buf), "%d%%", state.foot_valve_pos);
    lv_label_set_text(lbl_foot_valve_pos, buf);
    
    snprintf(buf, sizeof(buf), "%d%%", state.windscreen_valve_pos);
    lv_label_set_text(lbl_windscreen_valve_pos, buf);
    
    snprintf(buf, sizeof(buf), "%d%%", state.coolant_valve_pos);
    lv_label_set_text(lbl_coolant_valve_pos, buf);
    
    if (state.sensor_fault) {
        lv_label_set_text(lbl_status, "Sensor Fault");
        lv_obj_set_style_text_color(lbl_status, UI_COLOR_WARNING, 0);
    } else if (state.valve_fault) {
        lv_label_set_text(lbl_status, "Valve Fault");
        lv_obj_set_style_text_color(lbl_status, UI_COLOR_WARNING, 0);
    } else if (state.mode == HVAC_MODE_OFF) {
        lv_label_set_text(lbl_status, "OFF");
        lv_obj_set_style_text_color(lbl_status, UI_COLOR_TEXT_DIM, 0);
    } else if (state.is_heating) {
        lv_label_set_text(lbl_status, "Heating");
        lv_obj_set_style_text_color(lbl_status, UI_COLOR_HEAT, 0);
    } else if (state.is_cooling) {
        lv_label_set_text(lbl_status, "Cooling");
        lv_obj_set_style_text_color(lbl_status, UI_COLOR_COOL, 0);
    } else if (state.temp_reached) {
        lv_label_set_text(lbl_status, "Comfort");
        lv_obj_set_style_text_color(lbl_status, UI_COLOR_ON, 0);
    } else {
        lv_label_set_text(lbl_status, "Active");
        lv_obj_set_style_text_color(lbl_status, UI_COLOR_HIGHLIGHT, 0);
    }

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
        // Thermal adaptation status
        float heating_rate, cooling_rate, adaptive_gain;
        uint32_t sample_count;
        bool calibrated;
        hvac_controller_get_thermal_status(&heating_rate, &cooling_rate, &adaptive_gain, 
                                          &sample_count, &calibrated);
        
        snprintf(buf, sizeof(buf), 
                "Heat: %.3f°C/min\nCool: %.3f°C/min\nGain: %.1f\nSamples: %lu %s",
                heating_rate, cooling_rate, adaptive_gain, sample_count,
                calibrated ? "[CAL]" : "[LEARNING]");
        lv_label_set_text(lbl_thermal_status, buf);
        
        // Raw sensors
        snprintf(buf, sizeof(buf),
            "Cabin: %.2f°C\nHumid: %.1f%%\nCoolant: %.1f°C\nDuct: %.1f°C\nValves: 0x%02X\nFaults: S:%d V:%d",
            state.cabin_temp_c, state.cabin_humidity,
            state.coolant_temp_c, state.duct_temp_c,
            state.valve_fault_mask,
            state.sensor_fault, state.valve_fault);
        lv_label_set_text(lbl_raw_sensors, buf);
        
        // System info
        uint32_t uptime_sec = xTaskGetTickCount() * portTICK_PERIOD_MS / 1000;
        uint32_t free_heap = esp_get_free_heap_size();
        snprintf(buf, sizeof(buf),
                "Uptime: %luh %lum %lus\nFree Heap: %lu KB",
                uptime_sec / 3600, (uptime_sec % 3600) / 60, uptime_sec % 60,
                free_heap / 1024);
        lv_label_set_text(lbl_system_info, buf);
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
    }
    
    msg_box = lv_msgbox_create(NULL, "Error", message, NULL, true);
    lv_obj_center(msg_box);
}

void hvac_ui_show_info(const char *message)
{
    if (msg_box != NULL) {
        lv_msgbox_close(msg_box);
    }
    
    msg_box = lv_msgbox_create(NULL, "Info", message, NULL, true);
    lv_obj_center(msg_box);
}

void hvac_ui_hide_message(void)
{
    if (msg_box != NULL) {
        lv_msgbox_close(msg_box);
        msg_box = NULL;
    }
}