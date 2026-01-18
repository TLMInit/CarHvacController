/**
 * @file hvac_hw_config.h
 * @brief Hardware configuration for Automotive HVAC Control System
 * 
 * This file contains all pin definitions and hardware configuration
 * for the ESP32-S3 Touch LCD 2 board and HVAC components.
 */

#ifndef HVAC_HW_CONFIG_H
#define HVAC_HW_CONFIG_H

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/*******************************************************************************
 * Display Configuration (ST7789 via SPI)
 ******************************************************************************/
#define LCD_SPI_HOST            SPI2_HOST
#define LCD_PIN_SCLK            GPIO_NUM_39
#define LCD_PIN_MOSI            GPIO_NUM_38
#define LCD_PIN_MISO            GPIO_NUM_40
#define LCD_PIN_DC              GPIO_NUM_42
#define LCD_PIN_CS              GPIO_NUM_45
#define LCD_PIN_RST             -1              // Not connected
#define LCD_PIN_BACKLIGHT       GPIO_NUM_1

// Landscape orientation: 320 wide x 240 tall
#define LCD_H_RES               320
#define LCD_V_RES               240

// Make LCD resolution available without LCD_ prefix for touch config
#define DISPLAY_H_RES           LCD_H_RES
#define DISPLAY_V_RES           LCD_V_RES
#define LCD_PIXEL_CLOCK_HZ      (80 * 1000 * 1000)
#define LCD_CMD_BITS            8
#define LCD_PARAM_BITS          8
#define LCD_SPI_MODE            0
#define LCD_COLOR_SPACE         ESP_LCD_COLOR_SPACE_RGB
#define LCD_BITS_PER_PIXEL      16
#define LCD_DRAW_BUFFER_HEIGHT  50
#define LCD_INVERT_COLOR        true
#define LCD_MIRROR_X            false
#define LCD_MIRROR_Y            true
#define LCD_SWAP_XY             true   // Enable XY swap for landscape

// SPI DMA Configuration
#define LCD_SPI_DMA_CHANNEL     SPI_DMA_CH_AUTO
#define LCD_SPI_MAX_TRANSFER    4000
#define LCD_SPI_QUEUE_DEPTH     10

/*******************************************************************************
 * Backlight PWM Configuration
 ******************************************************************************/
#define BACKLIGHT_LEDC_TIMER    LEDC_TIMER_0
#define BACKLIGHT_LEDC_MODE     LEDC_LOW_SPEED_MODE
#define BACKLIGHT_LEDC_CHANNEL  LEDC_CHANNEL_0
#define BACKLIGHT_DUTY_RES      LEDC_TIMER_10_BIT
#define BACKLIGHT_MAX_DUTY      1024
#define BACKLIGHT_DEFAULT_DUTY  1024    // 100% brightness
#define BACKLIGHT_FREQ_HZ       10000   // 10 kHz

/*******************************************************************************
 * Touch Panel Configuration (CST816S via I2C)
 * Configured for landscape orientation with XY swap
 ******************************************************************************/
#define TOUCH_I2C_PORT          I2C_NUM_0
#define TOUCH_PIN_SDA           GPIO_NUM_48
#define TOUCH_PIN_SCL           GPIO_NUM_47
#define TOUCH_I2C_ADDR          0x15
#define TOUCH_I2C_SPEED_HZ      400000  // 400 kHz
#define TOUCH_X_MAX             LCD_H_RES   // 320 - landscape width
#define TOUCH_Y_MAX             LCD_V_RES   // 240 - landscape height
#define TOUCH_PIN_RST           -1
#define TOUCH_PIN_INT           -1

/*******************************************************************************
 * TB6612FNG Motor Driver Configuration (Valve Control)
 * 
 * TB6612FNG can drive 2 motors per chip. We need 5 valves:
 * - 2x Center vents valves (Motor A & B on TB6612FNG #1)
 * - 1x Foot vent valve (Motor A on TB6612FNG #2)
 * - 1x Windscreen valve (Motor B on TB6612FNG #2)
 * - 1x Coolant valve (Motor A on TB6612FNG #3)
 ******************************************************************************/

