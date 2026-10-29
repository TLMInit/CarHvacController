/**
 * @file node_config.h
 * @brief Pin map and link settings for the HVAC IO Node (ESP32-S3 DevKitC-1 on the carrier PCB).
 *
 * Every number here mirrors hvac_hw_config.h in the display firmware
 * (TLMInit/CarHvacController), which is the authority: the display sends GPIO
 * numbers over the link and the node simply obeys them. The boot tables below
 * exist because the display never sends GPIO_MODE or PWM_INIT for the valve
 * drivers; the old node evidently pre-configured those pins itself.
 */

#ifndef NODE_CONFIG_H
#define NODE_CONFIG_H

#include "driver/gpio.h"
#include "driver/uart.h"
#include "driver/i2c_master.h"

#define NODE_VERSION_STRING     "ionode 2.0 (2026-10-29)"

/* ---- UART link to the display -------------------------------------------
 * Display side: UART1, TX GPIO10 -> node RX, RX GPIO11 <- node TX, 921600 8N1.
 */
#define LINK_UART_NUM           UART_NUM_1
#define LINK_TX_PIN             GPIO_NUM_2
#define LINK_RX_PIN             GPIO_NUM_21
#define LINK_BAUD               921600
#define LINK_RX_BUF_SIZE        512
#define LINK_TX_BUF_SIZE        256
#define LINK_FRAME_TIMEOUT_MS   20      /* partial frame discarded after this much silence */
#define LINK_WATCHDOG_MS        3000    /* outputs forced to safe state after this much silence; 0 = off */
#define LINK_REPLY_ON_CRC_ERROR 0       /* 1 = answer a corrupt frame with ERROR 6 instead of staying silent */
#define LINK_PAD_TRAILING_ZERO  1       /* 1 = pad replies whose CRC ends in 0x00 (display's cobs_decode strips one trailing zero) */

/* ---- I2C bus: 2x ADS1115 (0x48, 0x49) and the BME280 (0x76/0x77) ---------- */
#define NODE_I2C_PORT           I2C_NUM_0
#define NODE_I2C_SDA            GPIO_NUM_8
#define NODE_I2C_SCL            GPIO_NUM_9
#define NODE_I2C_FREQ_HZ        400000
#define NODE_I2C_TIMEOUT_MS     20
#define NODE_I2C_MAX_DEVICES    8

/* ---- 1-Wire: DS18B20 duct sensor, 4.7 k pull-up on the carrier ------------ */
#define NODE_ONEWIRE_PIN        GPIO_NUM_7

/* ---- Outputs that must exist before the display's first command ----------
 * TB6612 #1: AIN1 4, AIN2 16, BIN1 5, BIN2 18      (center valves 1 and 2)
 * TB6612 #2: AIN1 38, AIN2 39, BIN1 41, BIN2 42    (foot, windscreen)
 * TB6612 #3: AIN1 12, AIN2 14                      (coolant)
 * A/C clutch relay: 6 (active high)
 */
#define NODE_BOOT_OUTPUT_PINS   { 4, 16, 5, 18, 38, 39, 41, 42, 12, 14, 6 }

/* PWM outputs, {pin, kHz}: TB6612 PWMA/PWMB at 20 kHz, BTS7960 RPWM at 25 kHz.
 * 8-bit duty, as the link protocol carries one duty byte.
 */
#define NODE_BOOT_PWM_PINS      { {17, 20}, {15, 20}, {40, 20}, {47, 20}, {13, 20}, {48, 25} }
#define NODE_DEFAULT_PWM_KHZ    20      /* used when PWM_SET arrives for a pin that never saw PWM_INIT */

/* ---- GPIO numbers the link may touch -------------------------------------
 * Everything else is refused with ERR_INVALID_PIN: strapping pins (0, 3, 45, 46),
 * USB (19, 20), flash/PSRAM (26-37), console (43, 44), the link itself (2, 21)
 * and the pins owned by I2C and 1-Wire (7, 8, 9).
 */
#define NODE_ALLOWED_PINS       { 1, 4, 5, 6, 10, 11, 12, 13, 14, 15, 16, 17, 18, \
                                  38, 39, 40, 41, 42, 47, 48 }

#endif /* NODE_CONFIG_H */
