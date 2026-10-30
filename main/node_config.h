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

#define NODE_VERSION_STRING     "ionode 2.3"   /* protocol_server.c appends the build's git describe */

/* A new OTA image must answer PING and VERSION within this time, or it rolls back */
#define OTA_CONFIRM_TIMEOUT_MS  60000

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
#define LINK_PAD_TRAILING_ZERO  1       /* 1 = pad replies whose CRC ends in 0x00 until a HELLO arrives (protocol 1 displays strip one trailing zero); 0 = never pad */

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

/* ---- Status LED (WS2812) --------------------------------------------------
 * The original CarHvacIONode drove the DevKitC's on-board RGB LED on GPIO48
 * (disassembled from its flash dump: strip_gpio_num = 48, max_leds = 1, RMT at
 * 10 MHz). On this carrier GPIO48 is also the blower PWM input, so every LED
 * refresh pulsed the BTS7960 for 30 us until the display's PWM_INIT(48) took
 * the pin and the LED went dark. The LED yields the pin the same way here:
 *   48: as the original (DevKitC-1 v1.0). Shows boot, waiting and link-up, then
 *       dark once the display claims the blower pin; two 30 us bursts per
 *       second reach the blower driver while waiting.
 *   38: DevKitC-1 v1.1 LED pin, also the foot valve's AIN1. Harmless while
 *       PWMA is 0; dark the first time the foot valve moves.
 * Any free GPIO with an external WS2812 keeps the LED alive for good.
 * -1 disables the LED entirely.
 */
#define NODE_STATUS_LED_GPIO        -1
#define NODE_STATUS_LED_BRIGHTNESS  24      /* 0-255; the on-board LED is bright */

/* ---- GPIO numbers the link may touch -------------------------------------
 * Exactly the carrier's outputs: the boot output pins and the boot PWM pins above.
 * Everything else is refused with ERR_INVALID_PIN, including strapping pins
 * (0, 3, 45, 46), USB (19, 20), flash/PSRAM (26-37), console (43, 44), the link
 * itself (2, 21), the I2C and 1-Wire pins (7, 8, 9) and unused pins (1, 10, 11).
 * On the DevKitC-1 the RGB LED data line is GPIO48 (v1.0) or GPIO38 (v1.1):
 * check the board revision before reading anything into LED flicker.
 */
#define NODE_ALLOWED_PINS       { 4, 5, 6, 12, 13, 14, 15, 16, 17, 18, \
                                  38, 39, 40, 41, 42, 47, 48 }

#endif /* NODE_CONFIG_H */
