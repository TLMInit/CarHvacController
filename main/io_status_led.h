/**
 * @file io_status_led.h
 * @brief WS2812 status LED (the DevKitC's on-board RGB LED, or an external one).
 *
 * The original CarHvacIONode drove the board's RGB LED through the led_strip
 * component (its log showed "Status LED initialized on GPIO %d"). This module
 * restores that, with one rule the original did not have: the LED yields its
 * GPIO the moment the display claims that pin for an output, so the LED can
 * never fight a valve or blower signal.
 *
 * Patterns, 100 ms resolution:
 *   boot                       white, steady (until the link task runs)
 *   waiting for the display    blue blink, 1 Hz
 *   link up                    green, steady
 *   link lost after being up   red blink, 2.5 Hz
 *   error or bad frame         amber flash, 300 ms, on top of the above
 */

#ifndef IO_STATUS_LED_H
#define IO_STATUS_LED_H

#include <stdbool.h>
#include "esp_err.h"

/** Start the LED on NODE_STATUS_LED_GPIO. A GPIO of -1 makes every call a no-op. */
esp_err_t io_status_led_init(void);

/**
 * Release the LED if it owns this GPIO: the task stops, the strip is deleted and
 * the pin is free for GPIO or LEDC. Returns true when a release happened.
 * Called by io_hw before it configures any pin.
 */
bool io_status_led_release_pin(int pin);

/** True while the LED still owns its GPIO. */
bool io_status_led_owns_pin(int pin);

#endif /* IO_STATUS_LED_H */
