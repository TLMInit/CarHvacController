/**
 * @file remote_io.c
 * @brief Remote IO implementation - Binary COBS protocol over UART with IO Node
 * 
 * Uses COBS framing with CRC16 for reliable communication.
 * Each command runs in the calling task under one mutex: one command on the
 * wire at a time, and nothing outlives the call that owns it.
 */

#include "hvac_hw_config.h"


#include "remote_io.h"
#include "cobs_protocol.h"
#include "event_log.h"
#include "ota_image.h"
#include "link_payload.h"
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
#define CMD_TIMEOUT_MS          200     // Response timeout; above the node's slowest reply (io_i2c.c ADS_POLL_MAX_STEPS ~20 ms, io_onewire.c DS18B20_READ_ATTEMPTS)
#define FRAME_TIMEOUT_MS        20      // Timeout to reset frame receiver state; = node node_config.h LINK_FRAME_TIMEOUT_MS
#define LINK_LOCK_TIMEOUT_MS    1000    // Max wait for another task's command to finish
#define SEARCH_TIMEOUT_MS       500     // 1-Wire search of up to 4 ROMs; above node io_onewire.c ONEWIRE_SEARCH_BUDGET_US + one ROM
#define OTA_CMD_TIMEOUT_MS      1000    // OTA_DATA may wait for a 4 KB sector erase
#define OTA_SLOW_TIMEOUT_MS     5000    // OTA_BEGIN / OTA_END (erase, sha256 + boot slot)
#define OTA_REBOOT_WAIT_MS      30000   // new image boot, incl. a possible bootloader check; below node node_config.h OTA_CONFIRM_TIMEOUT_MS
#define TIMEOUT_EVENT_PERIOD_MS 10000   // EVT_TIMEOUT at most this often, with a count

// Link health over the last minute: 6 slots of 10 s, each an RTT histogram.
// shortcut: histogram instead of a sample ring; ~50 commands/s would need ~3000 samples
// (24 KB) for a full minute. p95 is the upper edge of its bucket (100 us steps to 2 ms,
// then 1 ms steps to 30 ms, beyond that the slot maximum).
#define RTT_SLOTS               6
#define RTT_SLOT_MS             10000
#define RTT_BUCKETS             49

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
static volatile uint32_t s_last_response_ms;
static volatile bool s_has_response;
static uint32_t s_mismatches = 0;   // valid replies of the wrong type (late replies), discarded

// IO node identity from its VERSION reply
static char s_node_version[COBS_MAX_PAYLOAD_SIZE] = "?";
static uint32_t s_node_uptime_s = 0;
static uint32_t s_node_resets = 0;
static bool s_node_seen = false;
static bool s_expect_restart = false;       // set by remote_io_reset_node: next uptime drop is ours

// HELLO handshake state
static uint8_t s_node_proto = 0;            // 0 until HELLO answered; 1 = node without HELLO
static uint8_t s_node_caps = 0;
static uint32_t s_hello_seq = 0;            // answered HELLOs since boot (remote_io_hello_seq)
static bool s_hello_pending = false;        // serviced by the next caller, outside the mutex
static bool s_pad_possible = true;          // node may pad replies: no protocol >= 2 HELLO_RESP since its start
static bool s_node_no_window = false;       // this node refused the link window in HELLO (2.3 node)

_Static_assert(REMOTE_IO_LINK_WINDOW_MS >= LINK_WINDOW_MIN_MS && REMOTE_IO_LINK_WINDOW_MS <= LINK_WINDOW_MAX_MS,
               "the node clamps the HELLO link window to LINK_WINDOW_MIN_MS..LINK_WINDOW_MAX_MS");

typedef struct {
    uint32_t id;                            // now_ms / RTT_SLOT_MS this slot belongs to
    uint16_t hist[RTT_BUCKETS];
    uint16_t timeouts;
    uint32_t max_us;
} rtt_slot_t;

static rtt_slot_t s_rtt[RTT_SLOTS];
static uint32_t s_rtt_last_us = 0;
static portMUX_TYPE s_rtt_lock = portMUX_INITIALIZER_UNLOCKED;

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
        case MSG_HELLO:         return "HELLO";
        case MSG_GPIO_SET:      return "GPIO_SET";
        case MSG_GPIO_GET:      return "GPIO_GET";
        case MSG_GPIO_MODE:     return "GPIO_MODE";
        case MSG_PWM_SET:       return "PWM_SET";
        case MSG_PWM_INIT:      return "PWM_INIT";
        case MSG_PWM_INIT_HZ:   return "PWM_INIT_HZ";
        case MSG_I2C_WRITE:     return "I2C_WRITE";
        case MSG_I2C_READ:      return "I2C_READ";
        case MSG_ADS_READ:      return "ADS_READ";
        case MSG_DS18B20_START: return "DS18B20_START";
        case MSG_DS18B20_READ:  return "DS18B20_READ";
        case MSG_ONEWIRE_SEARCH: return "ONEWIRE_SEARCH";
        case MSG_OTA_BEGIN:     return "OTA_BEGIN";
        case MSG_OTA_DATA:      return "OTA_DATA";
        case MSG_OTA_END:       return "OTA_END";
        case MSG_OTA_STATUS:    return "OTA_STATUS";
        case MSG_RESET:         return "RESET";
        default:                return "?";
    }
}

