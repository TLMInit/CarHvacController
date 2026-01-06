/**
 * @file main.c
 * @brief Automotive HVAC Control System with LVGL Interface
 * 
 * Main application entry point for ESP32-S3 Touch LCD 2 board.
 * Controls an automotive HVAC system with:
 * - TB6612FNG motor drivers for valve control
 * - BTS7960 H-bridge for blower motor
 * - SHT30-D for cabin temperature/humidity
 * - DS18B20 sensors for coolant and duct temperatures
 * - LVGL touch interface for user control
 * 
 * Hardware: ESP32-S3-Touch-LCD-2 with ST7789 display and CST816S touch
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"

// ESP LCD and touch components
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch_cst816s.h"

// LVGL
#include "lvgl.h"

// HVAC modules
#include "hvac_hw_config.h"
#include "tb6612fng_driver.h"
#include "bts7960_driver.h"
#include "ads1115_driver.h"
#include "hvac_sensors.h"
#include "hvac_controller.h"
#include "hvac_ui.h"
#include <math.h>

static const char *TAG = "HVAC_MAIN";

/*******************************************************************************
 * LVGL Configuration
 ******************************************************************************/
#define LVGL_TICK_PERIOD_MS     2
#define LVGL_TASK_STACK_SIZE    (8 * 1024)
#define LVGL_TASK_PRIORITY      5

/*******************************************************************************
 * Global Variables
 ******************************************************************************/
static esp_lcd_panel_handle_t lcd_panel = NULL;
static esp_lcd_touch_handle_t touch_handle = NULL;
static SemaphoreHandle_t lvgl_mutex = NULL;
static lv_disp_draw_buf_t disp_buf;
static lv_disp_drv_t disp_drv;
static lv_indev_drv_t indev_drv;

/*******************************************************************************
 * LVGL Display Driver Callbacks
 ******************************************************************************/
static bool lcd_flush_ready_callback(esp_lcd_panel_io_handle_t panel_io, 
                                      esp_lcd_panel_io_event_data_t *edata, 
                                      void *user_ctx)
{
    lv_disp_drv_t *disp_driver = (lv_disp_drv_t *)user_ctx;
    lv_disp_flush_ready(disp_driver);
    return false;
}

static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t)drv->user_data;
    int offsetx1 = area->x1;
    int offsetx2 = area->x2;
    int offsety1 = area->y1;
    int offsety2 = area->y2;
    
    esp_lcd_panel_draw_bitmap(panel_handle, offsetx1, offsety1, 
                               offsetx2 + 1, offsety2 + 1, color_map);
}

/*******************************************************************************
 * LVGL Touch Driver Callback
 ******************************************************************************/
static void lvgl_touch_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    esp_lcd_touch_handle_t tp = (esp_lcd_touch_handle_t)drv->user_data;
    
    esp_lcd_touch_point_data_t point_data[1];
    uint8_t touch_cnt = 0;
    
    esp_lcd_touch_read_data(tp);
    
    esp_err_t ret = esp_lcd_touch_get_data(tp, point_data, &touch_cnt, 1);
    
    if (ret == ESP_OK && touch_cnt > 0) {
        data->point.x = point_data[0].x;
        data->point.y = point_data[0].y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

/*******************************************************************************
 * LVGL Tick Timer Callback
 ******************************************************************************/
static void lvgl_tick_timer_cb(void *arg)
{
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

/*******************************************************************************
 * Hardware Initialization Functions
 ******************************************************************************/
static esp_err_t init_spi_bus(void)
{
    ESP_LOGI(TAG, "Initializing SPI bus for LCD");
    
    spi_bus_config_t buscfg = {
        .sclk_io_num = LCD_PIN_SCLK,
        .mosi_io_num = LCD_PIN_MOSI,
        .miso_io_num = LCD_PIN_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * LCD_DRAW_BUFFER_HEIGHT * sizeof(uint16_t),
    };
    
    return spi_bus_initialize(LCD_SPI_HOST, &buscfg, LCD_SPI_DMA_CHANNEL);
}

static esp_err_t init_lcd_panel(void)
{
    ESP_LOGI(TAG, "Initializing LCD panel");
    
    esp_lcd_panel_io_handle_t io_handle = NULL;
    
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = LCD_PIN_DC,
        .cs_gpio_num = LCD_PIN_CS,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = LCD_CMD_BITS,
        .lcd_param_bits = LCD_PARAM_BITS,
        .spi_mode = LCD_SPI_MODE,
        .trans_queue_depth = LCD_SPI_QUEUE_DEPTH,
        .on_color_trans_done = lcd_flush_ready_callback,
        .user_ctx = &disp_drv,
    };
    
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, 
                                              &io_config, &io_handle));
    
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = LCD_PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = LCD_BITS_PER_PIXEL,
    };
    
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_config, &lcd_panel));
    
    ESP_ERROR_CHECK(esp_lcd_panel_reset(lcd_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(lcd_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(lcd_panel, LCD_INVERT_COLOR));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(lcd_panel, LCD_MIRROR_X, LCD_MIRROR_Y));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(lcd_panel, LCD_SWAP_XY));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(lcd_panel, true));
    
    ESP_LOGI(TAG, "LCD panel initialized");
    return ESP_OK;
}

