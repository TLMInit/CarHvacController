#pragma once
#include "FreeRTOS.h"
typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);
#define taskENTER_CRITICAL(m) ((void)(m))
#define taskEXIT_CRITICAL(m)  ((void)(m))
static inline BaseType_t xTaskCreate(TaskFunction_t f, const char *n, uint32_t s, void *a, int p, TaskHandle_t *h)
{ (void)f; (void)n; (void)s; (void)a; (void)p; if (h) *h = (void *)1; return pdPASS; }
static inline BaseType_t xTaskNotifyGive(TaskHandle_t h) { (void)h; return pdTRUE; }
static inline uint32_t ulTaskNotifyTake(BaseType_t c, TickType_t t) { (void)c; (void)t; return 0; }
