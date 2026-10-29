/**
 * @file io_onewire.h
 * @brief DS18B20 duct sensor on the node's 1-Wire bus.
 */

#ifndef IO_ONEWIRE_H
#define IO_ONEWIRE_H

#include <stdint.h>
#include "esp_err.h"

esp_err_t io_onewire_init(void);

/** Reset, Skip ROM (rom == NULL) or Match ROM, Convert T. ESP_ERR_NOT_FOUND if no presence pulse. */
esp_err_t io_ds18b20_start(const uint8_t *rom);

/**
 * Reset, Skip/Match ROM, Read Scratchpad. Call >= 750 ms after io_ds18b20_start().
 * ESP_ERR_NOT_FOUND: no presence pulse. ESP_ERR_INVALID_CRC: scratchpad CRC failed.
 */
esp_err_t io_ds18b20_read(const uint8_t *rom, int16_t *temp_x100);

#endif /* IO_ONEWIRE_H */
