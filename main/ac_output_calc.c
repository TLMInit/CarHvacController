/* Electric compressor output arithmetic: see ac_output_calc.h. No ESP-IDF. */
#include <math.h>
#include "ac_output_calc.h"

static float frac(float pct)
{
    return !(pct > 0.0f) ? 0.0f : (pct >= 100.0f ? 1.0f : pct / 100.0f);   /* NaN -> 0 */
}

unsigned ac_out_pwm_bits(uint32_t freq_hz)
{
    if (freq_hz == 0) return 0;
    for (unsigned bits = AC_OUT_PWM_BITS_MAX; bits >= 1; bits--) {
        uint64_t ticks = (uint64_t)freq_hz << bits;
        if (ticks <= AC_OUT_PWM_SRC_HZ) {
            return ticks >= AC_OUT_PWM_MIN_TICKS ? bits : 0;
        }
    }
    return 0;
}

uint16_t ac_out_pwm_counts(float pct, unsigned bits)
{
    return (uint16_t)lroundf(frac(pct) * (float)(1u << bits));
}

uint8_t ac_out_duty8(float pct)
{
    return (uint8_t)lroundf(frac(pct) * 255.0f);
}

uint16_t ac_out_dac_value(float pct)
{
    return (uint16_t)lroundf(frac(pct) * (float)AC_OUT_DAC_MAX);
}

size_t ac_out_dac_fast(uint8_t out[2], uint16_t value)
{
    value &= 0x0FFFu;
    out[0] = (uint8_t)(value >> 8);     /* C2 C1 = 00 fast mode, PD1 PD0 = 00 normal */
    out[1] = (uint8_t)value;
    return 2;
}

size_t ac_out_dac_eeprom(uint8_t out[3], uint16_t value)
{
    value &= 0x0FFFu;
    out[0] = 0x60;                      /* C2 C1 C0 = 011 write DAC and EEPROM, PD = 00 */
    out[1] = (uint8_t)(value >> 4);
    out[2] = (uint8_t)((value & 0x0Fu) << 4);
    return 3;
}
