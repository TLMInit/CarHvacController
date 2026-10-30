/**
 * @file link_payload.h
 * @brief Payload layouts of link commands, pure (no ESP-IDF): remote_io_binary.c sends them and
 * test/host checks them against the node README's byte examples.
 */
#ifndef LINK_PAYLOAD_H
#define LINK_PAYLOAD_H

#include <stddef.h>
#include <stdint.h>

/* PWM_INIT_HZ 0x22: pin, freq_hz u32 BE, resolution bits. Returns 6. */
size_t link_payload_pwm_init_hz(uint8_t out[6], uint8_t pin, uint32_t freq_hz, uint8_t bits);

/* 3-byte PWM_SET 0x20: pin, duty u16 BE in raw counts. Returns 3. */
size_t link_payload_pwm_set16(uint8_t out[3], uint8_t pin, uint16_t counts);

/* I2C_WRITE 0x30: addr, reg = data[0], len = n - 1, data[1..n-1]. Returns n + 2, or 0 when n is
 * 0 or the payload does not fit out_size. */
size_t link_payload_i2c_write(uint8_t *out, size_t out_size, uint8_t addr, const uint8_t *data, size_t n);

#endif /* LINK_PAYLOAD_H */
