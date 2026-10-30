#pragma once
#include <stdio.h>
/* No output; the if (0) keeps printf format checking and keeps TAG "used". */
#define ESP_LOG_NOP(tag, fmt, ...) do { if (0) printf("%s " fmt, tag, ##__VA_ARGS__); } while (0)
#define ESP_LOGE(tag, fmt, ...) ESP_LOG_NOP(tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) ESP_LOG_NOP(tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) ESP_LOG_NOP(tag, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) ESP_LOG_NOP(tag, fmt, ##__VA_ARGS__)
#define ESP_LOGV(tag, fmt, ...) ESP_LOG_NOP(tag, fmt, ##__VA_ARGS__)
