/* Host fake: just enough of esp_err.h for the firmware sources under test. */
#pragma once
#include <stdlib.h>
typedef int esp_err_t;
#define ESP_OK                    0
#define ESP_FAIL                  (-1)
#define ESP_ERR_NO_MEM            0x101
#define ESP_ERR_INVALID_ARG       0x102
#define ESP_ERR_INVALID_STATE     0x103
#define ESP_ERR_INVALID_SIZE      0x104
#define ESP_ERR_NOT_FOUND         0x105
#define ESP_ERR_NOT_SUPPORTED     0x106
#define ESP_ERR_TIMEOUT           0x107
#define ESP_ERR_INVALID_CRC       0x109
#define ESP_ERR_NVS_NOT_FOUND     0x1102
#define ESP_ERR_NVS_TYPE_MISMATCH 0x1101
static inline const char *esp_err_to_name(esp_err_t e) { return e == ESP_OK ? "ESP_OK" : "ESP_ERR"; }
#define ESP_ERROR_CHECK(x) do { if ((x) != ESP_OK) abort(); } while (0)
