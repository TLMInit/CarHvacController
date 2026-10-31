/**
 * @file io_onewire.c
 * @brief DS18B20 on the node's 1-Wire bus (espressif/onewire_bus, RMT backend).
 *
 * The display drives the sensor as two separate commands, Convert T and
 * Read Scratchpad, 750 ms apart, so the node never blocks on a conversion.
 */

#include <string.h>
#include "io_onewire.h"
#include "node_config.h"
#include "onewire_bus.h"
#include "onewire_bus_impl_rmt.h"
#include "onewire_device.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "IO_1W";

#define DS18B20_READ_ATTEMPTS   3       /* the original node retried scratchpad reads too; all inside the
                                           display's remote_io_binary.c CMD_TIMEOUT_MS */
/* One ROM takes ~200 RMT bit slots (~20-30 ms); no new ROM is started after this,
 * so the reply stays inside the display's remote_io_binary.c SEARCH_TIMEOUT_MS (500 ms). */
#define ONEWIRE_SEARCH_BUDGET_US 100000

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

static esp_err_t read_scratchpad_once(const uint8_t *rom, int16_t *temp_x100)
{
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

esp_err_t io_ds18b20_read(const uint8_t *rom, int16_t *temp_x100)
{
    if (temp_x100 == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = ESP_FAIL;
    for (int attempt = 1; attempt <= DS18B20_READ_ATTEMPTS; attempt++) {
        err = read_scratchpad_once(rom, temp_x100);
        if (err == ESP_OK) {
            if (attempt > 1) {
                ESP_LOGW(TAG, "Scratchpad read succeeded after %d retry(ies)", attempt - 1);
            }
            return ESP_OK;
        }
        if (err == ESP_ERR_NOT_FOUND) {
            return err;         /* no presence pulse: retrying will not help */
        }
        ESP_LOGW(TAG, "Scratchpad %s (attempt %d)",
                 err == ESP_ERR_INVALID_CRC ? "CRC mismatch" : "read failed", attempt);
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    return err;
}

esp_err_t io_onewire_search(uint8_t roms[][8], size_t max_roms, size_t *count)
{
    *count = 0;
    if (s_bus == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    onewire_device_iter_handle_t iter;
    esp_err_t err = onewire_new_device_iter(s_bus, &iter);
    if (err != ESP_OK) {
        return err;
    }
    int64_t t0 = esp_timer_get_time();
    while (*count < max_roms && esp_timer_get_time() - t0 < ONEWIRE_SEARCH_BUDGET_US) {
        onewire_device_t dev;
        err = onewire_device_iter_get_next(iter, &dev);
        if (err != ESP_OK) {
            break;
        }
        /* address holds the ROM bytes in bus order (family code first) on this little-endian CPU */
        memcpy(roms[(*count)++], &dev.address, 8);
    }
    onewire_del_device_iter(iter);
    ESP_LOGI(TAG, "Search: %u ROM(s) in %lld us", (unsigned)*count, (long long)(esp_timer_get_time() - t0));
    /* NOT_FOUND = search finished (or nothing on the bus); a later failure keeps what was found */
    return (err == ESP_ERR_NOT_FOUND || *count > 0) ? ESP_OK : err;
}
