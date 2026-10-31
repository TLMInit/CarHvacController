/**
 * @file io_status_led.c
 * @brief WS2812 status LED driven from the link statistics.
 *
 * The LED task never touches protocol_server internals: it polls
 * protocol_server_get_stats() every 100 ms and derives the pattern from the
 * counters, so it needs no hooks in the link code.
 */

#include <string.h>
#include "io_status_led.h"
#include "node_config.h"
#include "protocol_server.h"
#include "led_strip.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "IO_STATUS_LED";

#define LED_TICK_MS             100
#define LED_ERROR_FLASH_TICKS   3           /* 300 ms */
#define LED_TASK_STACK          2560
#define LED_TASK_PRIORITY       2

static led_strip_handle_t s_strip;
static SemaphoreHandle_t s_lock;
static volatile bool s_released;
static volatile bool s_task_running;

#if NODE_STATUS_LED_GPIO >= 0     /* only io_status_led_init() starts the task */
static void led_show(uint8_t r, uint8_t g, uint8_t b)
{
    if (s_lock == NULL || xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;
    }
    if (s_strip != NULL && !s_released) {
        led_strip_set_pixel(s_strip, 0, r, g, b);
        led_strip_refresh(s_strip);
    }
    xSemaphoreGive(s_lock);
}

static void led_task(void *arg)
{
    protocol_stats_t prev;
    protocol_stats_t st;
    uint32_t tick = 0;
    uint32_t flash_until = 0;
    const uint8_t v = NODE_STATUS_LED_BRIGHTNESS;

    memset(&prev, 0, sizeof(prev));
    s_task_running = true;

    while (!s_released) {
        protocol_server_get_stats(&st);
        if (st.errors_sent != prev.errors_sent || st.bad_frames != prev.bad_frames) {
            flash_until = tick + LED_ERROR_FLASH_TICKS;
        }
        prev = st;

        bool slow = (tick / 5) & 1;         /* 1 Hz */
        bool fast = (tick / 2) & 1;         /* 2.5 Hz */

        if (tick < flash_until) {
            led_show(v, v / 2, 0);                              /* amber */
        } else if (st.link_up) {
            led_show(0, v, 0);                                  /* green */
        } else if (st.rx_frames == 0) {
            led_show(0, 0, slow ? v : 0);                       /* blue blink: waiting */
        } else {
            led_show(fast ? v : 0, 0, 0);                       /* red blink: link lost */
        }
        tick++;
        vTaskDelay(pdMS_TO_TICKS(LED_TICK_MS));
    }

    s_task_running = false;
    vTaskDelete(NULL);
}
#endif

esp_err_t io_status_led_init(void)
{
#if NODE_STATUS_LED_GPIO < 0
    ESP_LOGI(TAG, "Status LED disabled (NODE_STATUS_LED_GPIO = -1)");
    return ESP_OK;
#else
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    led_strip_config_t strip_cfg = {
        .strip_gpio_num = NODE_STATUS_LED_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags.invert_out = false,
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 0,
        .flags.with_dma = false,
    };
    esp_err_t err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init LED strip on GPIO%d: %s", NODE_STATUS_LED_GPIO, esp_err_to_name(err));
        s_strip = NULL;
        return err;
    }
    s_released = false;
    led_show(NODE_STATUS_LED_BRIGHTNESS, NODE_STATUS_LED_BRIGHTNESS, NODE_STATUS_LED_BRIGHTNESS); /* boot: white */

    if (xTaskCreate(led_task, "status_led", LED_TASK_STACK, NULL, LED_TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create status LED task");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Status LED initialized on GPIO%d", NODE_STATUS_LED_GPIO);
    return ESP_OK;
#endif
}

bool io_status_led_owns_pin(int pin)
{
    return NODE_STATUS_LED_GPIO >= 0 && pin == NODE_STATUS_LED_GPIO && s_strip != NULL && !s_released;
}

bool io_status_led_release_pin(int pin)
{
    if (!io_status_led_owns_pin(pin)) {
        return false;
    }
    s_released = true;

    /* Let a running refresh finish, then take the strip down. */
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, pdMS_TO_TICKS(100));
    }
    led_strip_clear(s_strip);
    led_strip_del(s_strip);
    s_strip = NULL;
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }

    /* Do not wait for the task here: this runs inside the link task while the
     * display waits for a reply. The task sees s_released within one tick and
     * deletes itself; led_show() already refuses to touch a deleted strip. */
    ESP_LOGI(TAG, "Status LED released: GPIO%d claimed by the display", pin);
    return true;
}
