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
#include <stddef.h>

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

/**
 * @brief Flush UART buffers and clear IO Node command state
 */
void remote_io_flush(void);

/**
 * @brief Reinitialize UART driver completely
 * @return ESP_OK on success
 */
esp_err_t remote_io_reinit(void);

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

/**
 * @brief Get GPIO input level
 */
esp_err_t remote_gpio_get_level(int pin, int *level);

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
 * @brief Stop PWM on a pin
 */
esp_err_t remote_pwm_stop(int pin);

// ==================== I2C Functions ====================

/**
 * @brief Initialize I2C bus on IO Node
 */
esp_err_t remote_i2c_init(int bus_num, int sda_pin, int scl_pin, int freq_hz);

/**
 * @brief Deinitialize I2C bus
 */
esp_err_t remote_i2c_deinit(int bus_num);

/**
 * @brief Write data to I2C device
 */
esp_err_t remote_i2c_write(int bus_num, uint8_t addr, const uint8_t *data, size_t len);

/**
 * @brief Read data from I2C device
 */
esp_err_t remote_i2c_read(int bus_num, uint8_t addr, uint8_t *data, size_t len);

/**
 * @brief Write then read I2C (combined transaction)
 */
esp_err_t remote_i2c_write_read(int bus_num, uint8_t addr,
                                 const uint8_t *write_data, size_t write_len,
                                 uint8_t *read_data, size_t read_len);

/**
 * @brief Scan I2C bus for devices
 */
esp_err_t remote_i2c_scan(int bus_num);

// ==================== 1-Wire Functions ====================

/**
 * @brief Initialize 1-Wire bus on IO Node
 */
esp_err_t remote_onewire_init(int pin);

/**
 * @brief Scan for 1-Wire devices
 */
esp_err_t remote_onewire_scan(int pin, uint8_t rom_codes[][8], int max_devices, int *found_count);

/**
 * @brief Read temperature from DS18B20 (blocking - waits 750ms)
 */
esp_err_t remote_onewire_read_ds18b20(int pin, const uint8_t *rom_code, float *temperature_c);

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
 * @brief Stop all PWM outputs on IO Node
 * Used to ensure clean state after Controller reset
 */
esp_err_t remote_io_stop_all_pwm(void);

#endif // REMOTE_IO_H
