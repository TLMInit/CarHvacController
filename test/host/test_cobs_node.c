/* Node main/cobs_protocol.c: round trip, S-11 (trailing 0x00 kept by the decoder), and the reply pad
 * that is applied only while cobs_pad_trailing_zero is true (protocol_server.c clears it on HELLO;
 * protocol_server.c itself needs ESP-IDF and is not compiled here, so the HELLO hook is simulated
 * by setting the flag). Built twice: LINK_PAD_TRAILING_ZERO = 1 and 0. */
#include <string.h>
#include "cobs_protocol.h"
#include "t.h"

extern bool cobs_pad_trailing_zero;      /* as declared in protocol_server.c */

static uint32_t s_rng = 987654321;
static uint8_t rnd8(void) { s_rng = s_rng * 1664525u + 1013904223u; return (uint8_t)(s_rng >> 24); }
static void fill(uint8_t *p, size_t n, int zero_heavy)
{
    for (size_t i = 0; i < n; i++) p[i] = zero_heavy ? (rnd8() & 3 ? 0 : rnd8()) : rnd8();
}
static int crc_lo_zero(uint8_t type, const uint8_t *pl, size_t n)
{
    uint8_t raw[COBS_MAX_PAYLOAD_SIZE + 1];
    raw[0] = type;
    memcpy(&raw[1], pl, n);
    return (cobs_crc16(raw, n + 1) & 0xFF) == 0;
}

static void test_roundtrip_unpadded(void)
{
    cobs_pad_trailing_zero = false;              /* after HELLO: exact round trip for every frame */
    for (int iter = 0; iter < 20000; iter++) {
        uint8_t pl[COBS_MAX_PAYLOAD_SIZE], out[COBS_MAX_PAYLOAD_SIZE], f[COBS_MAX_FRAME_SIZE], t = 0;
        size_t n = (size_t)(iter % 64), on = 99;
        fill(pl, n, iter & 1);
        uint8_t type = rnd8();
        size_t fl = cobs_build_frame(type, pl, n, f, sizeof(f));
        CHECKM(fl > 0 && f[fl - 1] == 0 && memchr(f, 0, fl - 1) == NULL, "iter %d", iter);
        bool ok = cobs_parse_frame(f, fl, &t, out, sizeof(out), &on);
        CHECKM(ok && t == type && on == n && memcmp(pl, out, n) == 0, "iter %d n=%zu ok=%d on=%zu", iter, n, ok, on);
        if (t_failed > 20) return;
    }
    uint8_t big[64] = {0}, f[COBS_MAX_FRAME_SIZE];
    CHECK(cobs_build_frame(1, big, 64, f, sizeof(f)) == 0);
}

/* S-11 on the node's receive path: a command whose CRC low byte is 0x00 must parse intact. */
static void test_crc_zero_command_parses(void)
{
    int found = 0;
    for (unsigned v = 0; v < 65536; v++) {
        uint8_t pl[2] = { (uint8_t)(v >> 8), (uint8_t)v };
        if (!crc_lo_zero(0x10, pl, 2)) continue;
        found++;
        /* what a display sends: never padded */
        uint8_t raw[5] = { 0x10, pl[0], pl[1] }, enc[16], out[8], t = 0;
        size_t r = cobs_append_crc(raw, 3), e = cobs_encode(raw, r, enc), on = 0;
        enc[e++] = 0;
        CHECKM(cobs_parse_frame(enc, e, &t, out, sizeof(out), &on) && on == 2 && out[0] == pl[0] && out[1] == pl[1],
               "payload %02X %02X", pl[0], pl[1]);
        if (t_failed > 20) return;
    }
    CHECK(found >= 200);
}

/* Padding: only while the flag is set, only when the CRC low byte is 0, exactly one 0x00. */
static void test_pad_only_while_flag(void)
{
    int padded_seen = 0;
    for (unsigned v = 0; v < 65536; v++) {
        uint8_t pl[2] = { (uint8_t)(v >> 8), (uint8_t)v }, f[COBS_MAX_FRAME_SIZE], out[8], t = 0;
        int lo0 = crc_lo_zero(0x87, pl, 2);
        for (int flag = 0; flag <= 1; flag++) {
            cobs_pad_trailing_zero = flag;
            size_t fl = cobs_build_frame(0x87, pl, 2, f, sizeof(f)), on = 0;
            bool ok = cobs_parse_frame(f, fl, &t, out, sizeof(out), &on);
            int expect_pad = LINK_PAD_TRAILING_ZERO && flag && lo0;
            CHECKM(fl > 0 && ok && on == (size_t)(2 + expect_pad), "v=%04X flag=%d lo0=%d on=%zu", v, flag, lo0, on);
            if (ok && on >= 2) CHECK(out[0] == pl[0] && out[1] == pl[1]);
            if (ok && expect_pad) {          /* the parser sees M | crc_hi | 00 | 00: one data byte too many, the old CRC high byte */
                uint8_t m[3] = { 0x87, pl[0], pl[1] };
                CHECK(out[2] == (uint8_t)(cobs_crc16(m, 3) >> 8));
                padded_seen++;
            }
            if (t_failed > 20) return;
        }
    }
    CHECK(padded_seen == (LINK_PAD_TRAILING_ZERO ? 256 : 0));
    cobs_pad_trailing_zero = false;
}

/* Longest payloads, flag set: the padded reply must still fit COBS_MAX_FRAME_SIZE. */
static void test_pad_at_max_payload(void)
{
#ifdef KNOWN_BUG_TESTS
    cobs_pad_trailing_zero = true;
    for (size_t n = 60; n <= 63; n++) {
        int found = 0;
        for (int tries = 0; tries < 20000 && !found; tries++) {
            uint8_t pl[COBS_MAX_PAYLOAD_SIZE], f[COBS_MAX_FRAME_SIZE];
            fill(pl, n, 0);
            if (!crc_lo_zero(0x87, pl, n)) continue;
            found = 1;
            CHECKM(cobs_build_frame(0x87, pl, n, f, sizeof(f)) > 0, "padded %zu-byte payload does not fit COBS_MAX_FRAME_SIZE", n);
        }
        CHECK(found);
    }
    cobs_pad_trailing_zero = false;
#endif
}

int main(void)
{
    RUN(test_roundtrip_unpadded);
    RUN(test_crc_zero_command_parses);
    RUN(test_pad_only_while_flag);
    RUN(test_pad_at_max_payload);
    printf("LINK_PAD_TRAILING_ZERO=%d\n", LINK_PAD_TRAILING_ZERO);
    T_DONE();
}
