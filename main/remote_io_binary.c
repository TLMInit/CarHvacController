/**
 * @file remote_io.c
 * @brief Remote IO implementation - Binary COBS protocol over UART with IO Node
 * 
 * Uses COBS framing with CRC16 for reliable communication.
 * Each command runs in the calling task under one mutex: one command on the
 * wire at a time, and nothing outlives the call that owns it.
 */

#include "hvac_hw_config.h"

#if USE_REMOTE_IO

#include "remote_io.h"
#include "cobs_protocol.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "REMOTE_IO";

#if REMOTE_IO_LOG_ENABLE
#define REMOTE_IO_LOGI(...) ESP_LOGI(__VA_ARGS__)
#define REMOTE_IO_LOGW(...) ESP_LOGW(__VA_ARGS__)
#else
#define REMOTE_IO_LOGI(...) do { } while (0)
#define REMOTE_IO_LOGW(...) do { } while (0)
#endif

/*******************************************************************************
 * Configuration
 ******************************************************************************/

#define UART_RX_BUF_SIZE        512     // UART RX ring buffer
#define UART_TX_BUF_SIZE        256     // UART TX buffer
#define UART_EVENT_QUEUE_SIZE   16      // UART event queue depth
#define CMD_TIMEOUT_MS          200     // Response timeout (ADS needs ~5-10ms)
#define DS18B20_TIMEOUT_MS      1000    // Longer timeout for temperature conversion
#define FRAME_TIMEOUT_MS        20      // Timeout to reset frame receiver state
#define LINK_LOCK_TIMEOUT_MS    1000    // Max wait for another task's command to finish

/*******************************************************************************
 * Private Data
 ******************************************************************************/

static int s_uart_num = -1;
static QueueHandle_t s_uart_queue = NULL;
static SemaphoreHandle_t s_link_mutex = NULL;   // one command on the wire at a time
static bool s_initialized = false;
static cobs_receiver_t s_receiver;

// Statistics
static uint32_t s_tx_frames = 0;
static uint32_t s_rx_frames = 0;
static uint32_t s_crc_errors = 0;
static uint32_t s_timeouts = 0;

/*******************************************************************************
 * Private Functions
 ******************************************************************************/

static uint32_t get_time_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static const char *cmd_name(uint8_t cmd)
{
    switch (cmd) {
        case MSG_PING:          return "PING";
        case MSG_VERSION:       return "VERSION";
        case MSG_GPIO_SET:      return "GPIO_SET";
        case MSG_GPIO_GET:      return "GPIO_GET";
        case MSG_GPIO_MODE:     return "GPIO_MODE";
        case MSG_PWM_SET:       return "PWM_SET";
        case MSG_PWM_INIT:      return "PWM_INIT";
        case MSG_I2C_WRITE:     return "I2C_WRITE";
        case MSG_I2C_READ:      return "I2C_READ";
        case MSG_ADS_READ:      return "ADS_READ";
        case MSG_DS18B20_START: return "DS18B20_START";
        case MSG_DS18B20_READ:  return "DS18B20_READ";
        case MSG_RESET:         return "RESET";
        default:                return "?";
    }
}

