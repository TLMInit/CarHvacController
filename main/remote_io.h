/**
 * @file remote_io.h
 * @brief Remote IO interface for communicating with IO Node ESP32
 * 
 * This module sends UART commands to the IO Node and receives responses.
 * It provides functions that mirror local GPIO/PWM/I2C/1-Wire operations.
 */

#ifndef REMOTE_IO_H
#define REMOTE_IO_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* The link counts as "connected" this long after the node's last reply. HELLO sends it to the
 * node, which uses it as its link watchdog (node protocol_server.c, clamped to LINK_WINDOW_MIN_MS
 * ..LINK_WINDOW_MAX_MS in cobs_protocol.h). Re-assert periods must stay at most half of it:
 * MOTOR_LINK_REFRESH_MS (motor_link.h) and CTRL_REASSERT_MS (hvac_controller.c), both asserted. */
#define REMOTE_IO_LINK_WINDOW_MS    3000u

#ifndef REMOTE_IO_LOG_ENABLE
#define REMOTE_IO_LOG_ENABLE 0
#endif

/**
 * @brief Initialize remote IO communication
 * @param uart_num UART port number
 * @param tx_pin TX GPIO pin
 * @param rx_pin RX GPIO pin
 * @param baud_rate Baud rate
 * @return ESP_OK on success
 */
esp_err_t remote_io_init(int uart_num, int tx_pin, int rx_pin, int baud_rate);

/**
 * @brief Wait for IO Node to be ready
 * @param timeout_ms Timeout in milliseconds
 * @return ESP_OK when ready, ESP_ERR_TIMEOUT if not
 */
esp_err_t remote_io_wait_ready(int timeout_ms);

/** True when a CRC-valid response has arrived within the last 3 seconds. */
bool remote_io_is_connected(void);

/** True when the most recent command on the link, from any task, timed out. Turns false
 *  again with the next answered command (the sensors task polls every 100 ms), so it
 *  reports a broken link within one command instead of after REMOTE_IO_LINK_WINDOW_MS. */
bool remote_io_last_command_timed_out(void);

/** Snapshot command counters. Any output pointer may be NULL. */
void remote_io_get_stats(uint32_t *tx_frames, uint32_t *rx_frames,
                         uint32_t *crc_errors, uint32_t *timeouts);

// ==================== GPIO Functions ====================

typedef enum {
    REMOTE_GPIO_INPUT = 0,
    REMOTE_GPIO_OUTPUT = 1,
    REMOTE_GPIO_INPUT_PULLUP = 2,
    REMOTE_GPIO_INPUT_PULLDOWN = 3,
    REMOTE_GPIO_OUTPUT_OD = 4,
} remote_gpio_mode_t;

/**
 * @brief Set GPIO pin mode on IO Node
 */
esp_err_t remote_gpio_set_mode(int pin, remote_gpio_mode_t mode);

/**
 * @brief Set GPIO output level
 */
esp_err_t remote_gpio_set_level(int pin, int level);

// ==================== PWM Functions ====================

/**
 * @brief Configure PWM on a pin
 * @param pin GPIO pin
 * @param freq_hz PWM frequency in Hz
 * @param resolution_bits PWM resolution (1-15 bits)
 */
esp_err_t remote_pwm_configure(int pin, int freq_hz, int resolution_bits);

/**
 * @brief Set PWM duty cycle
 * @param pin GPIO pin (must be configured first)
 * @param duty Duty value (0 to (1<<resolution_bits)-1)
 */
esp_err_t remote_pwm_set_duty(int pin, int duty);

/**
 * @brief PWM_INIT_HZ: frequency in Hz and resolution (node rules: 1..40000 Hz, 1..14 bits,
 * 78126 <= freq x 2^bits <= 80 MHz). ESP_ERR_NOT_SUPPORTED without NODE_CAP_PWM_HZ (nothing sent).
 */
esp_err_t remote_pwm_init_hz(int pin, uint32_t freq_hz, int bits);

/**
 * @brief 3-byte PWM_SET: duty in raw counts of the channel, 0..2^bits (= full on).
 * ESP_ERR_NOT_SUPPORTED without NODE_CAP_PWM_HZ (nothing sent).
 */
esp_err_t remote_pwm_set_duty16(int pin, uint16_t counts);

// ==================== I2C Functions ====================

/**
 * @brief Write data to I2C device
 */
esp_err_t remote_i2c_write(int bus_num, uint8_t addr, const uint8_t *data, size_t len);

/**
 * @brief Write then read I2C (combined transaction)
 */
esp_err_t remote_i2c_write_read(int bus_num, uint8_t addr,
                                 const uint8_t *write_data, size_t write_len,
                                 uint8_t *read_data, size_t read_len);

// ==================== DS18B20 Async Functions ====================

/**
 * @brief Start DS18B20 temperature conversion (async - fire and forget)
 * If rom_code is NULL, uses Skip ROM (all devices); otherwise Match ROM.
 * Call this, then wait 750ms, then call remote_ds18b20_read_temp().
 * @return ESP_OK on success
 */
esp_err_t remote_ds18b20_start_conversion(const uint8_t *rom_code);

/**
 * @brief Read DS18B20 temperature result (call after conversion complete)
 * Must be called at least 750ms after remote_ds18b20_start_conversion()
 * If rom_code is NULL, uses Skip ROM (all devices); otherwise Match ROM.
 * @param temperature_c Output temperature in Celsius
 * @return ESP_OK on success
 */
