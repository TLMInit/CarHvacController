/* main/cobs_protocol.c (display): COBS, CRC16, frames, S-11 trailing-zero, and the pad rule.
 *
 * strip_pad() in main/remote_io_binary.c is static and that file needs UART/FreeRTOS, so it is
 * NOT reachable from here. ref_strip_pad() below re-implements the rule from the comment above
 * strip_pad() (line-for-line the same logic); what is tested against the real code is
 * cobs_parse_frame / cobs_crc16 on the padded and unpadded byte sequences from that comment. */
#include <stdlib.h>
#include <string.h>
#include "cobs_protocol.h"
#include "t.h"

static uint32_t s_rng = 12345;
static uint8_t rnd8(void) { s_rng = s_rng * 1664525u + 1013904223u; return (uint8_t)(s_rng >> 24); }

static void fill(uint8_t *p, size_t n, int zero_heavy)
{
    for (size_t i = 0; i < n; i++) p[i] = zero_heavy ? (rnd8() & 3 ? 0 : rnd8()) : rnd8();
}

/* Same rule as strip_pad() with s_pad_possible passed in. */
static size_t ref_strip_pad(int pad_possible, uint8_t type, const uint8_t *data, size_t len)
{
    if (!pad_possible || len < 1 || len > COBS_MAX_PAYLOAD_SIZE) return len;
    uint8_t msg[1 + COBS_MAX_PAYLOAD_SIZE];
    msg[0] = type;
    memcpy(&msg[1], data, len);
    if (cobs_crc16(msg, 1 + len) == 0 && cobs_crc16(msg, len) == ((uint16_t)data[len - 1] << 8)) return len - 1;
    return len;
}

/* What a protocol 1 node sends: message + CRC, one extra 0x00 when the CRC low byte is 0. */
static size_t padded_frame(uint8_t type, const uint8_t *pl, size_t n, uint8_t *out)
{
    uint8_t raw[COBS_MAX_PAYLOAD_SIZE + 4];
    size_t r = 0;
    raw[r++] = type;
    if (n) { memcpy(&raw[r], pl, n); r += n; }
    r = cobs_append_crc(raw, r);
    if (raw[r - 1] == 0x00) raw[r++] = 0x00;
    size_t e = cobs_encode(raw, r, out);
    out[e++] = COBS_FRAME_DELIMITER;
    return e;
}

static void test_crc_known_value(void)
{
    const uint8_t s[] = "123456789";
    CHECK(cobs_crc16(s, 9) == 0x29B1);          /* CRC-16/CCITT-FALSE check value */
    CHECK(cobs_crc16(NULL, 0) == 0xFFFF);
}

static void test_raw_cobs_roundtrip(void)
{
    uint8_t in[700], enc[800], dec[800];
    for (size_t n = 1; n <= 600; n++) {
        for (int mode = 0; mode < 3; mode++) {
            if (mode == 2) { for (size_t i = 0; i < n; i++) in[i] = (uint8_t)(1 + i % 255); }   /* no zeros: 0xFF blocks */
            else fill(in, n, mode);
            size_t e = cobs_encode(in, n, enc);
            int clean = 1;
            for (size_t i = 0; i < e; i++) clean &= enc[i] != 0;
            size_t d = cobs_decode(enc, e, dec);
            CHECKM(clean && d == n && memcmp(in, dec, n) == 0, "n=%zu mode=%d enc=%zu dec=%zu", n, mode, e, d);
        }
    }
    CHECK(cobs_encode(in, 0, enc) == 0);
}

static void test_frame_roundtrip_random(void)
{
    for (int iter = 0; iter < 20000; iter++) {
        uint8_t pl[COBS_MAX_PAYLOAD_SIZE], out[COBS_MAX_PAYLOAD_SIZE], frame[COBS_MAX_FRAME_SIZE];
        size_t n = (size_t)(iter % 64);             /* 0..63 */
        fill(pl, n, iter & 1);
        uint8_t type = rnd8(), t2 = 0xEE;
        size_t fl = cobs_build_frame(type, pl, n, frame, sizeof(frame));
        CHECKM(fl > 0 && frame[fl - 1] == 0 && memchr(frame, 0, fl - 1) == NULL, "iter %d n=%zu", iter, n);
        size_t on = 99;
        bool ok = cobs_parse_frame(frame, fl, &t2, out, sizeof(out), &on);
        CHECKM(ok && t2 == type && on == n && memcmp(pl, out, n) == 0, "iter %d n=%zu ok=%d on=%zu", iter, n, ok, on);
        if (t_failed > 20) return;
    }
    uint8_t big[64] = {0}, frame[COBS_MAX_FRAME_SIZE];
    CHECK(cobs_build_frame(1, big, 64, frame, sizeof(frame)) == 0);     /* 64 data bytes do not fit */
}

