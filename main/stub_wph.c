/* Placeholder implementations of selftest.h until WP-H lands. WP-H deletes this file. */
#include <stdio.h>
#include "selftest.h"

void selftest_start(void) {}
bool selftest_done(void) { return false; }
selftest_result_t selftest_overall(void) { return SELFTEST_PENDING; }
size_t selftest_get_items(selftest_item_t *out, size_t max) { (void)out; (void)max; return 0; }
void selftest_summary(char *buf, size_t len) { if (buf && len) snprintf(buf, len, "Self-test not available"); }
uint32_t selftest_seq(void) { return 0; }
