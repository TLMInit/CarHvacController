/**
 * @file selftest.h
 * @brief Boot self-test: what answered, what is calibrated, what is missing.
 *
 * INTERFACE (lead). Implemented by WP-H in selftest.c. Runs in its own short-lived
 * task started after the background tasks; waits (max 60 s) for calibration to
 * finish, then checks and publishes. Results go to the event log (EVT_FAULT /
 * EVT_INFO) and to the UI via the getters (the UI shows the summary 5 s after boot
 * and the fault badge uses selftest_overall()).
 */

#ifndef SELFTEST_H
#define SELFTEST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SELFTEST_PENDING = 0,
    SELFTEST_PASS,
    SELFTEST_WARN,      /* works, but something is missing or degraded */
    SELFTEST_FAIL,      /* a function is unavailable */
} selftest_result_t;

#define SELFTEST_MAX_ITEMS      12
#define SELFTEST_DETAIL_LEN     40

typedef struct {
    const char *name;                       /* "IO node", "Cabin sensor", "Duct sensor", "Valve C1", ... */
    selftest_result_t result;
    char detail[SELFTEST_DETAIL_LEN];       /* "ionode 2.2, proto 2", "stored cal", "not found", ... */
} selftest_item_t;

/** Start the self-test task (once, from app_main after the background tasks). */
void selftest_start(void);

bool selftest_done(void);
selftest_result_t selftest_overall(void);

/** Copy up to max items; returns the count. Safe from any task. */
size_t selftest_get_items(selftest_item_t *out, size_t max);

/** One line for the UI, e.g. "Self-test OK" or "Self-test: 2 warnings - Coolant sensor, Valve WS". */
void selftest_summary(char *buf, size_t len);

/** Increments when results change. */
uint32_t selftest_seq(void);

#ifdef __cplusplus
}
#endif

#endif /* SELFTEST_H */
