/* Link command payloads: see link_payload.h. No ESP-IDF. */
#include <string.h>
#include "link_payload.h"

size_t link_payload_pwm_init_hz(uint8_t out[6], uint8_t pin, uint32_t freq_hz, uint8_t bits)
{
    out[0] = pin;
    out[1] = (uint8_t)(freq_hz >> 24);
    out[2] = (uint8_t)(freq_hz >> 16);
    out[3] = (uint8_t)(freq_hz >> 8);
    out[4] = (uint8_t)freq_hz;
    out[5] = bits;
    return 6;
}

size_t link_payload_pwm_set16(uint8_t out[3], uint8_t pin, uint16_t counts)
{
    out[0] = pin;
    out[1] = (uint8_t)(counts >> 8);
    out[2] = (uint8_t)counts;
    return 3;
}

size_t link_payload_i2c_write(uint8_t *out, size_t out_size, uint8_t addr, const uint8_t *data, size_t n)
{
    if (n < 1 || n + 2 > out_size) {
        return 0;
    }
    out[0] = addr;
    out[1] = data[0];               /* register (or the first byte the device receives) */
    out[2] = (uint8_t)(n - 1);
    memcpy(&out[3], &data[1], n - 1);
    return n + 2;
}
