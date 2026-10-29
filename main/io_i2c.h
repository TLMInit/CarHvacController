/**
 * @file io_i2c.h
 * @brief The node's single I2C bus: raw register access (BME280) and ADS1115 reads.
 */

#ifndef IO_I2C_H
#define IO_I2C_H

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t io_i2c_init(void);

/** Write [reg, data...] to a 7-bit address. */
esp_err_t io_i2c_write(uint8_t addr, uint8_t reg, const uint8_t *data, size_t len);

/** Write reg, repeated start, read len bytes. */
esp_err_t io_i2c_read(uint8_t addr, uint8_t reg, uint8_t *data, size_t len);

/**
 * Single-shot, single-ended conversion on one ADS1115 input.
 * PGA +/-4.096 V, 860 SPS, comparator off. Returns the raw signed 16-bit result,
 * which is what the display's ads1115_driver.c expects (it scales by 4096/32767 mV).
 */
esp_err_t io_ads1115_read(uint8_t addr, uint8_t channel, int16_t *value);

#endif /* IO_I2C_H */
