/**
 * @file io_i2c.c
 * @brief I2C bus of the IO node (new i2c_master driver, ESP-IDF >= 5.2).
 *
 * Device handles are created on first use and cached per address, so the
 * display can talk to any 7-bit address without an explicit "add device" step.
 */

#include <string.h>
#include "io_i2c.h"
#include "node_config.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

static const char *TAG = "IO_I2C";

typedef struct {
    uint8_t addr;
    i2c_master_dev_handle_t dev;
} dev_entry_t;

static i2c_master_bus_handle_t s_bus;
static dev_entry_t s_devs[NODE_I2C_MAX_DEVICES];
static int s_dev_count;

/* ADS1115 register map and config bits */
#define ADS_REG_CONVERSION      0x00
#define ADS_REG_CONFIG          0x01
#define ADS_CFG_OS_START        0x8000              /* write: start; read: 1 = idle */
#define ADS_CFG_MUX_SINGLE(ch)  ((0x4U | ((ch) & 0x3U)) << 12)
#define ADS_CFG_PGA_4V096       (0x1U << 9)
#define ADS_CFG_MODE_SINGLE     (0x1U << 8)
#define ADS_CFG_DR_860SPS       (0x7U << 5)
#define ADS_CFG_COMP_DISABLE    0x0003
#define ADS_FIRST_WAIT_US       1000                /* conversion at 860 SPS takes ~1.16 ms */
#define ADS_POLL_STEP_US        100
#define ADS_POLL_MAX_STEPS      100                 /* ~1 ms + 100 x (100 us + one read) ~= 20 ms worst case */

esp_err_t io_i2c_init(void)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port = NODE_I2C_PORT,
        .sda_io_num = NODE_I2C_SDA,
        .scl_io_num = NODE_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,       /* the modules carry their own; harmless */
    };
    esp_err_t err = i2c_new_master_bus(&cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus: %s", esp_err_to_name(err));
        return err;
    }
    s_dev_count = 0;
    ESP_LOGI(TAG, "I2C ready on SDA GPIO%d / SCL GPIO%d, %d kHz",
             NODE_I2C_SDA, NODE_I2C_SCL, NODE_I2C_FREQ_HZ / 1000);
    return ESP_OK;
}

static esp_err_t get_device(uint8_t addr, i2c_master_dev_handle_t *dev)
{
    for (int i = 0; i < s_dev_count; i++) {
        if (s_devs[i].addr == addr) {
            *dev = s_devs[i].dev;
            return ESP_OK;
        }
    }
    if (s_dev_count >= NODE_I2C_MAX_DEVICES) {
        return ESP_ERR_NO_MEM;
    }
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = NODE_I2C_FREQ_HZ,
    };
    esp_err_t err = i2c_master_bus_add_device(s_bus, &cfg, dev);
    if (err != ESP_OK) {
        return err;
    }
    s_devs[s_dev_count].addr = addr;
    s_devs[s_dev_count].dev = *dev;
    s_dev_count++;
    return ESP_OK;
}

esp_err_t io_i2c_write(uint8_t addr, uint8_t reg, const uint8_t *data, size_t len)
{
    if (len > 62) {
        return ESP_ERR_INVALID_SIZE;
    }
    i2c_master_dev_handle_t dev;
    esp_err_t err = get_device(addr, &dev);
    if (err != ESP_OK) {
        return err;
    }
    uint8_t buf[64];
    buf[0] = reg;
    if (len > 0) {
        memcpy(&buf[1], data, len);
    }
    return i2c_master_transmit(dev, buf, len + 1, NODE_I2C_TIMEOUT_MS);
}

esp_err_t io_i2c_read(uint8_t addr, uint8_t reg, uint8_t *data, size_t len)
{
    if (len == 0 || len > 62 || data == NULL) {
        return ESP_ERR_INVALID_SIZE;
    }
    i2c_master_dev_handle_t dev;
    esp_err_t err = get_device(addr, &dev);
    if (err != ESP_OK) {
        return err;
    }
    return i2c_master_transmit_receive(dev, &reg, 1, data, len, NODE_I2C_TIMEOUT_MS);
}

esp_err_t io_ads1115_read(uint8_t addr, uint8_t channel, int16_t *value)
{
    if (channel > 3 || value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    i2c_master_dev_handle_t dev;
    esp_err_t err = get_device(addr, &dev);
    if (err != ESP_OK) {
        return err;
    }

    uint16_t cfg = ADS_CFG_OS_START | ADS_CFG_MUX_SINGLE(channel) | ADS_CFG_PGA_4V096 |
                   ADS_CFG_MODE_SINGLE | ADS_CFG_DR_860SPS | ADS_CFG_COMP_DISABLE;
    uint8_t start[3] = { ADS_REG_CONFIG, (uint8_t)(cfg >> 8), (uint8_t)(cfg & 0xFF) };
    err = i2c_master_transmit(dev, start, sizeof(start), NODE_I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }

    /* Conversion takes ~1.2 ms at 860 SPS; wait most of it, then poll the OS bit. */
    uint8_t reg = ADS_REG_CONFIG;
    uint8_t rb[2];
    bool done = false;
    esp_rom_delay_us(ADS_FIRST_WAIT_US);
    for (int i = 0; i < ADS_POLL_MAX_STEPS; i++) {
        if (i > 0) {
            esp_rom_delay_us(ADS_POLL_STEP_US);
        }
        err = i2c_master_transmit_receive(dev, &reg, 1, rb, 2, NODE_I2C_TIMEOUT_MS);
        if (err != ESP_OK) {
            return err;
        }
        if (rb[0] & 0x80) {
            done = true;
            break;
        }
    }
    if (!done) {
        return ESP_ERR_TIMEOUT;
    }

    reg = ADS_REG_CONVERSION;
    err = i2c_master_transmit_receive(dev, &reg, 1, rb, 2, NODE_I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }
    *value = (int16_t)(((uint16_t)rb[0] << 8) | rb[1]);
    return ESP_OK;
}
