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
    #include "driver/spi_master.h"
    #include "driver/i2c_master.h"
    #include "driver/uart.h"

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
    // TB6612FNG #1: Center Valves (2 motors) - STBY tied to 3V3 (always enabled)
    #define TB6612_1_AIN1           GPIO_NUM_4
    #define TB6612_1_AIN2           GPIO_NUM_16
    #define TB6612_1_PWMA           GPIO_NUM_17
    #define TB6612_1_BIN1           GPIO_NUM_5
    #define TB6612_1_BIN2           GPIO_NUM_18
    #define TB6612_1_PWMB           GPIO_NUM_15

    // TB6612FNG #2: Foot & Windscreen Valves - STBY tied to 3V3 (always enabled)
    #define TB6612_2_AIN1           GPIO_NUM_38     // Foot valve (from schematic)
    #define TB6612_2_AIN2           GPIO_NUM_39
    #define TB6612_2_PWMA           GPIO_NUM_40
    #define TB6612_2_BIN1           GPIO_NUM_41     // Windscreen valve
    #define TB6612_2_BIN2           GPIO_NUM_42
    #define TB6612_2_PWMB           GPIO_NUM_47

    // TB6612FNG #3: Coolant Valve & Spare - STBY tied to 3V3 (always enabled)
    #define TB6612_3_AIN1           GPIO_NUM_12     // Coolant valve (from schematic)
    #define TB6612_3_AIN2           GPIO_NUM_14
    #define TB6612_3_PWMA           GPIO_NUM_13
    // Spare motor output - NOT USED (no more GPIO available)
    // If needed in future, may need to remove some functionality or use I2C GPIO expander

    /*******************************************************************************
     * A/C Compressor Control (via relay on IO Node)
     * GPIO6 on IO Node controls A/C compressor clutch relay
     ******************************************************************************/
    #define AC_RELAY_PIN            GPIO_NUM_6      // A/C compressor request (IO Node)
    #define AC_RELAY_ACTIVE_HIGH    1               // 1 = relay active when pin HIGH

    /*******************************************************************************
     * BTS7960 Motor Driver Configuration (Blower Motor)
     * 
     * BTS7960 is a high-current H-bridge for the HVAC blower motor
     ******************************************************************************/
    #define BTS7960_RPWM            GPIO_NUM_48     // Right PWM (from schematic)
    // Note: R_EN tied to 3V3, L_EN tied to GND, L_PWM not connected

    // BTS7960 PWM Configuration
    #define BTS7960_PWM_MAX_DUTY    1023

    /*******************************************************************************
     * SHT30-D Sensor Configuration (I2C)
     * Measures cabin temperature and humidity
     ******************************************************************************/
    #define SHT30_I2C_PORT          I2C_NUM_1       // Use separate I2C bus
    #define SHT30_PIN_SDA           GPIO_NUM_43
    #define SHT30_PIN_SCL           GPIO_NUM_44

    /*******************************************************************************
     * Remote IO Node UART Configuration
     * All GPIO, PWM, I2C, 1-Wire operations go through UART to IO Node
     * IO Node pins: TX=GPIO2, RX=GPIO21 at 921600 baud
     ******************************************************************************/
    #define REMOTE_IO_UART_NUM      UART_NUM_1
    #define REMOTE_IO_TX_PIN        GPIO_NUM_10     // Controller TX -> IO Node RX (GPIO21)
    #define REMOTE_IO_RX_PIN        GPIO_NUM_11     // Controller RX <- IO Node TX (GPIO2)  
    #define REMOTE_IO_BAUD_RATE     921600

    /*******************************************************************************
     * ADS1115 ADC Configuration (I2C) - ON REMOTE IO NODE
     * Two 16-bit ADC modules for analog signal measurement
     * - ADS1115 #1: 4 analog inputs (0-3V3 range)
     * - ADS1115 #2: 4 analog inputs (0-3V3 range)
     * Note: I2C pins below are sent to IO Node via UART I2CINIT command.
     ******************************************************************************/
    #define ADS1115_I2C_SDA         GPIO_NUM_8      // Sent to IO Node
    #define ADS1115_I2C_SCL         GPIO_NUM_9      // Sent to IO Node

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

    // Valve enable flags - set to 0 to disable unconnected valves during testing
    #define VALVE_CENTER_1_ENABLED      1   // Center vent valve 1
    #define VALVE_CENTER_2_ENABLED      1   // Center vent valve 2 (disabled for testing)
    #define VALVE_FOOT_ENABLED          1   // Foot vent valve (disabled for testing)
    #define VALVE_WINDSCREEN_ENABLED    1   // Windscreen valve (disabled for testing)
    #define VALVE_COOLANT_ENABLED       1   // Coolant valve (disabled for testing)

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
