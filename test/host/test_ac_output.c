/* Electric compressor output: link payloads (main/link_payload.c, what remote_io_binary.c sends)
 * against the node README's bytes, framed with the display's cobs_build_frame; PWM resolution per
 * frequency, the pct mappings and the MCP4725 bytes (main/ac_output_calc.c). */
#include <string.h>
#include "ac_output_calc.h"
#include "cobs_protocol.h"
#include "link_payload.h"
#include "t.h"

/* Frame with the display's builder, parse back, compare to the expected payload. */
static int frame_is(uint8_t type, const uint8_t *pl, size_t n, const uint8_t *want, size_t wn)
{
    uint8_t f[COBS_MAX_FRAME_SIZE + 1], out[COBS_MAX_PAYLOAD_SIZE], t = 0;
    size_t on = 0;
    size_t fl = cobs_build_frame(type, pl, n, f, sizeof(f));
    return fl > 0 && cobs_parse_frame(f, fl, &t, out, sizeof(out), &on) && t == type && on == wn &&
           memcmp(out, want, wn) == 0;
}

static void test_pwm_init_hz_payload(void)
{
    uint8_t pl[6];
    /* README: pwm-hz 10 20000 8 = the analog_rc init: 22 0A 00 00 4E 20 08 */
    const uint8_t rc[6] = { 0x0A, 0x00, 0x00, 0x4E, 0x20, 0x08 };
    CHECK(link_payload_pwm_init_hz(pl, 10, 20000, 8) == 6);
    CHECK(frame_is(MSG_PWM_INIT_HZ, pl, 6, rc, 6));
    const uint8_t big[6] = { 0x0A, 0x01, 0x02, 0x03, 0x04, 0x0D };   /* byte order: big-endian */
    link_payload_pwm_init_hz(pl, 10, 0x01020304u, 13);
    CHECK(frame_is(MSG_PWM_INIT_HZ, pl, 6, big, 6));
    CHECK(MSG_PWM_INIT_HZ == 0x22 && MSG_PWM_SET == 0x20 && MSG_I2C_WRITE == 0x30 && NODE_CAP_PWM_HZ == 0x04);
}

static void test_pwm_set16_payload(void)
{
    uint8_t pl[3];
    /* README: pwm-set16 10 128 = 128 counts */
    const uint8_t half[3] = { 0x0A, 0x00, 0x80 };
    CHECK(link_payload_pwm_set16(pl, 10, 128) == 3);
    CHECK(frame_is(MSG_PWM_SET, pl, 3, half, 3));
    const uint8_t full13[3] = { 0x0A, 0x20, 0x00 };     /* 8192 = 2^13 = full on */
    link_payload_pwm_set16(pl, 10, 8192);
    CHECK(frame_is(MSG_PWM_SET, pl, 3, full13, 3));
}

static void test_bits(void)
{
    CHECK(ac_out_pwm_bits(100) == 13);      /* 819 kHz ticks */
    CHECK(ac_out_pwm_bits(1000) == 13);     /* 8.19 MHz */
    CHECK(ac_out_pwm_bits(20000) == 11);    /* 40.96 MHz; 12 bits would be 81.92 MHz */
    CHECK(ac_out_pwm_bits(9765) == 13 && ac_out_pwm_bits(9766) == 12);
    CHECK(ac_out_pwm_bits(40000) == 10);
    CHECK(ac_out_pwm_bits(0) == 0);
    CHECK(ac_out_pwm_bits(9) == 0);         /* 73728 < 78126 at 13 bits: below the node's divider limit */
    CHECK(ac_out_pwm_bits(10) == 13);       /* 81920 */
    /* the settings range 100..20000 Hz always gets a valid, maximal resolution */
    for (uint32_t f = 100; f <= 20000; f += 100) {
        unsigned b = ac_out_pwm_bits(f);
        uint64_t ticks = (uint64_t)f << b;
        CHECKM(b >= 1 && b <= 13 && ticks <= 80000000u && ticks >= 78126u &&
               (b == 13 || ((uint64_t)f << (b + 1)) > 80000000u), "f=%lu b=%u", (unsigned long)f, b);
    }
}

static void test_mapping(void)
{
    CHECK(ac_out_pwm_counts(0, 13) == 0 && ac_out_pwm_counts(100, 13) == 8192 && ac_out_pwm_counts(50, 13) == 4096);
    CHECK(ac_out_pwm_counts(50, 11) == 1024 && ac_out_pwm_counts(150, 11) == 2048 && ac_out_pwm_counts(-5, 11) == 0);
    CHECK(ac_out_pwm_counts(0.0f / 0.0f, 13) == 0);   /* NaN */
    CHECK(ac_out_duty8(0) == 0 && ac_out_duty8(100) == 255 && ac_out_duty8(50) == 128);
    CHECK(ac_out_dac_value(0) == 0 && ac_out_dac_value(100) == 4095 && ac_out_dac_value(50) == 2048);
    CHECK(ac_out_dac_value(44) == 1802);
}

static void test_dac_payload(void)
{
    uint8_t d[3], pl[8];
    /* README: 0x800 -> payload 60 08 01 00 */
    const uint8_t half[4] = { 0x60, 0x08, 0x01, 0x00 };
    CHECK(ac_out_dac_fast(d, 0x800) == 2);
    CHECK(link_payload_i2c_write(pl, sizeof(pl), 0x60, d, 2) == 4);
    CHECK(frame_is(MSG_I2C_WRITE, pl, 4, half, 4));
    const uint8_t full61[4] = { 0x61, 0x0F, 0x01, 0xFF };
    ac_out_dac_fast(d, 4095);
    link_payload_i2c_write(pl, sizeof(pl), 0x61, d, 2);
    CHECK(memcmp(pl, full61, 4) == 0);
    /* README: DAC and EEPROM to 0 = payload 60 60 02 00 00 */
    const uint8_t eep[5] = { 0x60, 0x60, 0x02, 0x00, 0x00 };
    CHECK(ac_out_dac_eeprom(d, 0) == 3);
    CHECK(link_payload_i2c_write(pl, sizeof(pl), 0x60, d, 3) == 5);
    CHECK(frame_is(MSG_I2C_WRITE, pl, 5, eep, 5));
    ac_out_dac_eeprom(d, 0xABC);
    CHECK(d[0] == 0x60 && d[1] == 0xAB && d[2] == 0xC0);
    CHECK(link_payload_i2c_write(pl, 4, 0x60, d, 3) == 0 && link_payload_i2c_write(pl, 8, 0x60, d, 0) == 0);
}

int main(void)
{
    RUN(test_pwm_init_hz_payload);
    RUN(test_pwm_set16_payload);
    RUN(test_bits);
    RUN(test_mapping);
    RUN(test_dac_payload);
    T_DONE();
}