// TB6612FNG #1: Center Valves (2x) - STBY tied to 3V3 (always enabled)
#define TB6612_1_AIN1           GPIO_NUM_4
#define TB6612_1_AIN2           GPIO_NUM_6
#define TB6612_1_PWMA           GPIO_NUM_2
#define TB6612_1_BIN1           GPIO_NUM_17
#define TB6612_1_BIN2           GPIO_NUM_18
#define TB6612_1_PWMB           GPIO_NUM_16

// TB6612FNG #2: Foot & Windscreen Valves - STBY tied to 3V3 (always enabled)
#define TB6612_2_AIN1           GPIO_NUM_8      // Foot valve
#define TB6612_2_AIN2           GPIO_NUM_7
#define TB6612_2_PWMA           GPIO_NUM_21
// Note: GPIO19/20 are USB D-/D+ on ESP32-S3, cannot use! 
// Note: GPIO3/46 are strapping pins, avoid if possible
// Using GPIO5 and GPIO41 instead
#define TB6612_2_BIN1           GPIO_NUM_5      // Windscreen valve direction 1
#define TB6612_2_BIN2           GPIO_NUM_41     // Windscreen valve direction 2
#define TB6612_2_PWMB           GPIO_NUM_10

// TB6612FNG #3: Coolant Valve & Spare - STBY tied to 3V3 (always enabled)
// Note: GPIO33-37 are reserved for octal PSRAM on ESP32-S3!
// Using GPIO11/12 for direction, GPIO13 for PWM
#define TB6612_3_AIN1           GPIO_NUM_11     // Coolant valve direction 1
#define TB6612_3_AIN2           GPIO_NUM_12     // Coolant valve direction 2
#define TB6612_3_PWMA           GPIO_NUM_13     // Coolant valve PWM
// Spare motor output - NOT USED (no more GPIO available)
// If needed in future, may need to remove some functionality or use I2C GPIO expander

// TB6612FNG PWM Configuration
#define TB6612_PWM_FREQ_HZ      20000   // 20 kHz for quiet operation
#define TB6612_PWM_RESOLUTION   LEDC_TIMER_8_BIT
#define TB6612_PWM_MAX_DUTY     255
#define TB6612_LEDC_TIMER       LEDC_TIMER_1
#define TB6612_LEDC_MODE        LEDC_LOW_SPEED_MODE

// PWM Channels for TB6612FNG motors
#define TB6612_1_PWMA_CHANNEL   LEDC_CHANNEL_1
#define TB6612_1_PWMB_CHANNEL   LEDC_CHANNEL_2
#define TB6612_2_PWMA_CHANNEL   LEDC_CHANNEL_3
#define TB6612_2_PWMB_CHANNEL   LEDC_CHANNEL_4
#define TB6612_3_PWMA_CHANNEL   LEDC_CHANNEL_5

/*******************************************************************************
 * BTS7960 Motor Driver Configuration (Blower Motor)
 * 
 * BTS7960 is a high-current H-bridge for the HVAC blower motor
 ******************************************************************************/
#define BTS7960_RPWM            GPIO_NUM_14     // Right PWM (speed control)
// Note: R_EN tied to 3V3, L_EN tied to GND, L_PWM not connected

// BTS7960 PWM Configuration
#define BTS7960_PWM_FREQ_HZ     25000   // 25 kHz for quiet blower operation
#define BTS7960_PWM_RESOLUTION  LEDC_TIMER_10_BIT
#define BTS7960_PWM_MAX_DUTY    1023
#define BTS7960_LEDC_TIMER      LEDC_TIMER_2
#define BTS7960_LEDC_MODE       LEDC_LOW_SPEED_MODE
#define BTS7960_RPWM_CHANNEL    LEDC_CHANNEL_6

/*******************************************************************************
 * SHT30-D Sensor Configuration (I2C)
 * Measures cabin temperature and humidity
 ******************************************************************************/
#define SHT30_I2C_PORT          I2C_NUM_1       // Use separate I2C bus
#define SHT30_PIN_SDA           GPIO_NUM_43
#define SHT30_PIN_SCL           GPIO_NUM_44
#define SHT30_I2C_ADDR          0x44            // Default address (ADDR pin LOW)
#define SHT30_I2C_SPEED_HZ      100000          // 100 kHz

/*******************************************************************************
 * DS18B20 Temperature Sensors (1-Wire)
 * - One for coolant temperature
 * - One for duct air temperature
 ******************************************************************************/
