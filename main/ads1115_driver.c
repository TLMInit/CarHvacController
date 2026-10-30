/**
 * @file ads1115_driver.c
 * @brief ADS1115 16-bit ADC driver implementation
 * 
 * Reads are performed via UART to the IO Node.
 */

#include "ads1115_driver.h"
#include "hvac_hw_config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <math.h>

#include "remote_io.h"

static const char *TAG = "ADS1115";

/**
 * @brief Driver state structure
 */
typedef struct {
    SemaphoreHandle_t mutex;
    bool initialized;
} ads1115_driver_t;

static ads1115_driver_t driver = {
    .mutex = NULL,
    .initialized = false
};

esp_err_t ads1115_init(i2c_master_bus_handle_t i2c_bus)
{
    if (driver.initialized) {
        return ESP_OK;  // Already initialized
    }
    
    // Remote IO mode - no local I2C setup needed
    // IO Node handles ADS1115 directly
    (void)i2c_bus;  // Unused in remote mode
    
    // Create mutex for thread safety
    driver.mutex = xSemaphoreCreateMutex();
    if (!driver.mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }
    
    driver.initialized = true;
    ESP_LOGI(TAG, "ADS1115 driver initialized (remote IO mode)");
    return ESP_OK;
}

esp_err_t ads1115_read_raw(ads1115_channel_t channel, ads1115_raw_t *value)
{
    if (!driver.initialized || channel >= ADS1115_CH_MAX || !value) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (xSemaphoreTake(driver.mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire mutex");
        return ESP_FAIL;
    }
    
    // Read via remote IO Node
    // Channels 0-3 use ADS1115_ADDR_1, channels 4-7 use ADS1115_ADDR_2
    uint8_t i2c_addr = (channel < 4) ? ADS1115_ADDR_1 : ADS1115_ADDR_2;
    uint8_t physical_ch = channel & 0x03;
    
    esp_err_t ret = remote_ads1115_read(0, i2c_addr, physical_ch, value);
    
    xSemaphoreGive(driver.mutex);
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read channel %d: %s", channel, esp_err_to_name(ret));
    }
    
    return ret;
}

esp_err_t ads1115_read_mv(ads1115_channel_t channel, ads1115_mv_t *voltage)
{
    ads1115_raw_t raw;
    esp_err_t ret = ads1115_read_raw(channel, &raw);
    
    if (ret != ESP_OK) {
        return ret;
    }
    
    // Convert to voltage in mV
    // ADS1115 with 4.096V gain gives ±4.096V full scale range
    // Single-ended input (0V-3.3V) appears as 0 to ~26214 raw (3.3/4.096 * 32767)
    // Conversion: (raw / 32767) * 4096 mV for positive readings
    
    int32_t mv;
    if (raw < 0) {
        // Negative readings shouldn't happen with single-ended, treat as 0
        mv = 0;
    } else {
        // Positive: scale raw value to mV using 4.096V full scale
        // raw 32767 = 4096mV, so: mv = raw * 4096 / 32767
        mv = ((int32_t)raw * 4096) / 32767;
    }
    
    if (mv < 0) mv = 0;
    if (mv > 3300) mv = 3300;
    
    *voltage = (ads1115_mv_t)mv;
    return ESP_OK;
}

valve_position_percent_t ads1115_voltage_to_position(ads1115_mv_t voltage)
{
    // Ignore tiny noise on floating channels
    if (voltage < 50) {
        return 0;
    }

    // Linear conversion: 0 mV = 0%, 3300 mV = 100%
    uint32_t percent = (voltage * 100) / 3300;
    
    if (percent > 100) {
        percent = 100;
    }
    
    return (valve_position_percent_t)percent;
}

esp_err_t ads1115_read_valve_position(ads1115_channel_t channel, 
                                      valve_position_percent_t *position)
{
    ads1115_mv_t voltage;
    esp_err_t ret = ads1115_read_mv(channel, &voltage);
    
    if (ret != ESP_OK) {
        return ret;
    }
    
    *position = ads1115_voltage_to_position(voltage);
    return ESP_OK;
}
