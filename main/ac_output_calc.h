/**
 * @file ac_output_calc.h
 * @brief Electric compressor output arithmetic, pure (no ESP-IDF): host-tested.
 * ac_output.c uses it to turn ac_electric's output_pct (0..100 % of full scale, already
 * inverted) into what goes over the link.
 */
#ifndef AC_OUTPUT_CALC_H
#define AC_OUTPUT_CALC_H

#include <stddef.h>
#include <stdint.h>

#define AC_OUT_PWM_SRC_HZ    80000000u  /* node LEDC on the APB clock (node io_pwm_calc.c) */
#define AC_OUT_PWM_MIN_TICKS 78126u     /* freq x 2^bits at the LEDC divider limit */
#define AC_OUT_PWM_BITS_MAX  13u        /* counts up to 8192 stay a u16 with headroom */
#define AC_OUT_DAC_MAX       4095u

/** Resolution for a PWM frequency: min(13, floor(log2(80 MHz / freq))), and freq x 2^bits must
 * reach 78126 (the node's lower limit). 0 when no 1..13 fits (freq 0 or above 40 kHz). */
unsigned ac_out_pwm_bits(uint32_t freq_hz);

/** pct 0..100 -> raw counts 0..2^bits (2^bits = full on, the node's clamp). */
uint16_t ac_out_pwm_counts(float pct, unsigned bits);

/** pct 0..100 -> 8-bit duty 0..255 for the 2-byte PWM_SET (255 = full on on a PWM_INIT_HZ channel). */
uint8_t ac_out_duty8(float pct);

/** pct 0..100 -> MCP4725 code 0..4095. */
uint16_t ac_out_dac_value(float pct);

/** MCP4725 fast write as I2C data for remote_i2c_write: [(PD 0 << 4) | (value >> 8), value & 0xFF].
 * Returns 2. */
size_t ac_out_dac_fast(uint8_t out[2], uint16_t value);

/** MCP4725 write DAC register and EEPROM: [0x60, D11..D4, D3..D0 << 4]. Returns 3. */
size_t ac_out_dac_eeprom(uint8_t out[3], uint16_t value);

#endif /* AC_OUTPUT_CALC_H */
