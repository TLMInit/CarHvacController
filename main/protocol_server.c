/**
 * @file protocol_server.c
 * @brief UART link to the display: COBS/CRC16 frames in, one reply per command out.
 *
 * Message types, payload layouts and error codes come from cobs_protocol.h,
 * which is shared with the display firmware. The display sends one command and
 * waits (2 ms grace, then up to 200 ms) for exactly one reply, so this task
 * never sends anything unsolicited.
 *
 * Link watchdog: when LINK_WATCHDOG_MS passes without a valid frame after at
 * least one has been seen, every PWM goes to 0 and every output pin low. The
 * display re-commands everything when it returns, so recovery is automatic.
 *
 * Protocol 2: a display that sends HELLO decodes standard COBS, so from then on
 * replies are no longer padded for the protocol 1 trailing-zero strip.
 */

#include <stdio.h>
#include <string.h>
#include "protocol_server.h"
#include "node_config.h"
#include "cobs_protocol.h"
#include "io_hw.h"
#include "io_i2c.h"
#include "io_onewire.h"
#include "io_ota.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "LINK";

extern bool cobs_pad_trailing_zero;     /* cobs_protocol.c; cobs_protocol.h is shared and unchanged */

#define LINK_TASK_STACK         6144
#define LINK_TASK_PRIORITY      10
#define LINK_EVENT_QUEUE_LEN    20
#define LINK_READ_CHUNK         128

#define LINK_WATCHDOG_PERIOD_MS 100

static QueueHandle_t s_uart_queue;
static cobs_receiver_t s_rx;
static protocol_stats_t s_stats;
/* Written by the link task, read by the watchdog timer */
static volatile uint32_t s_last_valid_ms;
static volatile bool s_seen_any_frame;
static volatile bool s_safe_state_applied;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* ------------------------------------------------------------------------- */
/* Replies                                                                   */

static void send_frame(uint8_t type, const uint8_t *payload, size_t len)
{
    /* +1: a full-size reply with the compatibility pad encodes to COBS_MAX_FRAME_SIZE,
     * and cobs_build_frame needs room for the delimiter after it */
    uint8_t frame[COBS_MAX_FRAME_SIZE + 1];
    size_t n = cobs_build_frame(type, payload, len, frame, sizeof(frame));
    if (n == 0) {
        ESP_LOGE(TAG, "Reply 0x%02X (%u bytes) does not fit a frame", type, (unsigned)len);
        return;
    }
    uart_write_bytes(LINK_UART_NUM, frame, n);
    s_stats.tx_frames++;
}

static void send_ok(void)
{
    send_frame(MSG_OK, NULL, 0);
}

static void send_error(cobs_error_t code)
{
    uint8_t c = (uint8_t)code;
    s_stats.errors_sent++;
    send_frame(MSG_ERROR, &c, 1);
}

static const char *reset_reason_name(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "POWERON";
    case ESP_RST_EXT:       return "EXT";
    case ESP_RST_SW:        return "SW";
    case ESP_RST_PANIC:     return "PANIC";
    case ESP_RST_INT_WDT:   return "INT_WDT";
    case ESP_RST_TASK_WDT:  return "TASK_WDT";
    case ESP_RST_WDT:       return "WDT";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    default:                return "OTHER";
    }
}

/* Map an esp_err_t from a hardware call onto the protocol's error codes.
 * invalid_arg: what ESP_ERR_INVALID_ARG means for this command (a pin for GPIO/PWM,
 * an argument for I2C, ADS and 1-Wire). */
static cobs_error_t map_err(esp_err_t err, cobs_error_t fallback, cobs_error_t invalid_arg)
{
    switch (err) {
    case ESP_ERR_INVALID_ARG:   return invalid_arg;
    case ESP_ERR_INVALID_SIZE:  return ERR_INVALID_ARGS;
    case ESP_ERR_TIMEOUT:       return ERR_TIMEOUT;
    case ESP_ERR_NOT_FOUND:     return ERR_TIMEOUT;        /* no DS18B20 presence pulse */
    case ESP_ERR_INVALID_CRC:   return ERR_CRC_MISMATCH;   /* DS18B20 scratchpad */
    case ESP_ERR_NO_MEM:        return ERR_OVERFLOW;
    default:                    return fallback;
    }
}