#define DS18B20_ONEWIRE_PIN     GPIO_NUM_9      // Shared 1-Wire bus for both sensors
#define DS18B20_MAX_SENSORS     2

/*******************************************************************************
 * ADS1115 ADC Configuration (I2C)
 * Two 16-bit ADC modules for analog signal measurement
 * - ADS1115 #1: 4 analog inputs (0-3V3 range)
 * - ADS1115 #2: 4 analog inputs (0-3V3 range)
 * Note: Shares I2C_NUM_1 with SHT30 sensor (ESP32-S3 has only 2 I2C controllers)
 ******************************************************************************/
#define ADS1115_I2C_PORT        I2C_NUM_1       // Shared with SHT30
#define ADS1115_PIN_SDA         GPIO_NUM_43     // Same as SHT30
#define ADS1115_PIN_SCL         GPIO_NUM_44     // Same as SHT30
#define ADS1115_I2C_SPEED_HZ    100000          // 100 kHz (match SHT30)

// ADS1115 I2C Addresses (configurable via ADDR pin)
#define ADS1115_ADDR_1          0x48            // ADDR pin → GND
#define ADS1115_ADDR_2          0x49            // ADDR pin → VDD
#define ADS1115_NUM_DEVICES     2
#define ADS1115_CHANNELS_PER_DEVICE  4

/*******************************************************************************
 * Valve Identifiers
 ******************************************************************************/
typedef enum {
    HVAC_VALVE_CENTER_1 = 0,    // Center vent valve 1
    HVAC_VALVE_CENTER_2,        // Center vent valve 2
    HVAC_VALVE_FOOT,            // Foot vent valve
    HVAC_VALVE_WINDSCREEN,      // Windscreen/defrost valve
    HVAC_VALVE_COOLANT,         // Coolant (heater core) valve
    HVAC_VALVE_MAX
} hvac_valve_id_t;

/*******************************************************************************
 * HVAC Operating Modes
 ******************************************************************************/
typedef enum {
    HVAC_MODE_OFF = 0,
    HVAC_MODE_FACE,             // Air to center vents only
    HVAC_MODE_FOOT,             // Air to foot vents only
    HVAC_MODE_FACE_FOOT,        // Air to face and foot vents
    HVAC_MODE_DEFROST,          // Air to windscreen only
    HVAC_MODE_DEFROST_FOOT,     // Air to windscreen and foot vents
    HVAC_MODE_AUTO,             // Automatic mode
    HVAC_MODE_MAX
} hvac_mode_t;

/*******************************************************************************
 * HVAC Fan Speed Levels
 ******************************************************************************/
typedef enum {
    HVAC_FAN_OFF = 0,
    HVAC_FAN_LOW,
    HVAC_FAN_MEDIUM_LOW,
    HVAC_FAN_MEDIUM,
    HVAC_FAN_MEDIUM_HIGH,
    HVAC_FAN_HIGH,
    HVAC_FAN_AUTO,
    HVAC_FAN_MAX
} hvac_fan_speed_t;

/*******************************************************************************
 * Function Declarations
 ******************************************************************************/

/**
 * @brief Initialize all GPIO pins for HVAC hardware
 * @return ESP_OK on success
 */
esp_err_t hvac_hw_gpio_init(void);

/**
 * @brief Initialize I2C buses for touch panel and sensors
 * @param[out] touch_i2c_bus Handle to touch I2C bus (I2C_NUM_0)
 * @param[out] sensor_i2c_bus Handle to sensor I2C bus (I2C_NUM_1, shared with ADS1115)
 * @return ESP_OK on success
 */
esp_err_t hvac_hw_i2c_init(i2c_master_bus_handle_t *touch_i2c_bus, 
                           i2c_master_bus_handle_t *sensor_i2c_bus);

/**
 * @brief Initialize LEDC PWM for all motor drivers
 * @return ESP_OK on success
 */
esp_err_t hvac_hw_pwm_init(void);

/**
 * @brief Initialize LCD backlight with PWM control
 * @return ESP_OK on success
 */
esp_err_t hvac_hw_backlight_init(void);

/**
 * @brief Set LCD backlight brightness
 * @param brightness_percent Brightness 0-100%
 */
void hvac_hw_backlight_set(uint8_t brightness_percent);

#ifdef __cplusplus
}
#endif

#endif // HVAC_HW_CONFIG_H
