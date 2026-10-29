/**
 * @file event_log.h
 * @brief Ring of the last 50 notable events, for the diagnostics page and /api/log.
 *
 * INTERFACE and implementation (lead, event_log.c). Callable from any task,
 * not from ISRs. No allocation; adding is O(1) and also prints one console line.
 */

#ifndef EVENT_LOG_H
#define EVENT_LOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EVENT_LOG_SIZE      50
#define EVENT_TEXT_LEN      48

typedef enum {
    EVT_TIMEOUT = 0,    /* link command timeouts */
    EVT_NODE_RESET,     /* IO node restarted */
    EVT_STALL,          /* valve stall / lost feedback */
    EVT_FAULT,          /* sensor or other faults */
    EVT_CAL,            /* calibration results */
    EVT_OTA,            /* firmware updates */
    EVT_INFO,           /* everything else worth keeping */
    EVT_CLASS_COUNT
} event_class_t;

typedef struct {
    uint32_t uptime_ms;
    event_class_t cls;
    char text[EVENT_TEXT_LEN];
} event_t;

void event_log_add(event_class_t cls, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/** Number of stored events (0..EVENT_LOG_SIZE). */
size_t event_log_count(void);

/** Copy event `index` (0 = newest). Returns false past the end. */
bool event_log_get(size_t index, event_t *out);

/** Visit events newest first; stop early by returning false. The callback runs with a copy, outside the lock. */
void event_log_iterate(bool (*cb)(const event_t *e, void *ctx), void *ctx);

void event_log_clear(void);

/** Increments on every add/clear; lets readers redraw only on change. */
uint32_t event_log_seq(void);

/** "TIMEOUT", "NODE_RESET", ... */
const char *event_class_name(event_class_t cls);

#ifdef __cplusplus
}
#endif

#endif /* EVENT_LOG_H */