// Send one command and wait for its reply; caller holds s_link_mutex
static esp_err_t execute_command_internal(uint8_t cmd, const uint8_t *payload, size_t payload_len,
                                          uint8_t *resp_type, uint8_t *resp_data, size_t *resp_len,
                                          size_t resp_max_len, int timeout_ms)
{
    // Build and send frame
    uint8_t frame[COBS_MAX_FRAME_SIZE];
    size_t frame_len = cobs_build_frame(cmd, payload, payload_len, frame, sizeof(frame));
    
    if (frame_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Flush RX buffer and reset receiver
    uart_flush_input(s_uart_num);
    cobs_receiver_reset(&s_receiver);
    
    // Send frame
    uart_write_bytes(s_uart_num, frame, frame_len);
    s_tx_frames++;
    
    // Small delay to allow IO Node to process command before response
    vTaskDelay(pdMS_TO_TICKS(2));
    
    // Wait for response
    uint32_t start_ms = get_time_ms();
    uint32_t now_ms;
    esp_err_t result = ESP_ERR_TIMEOUT;
    
    ESP_LOGD(TAG, "Waiting for response (timeout=%dms)...", timeout_ms);
    
    while ((now_ms = get_time_ms()) - start_ms < (uint32_t)timeout_ms) {
        // Read available bytes
        uint8_t rx_buf[64];
        int len = uart_read_bytes(s_uart_num, rx_buf, sizeof(rx_buf), pdMS_TO_TICKS(2));
        
        if (len > 0) {
            ESP_LOGD(TAG, "RX %d bytes", len);
            // Feed to receiver state machine
            if (cobs_receiver_feed(&s_receiver, rx_buf, len, now_ms)) {
                // Got a complete frame
                uint8_t rx_frame[COBS_MAX_FRAME_SIZE];
                size_t rx_frame_len = cobs_receiver_get_frame(&s_receiver, rx_frame, sizeof(rx_frame));
                
                if (rx_frame_len > 0) {
                    uint8_t msg_type;
                    uint8_t data[COBS_MAX_PAYLOAD_SIZE];
                    size_t data_len;
                    
                    if (cobs_parse_frame(rx_frame, rx_frame_len, &msg_type, data, sizeof(data), &data_len)) {
                        s_rx_frames++;
                        ESP_LOGD(TAG, "Got response: type=0x%02X, len=%d", msg_type, data_len);
                        
                        // Return response
                        if (resp_type) *resp_type = msg_type;
                        if (resp_data && resp_len) {
                            size_t copy_len = (data_len < resp_max_len) ? data_len : resp_max_len;
                            memcpy(resp_data, data, copy_len);
                            *resp_len = data_len;
                        }
                        
                        // Check for error response
                        if (msg_type == MSG_ERROR) {
                            result = ESP_FAIL;
                        } else {
                            result = ESP_OK;
                        }
                        break;
                    } else {
                        s_crc_errors++;
                        ESP_LOGW(TAG, "CRC error on reply to %s", cmd_name(cmd));
                    }
                }
                cobs_receiver_reset(&s_receiver);
            }
        }
    }
    
    if (result == ESP_ERR_TIMEOUT) {
        s_timeouts++;
    }

    return result;
}

// One console line per timed-out command; a node reset or a lost reply shows up here by name.
// After 10 in a row (node absent), only every 50th is printed.
static void log_link_result(uint8_t cmd, const uint8_t *payload, size_t payload_len,
                            esp_err_t result, uint32_t wait_ms, uint32_t cmd_ms)
{
    static uint32_t consecutive = 0;

    if (result != ESP_ERR_TIMEOUT) {
        if (consecutive > 0) {
            ESP_LOGW(TAG, "Link recovered after %lu timed-out command(s)", (unsigned long)consecutive);
            consecutive = 0;
        }
        return;
    }
    consecutive++;
    if (consecutive <= 10 || consecutive % 50 == 0) {
        ESP_LOGW(TAG, "TIMEOUT %s [%d,%d] from %s: link wait %lu ms + command %lu ms (%lu in a row, %lu total)",
                 cmd_name(cmd), payload_len > 0 ? payload[0] : -1, payload_len > 1 ? payload[1] : -1,
                 pcTaskGetName(NULL), (unsigned long)wait_ms, (unsigned long)cmd_ms,
                 (unsigned long)consecutive, (unsigned long)s_timeouts);
    }
}

// Send a binary command frame and wait for response (called from any task).
// Runs in the caller's context under s_link_mutex, so no pointer outlives its call.
static esp_err_t send_command_binary(uint8_t cmd, const uint8_t *payload, size_t payload_len,
                                     uint8_t *resp_type, uint8_t *resp_data, size_t *resp_len,
                                     int timeout_ms)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t t0 = get_time_ms();
    if (xSemaphoreTake(s_link_mutex, pdMS_TO_TICKS(LINK_LOCK_TIMEOUT_MS)) != pdTRUE) {
        s_timeouts++;
        ESP_LOGW(TAG, "%s not sent from %s: link busy for %d ms", cmd_name(cmd), pcTaskGetName(NULL),
                 LINK_LOCK_TIMEOUT_MS);
        return ESP_ERR_TIMEOUT;
    }
    uint32_t t1 = get_time_ms();
    esp_err_t result = execute_command_internal(cmd, payload, payload_len, resp_type, resp_data,
                                                resp_len, resp_len ? *resp_len : 0, timeout_ms);
    log_link_result(cmd, payload, payload_len, result, t1 - t0, get_time_ms() - t1);
    xSemaphoreGive(s_link_mutex);
    return result;
}