static void test_crc_ending_in_zero_not_truncated(void)    /* S-11 */
{
    int found = 0;
    for (unsigned v = 0; v < 65536; v++) {
        uint8_t pl[2] = { (uint8_t)(v >> 8), (uint8_t)v }, out[8], frame[COBS_MAX_FRAME_SIZE], t = 0;
        uint8_t raw[3] = { 0x87, pl[0], pl[1] };
        if ((cobs_crc16(raw, 3) & 0xFF) != 0) continue;
        found++;
        size_t fl = cobs_build_frame(0x87, pl, 2, frame, sizeof(frame)), on = 0;
        CHECKM(fl > 0 && cobs_parse_frame(frame, fl, &t, out, sizeof(out), &on) && on == 2 && out[0] == pl[0] && out[1] == pl[1],
               "payload %02X %02X", pl[0], pl[1]);
        if (t_failed > 20) return;
    }
    CHECK(found >= 200);
    /* decoder level: the last decoded byte is 0x00 and must survive */
    const uint8_t m[] = { 0x11, 0x22, 0x00 }, e[] = { 0x03, 0x11, 0x22, 0x01 };
    uint8_t enc[8], dec[8];
    size_t n = cobs_encode(m, 3, enc);
    CHECK(n == 4 && memcmp(enc, e, 4) == 0);
    CHECK(cobs_decode(enc, n, dec) == 3 && memcmp(dec, m, 3) == 0);
    /* also with a delimiter-terminated frame whose message ends in 0x00 0x00 */
    const uint8_t m2[] = { 0x05, 0x00, 0x00 };
    n = cobs_encode(m2, 3, enc);
    CHECK(cobs_decode(enc, n, dec) == 3 && memcmp(dec, m2, 3) == 0);
}

static void test_pad_rule_comment_bytes(void)
{
    /* TEMP_VALUE -1.27 C: type 0x87, data FF 81, CRC F0 00 */
    const uint8_t msg[] = { 0x87, 0xFF, 0x81 };
    CHECK(cobs_crc16(msg, 3) == 0xF000);
    const uint8_t m4[] = { 0x87, 0xFF, 0x81, 0xF0 };
    CHECK(cobs_crc16(m4, 4) == 0x0000);

    uint8_t pad_raw[] = { 0x87, 0xFF, 0x81, 0xF0, 0x00, 0x00 }, unpad_raw[] = { 0x87, 0xFF, 0x81, 0xF0, 0x00 };
    uint8_t f[16], t, d[16];
    size_t e, dl;

    e = cobs_encode(pad_raw, sizeof(pad_raw), f); f[e++] = 0;
    CHECK(cobs_parse_frame(f, e, &t, d, sizeof(d), &dl));                  /* padded: CRC-valid, one byte too long */
    CHECK(t == 0x87 && dl == 3 && d[0] == 0xFF && d[1] == 0x81 && d[2] == 0xF0);
    CHECK(ref_strip_pad(1, t, d, dl) == 2);                                /* stripped while padding is possible */
    CHECK(ref_strip_pad(0, t, d, dl) == 3);                                /* kept after protocol 2 HELLO_RESP */

    e = cobs_encode(unpad_raw, sizeof(unpad_raw), f); f[e++] = 0;
    CHECK(cobs_parse_frame(f, e, &t, d, sizeof(d), &dl));                  /* unpadded: parsed whole */
    CHECK(t == 0x87 && dl == 2 && d[0] == 0xFF && d[1] == 0x81);
    CHECK(ref_strip_pad(1, t, d, dl) == 2);                                /* crc16(87 FF 81) = F000 != 0: kept */
}