static uint8_t rtt_bucket(uint32_t us)
{
    if (us < 2000) return (uint8_t)(us / 100);
    if (us < 30000) return (uint8_t)(20 + (us - 2000) / 1000);
    return RTT_BUCKETS - 1;
}

static uint32_t rtt_bucket_upper_us(int b)
{
    return b < 20 ? (uint32_t)(b + 1) * 100 : 2000 + (uint32_t)(b - 19) * 1000;
}

// The slot for `now`, cleared when it last held an older 10 s window; caller holds s_rtt_lock
static rtt_slot_t *rtt_slot(uint32_t now_ms)
{
    uint32_t id = now_ms / RTT_SLOT_MS;
    rtt_slot_t *slot = &s_rtt[id % RTT_SLOTS];
    if (slot->id != id) {
        memset(slot, 0, sizeof(*slot));
        slot->id = id;
    }
    return slot;
}

// One reply (rtt_us) or one lost reply (timed_out); called under s_link_mutex
static void rtt_record(uint32_t rtt_us, bool timed_out)
{
    uint32_t now = get_time_ms();
    portENTER_CRITICAL(&s_rtt_lock);
    rtt_slot_t *slot = rtt_slot(now);
    if (timed_out) {
        if (slot->timeouts < UINT16_MAX) slot->timeouts++;
    } else {
        uint8_t b = rtt_bucket(rtt_us);
        if (slot->hist[b] < UINT16_MAX) slot->hist[b]++;
        if (rtt_us > slot->max_us) slot->max_us = rtt_us;
        s_rtt_last_us = rtt_us;
    }
    portEXIT_CRITICAL(&s_rtt_lock);
}

/*
 * Pad strip. A protocol 1 node, or a protocol 2 node before it has seen HELLO, appends one
 * 0x00 to a reply whose CRC low byte is 0x00. With this CRC (init 0xFFFF, no xorout) the
 * padded message M|crc_hi|00|00 is itself CRC-valid (CRC over M|crc_hi is always 0x0000), so
 * cobs_parse_frame returns the payload one byte too long. While padding is possible, a
 * reply whose CRC field was 0x0000 (crc16(type|data) == 0) and whose shorter form also
 * checks out loses its last byte. False match: a genuine CRC of 0x0000, 1 in 65536.
 * Worked bytes, TEMP_VALUE -1.27 C (FF 81), CRC = F0 00:
 *   padded   87 FF 81 F0 00 00 -> parsed data FF 81 F0 -> crc16(87 FF 81 F0) = 0000 and
 *            crc16(87 FF 81) = F000 = F0<<8 -> stripped to FF 81            (correct)
 *   unpadded 87 FF 81 F0 00    -> parsed data FF 81, crc16(87 FF 81) = F000 != 0 -> kept
 *   after a protocol 2 HELLO_RESP s_pad_possible is false: data FF 81 F0 would be kept
 *            (that is then a genuine 3-byte payload with CRC 0x0000).
 * Host-checked over all 65536 2-byte payloads (256 padded) during WP-C.
 */
static size_t strip_pad(uint8_t type, const uint8_t *data, size_t len)
{
    if (!s_pad_possible || len < 1 || len > COBS_MAX_PAYLOAD_SIZE) {
        return len;
    }
    uint8_t msg[1 + COBS_MAX_PAYLOAD_SIZE];
    msg[0] = type;
    memcpy(&msg[1], data, len);
    if (cobs_crc16(msg, 1 + len) == 0 && cobs_crc16(msg, len) == ((uint16_t)data[len - 1] << 8)) {
        return len - 1;
    }
    return len;
}

