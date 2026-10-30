/* Electric compressor speed output over the link: see ac_output.h. */
#include <string.h>
#include "ac_output.h"
#include "ac_output_calc.h"
#include "cobs_protocol.h"
#include "event_log.h"
#include "remote_io.h"
#include "settings.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "AC_OUT";

#define AC_OUT_RETRY_MS 1000u   /* failed init retried this often */

static ac_output_cfg_t s_cur;           /* what the channel is (to be) set up for; active false = nothing */
static ac_output_state_t s_state = AC_OUT_OFF;
static uint32_t s_init_seq;             /* remote_io_hello_seq() at the last init attempt */
static uint32_t s_retry_ms;
static unsigned s_bits;                 /* pwm resolution in use */
static int32_t s_sent = -1;             /* last value acknowledged (counts / duty / DAC code), -1 = none */
static uint8_t s_eeprom_addr;           /* DAC whose EEPROM got 0 this boot (0 = none yet) */
static ac_output_state_t s_logged;      /* last init result: a fault goes to the event log once */

static uint32_t now_ms(void)
{
    return xTaskGetTickCount() * portTICK_PERIOD_MS;
}

static esp_err_t dac_write(uint8_t addr, uint16_t value)
{
    uint8_t d[2];
    size_t n = ac_out_dac_fast(d, value);
    return remote_i2c_write(0, addr, d, n);
}

static int32_t value_for(float pct)
{
    switch (s_cur.type) {
    case SET_AC_OUT_PWM:       return ac_out_pwm_counts(pct, s_bits);
    case SET_AC_OUT_ANALOG_RC: return ac_out_duty8(pct);
    default:                   return ac_out_dac_value(pct);
    }
}

static esp_err_t send_value(int32_t v)
{
    switch (s_cur.type) {
    case SET_AC_OUT_PWM:       return remote_pwm_set_duty16(s_cur.pin, (uint16_t)v);
    case SET_AC_OUT_ANALOG_RC: return remote_pwm_set_duty(s_cur.pin, (int)v);
    default:                   return dac_write(s_cur.dac_addr, (uint16_t)v);
    }
}

/* The old output to 0 before it is given up (pin/type/address change, or clutch type). */
static void zero_old(const ac_output_cfg_t *old, const ac_output_cfg_t *next)
{
    bool old_dac = old->type == SET_AC_OUT_DAC_MCP4725;
    bool next_dac = next->active && next->type == SET_AC_OUT_DAC_MCP4725;
    if (!old_dac && (!next->active || next_dac || next->pin != old->pin)) {
        (void)remote_pwm_set_duty(old->pin, 0);     /* 2-byte 0 is off on either kind of channel */
        ESP_LOGI(TAG, "GPIO%u duty 0 (output moved or off)", old->pin);
    } else if (old_dac && (!next_dac || next->dac_addr != old->dac_addr)) {
        (void)dac_write(old->dac_addr, 0);
        ESP_LOGI(TAG, "DAC 0x%02X set to 0 (output moved or off)", old->dac_addr);
    }
}

static ac_output_state_t channel_init(void)
{
    remote_io_health_t h;
    remote_io_get_health(&h);
    if (!(h.node_caps & NODE_CAP_PWM_HZ)) {
        if (s_logged != AC_OUT_UNSUPPORTED) {
            event_log_add(EVT_FAULT, "A/C electric output unavailable: node lacks PWM_HZ (caps 0x%02X)",
                          (unsigned)h.node_caps);
        }
        return s_logged = AC_OUT_UNSUPPORTED;
    }
    esp_err_t err;
    switch (s_cur.type) {
    case SET_AC_OUT_PWM:
        s_bits = ac_out_pwm_bits(s_cur.freq_hz);
        err = s_bits ? remote_pwm_init_hz(s_cur.pin, s_cur.freq_hz, (int)s_bits) : ESP_ERR_INVALID_ARG;
        ESP_LOGI(TAG, "PWM GPIO%u %lu Hz %u bit: %s", s_cur.pin, (unsigned long)s_cur.freq_hz, s_bits,
                 esp_err_to_name(err));
        break;
    case SET_AC_OUT_ANALOG_RC:
        s_bits = 8;
        err = remote_pwm_init_hz(s_cur.pin, SETTINGS_AC_RC_FREQ_HZ, 8);
        ESP_LOGI(TAG, "analog RC GPIO%u %u Hz 8 bit: %s", s_cur.pin, (unsigned)SETTINGS_AC_RC_FREQ_HZ,
                 esp_err_to_name(err));
        break;
    default:
        err = ESP_OK;
        if (s_eeprom_addr != s_cur.dac_addr) {
            /* The MCP4725 powers up with its EEPROM value (factory 0x800 = half scale) */
            uint8_t d[3];
            size_t n = ac_out_dac_eeprom(d, 0);
            err = remote_i2c_write(0, s_cur.dac_addr, d, n);
            if (err == ESP_OK) {
                s_eeprom_addr = s_cur.dac_addr;
                event_log_add(EVT_INFO, "A/C DAC 0x%02X: output and EEPROM set to 0", s_cur.dac_addr);
            }
        }
        break;
    }
    if (err != ESP_OK) {
        if (s_logged != AC_OUT_FAILED) {
            event_log_add(EVT_FAULT, "A/C electric output init failed: %s", esp_err_to_name(err));
        }
        return s_logged = AC_OUT_FAILED;
    }
    return s_logged = AC_OUT_READY;
}

void ac_output_service(const ac_output_cfg_t *cfg, float pct)
{
    if (!cfg->active) {
        if (s_cur.active) {
            zero_old(&s_cur, cfg);
            memset(&s_cur, 0, sizeof(s_cur));
        }
        s_state = AC_OUT_OFF;
        return;
    }
    if (!s_cur.active || cfg->type != s_cur.type || cfg->pin != s_cur.pin ||
        cfg->freq_hz != s_cur.freq_hz || cfg->dac_addr != s_cur.dac_addr) {
        if (s_cur.active) zero_old(&s_cur, cfg);
        s_cur = *cfg;
        s_state = AC_OUT_WAIT;
    }

    uint32_t seq = remote_io_hello_seq();
    uint32_t now = now_ms();
    if (seq == 0 || !remote_io_is_connected()) {
        s_state = AC_OUT_WAIT;
        return;
    }
    if (s_state != AC_OUT_WAIT && seq != s_init_seq) {
        s_state = AC_OUT_WAIT;      /* node restarted or link recovered: its channel is gone */
    }
    if (s_state == AC_OUT_FAILED && now - s_retry_ms >= AC_OUT_RETRY_MS) {
        s_state = AC_OUT_WAIT;
    }
    if (s_state == AC_OUT_WAIT) {
        s_init_seq = seq;
        s_retry_ms = now;
        s_sent = -1;
        s_state = channel_init();
        return;                     /* value next cycle (and the DAC has finished its EEPROM write) */
    }
    if (s_state != AC_OUT_READY) {
        return;
    }
    int32_t v = value_for(pct);
    if (v != s_sent) {
        s_sent = send_value(v) == ESP_OK ? v : -1;
    }
}

void ac_output_reassert(void)
{
    if (s_state == AC_OUT_READY && s_sent >= 0) {
        (void)send_value(s_sent);
    }
}

ac_output_state_t ac_output_state(void)
{
    return s_state;
}
