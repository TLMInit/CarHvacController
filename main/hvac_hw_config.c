/**
 * @file hvac_hw_config.c
 * @brief Hardware initialization for Automotive HVAC Control System
 */

#include "hvac_hw_config.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/i2c_master.h"

static const char *TAG = "HVAC_HW";

esp_err_t hvac_hw_gpio_init(void)
{
    ESP_LOGI(TAG, "Initializing GPIO pins");
    
    gpio_config_t io_conf = {
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    
    // TB6612FNG #1 GPIO configuration (STBY hardwired to 3V3)
    io_conf.pin_bit_mask = (1ULL << TB6612_1_AIN1) |
                           (1ULL << TB6612_1_AIN2) |
                           (1ULL << TB6612_1_BIN1) |
                           (1ULL << TB6612_1_BIN2);
    ESP_ERROR_CHECK(gpio_config(&io_conf));
    
    // TB6612FNG #2 GPIO configuration (STBY hardwired to 3V3)
    io_conf.pin_bit_mask = (1ULL << TB6612_2_AIN1) |
                           (1ULL << TB6612_2_AIN2) |
                           (1ULL << TB6612_2_BIN1) |
                           (1ULL << TB6612_2_BIN2);
    ESP_ERROR_CHECK(gpio_config(&io_conf));
    
    // TB6612FNG #3 GPIO configuration (STBY hardwired to 3V3)
    // Only configure coolant valve (Motor A), spare motor (Motor B) is NC
    io_conf.pin_bit_mask = (1ULL << TB6612_3_AIN1) |
                           (1ULL << TB6612_3_AIN2);
    ESP_ERROR_CHECK(gpio_config(&io_conf));
    // Note: TB6612_3_BIN1, BIN2 (spare motor) not configured - NC for now
    
    // BTS7960: R_EN hardwired to 3V3, L_EN hardwired to GND
    // No GPIO control needed for enable pins
    
    // Note: All TB6612FNG STBY pins are hardwired to 3V3 (always enabled)
    // Note: BTS7960 R_EN is hardwired to 3V3, L_EN to GND
    
    ESP_LOGI(TAG, "GPIO initialization complete");
    return ESP_OK;
}

esp_err_t hvac_hw_i2c_init(i2c_master_bus_handle_t *touch_i2c_bus, 
                           i2c_master_bus_handle_t *sensor_i2c_bus)
{
    ESP_LOGI(TAG, "Initializing I2C buses");
    
    // Touch panel I2C bus configuration (I2C_NUM_0)
    i2c_master_bus_config_t touch_bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = TOUCH_I2C_PORT,
        .scl_io_num = TOUCH_PIN_SCL,
        .sda_io_num = TOUCH_PIN_SDA,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&touch_bus_config, touch_i2c_bus));
    
    // Sensor I2C bus configuration (I2C_NUM_1)
    // Shared by SHT30 sensor and ADS1115 ADC modules
    i2c_master_bus_config_t sensor_bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = SHT30_I2C_PORT,
        .scl_io_num = SHT30_PIN_SCL,
        .sda_io_num = SHT30_PIN_SDA,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&sensor_bus_config, sensor_i2c_bus));
    
    ESP_LOGI(TAG, "I2C buses initialized (I2C_NUM_1 shared by SHT30 and ADS1115)");
    return ESP_OK;
}

