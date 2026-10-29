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
 * @brief Get the main screen object
 * @return Pointer to main screen
 */
lv_obj_t* hvac_ui_get_screen(void);

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

/**
 * @brief Hide any message
 */
void hvac_ui_hide_message(void);

#ifdef __cplusplus
}
#endif

#endif // HVAC_UI_H