static esp_err_t init_touch_panel(i2c_master_bus_handle_t i2c_bus)
{
    ESP_LOGI(TAG, "Initializing touch panel");
    
    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    
    esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG();
    
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_bus, &tp_io_config, &tp_io_handle));
    
    esp_lcd_touch_config_t tp_cfg = {
        .x_max = TOUCH_X_MAX,
        .y_max = TOUCH_Y_MAX,
        .rst_gpio_num = TOUCH_PIN_RST,
        .int_gpio_num = TOUCH_PIN_INT,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };
    
    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_cst816s(tp_io_handle, &tp_cfg, &touch_handle));
    
    ESP_LOGI(TAG, "Touch panel initialized");
    return ESP_OK;
}

static esp_err_t init_lvgl(void)
{
    ESP_LOGI(TAG, "Initializing LVGL");
    
    lv_init();
    
    // Allocate draw buffers
    lv_color_t *buf1 = heap_caps_malloc(LCD_H_RES * LCD_DRAW_BUFFER_HEIGHT * sizeof(lv_color_t), 
                                         MALLOC_CAP_DMA);
    lv_color_t *buf2 = heap_caps_malloc(LCD_H_RES * LCD_DRAW_BUFFER_HEIGHT * sizeof(lv_color_t), 
                                         MALLOC_CAP_DMA);
    
    if (!buf1 || !buf2) {
        ESP_LOGE(TAG, "Failed to allocate LVGL draw buffers");
        return ESP_ERR_NO_MEM;
    }
    
    lv_disp_draw_buf_init(&disp_buf, buf1, buf2, LCD_H_RES * LCD_DRAW_BUFFER_HEIGHT);
    
    // Initialize display driver
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = LCD_H_RES;
    disp_drv.ver_res = LCD_V_RES;
    disp_drv.flush_cb = lvgl_flush_cb;
    disp_drv.draw_buf = &disp_buf;
    disp_drv.user_data = lcd_panel;
    lv_disp_drv_register(&disp_drv);
    
    // Initialize input device driver
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = lvgl_touch_cb;
    indev_drv.user_data = touch_handle;
    lv_indev_drv_register(&indev_drv);
    
    // Create tick timer
    const esp_timer_create_args_t lvgl_tick_timer_args = {
        .callback = &lvgl_tick_timer_cb,
        .name = "lvgl_tick"
    };
    esp_timer_handle_t lvgl_tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, LVGL_TICK_PERIOD_MS * 1000));
    
    ESP_LOGI(TAG, "LVGL initialized");
    return ESP_OK;
}

/*******************************************************************************
 * LVGL Task
 ******************************************************************************/
