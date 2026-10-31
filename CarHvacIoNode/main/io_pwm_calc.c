/**
 * @file io_pwm_calc.c
 * @brief Pure PWM arithmetic for io_hw.c (no ESP-IDF; host-tested in test/host).
 */

#include "io_pwm_calc.h"

bool pwm_hz_args_valid(uint32_t freq_hz, unsigned bits)
{
    if (freq_hz < PWM_HZ_MIN || freq_hz > PWM_HZ_MAX || bits < PWM_BITS_MIN || bits > PWM_BITS_MAX) {
        return false;
    }
    uint64_t ticks = (uint64_t)freq_hz << bits;     /* source clock needed at divider 1.0 */
    return ticks <= PWM_SRC_CLK_HZ && ticks * PWM_DIV_MAX > PWM_SRC_CLK_HZ;
}

uint32_t pwm_duty_from_8bit(uint8_t duty, unsigned bits, bool legacy)
{
    if (legacy) {
        return duty;
    }
    return (((uint32_t)duty << bits) + 127u) / 255u;
}

uint32_t pwm_duty_clamp(uint32_t counts, unsigned bits)
{
    uint32_t full = 1u << bits;
    return counts > full ? full : counts;
}
