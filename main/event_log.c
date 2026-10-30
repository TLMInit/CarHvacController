/**
 * @file event_log.c
 * @brief Fixed ring of EVENT_LOG_SIZE events behind a spinlock; no allocation.
 */

#include "event_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "EVENT";

static event_t s_ring[EVENT_LOG_SIZE];
static size_t s_head;       /* next slot to write */
static size_t s_count;
static uint32_t s_seq;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static const char *const s_class_names[EVT_CLASS_COUNT] = {
    "TIMEOUT", "NODE_RESET", "STALL", "FAULT", "CAL", "OTA", "INFO",
};

const char *event_class_name(event_class_t cls)
{
    return cls < EVT_CLASS_COUNT ? s_class_names[cls] : "?";
}

void event_log_add(event_class_t cls, const char *fmt, ...)
{
    event_t e;
    e.uptime_ms = (uint32_t)(esp_timer_get_time() / 1000);
    e.cls = cls;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e.text, sizeof(e.text), fmt, ap);
    va_end(ap);

    portENTER_CRITICAL(&s_lock);
    s_ring[s_head] = e;
    s_head = (s_head + 1) % EVENT_LOG_SIZE;
    if (s_count < EVENT_LOG_SIZE) {
        s_count++;
    }
    s_seq++;
    portEXIT_CRITICAL(&s_lock);

    ESP_LOGI(TAG, "%s: %s", event_class_name(cls), e.text);
}

bool event_log_get(size_t index, event_t *out)
{
    bool ok = false;
    portENTER_CRITICAL(&s_lock);
    if (index < s_count && out != NULL) {
        size_t slot = (s_head + EVENT_LOG_SIZE - 1 - index) % EVENT_LOG_SIZE;
        *out = s_ring[slot];
        ok = true;
    }
    portEXIT_CRITICAL(&s_lock);
    return ok;
}

void event_log_iterate(bool (*cb)(const event_t *e, void *ctx), void *ctx)
{
    event_t e;
    for (size_t i = 0; cb != NULL && event_log_get(i, &e); i++) {
        if (!cb(&e, ctx)) {
            break;
        }
    }
}

void event_log_clear(void)
{
    portENTER_CRITICAL(&s_lock);
    s_head = 0;
    s_count = 0;
    s_seq++;
    portEXIT_CRITICAL(&s_lock);
}

uint32_t event_log_seq(void)
{
    portENTER_CRITICAL(&s_lock);
    uint32_t seq = s_seq;
    portEXIT_CRITICAL(&s_lock);
    return seq;
}
