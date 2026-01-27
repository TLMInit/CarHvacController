/**
 * @file ads1115_driver.c
 * @brief ADS1115 16-bit ADC driver implementation
 * 
 * When USE_REMOTE_IO=1, reads are performed via UART to the IO Node.
 * When USE_REMOTE_IO=0, reads are performed via local I2C.
 */

#include "ads1115_driver.h"
#include "hvac_hw_config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <math.h>

#if USE_REMOTE_IO
#include "remote_io.h"
#endif

static const char *TAG = "ADS1115";

/**
 * @brief ADS1115 I2C Register Addresses
 */
#define ADS1115_REG_CONVERSION      0x00
#define ADS1115_REG_CONFIG          0x01
#define ADS1115_REG_LO_THRESH       0x02
#define ADS1115_REG_HI_THRESH       0x03

/**
 * @brief ADS1115 Configuration Register Bits
 */
#define ADS1115_CONFIG_MUX(ch)      ((ch & 0x07) << 12)      // Channel selection
#define ADS1115_CONFIG_GAIN(g)      ((g & 0x07) << 9)        // Programmable gain
#define ADS1115_CONFIG_MODE         (1 << 8)                 // Single-shot mode
#define ADS1115_CONFIG_DATARATE(dr) ((dr & 0x07) << 5)       // Data rate
#define ADS1115_CONFIG_OS           (1 << 15)                // Start conversion

/**
 * @brief Gain values (FS = Full Scale Range)
 */
#define ADS1115_GAIN_6p144V    0   // FS = ±6.144V
#define ADS1115_GAIN_4p096V    1   // FS = ±4.096V
#define ADS1115_GAIN_2p048V    2   // FS = ±2.048V (default)
#define ADS1115_GAIN_1p024V    3   // FS = ±1.024V
#define ADS1115_GAIN_0p512V    4   // FS = ±0.512V
#define ADS1115_GAIN_0p256V    5   // FS = ±0.256V

/**
 * @brief Data rate values (samples per second)
 */
#define ADS1115_DATARATE_8SPS   0
#define ADS1115_DATARATE_16SPS  1
#define ADS1115_DATARATE_32SPS  2
#define ADS1115_DATARATE_64SPS  3
#define ADS1115_DATARATE_128SPS 4   // Default
#define ADS1115_DATARATE_250SPS 5
#define ADS1115_DATARATE_475SPS 6
#define ADS1115_DATARATE_860SPS 7

/**
 * @brief Driver state structure
 */
typedef struct {
#if !USE_REMOTE_IO
    i2c_master_dev_handle_t ads1115_1;  // Device at address 0x48
    i2c_master_dev_handle_t ads1115_2;  // Device at address 0x49
#endif
    SemaphoreHandle_t mutex;
    bool initialized;
} ads1115_driver_t;

static ads1115_driver_t driver = {
#if !USE_REMOTE_IO
    .ads1115_1 = NULL,
    .ads1115_2 = NULL,
#endif
    .mutex = NULL,
    .initialized = false
};

#if !USE_REMOTE_IO
/**
 * @brief Get the correct I2C device handle based on channel
 */
static i2c_master_dev_handle_t ads1115_get_device(ads1115_channel_t channel)
{
    // Channels 0-3 are on device 1 (0x48)
    // Channels 4-7 are on device 2 (0x49)
    return (channel < 4) ? driver.ads1115_1 : driver.ads1115_2;
}

/**
 * @brief Get the physical channel on the device (0-3)
 */
static uint8_t ads1115_get_physical_channel(ads1115_channel_t channel)
{
    return channel & 0x03;  // Only lower 2 bits for 4 channels per device
}

/**
 * @brief Write configuration to ADS1115
 */
static esp_err_t ads1115_write_config(i2c_master_dev_handle_t device, uint16_t config)
{
    uint8_t data[3];
    data[0] = ADS1115_REG_CONFIG;
    data[1] = (config >> 8) & 0xFF;
    data[2] = config & 0xFF;
    
    return i2c_master_transmit(device, data, 3, -1);
}

/**
 * @brief Read conversion result from ADS1115
 */
