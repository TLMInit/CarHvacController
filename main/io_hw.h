/**
 * @file io_hw.h
 * @brief GPIO and PWM outputs of the IO node.
 */

#ifndef IO_HW_H
#define IO_HW_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/** Pre-configure every output the display expects (all low, PWM duty 0). */
esp_err_t io_hw_init(void);

/** True if the link is allowed to touch this GPIO number. */
bool io_pin_allowed(int pin);

/** mode: 0 input, 1 output, 2 input pull-up, 3 input pull-down, 4 open-drain output. */
esp_err_t io_gpio_mode(int pin, uint8_t mode);

/** Drive a pin. A pin never configured is made an output first; a PWM channel on it is released. */
esp_err_t io_gpio_set(int pin, int level);

/** Read a pin. */
esp_err_t io_gpio_get(int pin, int *level);

/** Attach (or re-time) an 8-bit LEDC channel on a pin at freq_khz. */
esp_err_t io_pwm_init(int pin, int freq_khz);

/** Set 8-bit duty (0-255). A pin without a channel gets one at NODE_DEFAULT_PWM_KHZ. */
esp_err_t io_pwm_set(int pin, uint8_t duty);

/** Every PWM channel to duty 0 and every output pin low. Used by the link watchdog. */
void io_outputs_safe_state(void);

#endif /* IO_HW_H */
