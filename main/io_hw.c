/**
 * @file io_hw.c
 * @brief GPIO and LEDC PWM outputs of the IO node.
 *
 * PWM channels and timers are allocated on demand: one LEDC timer per
 * distinct frequency (the carrier needs two: 20 kHz valves, 25 kHz blower),
 * one LEDC channel per pin (six on the carrier; the ESP32-S3 has eight).
 * Resolution is always 8 bits because the link carries one duty byte.
 */

#include <string.h>
#include "io_hw.h"
#include "node_config.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "io_status_led.h"

static const char *TAG = "IO_HW";

#define PWM_MODE        LEDC_LOW_SPEED_MODE     /* the only mode on ESP32-S3 */
#define PWM_RESOLUTION  LEDC_TIMER_8_BIT
#define MAX_PIN         GPIO_NUM_MAX

typedef struct {
    int pin;                /* -1 = free */
    ledc_timer_t timer;
} pwm_channel_t;

static uint32_t s_timer_freq_hz[LEDC_TIMER_MAX];     /* 0 = timer free */
static pwm_channel_t s_channels[LEDC_CHANNEL_MAX];
static uint64_t s_output_mask;                       /* pins configured as plain outputs */

/* ------------------------------------------------------------------------- */

bool io_pin_allowed(int pin)
{
    static const int allowed[] = NODE_ALLOWED_PINS;
    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
        if (allowed[i] == pin) {
            return true;
        }
    }
    return false;
}

static int find_channel(int pin)
{
    for (int i = 0; i < LEDC_CHANNEL_MAX; i++) {
        if (s_channels[i].pin == pin) {
            return i;
        }
    }
    return -1;
}

static int find_free_channel(void)
{
    return find_channel(-1);
}

static int find_timer(uint32_t freq_hz)
{
    for (int i = 0; i < LEDC_TIMER_MAX; i++) {
        if (s_timer_freq_hz[i] == freq_hz) {
            return i;
        }
    }
    return -1;
}

static esp_err_t get_or_create_timer(uint32_t freq_hz, ledc_timer_t *timer_out)
{
    int t = find_timer(freq_hz);
    if (t < 0) {
        t = find_timer(0);
        if (t < 0) {
            ESP_LOGE(TAG, "No free LEDC timer for %lu Hz", (unsigned long)freq_hz);
            return ESP_ERR_NO_MEM;
        }
        ledc_timer_config_t cfg = {
            .speed_mode = PWM_MODE,
            .duty_resolution = PWM_RESOLUTION,
            .timer_num = (ledc_timer_t)t,
            .freq_hz = freq_hz,
            .clk_cfg = LEDC_AUTO_CLK,
        };
        esp_err_t err = ledc_timer_config(&cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ledc_timer_config(%lu Hz): %s", (unsigned long)freq_hz, esp_err_to_name(err));
            return err;
        }
        s_timer_freq_hz[t] = freq_hz;
        ESP_LOGI(TAG, "LEDC timer %d = %lu Hz", t, (unsigned long)freq_hz);
    }
    *timer_out = (ledc_timer_t)t;
    return ESP_OK;
}

static void release_channel(int ch)
{
    if (ch < 0 || ch >= LEDC_CHANNEL_MAX || s_channels[ch].pin < 0) {
        return;
    }
    ledc_stop(PWM_MODE, (ledc_channel_t)ch, 0);
    s_channels[ch].pin = -1;
}

/* ------------------------------------------------------------------------- */