static void send_ota_ack(void)
{
    ota_state_t state;
    uint32_t bytes;
    io_ota_status(&state, &bytes);
    uint8_t resp[5] = { (uint8_t)state, (uint8_t)(bytes >> 24), (uint8_t)(bytes >> 16),
                        (uint8_t)(bytes >> 8), (uint8_t)bytes };
    send_frame(MSG_OTA_ACK, resp, sizeof(resp));
}

static void send_ota_result(cobs_error_t e)
{
    if (e != ERR_NONE) {
        send_error(e);
    } else {
        send_ota_ack();
    }
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* ------------------------------------------------------------------------- */
/* Command handlers                                                          */

static void handle_command(uint8_t type, const uint8_t *p, size_t len)
{
    esp_err_t err;

    switch (type) {

    case MSG_PING:
        send_frame(MSG_PONG, NULL, 0);
        io_ota_link_answered(type);
        return;

    case MSG_HELLO: {
        if (len != 1) { send_error(ERR_INVALID_ARGS); return; }
        if (cobs_pad_trailing_zero) {
            ESP_LOGI(TAG, "HELLO from display protocol %u: reply padding off", p[0]);
        }
        cobs_pad_trailing_zero = false;
        /* version(1) caps(1) string NUL: the string gets at most 60 bytes */
        uint8_t resp[COBS_MAX_PAYLOAD_SIZE - 1];
        resp[0] = PROTOCOL_VERSION;
        resp[1] = NODE_CAP_SEARCH | (io_ota_supported() ? NODE_CAP_OTA : 0);
        int n = snprintf((char *)&resp[2], sizeof(resp) - 2, "%s (%.20s)", NODE_VERSION_STRING,
                         esp_app_get_description()->version);
        if (n >= (int)sizeof(resp) - 2) {
            n = sizeof(resp) - 3;
        }
        send_frame(MSG_HELLO_RESP, resp, (size_t)n + 3);
        return;
    }

    case MSG_VERSION: {
        /* Uptime and reset reason let the display tell a node reset (e.g. brown-out
         * on blower inrush) from a lost reply. Worst case is 58 bytes + NUL. */
        char v[COBS_MAX_PAYLOAD_SIZE - 1];
        int n = snprintf(v, sizeof(v), "%s (%.20s) up=%lu rst=%s", NODE_VERSION_STRING,
                         esp_app_get_description()->version,
                         (unsigned long)(esp_timer_get_time() / 1000000), reset_reason_name());
        if (n >= (int)sizeof(v)) {
            n = sizeof(v) - 1;
        }
        send_frame(MSG_VERSION_RESP, (const uint8_t *)v, (size_t)n + 1);
        io_ota_link_answered(type);
        return;
    }

    case MSG_GPIO_MODE:
        if (len != 2) { send_error(ERR_INVALID_ARGS); return; }
        err = io_gpio_mode(p[0], p[1]);
        if (err != ESP_OK) { send_error(map_err(err, ERR_INVALID_ARGS, ERR_INVALID_PIN)); return; }
        send_ok();
        return;

    case MSG_GPIO_SET:
        if (len != 2) { send_error(ERR_INVALID_ARGS); return; }
        err = io_gpio_set(p[0], p[1]);
        if (err != ESP_OK) { send_error(map_err(err, ERR_INVALID_ARGS, ERR_INVALID_PIN)); return; }
        send_ok();
        return;

    case MSG_GPIO_GET: {
        if (len != 1) { send_error(ERR_INVALID_ARGS); return; }
        int level = 0;
        err = io_gpio_get(p[0], &level);
        if (err != ESP_OK) { send_error(map_err(err, ERR_INVALID_ARGS, ERR_INVALID_PIN)); return; }
        uint8_t resp[2] = { p[0], (uint8_t)level };
        send_frame(MSG_GPIO_VALUE, resp, 2);
        return;
    }

    case MSG_PWM_INIT:
        if (len != 2) { send_error(ERR_INVALID_ARGS); return; }
        err = io_pwm_init(p[0], p[1]);
        if (err != ESP_OK) { send_error(map_err(err, ERR_INVALID_ARGS, ERR_INVALID_PIN)); return; }
        send_ok();
        return;

    case MSG_PWM_SET:
        if (len != 2) { send_error(ERR_INVALID_ARGS); return; }
        err = io_pwm_set(p[0], p[1]);
        if (err != ESP_OK) { send_error(map_err(err, ERR_INVALID_ARGS, ERR_INVALID_PIN)); return; }
        send_ok();
        return;

    case MSG_I2C_WRITE: {
        /* addr, reg, len, data[len] */
        if (len < 3 || len != (size_t)3 + p[2]) { send_error(ERR_INVALID_ARGS); return; }
        err = io_i2c_write(p[0], p[1], &p[3], p[2]);
        if (err != ESP_OK) { send_error(map_err(err, ERR_I2C_FAIL, ERR_INVALID_ARGS)); return; }
        send_ok();
        return;
    }

    case MSG_I2C_READ: {
        /* addr, reg, len -> I2C_DATA: len, data[len] */
        if (len != 3) { send_error(ERR_INVALID_ARGS); return; }
        uint8_t n = p[2];
        /* Reply = len byte + data + CRC + possible pad must fit COBS_MAX_FRAME_SIZE; the display asks for at most 26. */
        if (n == 0 || n > 58) { send_error(ERR_INVALID_ARGS); return; }
        uint8_t resp[COBS_MAX_PAYLOAD_SIZE];
        err = io_i2c_read(p[0], p[1], &resp[1], n);
        if (err != ESP_OK) { send_error(map_err(err, ERR_I2C_FAIL, ERR_INVALID_ARGS)); return; }
        resp[0] = n;
        send_frame(MSG_I2C_DATA, resp, (size_t)n + 1);
        return;
    }

    case MSG_ADS_READ: {
        /* i2c_addr, channel -> ADS_VALUE: channel, hi, lo */
        if (len != 2) { send_error(ERR_INVALID_ARGS); return; }
        int16_t value = 0;
        err = io_ads1115_read(p[0], p[1], &value);
        if (err != ESP_OK) { send_error(map_err(err, ERR_I2C_FAIL, ERR_INVALID_ARGS)); return; }
        uint8_t resp[3] = { p[1], (uint8_t)((uint16_t)value >> 8), (uint8_t)((uint16_t)value & 0xFF) };
        send_frame(MSG_ADS_VALUE, resp, 3);
        return;
    }

    case MSG_DS18B20_START:
        if (len != 0 && len != 8) { send_error(ERR_INVALID_ARGS); return; }
        err = io_ds18b20_start(len == 8 ? p : NULL);
        if (err != ESP_OK) { send_error(map_err(err, ERR_TIMEOUT, ERR_INVALID_ARGS)); return; }
        send_ok();
        return;

    case MSG_DS18B20_READ: {
        if (len != 0 && len != 8) { send_error(ERR_INVALID_ARGS); return; }
        int16_t t = 0;
        err = io_ds18b20_read(len == 8 ? p : NULL, &t);
        if (err != ESP_OK) { send_error(map_err(err, ERR_TIMEOUT, ERR_INVALID_ARGS)); return; }
        uint8_t resp[2] = { (uint8_t)((uint16_t)t >> 8), (uint8_t)((uint16_t)t & 0xFF) };
        send_frame(MSG_TEMP_VALUE, resp, 2);
        return;
    }

    case MSG_ONEWIRE_SEARCH: {
        if (len != 0) { send_error(ERR_INVALID_ARGS); return; }
        uint8_t resp[1 + ONEWIRE_SEARCH_MAX_ROMS * 8];
        size_t count = 0;
        err = io_onewire_search((uint8_t (*)[8])&resp[1], ONEWIRE_SEARCH_MAX_ROMS, &count);
        if (err != ESP_OK) { send_error(map_err(err, ERR_TIMEOUT, ERR_INVALID_ARGS)); return; }
        resp[0] = (uint8_t)count;
        send_frame(MSG_ROM_LIST, resp, 1 + count * 8);
        return;
    }

    case MSG_OTA_BEGIN:
        if (len != 4 + 32) { send_error(ERR_INVALID_ARGS); return; }
        send_ota_result(io_ota_begin(be32(p), &p[4]));
        return;

    case MSG_OTA_DATA:
        if (len < 5 || len > 4 + OTA_DATA_MAX) { send_error(ERR_INVALID_ARGS); return; }
        send_ota_result(io_ota_data(be32(p), &p[4], len - 4));
        return;

    case MSG_OTA_END:
        if (len != 0) { send_error(ERR_INVALID_ARGS); return; }
        send_ota_result(io_ota_end());
        return;

    case MSG_OTA_STATUS:
        if (len != 0) { send_error(ERR_INVALID_ARGS); return; }
        send_ota_ack();
        return;

    case MSG_RESET:
        send_ok();
        uart_wait_tx_done(LINK_UART_NUM, pdMS_TO_TICKS(100));
        ESP_LOGW(TAG, "Reset requested by the display");
        io_outputs_safe_state();
        vTaskDelay(pdMS_TO_TICKS(20));
        esp_restart();
        return;

    default:
        ESP_LOGW(TAG, "Unknown command 0x%02X", type);
        send_error(ERR_INVALID_CMD);
        return;
    }
}

/* ------------------------------------------------------------------------- */
/* Frame handling and watchdog                                               */

static void process_frame(void)
{
    uint8_t frame[COBS_MAX_FRAME_SIZE];
    size_t frame_len = cobs_receiver_get_frame(&s_rx, frame, sizeof(frame));
    cobs_receiver_reset(&s_rx);
    if (frame_len == 0) {
        return;
    }

    uint8_t type;
    uint8_t payload[COBS_MAX_PAYLOAD_SIZE];
    size_t payload_len = 0;
    if (!cobs_parse_frame(frame, frame_len, &type, payload, sizeof(payload), &payload_len)) {
        s_stats.bad_frames++;
        ESP_LOGD(TAG, "Bad frame (%u bytes)", (unsigned)frame_len);
#if LINK_REPLY_ON_CRC_ERROR
        send_error(ERR_CRC_MISMATCH);
#endif
        return;
    }

    s_stats.rx_frames++;
    s_last_valid_ms = now_ms();
    s_seen_any_frame = true;
    if (s_safe_state_applied) {
        s_safe_state_applied = false;
        ESP_LOGI(TAG, "Link restored");
    }
    s_stats.link_up = true;

    ESP_LOGD(TAG, "Cmd 0x%02X, %u bytes", type, (unsigned)payload_len);
    handle_command(type, payload, payload_len);
    /* An OTA stream is one frame after another; feed per frame, not only per idle loop */
    esp_task_wdt_reset();
}

/* Runs from an esp_timer, not from link_task: if link_task hangs inside a driver
 * call, the outputs still go safe; the task watchdog then restarts the node. */
static void watchdog_timer_cb(void *arg)
{
#if LINK_WATCHDOG_MS > 0
    if (!s_seen_any_frame || s_safe_state_applied) {
        return;
    }
    if (now_ms() - s_last_valid_ms > LINK_WATCHDOG_MS) {
        io_outputs_safe_state();
        s_safe_state_applied = true;
        s_stats.link_up = false;
        s_stats.watchdog_trips++;
        ESP_LOGW(TAG, "No frames for %u ms: outputs forced to safe state", (unsigned)LINK_WATCHDOG_MS);
    }
#endif
}

static void link_task(void *arg)
{
    uart_event_t event;
    uint8_t buf[LINK_READ_CHUNK];

    ESP_LOGI(TAG, "Link task running on core %d", xPortGetCoreID());
    /* The loop wakes at least every 100 ms; a hang in a driver call restarts the node */
    esp_task_wdt_add(NULL);

    while (1) {
        if (xQueueReceive(s_uart_queue, &event, pdMS_TO_TICKS(100)) == pdTRUE) {
            switch (event.type) {
            case UART_DATA: {
                size_t remaining = event.size;
                while (remaining > 0) {
                    size_t want = remaining < sizeof(buf) ? remaining : sizeof(buf);
                    int n = uart_read_bytes(LINK_UART_NUM, buf, want, 0);
                    if (n <= 0) {
                        break;
                    }
                    remaining -= (size_t)n;
                    uint32_t now = now_ms();
                    /* One byte at a time so a frame directly behind another is not lost. */
                    for (int i = 0; i < n; i++) {
                        if (cobs_receiver_feed(&s_rx, &buf[i], 1, now)) {
                            process_frame();
                        }
                    }
                }
                break;
            }
            case UART_FIFO_OVF:
            case UART_BUFFER_FULL:
                ESP_LOGW(TAG, "UART overflow, flushing");
                uart_flush_input(LINK_UART_NUM);
                xQueueReset(s_uart_queue);
                cobs_receiver_reset(&s_rx);
                break;
            case UART_FRAME_ERR:
            case UART_PARITY_ERR:
                s_stats.bad_frames++;
                cobs_receiver_reset(&s_rx);
                break;
            default:
                break;
            }
        }
        cobs_receiver_check_timeout(&s_rx, now_ms());
        esp_task_wdt_reset();
    }
}

/* ------------------------------------------------------------------------- */

esp_err_t protocol_server_start(void)
{
    uart_config_t cfg = {
        .baud_rate = LINK_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_driver_install(LINK_UART_NUM, LINK_RX_BUF_SIZE, LINK_TX_BUF_SIZE,
                                        LINK_EVENT_QUEUE_LEN, &s_uart_queue, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install: %s", esp_err_to_name(err));
        return err;
    }
    err = uart_param_config(LINK_UART_NUM, &cfg);
    if (err != ESP_OK) {
        return err;
    }
    err = uart_set_pin(LINK_UART_NUM, LINK_TX_PIN, LINK_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        return err;
    }
    /* Deliver received bytes after ~4 symbol times of idle, i.e. right behind each frame. */
    uart_set_rx_timeout(LINK_UART_NUM, 4);

    cobs_receiver_init(&s_rx, LINK_FRAME_TIMEOUT_MS);
    memset(&s_stats, 0, sizeof(s_stats));
    s_seen_any_frame = false;
    s_safe_state_applied = false;

#if LINK_WATCHDOG_MS > 0
    const esp_timer_create_args_t wd_args = {
        .callback = watchdog_timer_cb,
        .name = "link_wd",
    };
    esp_timer_handle_t wd_timer;
    err = esp_timer_create(&wd_args, &wd_timer);
    if (err == ESP_OK) {
        err = esp_timer_start_periodic(wd_timer, LINK_WATCHDOG_PERIOD_MS * 1000);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "link watchdog timer: %s", esp_err_to_name(err));
        return err;
    }
#endif

#if CONFIG_FREERTOS_UNICORE
    const BaseType_t core = 0;
#else
    const BaseType_t core = 1;
#endif
    BaseType_t ok = xTaskCreatePinnedToCore(link_task, "link", LINK_TASK_STACK, NULL,
                                            LINK_TASK_PRIORITY, NULL, core);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "UART%d TX GPIO%d RX GPIO%d, %d baud, COBS+CRC16, watchdog %d ms",
             LINK_UART_NUM, LINK_TX_PIN, LINK_RX_PIN, LINK_BAUD, LINK_WATCHDOG_MS);
    return ESP_OK;
}

void protocol_server_get_stats(protocol_stats_t *out)
{
    if (out) {
        *out = s_stats;
    }
}
