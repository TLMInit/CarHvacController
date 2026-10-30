/**
 * @file io_hw.c
 * @brief GPIO and LEDC PWM outputs of the IO node.
 *
 * PWM channels and timers are allocated on demand: one LEDC timer per distinct
 * frequency + resolution (the carrier needs two: 20 kHz valves and 25 kHz blower,
 * both 8 bit; the A/C output GPIO10 at 20 kHz 8 bit shares the valves' timer),
 * one LEDC channel per pin (seven on the carrier; the ESP32-S3 has eight).
 * A timer no channel uses any more is free again. PWM_INIT channels are 8 bit and
 * take the duty byte as is; PWM_INIT_HZ sets 1..14 bits (io_pwm_calc.c).
 * Every timer runs from the 80 MHz APB clock (the S3's LEDC timers share one source).
 */

#include <string.h>
#include "io_hw.h"
#include "io_pwm_calc.h"
#include "node_config.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "soc/soc_caps.h"
#include "io_status_led.h"

static const char *TAG = "IO_HW";

#define PWM_MODE        LEDC_LOW_SPEED_MODE     /* the only mode on ESP32-S3 */
#define PWM_LEGACY_BITS 8                       /* PWM_INIT: one duty byte, as before 2.5 */
#define MAX_PIN         GPIO_NUM_MAX
_Static_assert(PWM_BITS_MAX <= SOC_LEDC_TIMER_BIT_WIDTH, "io_pwm_calc.h PWM_BITS_MAX above the LEDC timer width");

typedef struct {
    int pin;                /* -1 = free */
    ledc_timer_t timer;
    bool legacy;            /* set up by PWM_INIT: the duty byte goes to LEDC unscaled */
} pwm_channel_t;

typedef struct {
    uint32_t freq_hz;       /* 0 = timer free */
    uint8_t bits;
} pwm_timer_t;

static pwm_timer_t s_timers[LEDC_TIMER_MAX];
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

/* freq_hz 0 finds a free timer */
static int find_timer(uint32_t freq_hz, uint8_t bits)
{
    for (int i = 0; i < LEDC_TIMER_MAX; i++) {
        if (s_timers[i].freq_hz == freq_hz && (freq_hz == 0 || s_timers[i].bits == bits)) {
            return i;
        }
    }
    return -1;
}

/* A timer is shared only between identical frequency and resolution */
static esp_err_t get_or_create_timer(uint32_t freq_hz, uint8_t bits, ledc_timer_t *timer_out)
{
    int t = find_timer(freq_hz, bits);
    if (t < 0) {
        t = find_timer(0, 0);
        if (t < 0) {
            ESP_LOGE(TAG, "No free LEDC timer for %lu Hz %u bit", (unsigned long)freq_hz, bits);
            return ESP_ERR_NO_MEM;
        }
        ledc_timer_config_t cfg = {
            .speed_mode = PWM_MODE,
            .duty_resolution = (ledc_timer_bit_t)bits,
            .timer_num = (ledc_timer_t)t,
            .freq_hz = freq_hz,
            .clk_cfg = LEDC_USE_APB_CLK,    /* the 80 MHz io_pwm_calc.c checks against */
        };
        esp_err_t err = ledc_timer_config(&cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ledc_timer_config(%lu Hz %u bit): %s", (unsigned long)freq_hz, bits, esp_err_to_name(err));
            return err;
        }
        s_timers[t].freq_hz = freq_hz;
        s_timers[t].bits = bits;
        ESP_LOGI(TAG, "LEDC timer %d = %lu Hz %u bit", t, (unsigned long)freq_hz, bits);
    }
    *timer_out = (ledc_timer_t)t;
    return ESP_OK;
}

/* A timer no channel is bound to is free for another frequency/resolution */
static void free_unused_timers(void)
{
    for (int t = 0; t < LEDC_TIMER_MAX; t++) {
        bool used = false;
        for (int ch = 0; ch < LEDC_CHANNEL_MAX; ch++) {
            used |= s_channels[ch].pin >= 0 && s_channels[ch].timer == (ledc_timer_t)t;
        }
        if (!used) {
            s_timers[t].freq_hz = 0;
        }
    }
}