/* All 65536 2-byte payloads through a protocol 1 node (pad iff CRC low byte is 0) and a pad-aware
 * link layer (reference rule): the payload must come back exactly. Same sweep as the WP-C note. */
static void test_pad_rule_sweep(void)
{
    int padded = 0, bad = 0;
    for (unsigned v = 0; v < 65536; v++) {
        uint8_t pl[2] = { (uint8_t)(v >> 8), (uint8_t)v }, f[COBS_MAX_FRAME_SIZE + 4], d[8], t;
        uint8_t raw[3] = { 0x87, pl[0], pl[1] };
        padded += (cobs_crc16(raw, 3) & 0xFF) == 0;
        size_t e = padded_frame(0x87, pl, 2, f), dl = 0;
        if (!cobs_parse_frame(f, e, &t, d, sizeof(d), &dl)) { bad++; continue; }
        dl = ref_strip_pad(1, t, d, dl);
        if (dl != 2 || d[0] != pl[0] || d[1] != pl[1]) bad++;
    }
    CHECK(padded == 256);
    CHECKM(bad == 0, "%d of 65536 payloads wrong after pad strip", bad);

    /* After HELLO the node stops padding: unpadded frames must pass untouched with pad_possible = 0,
     * and with pad_possible = 1 only the CRC == 0x0000 corner (1 in 65536) is a false match. */
    int false_match = 0;
    for (unsigned v = 0; v < 65536; v++) {
        uint8_t pl[2] = { (uint8_t)(v >> 8), (uint8_t)v }, f[COBS_MAX_FRAME_SIZE], d[8], t;
        size_t e = cobs_build_frame(0x87, pl, 2, f, sizeof(f)), dl = 0;
        CHECK(cobs_parse_frame(f, e, &t, d, sizeof(d), &dl) && dl == 2);
        CHECK(ref_strip_pad(0, t, d, dl) == 2);
        if (ref_strip_pad(1, t, d, dl) != 2) false_match++;
    }
    CHECKM(false_match == 0 || false_match <= 2, "false matches %d", false_match);
}

static void test_receiver(void)
{
    cobs_receiver_t rx;
    cobs_receiver_init(&rx, 50);
    uint8_t f1[COBS_MAX_FRAME_SIZE], f2[COBS_MAX_FRAME_SIZE], got[COBS_MAX_FRAME_SIZE], pl[8] = {0, 1, 0, 2}, out[8], t;
    size_t n1 = cobs_build_frame(0x10, pl, 4, f1, sizeof(f1)), n2 = cobs_build_frame(0x11, NULL, 0, f2, sizeof(f2)), on;
    bool done = false;
    for (size_t i = 0; i < n1; i++) done = cobs_receiver_feed(&rx, &f1[i], 1, 100 + (uint32_t)i);
    CHECK(done);
    size_t gl = cobs_receiver_get_frame(&rx, got, sizeof(got));
    CHECK(gl == n1 - 1 && cobs_parse_frame(got, gl, &t, out, sizeof(out), &on) && t == 0x10 && on == 4 && memcmp(out, pl, 4) == 0);
    cobs_receiver_reset(&rx);
    CHECK(!cobs_receiver_feed(&rx, f2, 2, 500));                   /* partial */
    CHECK(cobs_receiver_feed(&rx, f2 + 2, n2 - 2, 510));
    gl = cobs_receiver_get_frame(&rx, got, sizeof(got));
    CHECK(cobs_parse_frame(got, gl, &t, out, sizeof(out), &on) && t == 0x11 && on == 0);
    /* a corrupted frame is rejected */
    f1[1] ^= 0x40;
    CHECK(!cobs_parse_frame(f1, n1, &t, out, sizeof(out), &on));
}

int main(void)
{
    RUN(test_crc_known_value);
    RUN(test_raw_cobs_roundtrip);
    RUN(test_frame_roundtrip_random);
    RUN(test_crc_ending_in_zero_not_truncated);
    RUN(test_pad_rule_comment_bytes);
    RUN(test_pad_rule_sweep);
    RUN(test_receiver);
    T_DONE();
}
