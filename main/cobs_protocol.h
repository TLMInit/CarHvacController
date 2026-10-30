/**
 * @file cobs_protocol.h
 * @brief COBS (Consistent Overhead Byte Stuffing) protocol with CRC16
 * 
 * This provides reliable framing and error detection for UART communication
 * between the HVAC Controller and IO Node.
 * 
 * Frame format:
 *   [COBS-encoded payload][0x00 delimiter]
 * 
 * Payload format (before COBS encoding):
 *   [1 byte: message type][N bytes: data][2 bytes: CRC16-CCITT]
 */

#ifndef COBS_PROTOCOL_H
#define COBS_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*******************************************************************************
 * Protocol Constants
 ******************************************************************************/

#define COBS_FRAME_DELIMITER    0x00
#define COBS_MAX_PAYLOAD_SIZE   64      // Maximum payload size before COBS encoding
#define COBS_MAX_FRAME_SIZE     (COBS_MAX_PAYLOAD_SIZE + 2 + 2)  // payload + COBS overhead + CRC

// Protocol version for compatibility checking.
// 2: HELLO handshake, ONEWIRE_SEARCH, node OTA. A display sends HELLO with this
// value at every connect; a node that has seen HELLO stops padding replies whose
// CRC ends in 0x00 (protocol 1 displays strip one trailing zero when decoding).
#define PROTOCOL_VERSION        2

// HELLO_RESP capability bits
#define NODE_CAP_SEARCH         0x01    // ONEWIRE_SEARCH supported
#define NODE_CAP_OTA            0x02    // OTA_BEGIN/DATA/END/STATUS supported

#define ONEWIRE_SEARCH_MAX_ROMS 4       // ROM_LIST carries at most this many 8-byte ROM codes
#define OTA_DATA_MAX            56      // OTA_DATA payload: offset(4) + up to 56 bytes = 60 <= 63

/*******************************************************************************
 * Message Types (Controller -> IO Node: 0x01-0x7F, IO Node -> Controller: 0x80-0xFF)
 ******************************************************************************/

// Commands from Controller to IO Node
typedef enum {
    MSG_PING            = 0x01,     // No data, expects PONG response
    MSG_VERSION         = 0x02,     // No data, expects version string
    MSG_HELLO           = 0x03,     // display PROTOCOL_VERSION(1) [, link window ms u16 BE], expects HELLO_RESP
    MSG_GPIO_SET        = 0x10,     // pin(1), level(1)
    MSG_GPIO_GET        = 0x11,     // pin(1), expects GPIO_VALUE response
    MSG_GPIO_MODE       = 0x12,     // pin(1), mode(1)
    MSG_PWM_SET         = 0x20,     // pin(1), duty(1)
    MSG_PWM_INIT        = 0x21,     // pin(1), freq_khz(1)
    MSG_I2C_WRITE       = 0x30,     // addr(1), reg(1), len(1), data(N)
    MSG_I2C_READ        = 0x31,     // addr(1), reg(1), len(1), expects I2C_DATA
    MSG_ADS_READ        = 0x40,     // channel(1), expects ADS_VALUE response
    MSG_DS18B20_START   = 0x50,     // Start temperature conversion
    MSG_DS18B20_READ    = 0x51,     // Read temperature, expects TEMP_VALUE
    MSG_ONEWIRE_SEARCH  = 0x52,     // No data, expects ROM_LIST
    MSG_OTA_BEGIN       = 0x60,     // size u32 BE, sha256[32]; expects OTA_ACK
    MSG_OTA_DATA        = 0x61,     // offset u32 BE, data[1..OTA_DATA_MAX]; expects OTA_ACK
    MSG_OTA_END         = 0x62,     // No data: verify sha256, set boot slot; expects OTA_ACK, then RESET reboots
    MSG_OTA_STATUS      = 0x63,     // No data, expects OTA_ACK
    MSG_RESET           = 0xF0,     // Reset IO Node
} cobs_msg_cmd_t;

// Responses from IO Node to Controller
typedef enum {
    MSG_OK              = 0x80,     // Success, optional data follows
    MSG_ERROR           = 0x81,     // Error code(1), optional message
    MSG_PONG            = 0x82,     // Response to PING
    MSG_VERSION_RESP    = 0x83,     // Version string (null-terminated)
    MSG_GPIO_VALUE      = 0x84,     // pin(1), level(1)
    MSG_I2C_DATA        = 0x85,     // len(1), data(N)
    MSG_ADS_VALUE       = 0x86,     // channel(1), value_hi(1), value_lo(1)
    MSG_TEMP_VALUE      = 0x87,     // temp_x100_hi(1), temp_x100_lo(1) (signed, °C*100)
    MSG_HELLO_RESP      = 0x88,     // node PROTOCOL_VERSION(1), capability bits(1), version string (NUL-terminated)
    MSG_ROM_LIST        = 0x89,     // count(1), count x rom(8), count <= ONEWIRE_SEARCH_MAX_ROMS
    MSG_OTA_ACK         = 0x8A,     // state(1) ota_state_t, bytes_received u32 BE
} cobs_msg_resp_t;

// Node OTA state carried in OTA_ACK
typedef enum {
    OTA_STATE_IDLE      = 0,        // no transfer
    OTA_STATE_RECEIVING = 1,        // OTA_BEGIN accepted, data being written to the inactive slot
    OTA_STATE_READY     = 2,        // OTA_END: hash matched, boot slot set; RESET to run it
    OTA_STATE_ERROR     = 3,        // write, size or hash failure; OTA_BEGIN starts over
    OTA_STATE_PENDING   = 4,        // running a new image not yet confirmed (rollback armed)
} ota_state_t;