// Note: send_simple_command removed - use send_command_binary directly

/*******************************************************************************
 * Public Functions - Initialization
 ******************************************************************************/

esp_err_t remote_io_init(int uart_num, int tx_pin, int rx_pin, int baud_rate)
{
    if (s_initialized) {
        return ESP_OK;
    }
    
    s_uart_num = uart_num;
    
    uart_config_t uart_config = {
        .baud_rate = baud_rate,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    
    // Install UART driver with event queue for efficient reception
    esp_err_t ret = uart_driver_install(uart_num, UART_RX_BUF_SIZE, UART_TX_BUF_SIZE,
                                        UART_EVENT_QUEUE_SIZE, &s_uart_queue, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = uart_param_config(uart_num, &uart_config);
    if (ret != ESP_OK) {
        uart_driver_delete(uart_num);
        return ret;
    }
    
    ret = uart_set_pin(uart_num, tx_pin, rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) {
        uart_driver_delete(uart_num);
        return ret;
    }
    
    // Priority-inheriting mutex: one command on the wire at a time
    s_link_mutex = xSemaphoreCreateMutex();
    if (s_link_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create link mutex");
        uart_driver_delete(uart_num);
        return ESP_ERR_NO_MEM;
    }

    // Initialize COBS receiver
    cobs_receiver_init(&s_receiver, FRAME_TIMEOUT_MS);

    s_initialized = true;
    
    ESP_LOGI(TAG, "============================================");
    ESP_LOGI(TAG, "Remote IO Binary Protocol Initialized");
    ESP_LOGI(TAG, "  UART Port:  %d", uart_num);
    ESP_LOGI(TAG, "  TX Pin:     GPIO%d", tx_pin);
    ESP_LOGI(TAG, "  RX Pin:     GPIO%d", rx_pin);
    ESP_LOGI(TAG, "  Baud Rate:  %d", baud_rate);
    ESP_LOGI(TAG, "  Protocol:   COBS + CRC16");
    ESP_LOGI(TAG, "============================================");
    
    return ESP_OK;
}

bool remote_io_ping(void)
{
    uint8_t resp_type;
    esp_err_t ret = send_command_binary(MSG_PING, NULL, 0, &resp_type, NULL, NULL, CMD_TIMEOUT_MS);
    if (ret != ESP_OK) {
        ESP_LOGD(TAG, "Ping failed: %s", esp_err_to_name(ret));
    }
    return (ret == ESP_OK && resp_type == MSG_PONG);
}

/*******************************************************************************
 * GPIO Functions
 ******************************************************************************/

esp_err_t remote_gpio_set_mode(int pin, remote_gpio_mode_t mode)
{
    uint8_t payload[2] = { (uint8_t)pin, (uint8_t)mode };
    return send_command_binary(MSG_GPIO_MODE, payload, 2, NULL, NULL, NULL, CMD_TIMEOUT_MS);
}

esp_err_t remote_gpio_set_level(int pin, int level)
{
    uint8_t payload[2] = { (uint8_t)pin, (uint8_t)(level ? 1 : 0) };
    return send_command_binary(MSG_GPIO_SET, payload, 2, NULL, NULL, NULL, CMD_TIMEOUT_MS);
}

esp_err_t remote_gpio_get_level(int pin, int *level)
{
    uint8_t payload[1] = { (uint8_t)pin };
    uint8_t resp_type;
    uint8_t resp_data[2];
    size_t resp_len = sizeof(resp_data);
    
    esp_err_t ret = send_command_binary(MSG_GPIO_GET, payload, 1, &resp_type, resp_data, &resp_len, CMD_TIMEOUT_MS);
    
    if (ret == ESP_OK && resp_type == MSG_GPIO_VALUE && resp_len >= 2) {
        if (level) *level = resp_data[1];  // [pin, level]
        return ESP_OK;
    }
    return ret == ESP_OK ? ESP_FAIL : ret;
}

/*******************************************************************************
 * PWM Functions
 ******************************************************************************/

esp_err_t remote_pwm_configure(int pin, int freq_hz, int resolution_bits)
{
    // Convert freq_hz to kHz for compact payload, ignore resolution (use 8-bit on IO Node)
    uint8_t freq_khz = (uint8_t)(freq_hz / 1000);
    if (freq_khz == 0) freq_khz = 1;
    uint8_t payload[2] = { (uint8_t)pin, freq_khz };
    return send_command_binary(MSG_PWM_INIT, payload, 2, NULL, NULL, NULL, CMD_TIMEOUT_MS);
}

esp_err_t remote_pwm_set_duty(int pin, int duty)
{
    uint8_t payload[2] = { (uint8_t)pin, (uint8_t)duty };
    ESP_LOGD(TAG, "PWM pin=%d duty=%d", pin, duty);
    return send_command_binary(MSG_PWM_SET, payload, 2, NULL, NULL, NULL, CMD_TIMEOUT_MS);
}

esp_err_t remote_pwm_stop(int pin)
{
    // Stop PWM by setting duty to 0
    return remote_pwm_set_duty(pin, 0);
}

/*******************************************************************************
 * I2C Functions
 ******************************************************************************/

esp_err_t remote_i2c_init(int bus_num, int sda_pin, int scl_pin, int freq_hz)
{
    // For binary protocol, I2C is pre-initialized on IO Node
    // This is a no-op but returns success for compatibility
    (void)bus_num; (void)sda_pin; (void)scl_pin; (void)freq_hz;
    return ESP_OK;
}

esp_err_t remote_i2c_deinit(int bus_num)
{
    // No-op for binary protocol
    (void)bus_num;
    return ESP_OK;
}

esp_err_t remote_i2c_write(int bus_num, uint8_t addr, const uint8_t *data, size_t len)
{
    if (len > COBS_MAX_PAYLOAD_SIZE - 3 || len < 1) {
        return ESP_ERR_INVALID_SIZE;
    }
    
    (void)bus_num; // Binary protocol assumes bus 0
    
    // First byte of data is register address for most I2C devices
    uint8_t payload[COBS_MAX_PAYLOAD_SIZE];
    payload[0] = addr;
    payload[1] = data[0];  // reg address
    payload[2] = (uint8_t)(len - 1);  // data length (excluding reg)
    if (len > 1) {
        memcpy(&payload[3], &data[1], len - 1);
    }
    
    return send_command_binary(MSG_I2C_WRITE, payload, 3 + len - 1, NULL, NULL, NULL, CMD_TIMEOUT_MS);
}

esp_err_t remote_i2c_read(int bus_num, uint8_t addr, uint8_t *data, size_t len)
{
    // For read without write, we can't specify register - use write_read instead
    // This is a simplified read that just reads len bytes
    (void)bus_num;
    
    uint8_t payload[3] = { addr, 0, (uint8_t)len };  // reg=0, read len bytes
    uint8_t resp_type;
    uint8_t resp_data[COBS_MAX_PAYLOAD_SIZE];
    size_t resp_len = sizeof(resp_data);
    
    esp_err_t ret = send_command_binary(MSG_I2C_READ, payload, 3, &resp_type, resp_data, &resp_len, CMD_TIMEOUT_MS);
    
    if (ret == ESP_OK && resp_type == MSG_I2C_DATA && resp_len >= 1) {
        size_t data_len = resp_data[0];
        if (data_len > len) data_len = len;
        memcpy(data, &resp_data[1], data_len);
        return ESP_OK;
    }
    return ret == ESP_OK ? ESP_FAIL : ret;
}

esp_err_t remote_i2c_write_read(int bus_num, uint8_t addr,
                                 const uint8_t *write_data, size_t write_len,
                                 uint8_t *read_data, size_t read_len)
{
    // Write register address, then read
    if (write_len < 1) {
        return ESP_ERR_INVALID_ARG;
    }
    
    (void)bus_num;
    
    uint8_t payload[3] = { addr, write_data[0], (uint8_t)read_len };
    uint8_t resp_type;
    uint8_t resp_data[COBS_MAX_PAYLOAD_SIZE];
    size_t resp_len = sizeof(resp_data);
    
    esp_err_t ret = send_command_binary(MSG_I2C_READ, payload, 3, &resp_type, resp_data, &resp_len, CMD_TIMEOUT_MS);
    
    if (ret == ESP_OK && resp_type == MSG_I2C_DATA && resp_len >= 1) {
        size_t data_len = resp_data[0];
        if (data_len > read_len) data_len = read_len;
        memcpy(read_data, &resp_data[1], data_len);
        return ESP_OK;
    }
    return ret == ESP_OK ? ESP_FAIL : ret;
}

esp_err_t remote_i2c_scan(int bus_num)
{
    // Not implemented in binary protocol
    (void)bus_num;
    return ESP_ERR_NOT_SUPPORTED;
}

/*******************************************************************************
 * ADS1115 Functions
 ******************************************************************************/

esp_err_t remote_ads1115_read_bulk(int bus_num, uint8_t i2c_addr, 
                                   const uint8_t *channels, int num_channels,
                                   int16_t *values)
{
    // Read each channel individually
    (void)bus_num;  // Bus number not used, IO Node has one I2C bus
    
    for (int i = 0; i < num_channels; i++) {
        // Payload: [i2c_addr, channel]
        uint8_t payload[2] = { i2c_addr, channels[i] };
        uint8_t resp_type;
        uint8_t resp_data[3];
        size_t resp_len = sizeof(resp_data);
        
        esp_err_t ret = send_command_binary(MSG_ADS_READ, payload, 2, &resp_type, resp_data, &resp_len, CMD_TIMEOUT_MS);
        
        if (ret != ESP_OK || resp_type != MSG_ADS_VALUE || resp_len < 3) {
            return ret == ESP_OK ? ESP_FAIL : ret;
        }
        
        values[i] = (int16_t)((resp_data[1] << 8) | resp_data[2]);
    }
    
    return ESP_OK;
}

esp_err_t remote_ads1115_read(int bus_num, uint8_t i2c_addr, uint8_t channel, int16_t *value)
{
    return remote_ads1115_read_bulk(bus_num, i2c_addr, &channel, 1, value);
}

/*******************************************************************************
 * DS18B20 / 1-Wire Temperature Sensor Functions (Async Pattern)
 ******************************************************************************/

esp_err_t remote_onewire_init(int pin)
{
    // 1-Wire is pre-initialized on IO Node
    (void)pin;
    return ESP_OK;
}

esp_err_t remote_onewire_scan(int pin, uint8_t rom_codes[][8], int max_devices, int *found_count)
{
    // Not implemented in compact binary protocol
    (void)pin; (void)rom_codes; (void)max_devices;
    if (found_count) *found_count = 0;
    return ESP_ERR_NOT_SUPPORTED;
}

/**
 * @brief Start DS18B20 temperature conversion (async - fire and forget)
 * Call this, then wait 750ms, then call remote_ds18b20_read_temp()
 */
esp_err_t remote_ds18b20_start_conversion(const uint8_t *rom_code)
{
    // If a ROM is provided, include it so the IO node can Match ROM; otherwise Skip ROM
    const uint8_t *payload = rom_code;
    size_t payload_len = (rom_code != NULL) ? 8 : 0;
    return send_command_binary(MSG_DS18B20_START, payload, payload_len, NULL, NULL, NULL, CMD_TIMEOUT_MS);
}

/**
 * @brief Read DS18B20 temperature result (call after conversion complete)
 * Must be called at least 750ms after remote_ds18b20_start_conversion()
 */
esp_err_t remote_ds18b20_read_temp(const uint8_t *rom_code, float *temperature_c)
{
    uint8_t resp_type;
    uint8_t resp_data[2];
    size_t resp_len = sizeof(resp_data);
    
    const uint8_t *payload = rom_code;
    size_t payload_len = (rom_code != NULL) ? 8 : 0;
    
    esp_err_t ret = send_command_binary(MSG_DS18B20_READ, payload, payload_len, &resp_type, resp_data, &resp_len, CMD_TIMEOUT_MS);
    
    if (ret == ESP_OK && resp_type == MSG_TEMP_VALUE && resp_len >= 2) {
        int16_t temp_x100 = (int16_t)((resp_data[0] << 8) | resp_data[1]);
        *temperature_c = temp_x100 / 100.0f;
        return ESP_OK;
    }
    return ret == ESP_OK ? ESP_FAIL : ret;
}

/**
 * @brief Blocking read - starts conversion, waits, then reads
 * For backwards compatibility - use async pattern for better performance
 */
esp_err_t remote_onewire_read_ds18b20(int pin, const uint8_t *rom_code, float *temperature_c)
{
    (void)pin; (void)rom_code;  // Ignored in binary protocol
    
    // Start conversion
    esp_err_t ret = remote_ds18b20_start_conversion(rom_code);
    if (ret != ESP_OK) return ret;
    
    // Wait for conversion (750ms max for 12-bit)
    vTaskDelay(pdMS_TO_TICKS(750));
    
    // Read temperature result
    return remote_ds18b20_read_temp(rom_code, temperature_c);
}

/*******************************************************************************
 * Utility Functions
 ******************************************************************************/

esp_err_t remote_io_wait_ready(int timeout_ms)
{
    // Ping the IO Node until it responds
    int elapsed = 0;
    int attempts = 0;
    while (elapsed < timeout_ms) {
        attempts++;
        if (remote_io_ping()) {
            ESP_LOGI(TAG, "IO Node responded after %d attempts (%d ms)", attempts, elapsed);
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
        elapsed += 100;
    }
    ESP_LOGW(TAG, "IO Node not responding after %d attempts", attempts);
    return ESP_ERR_TIMEOUT;
}

void remote_io_flush(void)
{
    if (!s_initialized) return;
    
    // Flush is handled internally by the IO task before each command
    // No direct UART access needed from other cores
    ESP_LOGD(TAG, "Flush requested (handled by IO task)");
}

esp_err_t remote_io_reset(void)
{
    return send_command_binary(MSG_RESET, NULL, 0, NULL, NULL, NULL, CMD_TIMEOUT_MS);
}

void remote_io_get_stats(uint32_t *tx_frames, uint32_t *rx_frames, 
                         uint32_t *crc_errors, uint32_t *timeouts)
{
    if (tx_frames) *tx_frames = s_tx_frames;
    if (rx_frames) *rx_frames = s_rx_frames;
    if (crc_errors) *crc_errors = s_crc_errors;
    if (timeouts) *timeouts = s_timeouts;
}

bool remote_io_is_initialized(void)
{
    return s_initialized;
}

esp_err_t remote_io_reset_node(void)
{
    return send_command_binary(MSG_RESET, NULL, 0, NULL, NULL, NULL, CMD_TIMEOUT_MS);
}

esp_err_t remote_io_stop_all_pwm(void)
{
    // Stop PWM by setting all potential channels to 0
    // This is a safety function after controller reset
    // Could be enhanced with a bulk STOP_ALL message
    return ESP_OK;  // No-op for now
}

esp_err_t remote_io_reinit(void)
{
    // Re-initialize communication state
    // Flush is now handled internally by IO task
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "Reinit requested");
    return ESP_OK;
}

#endif // USE_REMOTE_IO
