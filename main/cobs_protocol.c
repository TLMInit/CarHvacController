/**
 * @file cobs_protocol.c
 * @brief COBS (Consistent Overhead Byte Stuffing) protocol implementation
 */

/*
 * Copied from CarHvacController/main/cobs_protocol.c with two deliberate changes,
 * both marked NODE CHANGE below:
 *
 * 1. cobs_decode() no longer strips a trailing 0x00 from the decoded message.
 *    The display's copy does, which corrupts every frame whose CRC low byte is
 *    0x00 (1 in 256). The node must parse those commands correctly.
 *
 * 2. cobs_build_frame() appends one extra 0x00 when the CRC low byte is 0x00
 *    (LINK_PAD_TRAILING_ZERO in node_config.h). The display's decoder then
 *    strips the pad instead of the CRC and sees a correct frame. Set the flag
 *    to 0 once the display's cobs_decode() is fixed the same way as here.
 *
 * Everything else (CRC16-CCITT, COBS encoding, receiver state machine) is
 * byte-identical to the display, which is what makes the two ends compatible.
 */

#include "cobs_protocol.h"
#ifndef LINK_PAD_TRAILING_ZERO
#include "node_config.h"     /* host-side tests pass -DLINK_PAD_TRAILING_ZERO instead */
#endif
#include <string.h>

/*******************************************************************************
 * CRC16-CCITT Implementation (polynomial 0x1021, init 0xFFFF)
 ******************************************************************************/

// CRC16-CCITT lookup table for fast computation
static const uint16_t crc16_table[256] = {
    0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50A5, 0x60C6, 0x70E7,
    0x8108, 0x9129, 0xA14A, 0xB16B, 0xC18C, 0xD1AD, 0xE1CE, 0xF1EF,
    0x1231, 0x0210, 0x3273, 0x2252, 0x52B5, 0x4294, 0x72F7, 0x62D6,
    0x9339, 0x8318, 0xB37B, 0xA35A, 0xD3BD, 0xC39C, 0xF3FF, 0xE3DE,
    0x2462, 0x3443, 0x0420, 0x1401, 0x64E6, 0x74C7, 0x44A4, 0x5485,
    0xA56A, 0xB54B, 0x8528, 0x9509, 0xE5EE, 0xF5CF, 0xC5AC, 0xD58D,
    0x3653, 0x2672, 0x1611, 0x0630, 0x76D7, 0x66F6, 0x5695, 0x46B4,
    0xB75B, 0xA77A, 0x9719, 0x8738, 0xF7DF, 0xE7FE, 0xD79D, 0xC7BC,
    0x48C4, 0x58E5, 0x6886, 0x78A7, 0x0840, 0x1861, 0x2802, 0x3823,
    0xC9CC, 0xD9ED, 0xE98E, 0xF9AF, 0x8948, 0x9969, 0xA90A, 0xB92B,
    0x5AF5, 0x4AD4, 0x7AB7, 0x6A96, 0x1A71, 0x0A50, 0x3A33, 0x2A12,
    0xDBFD, 0xCBDC, 0xFBBF, 0xEB9E, 0x9B79, 0x8B58, 0xBB3B, 0xAB1A,
    0x6CA6, 0x7C87, 0x4CE4, 0x5CC5, 0x2C22, 0x3C03, 0x0C60, 0x1C41,
    0xEDAE, 0xFD8F, 0xCDEC, 0xDDCD, 0xAD2A, 0xBD0B, 0x8D68, 0x9D49,
    0x7E97, 0x6EB6, 0x5ED5, 0x4EF4, 0x3E13, 0x2E32, 0x1E51, 0x0E70,
    0xFF9F, 0xEFBE, 0xDFDD, 0xCFFC, 0xBF1B, 0xAF3A, 0x9F59, 0x8F78,
    0x9188, 0x81A9, 0xB1CA, 0xA1EB, 0xD10C, 0xC12D, 0xF14E, 0xE16F,
    0x1080, 0x00A1, 0x30C2, 0x20E3, 0x5004, 0x4025, 0x7046, 0x6067,
    0x83B9, 0x9398, 0xA3FB, 0xB3DA, 0xC33D, 0xD31C, 0xE37F, 0xF35E,
    0x02B1, 0x1290, 0x22F3, 0x32D2, 0x4235, 0x5214, 0x6277, 0x7256,
    0xB5EA, 0xA5CB, 0x95A8, 0x8589, 0xF56E, 0xE54F, 0xD52C, 0xC50D,
    0x34E2, 0x24C3, 0x14A0, 0x0481, 0x7466, 0x6447, 0x5424, 0x4405,
    0xA7DB, 0xB7FA, 0x8799, 0x97B8, 0xE75F, 0xF77E, 0xC71D, 0xD73C,
    0x26D3, 0x36F2, 0x0691, 0x16B0, 0x6657, 0x7676, 0x4615, 0x5634,
    0xD94C, 0xC96D, 0xF90E, 0xE92F, 0x99C8, 0x89E9, 0xB98A, 0xA9AB,
    0x5844, 0x4865, 0x7806, 0x6827, 0x18C0, 0x08E1, 0x3882, 0x28A3,
    0xCB7D, 0xDB5C, 0xEB3F, 0xFB1E, 0x8BF9, 0x9BD8, 0xABBB, 0xBB9A,
    0x4A75, 0x5A54, 0x6A37, 0x7A16, 0x0AF1, 0x1AD0, 0x2AB3, 0x3A92,
    0xFD2E, 0xED0F, 0xDD6C, 0xCD4D, 0xBDAA, 0xAD8B, 0x9DE8, 0x8DC9,
    0x7C26, 0x6C07, 0x5C64, 0x4C45, 0x3CA2, 0x2C83, 0x1CE0, 0x0CC1,
    0xEF1F, 0xFF3E, 0xCF5D, 0xDF7C, 0xAF9B, 0xBFBA, 0x8FD9, 0x9FF8,
    0x6E17, 0x7E36, 0x4E55, 0x5E74, 0x2E93, 0x3EB2, 0x0ED1, 0x1EF0
};

