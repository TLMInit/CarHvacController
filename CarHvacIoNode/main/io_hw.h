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

/** Attach (or re-time) an 8-bit LEDC channel on a pin at freq_khz; duty bytes pass through unscaled. */
esp_err_t io_pwm_init(int pin, int freq_khz);

/** Attach (or re-time) a channel at freq_hz with 1..14 bits (pwm_hz_args_valid).
 *  ESP_ERR_INVALID_ARG: pin not allowed; ESP_ERR_INVALID_SIZE: freq/bits refused;
 *  ESP_ERR_NO_MEM: no LEDC timer (4) or channel (8) left. A resolution change sets duty 0. */
esp_err_t io_pwm_init_hz(int pin, uint32_t freq_hz, uint8_t bits);

/** Set 8-bit duty (0-255), scaled to an io_pwm_init_hz channel's resolution (255 = full on).
 *  A pin without a channel gets one at NODE_DEFAULT_PWM_KHZ. */
esp_err_t io_pwm_set(int pin, uint8_t duty);

/** Set duty in raw counts of the channel, clamped to 2^bits (full on). */
esp_err_t io_pwm_set_counts(int pin, uint16_t counts);

/** Every PWM channel to duty 0 and every output pin low. Used by the link watchdog. */
void io_outputs_safe_state(void);

#endif /* IO_HW_H */