static esp_err_t ads1115_read_conversion(i2c_master_dev_handle_t device, int16_t *value)
{
    uint8_t reg = ADS1115_REG_CONVERSION;
    uint8_t data[2];
    
    esp_err_t ret = i2c_master_transmit_receive(device, &reg, 1, data, 2, -1);
    if (ret != ESP_OK) {
        return ret;
    }
    
    // Combine bytes into 16-bit signed value
    *value = (int16_t)((data[0] << 8) | data[1]);
    return ESP_OK;
}

/**
 * @brief Perform a single conversion and read the result
 */
static esp_err_t ads1115_read_channel(i2c_master_dev_handle_t device, 
                                      uint8_t physical_channel, 
                                      int16_t *value)
{
    if (!device || !value) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Configure for single-shot conversion
    // Gain = 4.096V (allows full 0-3.3V range without clipping)
    // Data rate = 128 SPS (balanced speed/accuracy)
    uint16_t config = 0x8000;  // OS bit set
    config |= ADS1115_CONFIG_MUX(physical_channel);
    config |= ADS1115_CONFIG_GAIN(ADS1115_GAIN_4p096V);
    config |= ADS1115_CONFIG_MODE;
    config |= ADS1115_CONFIG_DATARATE(ADS1115_DATARATE_128SPS);
    
    esp_err_t ret = ads1115_write_config(device, config);
    if (ret != ESP_OK) {
        return ret;
    }
    
    // Wait for conversion to complete (~8ms at 128 SPS)
    vTaskDelay(pdMS_TO_TICKS(10));
    
    // Read the result
    return ads1115_read_conversion(device, value);
}
#endif // !USE_REMOTE_IO

esp_err_t ads1115_init(i2c_master_bus_handle_t i2c_bus)
{
    if (driver.initialized) {
        return ESP_OK;  // Already initialized
    }
    
#if USE_REMOTE_IO
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
#else
    if (!i2c_bus) {
        ESP_LOGE(TAG, "Invalid I2C bus handle");
        return ESP_ERR_INVALID_ARG;
    }
    
    // Create I2C device handles
    i2c_device_config_t dev_cfg = {
        .scl_speed_hz = ADS1115_I2C_SPEED_HZ,
    };
    
    // Add ADS1115 #1 (0x48)
    dev_cfg.device_address = ADS1115_ADDR_1;
    esp_err_t ret = i2c_master_bus_add_device(i2c_bus, &dev_cfg, &driver.ads1115_1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add ADS1115 #1 to I2C bus: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Add ADS1115 #2 (0x49)
    dev_cfg.device_address = ADS1115_ADDR_2;
    ret = i2c_master_bus_add_device(i2c_bus, &dev_cfg, &driver.ads1115_2);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add ADS1115 #2 to I2C bus: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Create mutex for thread safety
    driver.mutex = xSemaphoreCreateMutex();
    if (!driver.mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }
    
    driver.initialized = true;
    ESP_LOGI(TAG, "ADS1115 driver initialized successfully");
    
    return ESP_OK;
#endif // USE_REMOTE_IO
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
    
#if USE_REMOTE_IO
    // Read via remote IO Node
    // Channels 0-3 use ADS1115_ADDR_1, channels 4-7 use ADS1115_ADDR_2
    uint8_t i2c_addr = (channel < 4) ? ADS1115_ADDR_1 : ADS1115_ADDR_2;
    uint8_t physical_ch = channel & 0x03;
    
    esp_err_t ret = remote_ads1115_read(0, i2c_addr, physical_ch, value);
#else
    i2c_master_dev_handle_t device = ads1115_get_device(channel);
    uint8_t physical_ch = ads1115_get_physical_channel(channel);
    
    esp_err_t ret = ads1115_read_channel(device, physical_ch, value);
#endif
    
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

esp_err_t ads1115_deinit(void)
{
    if (!driver.initialized) {
        return ESP_OK;
    }
    
#if !USE_REMOTE_IO
    if (driver.ads1115_1) {
        i2c_master_bus_rm_device(driver.ads1115_1);
        driver.ads1115_1 = NULL;
    }
    
    if (driver.ads1115_2) {
        i2c_master_bus_rm_device(driver.ads1115_2);
        driver.ads1115_2 = NULL;
    }
#endif
    
    if (driver.mutex) {
        vSemaphoreDelete(driver.mutex);
        driver.mutex = NULL;
    }
    
    driver.initialized = false;
    ESP_LOGI(TAG, "ADS1115 driver deinitialized");
    
    return ESP_OK;
}
