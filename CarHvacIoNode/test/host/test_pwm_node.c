/* PWM_INIT_HZ and the 3-byte PWM_SET: frame round trip (payloads big-endian, as protocol_server.c
 * reads them) and the pure arithmetic in main/io_pwm_calc.c (argument check, 8-bit scaling, clamp). */
#include <string.h>
#include "cobs_protocol.h"
#include "io_pwm_calc.h"
#include "t.h"

static int roundtrip(uint8_t type, const uint8_t *pl, size_t n, uint8_t *out, size_t *on)
{
    uint8_t f[COBS_MAX_FRAME_SIZE + 1], t = 0;
    size_t fl = cobs_build_frame(type, pl, n, f, sizeof(f));
    return fl > 0 && cobs_parse_frame(f, fl, &t, out, COBS_MAX_PAYLOAD_SIZE, on) && t == type;
}

static void test_pwm_init_hz_frame(void)
{
    const uint32_t freqs[] = { 1, 20000, 25000, 40000, 0x01020304 };
    for (size_t i = 0; i < sizeof(freqs) / sizeof(freqs[0]); i++) {
        uint32_t f = freqs[i];
        uint8_t pl[6] = { 10, (uint8_t)(f >> 24), (uint8_t)(f >> 16), (uint8_t)(f >> 8), (uint8_t)f, 8 };
        uint8_t out[COBS_MAX_PAYLOAD_SIZE];
        size_t on = 0;
        CHECK(roundtrip(MSG_PWM_INIT_HZ, pl, 6, out, &on) && on == 6);
        uint32_t back = ((uint32_t)out[1] << 24) | ((uint32_t)out[2] << 16) | ((uint32_t)out[3] << 8) | out[4];
        CHECKM(out[0] == 10 && back == f && out[5] == 8, "freq %lu", (unsigned long)f);
    }
    /* the analog_rc frame the display sends: 22 0A 00 00 4E 20 08 */
    uint8_t pl[6] = { 0x0A, 0x00, 0x00, 0x4E, 0x20, 0x08 };
    CHECK((((uint32_t)pl[3] << 8) | pl[4]) == 20000);
}

static void test_pwm_set16_frame(void)
{
    const uint16_t counts[] = { 0, 1, 0x00FF, 0x0100, 0x1234, 16384, 0xFFFF };
    for (size_t i = 0; i < sizeof(counts) / sizeof(counts[0]); i++) {
        uint8_t pl[3] = { 10, (uint8_t)(counts[i] >> 8), (uint8_t)counts[i] }, out[COBS_MAX_PAYLOAD_SIZE];
        size_t on = 0;
        CHECK(roundtrip(MSG_PWM_SET, pl, 3, out, &on) && on == 3);
        CHECK(out[0] == 10 && (uint16_t)((out[1] << 8) | out[2]) == counts[i]);
    }
    uint8_t pl2[2] = { 10, 255 }, out[COBS_MAX_PAYLOAD_SIZE];   /* 2-byte form unchanged */
    size_t on = 0;
    CHECK(roundtrip(MSG_PWM_SET, pl2, 2, out, &on) && on == 2 && out[1] == 255);
}

static void test_args_valid(void)
{
    CHECK(pwm_hz_args_valid(20000, 8));         /* analog_rc, valves */
    CHECK(pwm_hz_args_valid(25000, 10));        /* the blower's 25 kHz 10 bit */
    CHECK(pwm_hz_args_valid(40000, 10));        /* 40.96 MHz */
    CHECK(pwm_hz_args_valid(4882, 14));         /* 79.99 MHz */
    CHECK(!pwm_hz_args_valid(4883, 14));        /* 80.003 MHz > APB */
    CHECK(!pwm_hz_args_valid(40000, 11));       /* 81.92 MHz */
    CHECK(!pwm_hz_args_valid(0, 8));
    CHECK(!pwm_hz_args_valid(40001, 1));
    CHECK(!pwm_hz_args_valid(1000, 0));
    CHECK(!pwm_hz_args_valid(1000, 15));
    CHECK(!pwm_hz_args_valid(1, 14));           /* 16384: divider would exceed 1024 */
    CHECK(!pwm_hz_args_valid(305, 8));          /* 78080 Hz: divider 1024.6 */
    CHECK(pwm_hz_args_valid(306, 8));           /* 78336 Hz: divider 1021.2 */
    CHECK(pwm_hz_args_valid(5, 14));            /* 81920 */
    /* exact edges, divider 1.0 .. < 1024 (78126 is the driver's first valid product) */
    CHECK(pwm_hz_args_valid(40000, 1) && pwm_hz_args_valid(39063, 1) && !pwm_hz_args_valid(39062, 1));
    for (unsigned b = 1; b <= PWM_BITS_MAX; b++) {
        for (uint32_t f = 1; f <= PWM_HZ_MAX; f += 7) {
            uint64_t p = (uint64_t)f << b;
            CHECKM(pwm_hz_args_valid(f, b) == (p <= 80000000u && p >= 78126u), "f=%lu b=%u", (unsigned long)f, b);
            if (t_failed > 20) return;
        }
    }
}

static void test_scale_8bit(void)
{
    for (unsigned d = 0; d < 256; d++) {
        CHECK(pwm_duty_from_8bit((uint8_t)d, 8, true) == d);    /* PWM_INIT channels: as before */
    }
    for (unsigned b = PWM_BITS_MIN; b <= PWM_BITS_MAX; b++) {
        uint32_t full = 1u << b, prev = 0;
        CHECK(pwm_duty_from_8bit(0, b, false) == 0);
        CHECKM(pwm_duty_from_8bit(255, b, false) == full, "bits %u", b);
        for (unsigned d = 1; d < 256; d++) {
            uint32_t v = pwm_duty_from_8bit((uint8_t)d, b, false);
            CHECK(v >= prev && v <= full);
            prev = v;
        }
    }
    CHECK(pwm_duty_from_8bit(128, 8, false) == 129);    /* round(128 * 256 / 255) */
    CHECK(pwm_duty_from_8bit(128, 10, false) == 514);
    CHECK(pwm_duty_from_8bit(1, 14, false) == 64);
}

static void test_clamp(void)
{
    CHECK(pwm_duty_clamp(0, 8) == 0);
    CHECK(pwm_duty_clamp(255, 8) == 255);
    CHECK(pwm_duty_clamp(256, 8) == 256);
    CHECK(pwm_duty_clamp(257, 8) == 256);
    CHECK(pwm_duty_clamp(0xFFFF, 14) == 16384);
    CHECK(pwm_duty_clamp(0xFFFF, 1) == 2);
    CHECK(pwm_duty_clamp(1000, 10) == 1000);
}

int main(void)
{
    RUN(test_pwm_init_hz_frame);
    RUN(test_pwm_set16_frame);
    RUN(test_args_valid);
    RUN(test_scale_8bit);
    RUN(test_clamp);
    T_DONE();
}
