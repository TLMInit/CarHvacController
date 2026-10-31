/* Tiny test runner: CHECK() counts, RUN() prints per-function result, T_DONE() sets the exit code. */
#pragma once
#include <stdio.h>
static int t_checks, t_failed;
#define CHECK(c) do { t_checks++; if (!(c)) { t_failed++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECKM(c, ...) do { t_checks++; if (!(c)) { t_failed++; printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define RUN(fn) do { int b_ = t_failed; fn(); printf("%s %s\n", t_failed == b_ ? "[ ok ]" : "[FAIL]", #fn); } while (0)
#define T_DONE() do { printf("%d checks, %d failed\n", t_checks, t_failed); return t_failed ? 1 : 0; } while (0)
