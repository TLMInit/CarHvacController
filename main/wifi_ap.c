/**
 * @file wifi_ap.c
 * @brief Phone access point "HVAC-<mac4>", WPA2, mode from SET_WIFI_AP_MODE (WP-G).
 *
 * All Wi-Fi start/stop work runs in one small task; wifi_ap_apply_settings() only
 * notifies it. When off, the driver is deinitialised so its
 * buffers are returned and the radio is unpowered.
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "wifi_ap.h"
#include "web_server.h"
#include "settings.h"
#include "event_log.h"

static const char *TAG = "WIFI_AP";

#define AP_CHANNEL          1
#define AP_MAX_CLIENTS      2
#define AP_IDLE_TIMEOUT_US  (10LL * 60 * 1000000)

static TaskHandle_t s_task;
static char s_ssid[12];
static volatile bool s_on;
static volatile int s_clients;
static volatile int64_t s_idle_since_us;    /* last moment a client was connected (or AP start) */
static volatile bool s_ten_min;             /* AP is on under the 10MIN rule */

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)data;
    if (id == WIFI_EVENT_AP_STACONNECTED) {
        s_clients++;
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        if (s_clients > 0) {
            s_clients--;
        }
    } else {
        return;
    }
    s_idle_since_us = esp_timer_get_time();
    event_log_add(EVT_INFO, "%s: %d phone(s) connected", s_ssid, s_clients);
}

static void ap_start(void)
{
    if (s_on) {
        return;
    }
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&init);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init: %s", esp_err_to_name(err));
        return;
    }
    wifi_config_t cfg = {
        .ap = {
            .channel = AP_CHANNEL,
            .max_connection = AP_MAX_CLIENTS,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = { .required = false },
        },
    };
    memcpy(cfg.ap.ssid, s_ssid, strlen(s_ssid));
    cfg.ap.ssid_len = strlen(s_ssid);
    settings_get_str(SET_AP_PASSWORD, (char *)cfg.ap.password, sizeof(cfg.ap.password));

    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if ((err = esp_wifi_set_mode(WIFI_MODE_AP)) != ESP_OK ||
        (err = esp_wifi_set_config(WIFI_IF_AP, &cfg)) != ESP_OK ||
        (err = esp_wifi_start()) != ESP_OK) {
        ESP_LOGE(TAG, "AP start failed: %s", esp_err_to_name(err));
        esp_wifi_deinit();
        return;
    }
    s_clients = 0;
    s_idle_since_us = esp_timer_get_time();
    s_on = true;
    web_server_start();
    event_log_add(EVT_INFO, "AP on: %s", s_ssid);
    ESP_LOGI(TAG, "Free internal heap with AP on: %u (largest block %u)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

static void ap_stop(const char *why)
{
    if (!s_on) {
        return;
    }
    web_server_stop();
    esp_wifi_stop();
    esp_wifi_deinit();
    s_on = false;
    s_clients = 0;
    event_log_add(EVT_INFO, "AP off: %s (%s)", s_ssid, why);
}

static void ap_task(void *arg)
{
    (void)arg;
    int applied = -1;
    for (;;) {
        /* Woken by apply_settings / client events, else once a second for the idle timer */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        int mode = settings_get_int(SET_WIFI_AP_MODE);
        if (mode != applied) {
            applied = mode;
            s_ten_min = (mode == SET_AP_10MIN);
            if (mode == SET_AP_OFF) {
                ap_stop("setting off");
            } else {
                ap_start();
                s_idle_since_us = esp_timer_get_time();     /* re-selecting 10MIN restarts the window */
            }
        }
        if (s_on && s_ten_min && s_clients == 0 &&
            esp_timer_get_time() - s_idle_since_us >= AP_IDLE_TIMEOUT_US) {
            ap_stop("10 min without a phone");
        }
    }
}

esp_err_t wifi_ap_init(void)
{
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP));
    snprintf(s_ssid, sizeof(s_ssid), "HVAC-%02X%02X", mac[4], mac[5]);

    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    if (!esp_netif_create_default_wifi_ap()) {
        return ESP_FAIL;
    }
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    if (xTaskCreate(ap_task, "wifi_ap", 4096, NULL, 3, &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void wifi_ap_apply_settings(void)
{
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

bool wifi_ap_is_on(void)
{
    return s_on;
}

const char *wifi_ap_ssid(void)
{
    return s_ssid;
}

int wifi_ap_client_count(void)
{
    return s_clients;
}

uint32_t wifi_ap_remaining_s(void)
{
    if (!s_on || !s_ten_min || s_clients > 0) {
        return 0;
    }
    int64_t left = AP_IDLE_TIMEOUT_US - (esp_timer_get_time() - s_idle_since_us);
    return left > 0 ? (uint32_t)(left / 1000000) : 0;
}
