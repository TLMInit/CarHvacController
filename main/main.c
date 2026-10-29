/**
 * @file main.c
 * @brief HVAC IO Node - the carrier-board half of TLMInit/CarHvacController.
 *
 * The display board (Waveshare ESP32-S3-Touch-LCD-2) runs the HVAC logic and
 * drives this node over UART. The node owns the hardware on the carrier PCB:
 *   - 3x TB6612FNG (five valve actuators)        GPIO 4/16/17, 5/18/15, 38/39/40, 41/42/47, 12/14/13
 *   - BTS7960 blower H-bridge (RPWM only)        GPIO 48
 *   - A/C compressor clutch relay                GPIO 6
 *   - 2x ADS1115 + BME280 on I2C                 GPIO 8 (SDA) / 9 (SCL)
 *   - DS18B20 duct sensor on 1-Wire              GPIO 7
 *   - UART link to the display, 921600 baud      GPIO 2 (TX) / 21 (RX)
 *   - WS2812 status LED (optional)              NODE_STATUS_LED_GPIO, yields its pin to the display
 *
 * Bring-up order matters: outputs first (everything off), then the buses,
 * then the link, so the display's first PING finds a node that is ready.
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_app_desc.h"
#include "node_config.h"
#include "io_hw.h"
#include "io_i2c.h"
#include "io_onewire.h"
#include "io_status_led.h"
#include "protocol_server.h"

static const char *TAG = "IONODE";

void app_main(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, " HVAC IO Node  %s (%s)", NODE_VERSION_STRING, esp_app_get_description()->version);
    ESP_LOGI(TAG, "========================================");

    if (io_status_led_init() != ESP_OK) {
        ESP_LOGW(TAG, "Status LED unavailable; continuing without it");
    }
    ESP_ERROR_CHECK(io_hw_init());
    ESP_ERROR_CHECK(io_i2c_init());

    if (io_onewire_init() != ESP_OK) {
        ESP_LOGW(TAG, "1-Wire unavailable; DS18B20 commands will return errors");
    }

    ESP_ERROR_CHECK(protocol_server_start());
    ESP_LOGI(TAG, "Ready, waiting for the display");

    protocol_stats_t st;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        protocol_server_get_stats(&st);
        ESP_LOGI(TAG, "link %s | rx %lu tx %lu bad %lu err %lu wdt %lu",
                 st.link_up ? "up" : "down",
                 (unsigned long)st.rx_frames, (unsigned long)st.tx_frames,
                 (unsigned long)st.bad_frames, (unsigned long)st.errors_sent,
                 (unsigned long)st.watchdog_trips);
    }
}