esp_err_t hvac_hw_pwm_init(void)
{
    ESP_LOGI(TAG, "Initializing PWM timers and channels");
    
    // TB6612FNG PWM Timer configuration
    ledc_timer_config_t tb6612_timer = {
        .speed_mode = TB6612_LEDC_MODE,
        .timer_num = TB6612_LEDC_TIMER,
        .duty_resolution = TB6612_PWM_RESOLUTION,
        .freq_hz = TB6612_PWM_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&tb6612_timer));
    
    // BTS7960 PWM Timer configuration
    ledc_timer_config_t bts7960_timer = {
        .speed_mode = BTS7960_LEDC_MODE,
        .timer_num = BTS7960_LEDC_TIMER,
        .duty_resolution = BTS7960_PWM_RESOLUTION,
        .freq_hz = BTS7960_PWM_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&bts7960_timer));
    
    // TB6612FNG #1 Motor A PWM channel
    ledc_channel_config_t tb6612_1_pwma = {
        .speed_mode = TB6612_LEDC_MODE,
        .channel = TB6612_1_PWMA_CHANNEL,
        .timer_sel = TB6612_LEDC_TIMER,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = TB6612_1_PWMA,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&tb6612_1_pwma));
    
    // TB6612FNG #1 Motor B PWM channel
    ledc_channel_config_t tb6612_1_pwmb = {
        .speed_mode = TB6612_LEDC_MODE,
        .channel = TB6612_1_PWMB_CHANNEL,
        .timer_sel = TB6612_LEDC_TIMER,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = TB6612_1_PWMB,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&tb6612_1_pwmb));
    
    // TB6612FNG #2 Motor A PWM channel (Foot valve)
    ledc_channel_config_t tb6612_2_pwma = {
        .speed_mode = TB6612_LEDC_MODE,
        .channel = TB6612_2_PWMA_CHANNEL,
        .timer_sel = TB6612_LEDC_TIMER,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = TB6612_2_PWMA,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&tb6612_2_pwma));
    
    // TB6612FNG #2 Motor B PWM channel (Windscreen valve)
    ledc_channel_config_t tb6612_2_pwmb = {
        .speed_mode = TB6612_LEDC_MODE,
        .channel = TB6612_2_PWMB_CHANNEL,
        .timer_sel = TB6612_LEDC_TIMER,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = TB6612_2_PWMB,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&tb6612_2_pwmb));
    
    // TB6612FNG #3 Motor A PWM channel (Coolant valve)
    ledc_channel_config_t tb6612_3_pwma = {
        .speed_mode = TB6612_LEDC_MODE,
        .channel = TB6612_3_PWMA_CHANNEL,
        .timer_sel = TB6612_LEDC_TIMER,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = TB6612_3_PWMA,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&tb6612_3_pwma));
    
    // BTS7960 RPWM channel (L_PWM not connected)
    ledc_channel_config_t bts7960_rpwm = {
        .speed_mode = BTS7960_LEDC_MODE,
        .channel = BTS7960_RPWM_CHANNEL,
        .timer_sel = BTS7960_LEDC_TIMER,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = BTS7960_RPWM,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&bts7960_rpwm));
    
    ESP_LOGI(TAG, "PWM initialization complete");
    return ESP_OK;
}

esp_err_t hvac_hw_backlight_init(void)
{
    ESP_LOGI(TAG, "Initializing LCD backlight PWM");
    
    // First set GPIO direction to ensure backlight is controlled
    gpio_set_direction(LCD_PIN_BACKLIGHT, GPIO_MODE_OUTPUT);
    gpio_set_level(LCD_PIN_BACKLIGHT, 0);  // Start with backlight off
    
    // Backlight PWM Timer configuration
    ledc_timer_config_t bl_timer = {
        .speed_mode = BACKLIGHT_LEDC_MODE,
        .timer_num = BACKLIGHT_LEDC_TIMER,
        .duty_resolution = BACKLIGHT_DUTY_RES,
        .freq_hz = BACKLIGHT_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&bl_timer));
    
    // Backlight PWM channel configuration - start with 0% duty (off)
    ledc_channel_config_t bl_channel = {
        .speed_mode = BACKLIGHT_LEDC_MODE,
        .channel = BACKLIGHT_LEDC_CHANNEL,
        .timer_sel = BACKLIGHT_LEDC_TIMER,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = LCD_PIN_BACKLIGHT,
        .duty = 0,  // Start off, will be turned on after display init
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&bl_channel));
    
    ESP_LOGI(TAG, "Backlight initialized (off, will turn on after display ready)");
    return ESP_OK;
}

void hvac_hw_backlight_set(uint8_t brightness_percent)
{
    if (brightness_percent > 100) {
        brightness_percent = 100;
    }
    
    uint32_t duty = (BACKLIGHT_MAX_DUTY * brightness_percent) / 100;
    ledc_set_duty(BACKLIGHT_LEDC_MODE, BACKLIGHT_LEDC_CHANNEL, duty);
    ledc_update_duty(BACKLIGHT_LEDC_MODE, BACKLIGHT_LEDC_CHANNEL);
    
    ESP_LOGD(TAG, "Backlight set to %d%% (duty: %lu)", brightness_percent, duty);
}