// Send one command and wait for its reply of type `expect` (or MSG_ERROR); caller holds s_link_mutex
static esp_err_t execute_command_internal(uint8_t cmd, uint8_t expect, const uint8_t *payload, size_t payload_len,
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
    int64_t sent_us = esp_timer_get_time();

    // Wait for response (no fixed grace delay: the read below returns as soon as bytes arrive)
    uint32_t start_ms = get_time_ms();
    uint32_t now_ms;
    esp_err_t result = ESP_ERR_TIMEOUT;
    
    ESP_LOGD(TAG, "Waiting for response (timeout=%dms)...", timeout_ms);
    
    while ((now_ms = get_time_ms()) - start_ms < (uint32_t)timeout_ms) {
        // Take what is buffered, or block for the first byte. Asking for 64 bytes made
        // uart_read_bytes wait out its whole 2-tick slice for every 5-9 byte reply.
        uint8_t rx_buf[64];
        size_t buffered = 0;
        uart_get_buffered_data_len(s_uart_num, &buffered);
        size_t want = (buffered == 0) ? 1 : (buffered < sizeof(rx_buf) ? buffered : sizeof(rx_buf));
        int len = uart_read_bytes(s_uart_num, rx_buf, want, pdMS_TO_TICKS(2));
        
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
                        data_len = strip_pad(msg_type, data, data_len);
                        // Link down -> up after first contact: the node may have lost HELLO
                        if (s_node_seen && !remote_io_is_connected()) {
                            ESP_LOGI(TAG, "Link up again: HELLO queued");
                            s_pad_possible = true;      // the node may have restarted unseen
                            __atomic_store_n(&s_hello_pending, true, __ATOMIC_RELEASE);
                        }
                        /* A CRC-valid response proves the remote link is alive, even
                         * when the remote endpoint returns a command-level error. */
                        s_last_response_ms = get_time_ms();
                        s_has_response = true;
                        ESP_LOGD(TAG, "Got response: type=0x%02X, len=%d", msg_type, data_len);

                        if (msg_type != expect && msg_type != MSG_ERROR) {
                            // A late reply to an earlier command, not ours: keep waiting
                            s_mismatches++;
                            ESP_LOGW(TAG, "Reply 0x%02X to %s discarded (expected 0x%02X, %lu so far)",
                                     msg_type, cmd_name(cmd), expect, (unsigned long)s_mismatches);
                            cobs_receiver_reset(&s_receiver);
                            continue;
                        }

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
        rtt_record(0, true);
    } else {
        rtt_record((uint32_t)(esp_timer_get_time() - sent_us), false);
    }

    return result;
}