// GPIO modes (matches io_gpio.h, prefixed to avoid ESP-IDF conflict)
typedef enum {
    COBS_GPIO_INPUT         = 0,
    COBS_GPIO_OUTPUT        = 1,
    COBS_GPIO_INPUT_PULLUP  = 2,
    COBS_GPIO_INPUT_PULLDOWN= 3,
} cobs_gpio_mode_t;

// Error codes
typedef enum {
    ERR_NONE            = 0,
    ERR_INVALID_CMD     = 1,
    ERR_INVALID_ARGS    = 2,
    ERR_INVALID_PIN     = 3,
    ERR_I2C_FAIL        = 4,
    ERR_TIMEOUT         = 5,
    ERR_CRC_MISMATCH    = 6,
    ERR_FRAME_ERROR     = 7,
    ERR_OVERFLOW        = 8,
    ERR_OTA_STATE       = 9,        // OTA command out of order
    ERR_OTA_WRITE       = 10,       // flash write failed / offset not contiguous
    ERR_OTA_VERIFY      = 11,       // size or sha256 mismatch at OTA_END
    ERR_OTA_IMAGE       = 12,       // image header: not an ESP32-S3 app of this project
} cobs_error_t;

/*******************************************************************************
 * COBS Encoding/Decoding Functions
 ******************************************************************************/

/**
 * @brief COBS encode data
 * @param input Input data buffer
 * @param input_len Length of input data
 * @param output Output buffer (must be at least input_len + ceil(input_len/254) + 1)
 * @return Encoded length, or 0 on error
 */
size_t cobs_encode(const uint8_t *input, size_t input_len, uint8_t *output);

/**
 * @brief COBS decode data
 * @param input COBS encoded data (without delimiter)
 * @param input_len Length of encoded data
 * @param output Output buffer (at least input_len bytes)
 * @return Decoded length, or 0 on error
 */
size_t cobs_decode(const uint8_t *input, size_t input_len, uint8_t *output);

/*******************************************************************************
 * CRC16-CCITT Functions
 ******************************************************************************/

/**
 * @brief Calculate CRC16-CCITT
 * @param data Data buffer
 * @param len Data length
 * @return CRC16 value
 */
uint16_t cobs_crc16(const uint8_t *data, size_t len);

/**
 * @brief Verify CRC16 of a message (CRC is last 2 bytes)
 * @param data Data buffer including CRC
 * @param len Total length including CRC
 * @return true if CRC matches
 */
bool cobs_verify_crc(const uint8_t *data, size_t len);

/**
 * @brief Append CRC16 to message
 * @param data Data buffer (must have 2 bytes extra space)
 * @param len Data length (without CRC)
 * @return New length (len + 2)
 */
size_t cobs_append_crc(uint8_t *data, size_t len);

/*******************************************************************************
 * Frame Building Helpers
 ******************************************************************************/

/**
 * @brief Build a complete frame ready to send
 * @param msg_type Message type
 * @param payload Optional payload data (can be NULL)
 * @param payload_len Payload length
 * @param output Output buffer for complete frame (with COBS encoding and delimiter)
 * @param output_size Size of output buffer
 * @return Frame length including delimiter, or 0 on error
 */
size_t cobs_build_frame(uint8_t msg_type, const uint8_t *payload, size_t payload_len,
                        uint8_t *output, size_t output_size);

/**
 * @brief Parse a received frame
 * @param frame Received frame data (including delimiter)
 * @param frame_len Frame length
 * @param msg_type Output: message type
 * @param payload Output: payload buffer
 * @param payload_size Size of payload buffer
 * @param payload_len Output: actual payload length
 * @return true if frame is valid, false on CRC or framing error
 */
bool cobs_parse_frame(const uint8_t *frame, size_t frame_len,
                      uint8_t *msg_type, uint8_t *payload, size_t payload_size,
                      size_t *payload_len);

/*******************************************************************************
 * Frame Receiver State Machine
 ******************************************************************************/

typedef struct {
    uint8_t frame_buf[COBS_MAX_FRAME_SIZE];
    size_t pos;
    uint32_t last_rx_time;          // For timeout detection
    uint32_t timeout_ms;            // Timeout to reset state machine
    bool frame_ready;               // A complete frame is in buffer
    size_t frame_len;               // Length of complete frame
} cobs_receiver_t;

/**
 * @brief Initialize receiver state machine
 * @param rx Receiver state
 * @param timeout_ms Frame timeout in milliseconds
 */
void cobs_receiver_init(cobs_receiver_t *rx, uint32_t timeout_ms);

/**
 * @brief Feed received bytes to state machine
 * @param rx Receiver state
 * @param data Received bytes
 * @param len Number of bytes
 * @param current_time_ms Current time for timeout detection
 * @return true if a complete frame is ready
 */
bool cobs_receiver_feed(cobs_receiver_t *rx, const uint8_t *data, size_t len,
                        uint32_t current_time_ms);

/**
 * @brief Get the received frame (call after feed returns true)
 * @param rx Receiver state
 * @param frame Output buffer
 * @param max_len Maximum output length
 * @return Frame length, or 0 if no frame ready
 */
size_t cobs_receiver_get_frame(cobs_receiver_t *rx, uint8_t *frame, size_t max_len);

/**
 * @brief Check for frame timeout and reset if needed
 * @param rx Receiver state
 * @param now_ms Current time in milliseconds
 */
void cobs_receiver_check_timeout(cobs_receiver_t *rx, uint32_t now_ms);

/**
 * @brief Reset receiver state (call after processing frame or on timeout)
 * @param rx Receiver state
 */
void cobs_receiver_reset(cobs_receiver_t *rx);

#ifdef __cplusplus
}
#endif

#endif // COBS_PROTOCOL_H
