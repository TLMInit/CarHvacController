/**
 * @file io_onewire.c
 * @brief DS18B20 on the node's 1-Wire bus (espressif/onewire_bus, RMT backend).
 *
 * The display drives the sensor as two separate commands, Convert T and
 * Read Scratchpad, 750 ms apart, so the node never blocks on a conversion.
 */

#include "io_onewire.h"
#include "node_config.h"
#include "onewire_bus.h"
#include "onewire_bus_impl_rmt.h"
#include "esp_log.h"

static const char *TAG = "IO_1W";

#define OW_CMD_SKIP_ROM         0xCC
#define OW_CMD_MATCH_ROM        0x55
#define DS18B20_CMD_CONVERT_T   0x44
#define DS18B20_CMD_READ_SCRATCH 0xBE

static onewire_bus_handle_t s_bus;

/* Dallas CRC-8, polynomial 0x31 reflected (0x8C), init 0 */
static uint8_t crc8_dallas(const uint8_t *data, size_t len)
{
    uint8_t crc = 0;
    while (len--) {
        uint8_t b = *data++;
        for (int i = 0; i < 8; i++) {
            uint8_t mix = (crc ^ b) & 0x01;
            crc >>= 1;
            if (mix) {
                crc ^= 0x8C;
            }
            b >>= 1;
        }
    }
    return crc;
}

esp_err_t io_onewire_init(void)
{
    onewire_bus_config_t bus_cfg = {
        .bus_gpio_num = NODE_ONEWIRE_PIN,
    };
    onewire_bus_rmt_config_t rmt_cfg = {
        .max_rx_bytes = 10,     /* scratchpad is 9 bytes */
    };
    esp_err_t err = onewire_new_bus_rmt(&bus_cfg, &rmt_cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "onewire_new_bus_rmt: %s", esp_err_to_name(err));
        s_bus = NULL;
        return err;
    }
    err = onewire_bus_reset(s_bus);
    ESP_LOGI(TAG, "1-Wire on GPIO%d: %s", NODE_ONEWIRE_PIN,
             err == ESP_OK ? "device present" : "no device yet");
    return ESP_OK;
}

static esp_err_t select_device(const uint8_t *rom)
{
    if (s_bus == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = onewire_bus_reset(s_bus);
    if (err != ESP_OK) {
        return err;        /* ESP_ERR_NOT_FOUND when nothing answers */
    }
    if (rom) {
        uint8_t cmd[9];
        cmd[0] = OW_CMD_MATCH_ROM;
        for (int i = 0; i < 8; i++) {
            cmd[1 + i] = rom[i];
        }
        return onewire_bus_write_bytes(s_bus, cmd, sizeof(cmd));
    }
    uint8_t skip = OW_CMD_SKIP_ROM;
    return onewire_bus_write_bytes(s_bus, &skip, 1);
}

esp_err_t io_ds18b20_start(const uint8_t *rom)
{
    esp_err_t err = select_device(rom);
    if (err != ESP_OK) {
        return err;
    }
    uint8_t cmd = DS18B20_CMD_CONVERT_T;
    return onewire_bus_write_bytes(s_bus, &cmd, 1);
}

esp_err_t io_ds18b20_read(const uint8_t *rom, int16_t *temp_x100)
{
    if (temp_x100 == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = select_device(rom);
    if (err != ESP_OK) {
        return err;
    }
    uint8_t cmd = DS18B20_CMD_READ_SCRATCH;
    err = onewire_bus_write_bytes(s_bus, &cmd, 1);
    if (err != ESP_OK) {
        return err;
    }
    uint8_t sp[9];
    err = onewire_bus_read_bytes(s_bus, sp, sizeof(sp));
    if (err != ESP_OK) {
        return err;
    }
    if (crc8_dallas(sp, 8) != sp[8]) {
        return ESP_ERR_INVALID_CRC;
    }
    /* 12-bit default resolution: 1/16 degC per LSB, sign-extended */
    int16_t raw = (int16_t)(((uint16_t)sp[1] << 8) | sp[0]);
    *temp_x100 = (int16_t)((raw * 100) / 16);
    return ESP_OK;
}