uint16_t cobs_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    while (len--) {
        crc = (crc << 8) ^ crc16_table[((crc >> 8) ^ *data++) & 0xFF];
    }
    return crc;
}

bool cobs_verify_crc(const uint8_t *data, size_t len)
{
    if (len < 3) return false;  // At least 1 byte msg + 2 bytes CRC
    
    uint16_t received_crc = (data[len - 2] << 8) | data[len - 1];
    uint16_t calculated_crc = cobs_crc16(data, len - 2);
    return received_crc == calculated_crc;
}

size_t cobs_append_crc(uint8_t *data, size_t len)
{
    uint16_t crc = cobs_crc16(data, len);
    data[len] = (crc >> 8) & 0xFF;      // CRC high byte
    data[len + 1] = crc & 0xFF;         // CRC low byte
    return len + 2;
}

/*******************************************************************************
 * COBS Encoding/Decoding
 ******************************************************************************/

size_t cobs_encode(const uint8_t *input, size_t input_len, uint8_t *output)
{
    if (input == NULL || output == NULL || input_len == 0) {
        return 0;
    }
    
    size_t read_idx = 0;
    size_t write_idx = 1;       // Leave space for first code byte
    size_t code_idx = 0;        // Position of current code byte
    uint8_t code = 1;           // Current code (distance to next zero or end)
    
    while (read_idx < input_len) {
        if (input[read_idx] == 0) {
            // Found a zero - write the code and start new block
            output[code_idx] = code;
            code_idx = write_idx++;
            code = 1;
            read_idx++;
        } else {
            // Non-zero byte - copy it
            output[write_idx++] = input[read_idx++];
            code++;
            
            // If we've hit max code value (254), need to start new block
            if (code == 0xFF) {
                output[code_idx] = code;
                code_idx = write_idx++;
                code = 1;
            }
        }
    }
    
    // Write final code byte
    output[code_idx] = code;
    
    return write_idx;
}

size_t cobs_decode(const uint8_t *input, size_t input_len, uint8_t *output)
{
    if (input == NULL || output == NULL || input_len == 0) {
        return 0;
    }
    
    size_t read_idx = 0;
    size_t write_idx = 0;
    
    while (read_idx < input_len) {
        uint8_t code = input[read_idx++];
        
        if (code == 0) {
            // Unexpected zero in encoded data - error
            return 0;
        }
        
        // Copy code-1 non-zero bytes
        for (uint8_t i = 1; i < code && read_idx < input_len; i++) {
            output[write_idx++] = input[read_idx++];
        }
        
        // If code < 255 and not at end, insert a zero
        if (code < 0xFF && read_idx < input_len) {
            output[write_idx++] = 0;
        }
    }
    
    /* NODE CHANGE: the display's version strips a trailing 0x00 here, which
     * drops a genuine CRC byte of 0x00. Standard COBS needs no such step. */
    return write_idx;
}

/*******************************************************************************
 * Frame Building and Parsing
 ******************************************************************************/

