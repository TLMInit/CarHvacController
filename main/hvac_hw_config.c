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
    
    
    ESP_LOGI(TAG, "Remote IO enabled - skipping local TB6612 GPIO config");
    
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
    
    ESP_LOGI(TAG, "Remote IO mode - skipping local motor PWM configuration");
    
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
