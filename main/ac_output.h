/**
 * @file ac_output.h
 * @brief Electric compressor speed output over the link (SET_AC_OUT_TYPE). Called only from the
 * controller task, outside its state mutex; no locking of its own.
 *
 *  - pwm:         PWM_INIT_HZ(pin, freq, ac_out_pwm_bits(freq)), duty as raw counts (3-byte PWM_SET).
 *  - analog_rc:   PWM_INIT_HZ(pin, SETTINGS_AC_RC_FREQ_HZ, 8), 8-bit duty (2-byte PWM_SET).
 *  - dac_mcp4725: I2C_WRITE fast write to SET_AC_DAC_ADDR; once per boot and address the DAC
 *                 register and EEPROM are set to 0 first (safe power-up level).
 * Every type needs NODE_CAP_PWM_HZ (a node new enough for the compressor output); without it the
 * state is AC_OUT_UNSUPPORTED (event log once per HELLO) and the controller keeps enable low.
 * The channel is (re)initialised when the type/pin/frequency/address changes and whenever
 * remote_io_hello_seq() moves (boot, link recovered, node restart). A pin, type or address change
 * first sets the old pin's duty (or the old DAC) to 0. Leaving the electric type zeroes it too.
 */
#ifndef AC_OUTPUT_H
#define AC_OUTPUT_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool active;                /* SET_AC_COMPRESSOR = electric */
    uint8_t type;               /* set_ac_out_type_t */
    uint8_t pin;                /* node GPIO (SETTINGS_AC_OUT_PINS[SET_AC_OUT_PIN]) */
    uint32_t freq_hz;           /* SET_AC_OUT_FREQ_HZ (pwm only) */
    uint8_t dac_addr;           /* 0x60 + SET_AC_DAC_ADDR */
} ac_output_cfg_t;

typedef enum {
    AC_OUT_OFF = 0,             /* clutch type: output not used */
    AC_OUT_WAIT,                /* no HELLO yet or link down: nothing sent */
    AC_OUT_READY,               /* channel initialised, value follows */
    AC_OUT_UNSUPPORTED,         /* node lacks NODE_CAP_PWM_HZ */
    AC_OUT_FAILED,              /* init refused or timed out; retried once a second */
} ac_output_state_t;

/** Each control cycle: apply cfg, (re)initialise when due, send pct (0..100 % of full scale) on change. */
void ac_output_service(const ac_output_cfg_t *cfg, float pct);

/** Re-send the last value (CTRL_REASSERT_MS), so the output recovers after the node's safe state. */
void ac_output_reassert(void);

ac_output_state_t ac_output_state(void);

#endif /* AC_OUTPUT_H */