static void lvgl_task(void *pvParameter)
{
    ESP_LOGI(TAG, "LVGL task started");
    
    uint32_t ui_update_counter = 0;
    
    while (1) {
        if (xSemaphoreTake(lvgl_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            lv_timer_handler();
            
            // Update HVAC UI every 10 iterations (100ms if task runs at 10ms)
            if (++ui_update_counter >= 10) {
                hvac_ui_update();
                ui_update_counter = 0;
            }
            
            xSemaphoreGive(lvgl_mutex);
        }
        
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/*******************************************************************************
 * Main Application Entry Point
 ******************************************************************************/
void app_main(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  Automotive HVAC Control System");
    ESP_LOGI(TAG, "  ESP32-S3-Touch-LCD-2");
    ESP_LOGI(TAG, "========================================");
    
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    // Create LVGL mutex
    lvgl_mutex = xSemaphoreCreateMutex();
    if (lvgl_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create LVGL mutex");
        return;
    }
    
    // Initialize hardware
    ESP_LOGI(TAG, "Initializing hardware...");
    
    // GPIO initialization
    ESP_ERROR_CHECK(hvac_hw_gpio_init());
    
    // PWM initialization
    ESP_ERROR_CHECK(hvac_hw_pwm_init());
    
    // Backlight initialization
    ESP_ERROR_CHECK(hvac_hw_backlight_init());
    
    // I2C buses initialization
    i2c_master_bus_handle_t touch_i2c_bus = NULL;
    i2c_master_bus_handle_t sensor_i2c_bus = NULL;
    ESP_ERROR_CHECK(hvac_hw_i2c_init(&touch_i2c_bus, &sensor_i2c_bus));
    
    // SPI bus initialization
    ESP_ERROR_CHECK(init_spi_bus());
    
    // LCD panel initialization
    ESP_ERROR_CHECK(init_lcd_panel());
    
    // Touch panel initialization
    ESP_ERROR_CHECK(init_touch_panel(touch_i2c_bus));
    
    // LVGL initialization
    ESP_ERROR_CHECK(init_lvgl());
    
    // Initialize ADS1115 ADC for valve position monitoring (shares I2C_NUM_1)
    ESP_ERROR_CHECK(ads1115_init(sensor_i2c_bus));
    
    // Initialize motor drivers
    ESP_LOGI(TAG, "Initializing motor drivers...");
    ESP_ERROR_CHECK(tb6612fng_init());
    ESP_ERROR_CHECK(bts7960_init());
    
    // Initialize sensors
    ESP_LOGI(TAG, "Initializing sensors...");
    ESP_ERROR_CHECK(hvac_sensors_init());
    
    // Initialize HVAC controller
    ESP_LOGI(TAG, "Initializing HVAC controller...");
    ESP_ERROR_CHECK(hvac_controller_init());
    
    // Initialize HVAC UI
    ESP_LOGI(TAG, "Initializing HVAC UI...");
    if (xSemaphoreTake(lvgl_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        ESP_ERROR_CHECK(hvac_ui_init());
        xSemaphoreGive(lvgl_mutex);
    }
    
    // Start background tasks
    ESP_LOGI(TAG, "Starting background tasks...");
    ESP_ERROR_CHECK(tb6612fng_start_task());
    ESP_ERROR_CHECK(bts7960_start_task());
    ESP_ERROR_CHECK(hvac_sensors_start_task());
    ESP_ERROR_CHECK(hvac_controller_start_task());
    
    // Enable motor drivers (standby off)
    tb6612fng_enable_all();
    bts7960_enable();
    
    // Start LVGL task
    xTaskCreate(lvgl_task, "lvgl", LVGL_TASK_STACK_SIZE, NULL, LVGL_TASK_PRIORITY, NULL);
    
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  System initialization complete!");
    ESP_LOGI(TAG, "========================================");
    
    // Main loop - can be used for monitoring or additional processing
    while (1) {
        // Print sensor status periodically (for debugging)
        static uint32_t last_print = 0;
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000000);  // seconds
        
        if (now - last_print >= 10) {  // Every 10 seconds
            last_print = now;
            
            hvac_state_t state;
            hvac_controller_get_state(&state);
            
            ESP_LOGI(TAG, "Status: Mode=%s, Fan=%s, Target=%.1f°C, Cabin=%.1f°C (%.0f%%RH)",
                     hvac_controller_get_mode_name(state.mode),
                     hvac_controller_get_fan_level_name(state.fan_level),
                     state.target_temp_c,
                     state.cabin_temp_c,
                     state.cabin_humidity);
            
            if (!isnan(state.coolant_temp_c)) {
                ESP_LOGI(TAG, "Coolant: %.1f°C, Duct: %.1f°C",
                         state.coolant_temp_c, state.duct_temp_c);
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
