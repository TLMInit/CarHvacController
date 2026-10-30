/**
 * @file hvac_ui.h
 * @brief LVGL-based HVAC User Interface
 * 
 * Provides a touch-friendly automotive HVAC control interface
 */

#ifndef HVAC_UI_H
#define HVAC_UI_H

#include "esp_err.h"

#include "lvgl.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*******************************************************************************
 * Function Declarations
 ******************************************************************************/

/**
 * @brief Initialize the HVAC UI
 * @return ESP_OK on success
 */
esp_err_t hvac_ui_init(void);

/**
 * @brief Update the UI with current state (call periodically)
 */
void hvac_ui_update(void);

/**
 * @brief Set screen brightness
 * @param brightness Brightness 0-100%
 */
void hvac_ui_set_brightness(uint8_t brightness);

/** Apply the persisted DAY/NIGHT brightness after the display backlight is ready. */
void hvac_ui_apply_saved_brightness(void);

/**
 * @brief Show error message
 * @param message Error message to display
 */
void hvac_ui_show_error(const char *message);

/**
 * @brief Show info message
 * @param message Info message to display
 */
void hvac_ui_show_info(const char *message);

#define HVAC_UI_TREND_POINTS 60     /* the trend chart's ring: cabin, duct, setpoint */
#define HVAC_UI_TREND_SERIES 3

/**
 * @brief Copy the trend chart's ring (tenths of degC, oldest first, LV_CHART_POINT_NONE = no
 * reading) for the phone page. Any task. False (out untouched) before the UI started it.
 * @param interval_s seconds between points (SET_CHART_INTERVAL_S)
 */
bool hvac_ui_trend_copy(lv_coord_t out[HVAC_UI_TREND_SERIES][HVAC_UI_TREND_POINTS], uint32_t *interval_s);

#ifdef __cplusplus
}
#endif

#endif // HVAC_UI_H