static void release_channel(int ch)
{
    if (ch < 0 || ch >= LEDC_CHANNEL_MAX || s_channels[ch].pin < 0) {
        return;
    }
    ledc_stop(PWM_MODE, (ledc_channel_t)ch, 0);
    s_channels[ch].pin = -1;
    free_unused_timers();
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

static esp_err_t pwm_attach(int pin, uint32_t freq_hz, uint8_t bits, bool legacy)
{
    io_status_led_release_pin(pin);     /* the display wins the pin over the LED */

    ledc_timer_t timer;
    esp_err_t err = get_or_create_timer(freq_hz, bits, &timer);
    if (err != ESP_OK) {
        return err;
    }

    int ch = find_channel(pin);
    if (ch >= 0) {
        if (s_channels[ch].timer != timer) {
            bool new_bits = s_timers[s_channels[ch].timer].bits != bits;
            err = ledc_bind_channel_timer(PWM_MODE, (ledc_channel_t)ch, timer);
            if (err == ESP_OK) {
                s_channels[ch].timer = timer;
            }
            free_unused_timers();
            if (err != ESP_OK) {
                return err;
            }
            if (new_bits) {
                /* old counts mean something else now (8000 at 14 bit = full on at 8 bit) */
                ledc_set_duty(PWM_MODE, (ledc_channel_t)ch, 0);
                ledc_update_duty(PWM_MODE, (ledc_channel_t)ch);
            }
        }
        s_channels[ch].legacy = legacy;
        return ESP_OK;
    }

    ch = find_free_channel();
    if (ch < 0) {
        ESP_LOGE(TAG, "No free LEDC channel for GPIO%d", pin);
        free_unused_timers();
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
        free_unused_timers();
        return err;
    }
    s_channels[ch].pin = pin;
    s_channels[ch].timer = timer;
    s_channels[ch].legacy = legacy;
    s_output_mask &= ~(1ULL << pin);       /* now owned by LEDC, not plain GPIO */
    ESP_LOGI(TAG, "PWM GPIO%d: channel %d, %lu Hz %u bit", pin, ch, (unsigned long)freq_hz, bits);
    return ESP_OK;
}

esp_err_t io_pwm_init(int pin, int freq_khz)
{
    if (!io_pin_allowed(pin) || freq_khz < 1 || freq_khz > 200) {
        return ESP_ERR_INVALID_ARG;
    }
    return pwm_attach(pin, (uint32_t)freq_khz * 1000U, PWM_LEGACY_BITS, true);
}

esp_err_t io_pwm_init_hz(int pin, uint32_t freq_hz, uint8_t bits)
{
    if (!io_pin_allowed(pin)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!pwm_hz_args_valid(freq_hz, bits)) {
        return ESP_ERR_INVALID_SIZE;    /* protocol_server maps it to ERR_INVALID_ARGS, not ERR_INVALID_PIN */
    }
    return pwm_attach(pin, freq_hz, bits, false);
}

/* The channel on pin; one at NODE_DEFAULT_PWM_KHZ (PWM_INIT style) if it has none */
static esp_err_t pwm_channel(int pin, int *ch_out)
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
    *ch_out = ch;
    return ESP_OK;
}

static esp_err_t pwm_write(int ch, uint32_t counts)
{
    esp_err_t err = ledc_set_duty(PWM_MODE, (ledc_channel_t)ch, counts);
    if (err != ESP_OK) {
        return err;
    }
    return ledc_update_duty(PWM_MODE, (ledc_channel_t)ch);
}

esp_err_t io_pwm_set(int pin, uint8_t duty)
{
    int ch;
    esp_err_t err = pwm_channel(pin, &ch);
    if (err != ESP_OK) {
        return err;
    }
    return pwm_write(ch, pwm_duty_from_8bit(duty, s_timers[s_channels[ch].timer].bits, s_channels[ch].legacy));
}

esp_err_t io_pwm_set_counts(int pin, uint16_t counts)
{
    int ch;
    esp_err_t err = pwm_channel(pin, &ch);
    if (err != ESP_OK) {
        return err;
    }
    return pwm_write(ch, pwm_duty_clamp(counts, s_timers[s_channels[ch].timer].bits));
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
    memset(s_timers, 0, sizeof(s_timers));
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