size_t cobs_build_frame(uint8_t msg_type, const uint8_t *payload, size_t payload_len,
                        uint8_t *output, size_t output_size)
{
    // Build raw message: [msg_type][payload][crc16]
    uint8_t raw[COBS_MAX_PAYLOAD_SIZE + 3];  // +1 msg_type, +2 crc
    size_t raw_len = 0;
    
    // Check size
    if (payload_len > COBS_MAX_PAYLOAD_SIZE - 1) {  // -1 for msg_type
        return 0;
    }
    
    // Message type
    raw[raw_len++] = msg_type;
    
    // Payload
    if (payload != NULL && payload_len > 0) {
        memcpy(&raw[raw_len], payload, payload_len);
        raw_len += payload_len;
    }
    
    // Append CRC
    raw_len = cobs_append_crc(raw, raw_len);

#if LINK_PAD_TRAILING_ZERO
    /* NODE CHANGE: compatibility pad for the display's trailing-zero strip. */
    if (raw[raw_len - 1] == 0x00 && raw_len < sizeof(raw)) {
        raw[raw_len++] = 0x00;
    }
#endif
    
    // COBS encode
    size_t encoded_len = cobs_encode(raw, raw_len, output);
    if (encoded_len == 0 || encoded_len >= output_size) {
        return 0;
    }
    
    // Add frame delimiter
    output[encoded_len++] = COBS_FRAME_DELIMITER;
    
    return encoded_len;
}

bool cobs_parse_frame(const uint8_t *frame, size_t frame_len,
                      uint8_t *msg_type, uint8_t *payload, size_t payload_size,
                      size_t *payload_len)
{
    if (frame == NULL || frame_len < 4) {  // Min: 1 COBS + 1 msg + 2 CRC
        return false;
    }
    
    // Remove trailing delimiter if present
    size_t encoded_len = frame_len;
    if (frame[frame_len - 1] == COBS_FRAME_DELIMITER) {
        encoded_len--;
    }
    
    if (encoded_len == 0) {
        return false;
    }
    
    // COBS decode
    uint8_t decoded[COBS_MAX_PAYLOAD_SIZE + 3];
    size_t decoded_len = cobs_decode(frame, encoded_len, decoded);
    
    if (decoded_len < 3) {  // Min: 1 msg_type + 2 CRC
        return false;
    }
    
    // Verify CRC
    if (!cobs_verify_crc(decoded, decoded_len)) {
        return false;
    }
    
    // Extract message type
    *msg_type = decoded[0];
    
    // Extract payload (everything except msg_type and CRC)
    *payload_len = decoded_len - 3;  // -1 msg_type, -2 CRC
    if (*payload_len > 0) {
        if (*payload_len > payload_size) {
            return false;  // Payload too large for buffer
        }
        memcpy(payload, &decoded[1], *payload_len);
    }
    
    return true;
}

/*******************************************************************************
 * Frame Receiver State Machine
 ******************************************************************************/

void cobs_receiver_init(cobs_receiver_t *rx, uint32_t timeout_ms)
{
    memset(rx, 0, sizeof(cobs_receiver_t));
    rx->timeout_ms = timeout_ms;
}

bool cobs_receiver_feed(cobs_receiver_t *rx, const uint8_t *data, size_t len,
                        uint32_t current_time_ms)
{
    // Check for timeout - reset if too long since last byte
    if (rx->pos > 0 && 
        (current_time_ms - rx->last_rx_time) > rx->timeout_ms) {
        cobs_receiver_reset(rx);
    }
    
    for (size_t i = 0; i < len; i++) {
        uint8_t byte = data[i];
        rx->last_rx_time = current_time_ms;
        
        if (byte == COBS_FRAME_DELIMITER) {
            // End of frame
            if (rx->pos > 0) {
                rx->frame_ready = true;
                rx->frame_len = rx->pos;
                return true;
            }
            // Empty frame or consecutive delimiters - ignore
        } else {
            // Add byte to buffer
            if (rx->pos < sizeof(rx->frame_buf)) {
                rx->frame_buf[rx->pos++] = byte;
            } else {
                // Buffer overflow - reset
                cobs_receiver_reset(rx);
            }
        }
    }
    
    return rx->frame_ready;
}

size_t cobs_receiver_get_frame(cobs_receiver_t *rx, uint8_t *frame, size_t max_len)
{
    if (!rx->frame_ready || rx->frame_len == 0) {
        return 0;
    }
    
    size_t copy_len = (rx->frame_len < max_len) ? rx->frame_len : max_len;
    memcpy(frame, rx->frame_buf, copy_len);
    
    return copy_len;
}

void cobs_receiver_check_timeout(cobs_receiver_t *rx, uint32_t now_ms)
{
    // If we have partial data and too much time has passed, reset
    if (rx->pos > 0 && !rx->frame_ready) {
        if ((now_ms - rx->last_rx_time) > rx->timeout_ms) {
            cobs_receiver_reset(rx);
        }
    }
}

void cobs_receiver_reset(cobs_receiver_t *rx)
{
    rx->pos = 0;
    rx->frame_ready = false;
    rx->frame_len = 0;
}