esp_err_t io_gpio_mode(int pin, uint8_t mode)
{
    if (!io_pin_allowed(pin)) {
        return ESP_ERR_INVALID_ARG;
    }
    io_status_led_release_pin(pin);     /* the display wins the pin over the LED */
    release_channel(find_channel(pin));

    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << pin,
        .intr_type = GPIO_INTR_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    bool is_output = false;
    switch (mode) {
    case 0: cfg.mode = GPIO_MODE_INPUT; break;
    case 1: cfg.mode = GPIO_MODE_OUTPUT; is_output = true; break;
    case 2: cfg.mode = GPIO_MODE_INPUT; cfg.pull_up_en = GPIO_PULLUP_ENABLE; break;
    case 3: cfg.mode = GPIO_MODE_INPUT; cfg.pull_down_en = GPIO_PULLDOWN_ENABLE; break;
    case 4: cfg.mode = GPIO_MODE_OUTPUT_OD; is_output = true; break;
    default: return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = gpio_config(&cfg);
    if (err == ESP_OK) {
        if (is_output) {
            s_output_mask |= 1ULL << pin;
        } else {
            s_output_mask &= ~(1ULL << pin);
        }
    }
    return err;
}

esp_err_t io_gpio_set(int pin, int level)
{
    if (!io_pin_allowed(pin)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!(s_output_mask & (1ULL << pin))) {
        /* The display never sends GPIO_MODE for the valve direction pins. */
        esp_err_t err = io_gpio_mode(pin, 1);
        if (err != ESP_OK) {
            return err;
        }
    }
    return gpio_set_level((gpio_num_t)pin, level ? 1 : 0);
}

esp_err_t io_gpio_get(int pin, int *level)
{
    if (!io_pin_allowed(pin) || level == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *level = gpio_get_level((gpio_num_t)pin);
    return ESP_OK;
}

/* ------------------------------------------------------------------------- */

esp_err_t io_pwm_init(int pin, int freq_khz)
{
    if (!io_pin_allowed(pin) || freq_khz < 1 || freq_khz > 200) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t freq_hz = (uint32_t)freq_khz * 1000U;
    io_status_led_release_pin(pin);     /* the display wins the pin over the LED */

    ledc_timer_t timer;
    esp_err_t err = get_or_create_timer(freq_hz, &timer);
    if (err != ESP_OK) {
        return err;
    }

    int ch = find_channel(pin);
    if (ch >= 0) {
        if (s_channels[ch].timer != timer) {
            err = ledc_bind_channel_timer(PWM_MODE, (ledc_channel_t)ch, timer);
            if (err != ESP_OK) {
                return err;
            }
            s_channels[ch].timer = timer;
        }
        return ESP_OK;
    }

    ch = find_free_channel();
    if (ch < 0) {
        ESP_LOGE(TAG, "No free LEDC channel for GPIO%d", pin);
        return ESP_ERR_NO_MEM;
    }
    ledc_channel_config_t cfg = {
        .gpio_num = pin,
        .speed_mode = PWM_MODE,
        .channel = (ledc_channel_t)ch,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = timer,
        .duty = 0,
        .hpoint = 0,
    };
    err = ledc_channel_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ledc_channel_config(GPIO%d): %s", pin, esp_err_to_name(err));
        return err;
    }
    s_channels[ch].pin = pin;
    s_channels[ch].timer = timer;
    s_output_mask &= ~(1ULL << pin);       /* now owned by LEDC, not plain GPIO */
    ESP_LOGI(TAG, "PWM GPIO%d: channel %d, %d kHz", pin, ch, freq_khz);
    return ESP_OK;
}

esp_err_t io_pwm_set(int pin, uint8_t duty)
{
    if (!io_pin_allowed(pin)) {
        return ESP_ERR_INVALID_ARG;
    }
    int ch = find_channel(pin);
    if (ch < 0) {
        esp_err_t err = io_pwm_init(pin, NODE_DEFAULT_PWM_KHZ);
        if (err != ESP_OK) {
            return err;
        }
        ch = find_channel(pin);
    }
    esp_err_t err = ledc_set_duty(PWM_MODE, (ledc_channel_t)ch, duty);
    if (err != ESP_OK) {
        return err;
    }
    return ledc_update_duty(PWM_MODE, (ledc_channel_t)ch);
}

/* ------------------------------------------------------------------------- */

void io_outputs_safe_state(void)
{
    for (int ch = 0; ch < LEDC_CHANNEL_MAX; ch++) {
        if (s_channels[ch].pin >= 0) {
            ledc_set_duty(PWM_MODE, (ledc_channel_t)ch, 0);
            ledc_update_duty(PWM_MODE, (ledc_channel_t)ch);
        }
    }
    for (int pin = 0; pin < MAX_PIN; pin++) {
        if (s_output_mask & (1ULL << pin)) {
            gpio_set_level((gpio_num_t)pin, 0);
        }
    }
}

esp_err_t io_hw_init(void)
{
    memset(s_timer_freq_hz, 0, sizeof(s_timer_freq_hz));
    for (int i = 0; i < LEDC_CHANNEL_MAX; i++) {
        s_channels[i].pin = -1;
    }
    s_output_mask = 0;

    static const int outputs[] = NODE_BOOT_OUTPUT_PINS;
    for (size_t i = 0; i < sizeof(outputs) / sizeof(outputs[0]); i++) {
        if (outputs[i] == NODE_STATUS_LED_GPIO) {
            continue;       /* stays with the LED until the display claims it */
        }
        esp_err_t err = io_gpio_mode(outputs[i], 1);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Boot output GPIO%d: %s", outputs[i], esp_err_to_name(err));
            return err;
        }
        gpio_set_level((gpio_num_t)outputs[i], 0);
    }

    static const int pwm[][2] = NODE_BOOT_PWM_PINS;
    for (size_t i = 0; i < sizeof(pwm) / sizeof(pwm[0]); i++) {
        if (pwm[i][0] == NODE_STATUS_LED_GPIO) {
            continue;       /* stays with the LED until the display claims it */
        }
        esp_err_t err = io_pwm_init(pwm[i][0], pwm[i][1]);
        if (err != ESP_OK) {
            return err;
        }
        io_pwm_set(pwm[i][0], 0);
    }

    ESP_LOGI(TAG, "Outputs pre-configured: %u GPIO low, %u PWM at duty 0",
             (unsigned)(sizeof(outputs) / sizeof(outputs[0])),
             (unsigned)(sizeof(pwm) / sizeof(pwm[0])));
    return ESP_OK;
}