esp_err_t remote_ds18b20_read_temp(const uint8_t *rom_code, float *temperature_c);

// ==================== Bulk ADC Functions ====================

/**
 * @brief Read multiple ADS1115 channels in one transaction (fast)
 * 
 * This reduces UART round-trip overhead by reading multiple channels at once.
 * 
 * @param bus_num I2C bus number
 * @param i2c_addr ADS1115 I2C address
 * @param channels Array of channel numbers to read (0-3)
 * @param num_channels Number of channels to read
 * @param values Output array for raw ADC values
 * @return ESP_OK on success
 */
esp_err_t remote_ads1115_read_bulk(int bus_num, uint8_t i2c_addr, 
                                   const uint8_t *channels, int num_channels,
                                   int16_t *values);

/**
 * @brief Read single ADS1115 channel (convenience function)
 * 
 * @param bus_num I2C bus number  
 * @param i2c_addr ADS1115 I2C address
 * @param channel Channel number (0-3)
 * @param value Output raw ADC value
 * @return ESP_OK on success
 */
esp_err_t remote_ads1115_read(int bus_num, uint8_t i2c_addr, uint8_t channel, int16_t *value);

// ==================== Node Control ====================

/**
 * @brief Send reset command to IO Node
 * This will restart the IO Node entirely
 */
esp_err_t remote_io_reset_node(void);

/**
 * @brief Ask the node for its VERSION string (version, uptime, reset reason).
 * Logs the first answer, and a warning whenever the node's uptime went backwards (it restarted).
 */
esp_err_t remote_io_poll_node(void);

/**
 * @brief Last node info from remote_io_poll_node(); version is "?" until the first answer
 */
void remote_io_get_node_info(const char **version, uint32_t *uptime_s, uint32_t *resets);

// ==================== v2.1 interfaces (implemented by WP-C) ====================

/**
 * @brief HELLO handshake: sends PROTOCOL_VERSION and REMOTE_IO_LINK_WINDOW_MS (u16 BE), records
 * the node's protocol and capability bits. Sent by the link layer at every connect (boot, link
 * recovered, node restart); callable directly. A node without HELLO answers ERR_INVALID_CMD:
 * then node_proto = 1, caps = 0 and the decoder keeps tolerating the trailing pad. A 2.3 node
 * answers the window with ERR_INVALID_ARGS: HELLO is repeated with the version byte only and
 * that node keeps its own LINK_WATCHDOG_MS (until it restarts).
 */
esp_err_t remote_io_hello(uint8_t *node_proto, uint8_t *node_caps);

/** @brief Answered HELLOs since boot (0 = none yet). It changes at every connect (boot, link
 * recovered, node restart): outputs the node does not set up at its own boot (PWM_INIT_HZ
 * channels) re-initialise when it moves. */
uint32_t remote_io_hello_seq(void);

/** @brief 1-Wire search on the node's bus. Fills up to max ROM codes; needs NODE_CAP_SEARCH. */
esp_err_t remote_ds18b20_search(uint8_t roms[][8], int max, int *count);

/** @brief Progress of a node OTA: bytes sent so far of total. Called from the OTA caller's task. */
typedef void (*remote_ota_progress_cb_t)(uint32_t done, uint32_t total, void *ctx);

/**
 * @brief Stream a node firmware image (already in RAM/PSRAM) to the node:
 * OTA_BEGIN, OTA_DATA at full link speed, OTA_END, then RESET; waits for the node
 * to come back and answer PING and VERSION (which confirms the image on the node).
 * Blocking; call from a worker task, never from the LVGL task. Needs NODE_CAP_OTA.
 * ESP_ERR_INVALID_VERSION: the image is not an ESP32-S3 NODE_PROJECT_NAME build
 * (checked here before BEGIN, and again by the node). ESP_ERR_INVALID_SIZE: larger than
 * NODE_OTA_SLOT_SIZE (the node also checks its real slot at OTA_BEGIN).
 */
#define NODE_PROJECT_NAME "CarHvacIoNode"   /* = project() in the node's CMakeLists.txt */
#define NODE_OTA_SLOT_SIZE 0xF0000u         /* 960 KB = ota_0 / ota_1 size in the node's partitions.csv */
esp_err_t remote_io_ota_node(const uint8_t *image, size_t len, const uint8_t sha256[32],
                             remote_ota_progress_cb_t cb, void *ctx);

/** @brief Link health snapshot for the diagnostics page and /api/diag. */
typedef struct {
    bool connected;                 /* a valid frame within REMOTE_IO_LINK_WINDOW_MS */
    uint32_t rtt_last_us;           /* last completed round trip */
    uint32_t rtt_p95_us;            /* 95th percentile over the last minute */
    uint32_t timeouts_last_min;
    uint32_t tx_frames, rx_frames, crc_errors, timeouts, mismatches;
    uint8_t node_proto;             /* 0 until HELLO answered; 1 = node without HELLO */
    uint8_t node_caps;              /* NODE_CAP_* */
    char node_version[48];          /* from VERSION, without the up=/rst= part; "?" if unknown */
    uint32_t node_uptime_s;
    uint32_t node_restarts;         /* restarts seen since display boot (not counting the boot reset) */
} remote_io_health_t;

void remote_io_get_health(remote_io_health_t *out);

#endif // REMOTE_IO_H
