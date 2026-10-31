/**
 * @file io_pwm_calc.h
 * @brief Pure PWM arithmetic for io_hw.c (no ESP-IDF; host-tested in test/host).
 */

#ifndef IO_PWM_CALC_H
#define IO_PWM_CALC_H

#include <stdbool.h>
#include <stdint.h>

#define PWM_HZ_MIN          1u
#define PWM_HZ_MAX          40000u
#define PWM_BITS_MIN        1u
#define PWM_BITS_MAX        14u         /* SOC_LEDC_TIMER_BIT_WIDTH on the ESP32-S3 */
#define PWM_SRC_CLK_HZ      80000000u   /* APB: io_hw.c configures every timer with LEDC_USE_APB_CLK */
#define PWM_DIV_MAX         1024u       /* LEDC divider is 1.0 .. 1023.996 (ledc.c LEDC_TIMER_DIV_NUM_MAX) */

/** PWM_INIT_HZ arguments: freq 1..40000 Hz, bits 1..14, and freq * 2^bits within what
 *  the 80 MHz APB clock and the LEDC divider can make: 78126 Hz .. 80 MHz. */
bool pwm_hz_args_valid(uint32_t freq_hz, unsigned bits);

/** 8-bit PWM_SET duty in counts of a `bits`-bit channel: round(duty * 2^bits / 255), so
 *  255 is full on (2^bits). legacy (a PWM_INIT channel): duty unchanged, as before 2.5. */
uint32_t pwm_duty_from_8bit(uint8_t duty, unsigned bits, bool legacy);

/** Raw counts clamped to full on (2^bits). */
uint32_t pwm_duty_clamp(uint32_t counts, unsigned bits);

#endif /* IO_PWM_CALC_H */