// One console line per timed-out command; a node reset or a lost reply shows up here by name.
// After 10 in a row (node absent), only every 50th is printed.
static void log_link_result(uint8_t cmd, const uint8_t *payload, size_t payload_len,
                            esp_err_t result, uint32_t wait_ms, uint32_t cmd_ms)
{
    static uint32_t consecutive = 0;
    static uint32_t unlogged = 0;           // timeouts not yet in the event log
    static uint32_t last_event_ms = 0;
    static bool event_logged = false;

    if (result == ESP_ERR_TIMEOUT) {
        unlogged++;
    }
    // Flushed by the first command after the period, so a burst is reported once with its count
    if (unlogged > 0 && (!event_logged || get_time_ms() - last_event_ms >= TIMEOUT_EVENT_PERIOD_MS)) {
        event_log_add(EVT_TIMEOUT, "%lu link timeout(s), last %s", (unsigned long)unlogged, cmd_name(cmd));
        unlogged = 0;
        last_event_ms = get_time_ms();
        event_logged = true;
    }

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
static void link_service_hello(void);

static volatile bool s_last_cmd_timed_out;

static esp_err_t send_command_binary(uint8_t cmd, uint8_t expect, const uint8_t *payload, size_t payload_len,
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
    esp_err_t result = execute_command_internal(cmd, expect, payload, payload_len, resp_type, resp_data,
                                                resp_len, resp_len ? *resp_len : 0, timeout_ms);
    log_link_result(cmd, payload, payload_len, result, t1 - t0, get_time_ms() - t1);
    s_last_cmd_timed_out = (result == ESP_ERR_TIMEOUT);
    xSemaphoreGive(s_link_mutex);
    if (cmd != MSG_HELLO) {
        link_service_hello();
    }
    return result;
}

// Sends a queued HELLO from whichever task just finished a command (one extra round trip,
// outside the link mutex); the exchange makes sure only one task sends it
static void link_service_hello(void)
{
    if (__atomic_load_n(&s_hello_pending, __ATOMIC_ACQUIRE) &&
        __atomic_exchange_n(&s_hello_pending, false, __ATOMIC_ACQ_REL)) {
        remote_io_hello(NULL, NULL);
    }
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

static bool remote_io_ping(void)
{
    uint8_t resp_type;
    esp_err_t ret = send_command_binary(MSG_PING, MSG_PONG, NULL, 0, &resp_type, NULL, NULL, CMD_TIMEOUT_MS);
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
    return send_command_binary(MSG_GPIO_MODE, MSG_OK, payload, 2, NULL, NULL, NULL, CMD_TIMEOUT_MS);
}

esp_err_t remote_gpio_set_level(int pin, int level)
{
    uint8_t payload[2] = { (uint8_t)pin, (uint8_t)(level ? 1 : 0) };
    return send_command_binary(MSG_GPIO_SET, MSG_OK, payload, 2, NULL, NULL, NULL, CMD_TIMEOUT_MS);
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
    return send_command_binary(MSG_PWM_INIT, MSG_OK, payload, 2, NULL, NULL, NULL, CMD_TIMEOUT_MS);
}

esp_err_t remote_pwm_set_duty(int pin, int duty)
{
    uint8_t payload[2] = { (uint8_t)pin, (uint8_t)duty };
    ESP_LOGD(TAG, "PWM pin=%d duty=%d", pin, duty);
    return send_command_binary(MSG_PWM_SET, MSG_OK, payload, 2, NULL, NULL, NULL, CMD_TIMEOUT_MS);
}

esp_err_t remote_pwm_init_hz(int pin, uint32_t freq_hz, int bits)
{
    if (!(s_node_caps & NODE_CAP_PWM_HZ)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    uint8_t payload[6];
    size_t n = link_payload_pwm_init_hz(payload, (uint8_t)pin, freq_hz, (uint8_t)bits);
    return send_command_binary(MSG_PWM_INIT_HZ, MSG_OK, payload, n, NULL, NULL, NULL, CMD_TIMEOUT_MS);
}

esp_err_t remote_pwm_set_duty16(int pin, uint16_t counts)
{
    if (!(s_node_caps & NODE_CAP_PWM_HZ)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    uint8_t payload[3];
    size_t n = link_payload_pwm_set16(payload, (uint8_t)pin, counts);
    return send_command_binary(MSG_PWM_SET, MSG_OK, payload, n, NULL, NULL, NULL, CMD_TIMEOUT_MS);
}

/*******************************************************************************
 * I2C Functions
 ******************************************************************************/

esp_err_t remote_i2c_write(int bus_num, uint8_t addr, const uint8_t *data, size_t len)
{
    if (len > COBS_MAX_PAYLOAD_SIZE - 3 || len < 1) {
        return ESP_ERR_INVALID_SIZE;
    }
    
    (void)bus_num; // Binary protocol assumes bus 0
    
    // First byte of data is register address for most I2C devices
    uint8_t payload[COBS_MAX_PAYLOAD_SIZE];
    size_t n = link_payload_i2c_write(payload, sizeof(payload), addr, data, len);
    return send_command_binary(MSG_I2C_WRITE, MSG_OK, payload, n, NULL, NULL, NULL, CMD_TIMEOUT_MS);
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
    
    esp_err_t ret = send_command_binary(MSG_I2C_READ, MSG_I2C_DATA, payload, 3, &resp_type, resp_data, &resp_len, CMD_TIMEOUT_MS);
    
    if (ret == ESP_OK && resp_type == MSG_I2C_DATA && resp_len >= 1) {
        size_t data_len = resp_data[0];
        if (data_len > read_len) data_len = read_len;
        memcpy(read_data, &resp_data[1], data_len);
        return ESP_OK;
    }
    return ret == ESP_OK ? ESP_FAIL : ret;
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
        
        esp_err_t ret = send_command_binary(MSG_ADS_READ, MSG_ADS_VALUE, payload, 2, &resp_type, resp_data, &resp_len, CMD_TIMEOUT_MS);
        
        if (ret != ESP_OK || resp_type != MSG_ADS_VALUE || resp_len < 3) {
            return ret == ESP_OK ? ESP_FAIL : ret;
        }
        if (resp_data[0] != channels[i]) {
            // Another channel's value would be fed to the wrong valve
            ESP_LOGW(TAG, "ADS reply for channel %d, asked for %d: discarded", resp_data[0], channels[i]);
            return ESP_ERR_INVALID_RESPONSE;
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

/**
 * @brief Start DS18B20 temperature conversion (async - fire and forget)
 * Call this, then wait 750ms, then call remote_ds18b20_read_temp()
 */
esp_err_t remote_ds18b20_start_conversion(const uint8_t *rom_code)
{
    // If a ROM is provided, include it so the IO node can Match ROM; otherwise Skip ROM
    const uint8_t *payload = rom_code;
    size_t payload_len = (rom_code != NULL) ? 8 : 0;
    return send_command_binary(MSG_DS18B20_START, MSG_OK, payload, payload_len, NULL, NULL, NULL, CMD_TIMEOUT_MS);
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
    
    esp_err_t ret = send_command_binary(MSG_DS18B20_READ, MSG_TEMP_VALUE, payload, payload_len, &resp_type, resp_data, &resp_len, CMD_TIMEOUT_MS);
    
    if (ret == ESP_OK && resp_type == MSG_TEMP_VALUE && resp_len >= 2) {
        int16_t temp_x100 = (int16_t)((resp_data[0] << 8) | resp_data[1]);
        *temperature_c = temp_x100 / 100.0f;
        return ESP_OK;
    }
    return ret == ESP_OK ? ESP_FAIL : ret;
}

/*******************************************************************************
 * Utility Functions
 ******************************************************************************/

esp_err_t remote_io_poll_node(void)
{
    uint8_t resp_type = 0;
    uint8_t data[COBS_MAX_PAYLOAD_SIZE];
    size_t resp_len = sizeof(data) - 1;

    esp_err_t ret = send_command_binary(MSG_VERSION, MSG_VERSION_RESP, NULL, 0, &resp_type, data, &resp_len, CMD_TIMEOUT_MS);
    if (ret != ESP_OK || resp_type != MSG_VERSION_RESP) {
        return ret == ESP_OK ? ESP_FAIL : ret;
    }
    data[resp_len < sizeof(data) - 1 ? resp_len : sizeof(data) - 1] = '\0';

    // "ionode 2.0 (<ver>) up=<s> rst=<reason>"; older nodes send only the first part
    char *text = (char *)data;
    char *up = strstr(text, " up=");
    char *rst = strstr(text, " rst=");
    uint32_t uptime = up ? (uint32_t)strtoul(up + 4, NULL, 10) : 0;

    if (!s_node_seen) {
        // Each display boot resets the node (S-04), so up=0 rst=SW here is expected;
        // restarts are counted only after this first contact
        ESP_LOGI(TAG, "IO node (first contact since display boot): %s", text);
        __atomic_store_n(&s_hello_pending, true, __ATOMIC_RELEASE);
    } else if (up && uptime < s_node_uptime_s) {
        if (s_expect_restart) {
            ESP_LOGI(TAG, "IO node back after commanded reset: %s", text);
        } else {
            s_node_resets++;
            s_pad_possible = true;
            s_node_no_window = false;
            ESP_LOGW(TAG, "IO NODE RESTARTED: uptime %lu s -> %lu s, reset reason %s (%lu since boot)",
                     (unsigned long)s_node_uptime_s, (unsigned long)uptime, rst ? rst + 5 : "?",
                     (unsigned long)s_node_resets);
            event_log_add(EVT_NODE_RESET, "IO node restarted, rst=%.20s", rst ? rst + 5 : "?");
        }
        __atomic_store_n(&s_hello_pending, true, __ATOMIC_RELEASE);
    } else if (s_node_proto == 0) {
        __atomic_store_n(&s_hello_pending, true, __ATOMIC_RELEASE);   // HELLO unanswered: retry every poll
    }
    s_expect_restart = false;
    s_node_seen = true;
    s_node_uptime_s = uptime;
    if (up) {
        *up = '\0';
    }
    snprintf(s_node_version, sizeof(s_node_version), "%s", text);
    link_service_hello();
    return ESP_OK;
}

void remote_io_get_node_info(const char **version, uint32_t *uptime_s, uint32_t *resets)
{
    if (version) *version = s_node_version;
    if (uptime_s) *uptime_s = s_node_uptime_s;
    if (resets) *resets = s_node_resets;
}

esp_err_t remote_io_wait_ready(int timeout_ms)
{
    // Ping the IO Node until it responds. The bound is wall time: a failed ping
    // itself takes ~204 ms, so counting 100 ms per attempt made "3000" about 9 s.
    uint32_t start_ms = get_time_ms();
    int attempts = 0;
    do {
        attempts++;
        if (remote_io_ping()) {
            ESP_LOGI(TAG, "IO Node responded after %d attempts (%lu ms)", attempts,
                     (unsigned long)(get_time_ms() - start_ms));
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    } while (get_time_ms() - start_ms < (uint32_t)timeout_ms);
    ESP_LOGW(TAG, "IO Node not responding after %d attempts (%lu ms)", attempts,
             (unsigned long)(get_time_ms() - start_ms));
    return ESP_ERR_TIMEOUT;
}

void remote_io_get_stats(uint32_t *tx_frames, uint32_t *rx_frames,
                         uint32_t *crc_errors, uint32_t *timeouts)
{
    if (tx_frames) *tx_frames = s_tx_frames;
    if (rx_frames) *rx_frames = s_rx_frames;
    if (crc_errors) *crc_errors = s_crc_errors;
    if (timeouts) *timeouts = s_timeouts;
}

bool remote_io_last_command_timed_out(void)
{
    return s_last_cmd_timed_out;
}

bool remote_io_is_connected(void)
{
    if (!s_initialized || !s_has_response) return false;
    return (uint32_t)(get_time_ms() - s_last_response_ms) < REMOTE_IO_LINK_WINDOW_MS;
}

esp_err_t remote_io_reset_node(void)
{
    s_expect_restart = true;    // a commanded restart is not counted as a node restart
    s_node_proto = 0;           // the restarted node has not seen HELLO yet
    s_pad_possible = true;
    s_node_no_window = false;   // it may come back as another version (node OTA)
    return send_command_binary(MSG_RESET, MSG_OK, NULL, 0, NULL, NULL, NULL, CMD_TIMEOUT_MS);
}

/*******************************************************************************
 * v2.1: HELLO, 1-Wire search, node OTA, link health
 ******************************************************************************/

// Called from main (boot) or from whichever task services a queued HELLO
esp_err_t remote_io_hello(uint8_t *node_proto, uint8_t *node_caps)
{
    // version, then the link window u16 BE: the node's watchdog uses it (one number for both ends)
    const uint8_t req[3] = { PROTOCOL_VERSION, (uint8_t)(REMOTE_IO_LINK_WINDOW_MS >> 8),
                             (uint8_t)(REMOTE_IO_LINK_WINDOW_MS & 0xFFu) };
    uint8_t resp_type = 0;
    uint8_t data[COBS_MAX_PAYLOAD_SIZE];
    size_t resp_len = sizeof(data) - 1;

    esp_err_t ret = send_command_binary(MSG_HELLO, MSG_HELLO_RESP, req, s_node_no_window ? 1 : 3, &resp_type,
                                        data, &resp_len, CMD_TIMEOUT_MS);
    if (!s_node_no_window && ret == ESP_FAIL && resp_type == MSG_ERROR && resp_len >= 1 &&
        data[0] == ERR_INVALID_ARGS) {
        // 2.3 node: HELLO takes the version byte only; its watchdog stays its own LINK_WATCHDOG_MS
        s_node_no_window = true;
        ESP_LOGW(TAG, "HELLO: node takes no link window, its watchdog stays its own; HELLO repeated without it");
        resp_type = 0;
        resp_len = sizeof(data) - 1;
        ret = send_command_binary(MSG_HELLO, MSG_HELLO_RESP, req, 1, &resp_type, data, &resp_len, CMD_TIMEOUT_MS);
    }
    if (ret == ESP_OK && resp_type == MSG_HELLO_RESP && resp_len >= 2) {
        data[resp_len < sizeof(data) - 1 ? resp_len : sizeof(data) - 1] = '\0';
        s_node_proto = data[0];
        s_pad_possible = (s_node_proto < 2);    // a protocol 2 node stops padding after HELLO
        s_node_caps = data[1];
        ESP_LOGI(TAG, "HELLO: node protocol %u, caps 0x%02X (%s%s%s), %s", s_node_proto, s_node_caps,
                 (s_node_caps & NODE_CAP_SEARCH) ? "search " : "", (s_node_caps & NODE_CAP_OTA) ? "ota " : "",
                 (s_node_caps & NODE_CAP_PWM_HZ) ? "pwm_hz" : "",
                 resp_len > 2 ? (const char *)&data[2] : "?");
    } else if (ret == ESP_FAIL && resp_type == MSG_ERROR && resp_len >= 1 && data[0] == ERR_INVALID_CMD) {
        // Protocol 1 node: no HELLO and it keeps padding; cobs_parse_frame tolerates the pad
        s_node_proto = 1;
        s_pad_possible = true;
        s_node_caps = 0;
        ret = ESP_OK;
        ESP_LOGI(TAG, "HELLO: node protocol 1 (no HELLO), caps 0x00");
    } else {
        ret = (ret == ESP_OK || ret == ESP_FAIL) ? ESP_ERR_INVALID_RESPONSE : ret;
        ESP_LOGW(TAG, "HELLO failed: %s", esp_err_to_name(ret));
        return ret;
    }
    __atomic_add_fetch(&s_hello_seq, 1, __ATOMIC_RELEASE);
    if (node_proto) *node_proto = s_node_proto;
    if (node_caps) *node_caps = s_node_caps;
    return ret;
}

uint32_t remote_io_hello_seq(void)
{
    return __atomic_load_n(&s_hello_seq, __ATOMIC_ACQUIRE);
}

// Called from the sensors task (sensor_roles search)
esp_err_t remote_ds18b20_search(uint8_t roms[][8], int max, int *count)
{
    if (count) *count = 0;
    if (!(s_node_caps & NODE_CAP_SEARCH)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    uint8_t resp_type = 0;
    uint8_t data[1 + ONEWIRE_SEARCH_MAX_ROMS * 8];
    size_t resp_len = sizeof(data);
    esp_err_t ret = send_command_binary(MSG_ONEWIRE_SEARCH, MSG_ROM_LIST, NULL, 0, &resp_type, data, &resp_len,
                                        SEARCH_TIMEOUT_MS);
    if (ret != ESP_OK || resp_type != MSG_ROM_LIST || resp_len < 1) {
        return ret == ESP_OK ? ESP_ERR_INVALID_RESPONSE : ret;
    }
    int n = data[0];
    if (n > ONEWIRE_SEARCH_MAX_ROMS || resp_len < 1 + (size_t)n * 8) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (n > max) n = max;
    for (int i = 0; i < n; i++) {
        memcpy(roms[i], &data[1 + i * 8], 8);
    }
    if (count) *count = n;
    return ESP_OK;
}

// One OTA command; ESP_OK only for an OTA_ACK, with its state and byte count
static esp_err_t ota_command(uint8_t cmd, const uint8_t *payload, size_t len, int timeout_ms,
                             uint8_t *state, uint32_t *bytes)
{
    uint8_t resp_type = 0;
    uint8_t data[8];
    size_t resp_len = sizeof(data);
    esp_err_t ret = send_command_binary(cmd, MSG_OTA_ACK, payload, len, &resp_type, data, &resp_len, timeout_ms);
    if (ret == ESP_FAIL && resp_type == MSG_ERROR && resp_len >= 1) {
        ESP_LOGW(TAG, "%s: node error %u", cmd_name(cmd), data[0]);
        return data[0] == ERR_OTA_IMAGE ? ESP_ERR_INVALID_VERSION : ESP_FAIL;
    }
    if (ret != ESP_OK || resp_type != MSG_OTA_ACK || resp_len < 5) {
        return ret == ESP_OK ? ESP_ERR_INVALID_RESPONSE : ret;
    }
    *state = data[0];
    *bytes = ((uint32_t)data[1] << 24) | ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 8) | data[4];
    return ESP_OK;
}

static void put_u32_be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static esp_err_t ota_fail(const char *step, uint32_t offset, esp_err_t err)
{
    ESP_LOGE(TAG, "Node OTA failed at %s (offset %lu): %s", step, (unsigned long)offset, esp_err_to_name(err));
    event_log_add(EVT_OTA, "Node OTA failed: %s @%lu", step, (unsigned long)offset);
    return err;
}

// Called from the OTA worker task (system OFF). Holds the link mutex per command only,
// so the other tasks keep polling between chunks.
esp_err_t remote_io_ota_node(const uint8_t *image, size_t len, const uint8_t sha256[32],
                             remote_ota_progress_cb_t cb, void *ctx)
{
    if (image == NULL || sha256 == NULL || len == 0 || len > UINT32_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!(s_node_caps & NODE_CAP_OTA)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (len > NODE_OTA_SLOT_SIZE) {
        ESP_LOGE(TAG, "Node OTA refused: %lu bytes, the node's slot holds %lu", (unsigned long)len,
                 (unsigned long)NODE_OTA_SLOT_SIZE);
        return ESP_ERR_INVALID_SIZE;
    }
    char why[96];
    if (!ota_image_check(image, len, NODE_PROJECT_NAME, why, sizeof(why))) {
        ESP_LOGE(TAG, "Node OTA refused: %s", why);
        return ESP_ERR_INVALID_VERSION;
    }
    const uint32_t total = (uint32_t)len;
    uint8_t payload[4 + OTA_DATA_MAX];
    uint8_t state = 0;
    uint32_t bytes = 0;
    uint32_t t_start = get_time_ms();

    ESP_LOGI(TAG, "Node OTA: %lu bytes", (unsigned long)total);
    event_log_add(EVT_OTA, "Node OTA start, %lu bytes", (unsigned long)total);

    put_u32_be(payload, total);
    memcpy(&payload[4], sha256, 32);
    esp_err_t ret = ota_command(MSG_OTA_BEGIN, payload, 4 + 32, OTA_SLOW_TIMEOUT_MS, &state, &bytes);
    if (ret == ESP_OK && state != OTA_STATE_RECEIVING) ret = ESP_ERR_INVALID_STATE;
    if (ret != ESP_OK) return ota_fail("BEGIN", 0, ret);

    uint32_t offset = 0;
    int last_pct = -1;
    int retries = 0;
    while (offset < total) {
        uint32_t n = (total - offset < OTA_DATA_MAX) ? total - offset : OTA_DATA_MAX;
        put_u32_be(payload, offset);
        memcpy(&payload[4], &image[offset], n);
        ret = ota_command(MSG_OTA_DATA, payload, 4 + n, OTA_CMD_TIMEOUT_MS, &state, &bytes);
        if (ret == ESP_ERR_TIMEOUT || ret == ESP_ERR_INVALID_RESPONSE) {
            // The chunk may have landed with its ACK lost: continue where the node says it is
            if (++retries > 3) return ota_fail("DATA", offset, ret);
            if (ota_command(MSG_OTA_STATUS, NULL, 0, OTA_CMD_TIMEOUT_MS, &state, &bytes) == ESP_OK &&
                state == OTA_STATE_RECEIVING && bytes <= total) {
                offset = bytes;
            }
            continue;
        }
        if (ret == ESP_OK && (state != OTA_STATE_RECEIVING || bytes != offset + n)) ret = ESP_ERR_INVALID_STATE;
        if (ret != ESP_OK) return ota_fail("DATA", offset, ret);
        retries = 0;
        offset += n;
        int pct = (int)((uint64_t)offset * 100 / total);
        if (cb && pct != last_pct) {
            last_pct = pct;
            cb(offset, total, ctx);
        }
    }

    ret = ota_command(MSG_OTA_END, NULL, 0, OTA_SLOW_TIMEOUT_MS, &state, &bytes);
    if (ret == ESP_OK && state != OTA_STATE_READY) ret = ESP_ERR_INVALID_CRC;
    if (ret != ESP_OK) return ota_fail("END", offset, ret);
    ESP_LOGI(TAG, "Node OTA: %lu bytes sent and verified in %lu ms, restarting node", (unsigned long)total,
             (unsigned long)(get_time_ms() - t_start));

    ret = remote_io_reset_node();
    if (ret != ESP_OK) return ota_fail("RESET", offset, ret);
    vTaskDelay(pdMS_TO_TICKS(200));     // let the node go down before pinging it
    ret = remote_io_wait_ready(OTA_REBOOT_WAIT_MS);
    if (ret != ESP_OK) return ota_fail("REBOOT", offset, ret);
    ret = remote_io_poll_node();        // VERSION confirms the new image on the node
    if (ret != ESP_OK) return ota_fail("VERSION", offset, ret);
    if (s_node_proto == 0) {
        remote_io_hello(NULL, NULL);    // normally already done by poll_node
    }

    ESP_LOGI(TAG, "Node OTA done in %lu ms: %s", (unsigned long)(get_time_ms() - t_start), s_node_version);
    event_log_add(EVT_OTA, "Node OTA ok: %.30s", s_node_version);
    return ESP_OK;
}

// Called from the diagnostics page / web server tasks; never waits for the link
void remote_io_get_health(remote_io_health_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->connected = remote_io_is_connected();
    out->tx_frames = s_tx_frames;
    out->rx_frames = s_rx_frames;
    out->crc_errors = s_crc_errors;
    out->timeouts = s_timeouts;
    out->mismatches = s_mismatches;
    out->node_proto = s_node_proto;
    out->node_caps = s_node_caps;
    snprintf(out->node_version, sizeof(out->node_version), "%.47s", s_node_version);
    out->node_uptime_s = s_node_uptime_s;
    out->node_restarts = s_node_resets;

    uint32_t hist[RTT_BUCKETS] = {0};
    uint32_t count = 0, max_us = 0;
    uint32_t now_id = get_time_ms() / RTT_SLOT_MS;
    portENTER_CRITICAL(&s_rtt_lock);
    out->rtt_last_us = s_rtt_last_us;
    for (int i = 0; i < RTT_SLOTS; i++) {
        const rtt_slot_t *slot = &s_rtt[i];
        if (slot->id + RTT_SLOTS <= now_id) continue;   // older than the last minute
        out->timeouts_last_min += slot->timeouts;
        for (int b = 0; b < RTT_BUCKETS; b++) {
            hist[b] += slot->hist[b];
            count += slot->hist[b];
        }
        if (slot->max_us > max_us) max_us = slot->max_us;
    }
    portEXIT_CRITICAL(&s_rtt_lock);

    if (count > 0) {
        uint32_t rank = (count * 95 + 99) / 100;    // ceil(0.95 * count)
        uint32_t seen = 0;
        for (int b = 0; b < RTT_BUCKETS; b++) {
            seen += hist[b];
            if (seen >= rank) {
                uint32_t upper = (b == RTT_BUCKETS - 1) ? max_us : rtt_bucket_upper_us(b);
                out->rtt_p95_us = upper < max_us ? upper : max_us;
                break;
            }
        }
    }
}

