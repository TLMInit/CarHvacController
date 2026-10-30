/* settings_ns_open (main/settings.c) against the in-memory fake NVS. */
#include <string.h>
#include "settings.h"
#include "t.h"

static int s_migrate_calls, s_from, s_to;
static esp_err_t s_migrate_ret;

static esp_err_t migrate_keep(nvs_handle_t h, uint16_t from, uint16_t to)
{
    s_migrate_calls++; s_from = from; s_to = to;
    (void)nvs_set_i32(h, "added_in_v2", 7);       /* a migration may touch data; it must survive */
    return s_migrate_ret;
}

static uint16_t schema_of(nvs_handle_t h)
{
    uint16_t v = 0xFFFF;
    (void)nvs_get_u16(h, "schema", &v);
    return v;
}

static void setup(void) { fake_nvs_reset(); s_migrate_calls = 0; s_migrate_ret = ESP_OK; }

static void seed(const char *ns, uint16_t schema)      /* a namespace with data at an older schema */
{
    nvs_handle_t h;
    nvs_open(ns, NVS_READWRITE, &h);
    if (schema) nvs_set_u16(h, "schema", schema);
    nvs_set_i32(h, "keep_me", 42);
}

static void test_fresh_namespace_gets_schema(void)
{
    setup();
    nvs_handle_t h = 0;
    CHECK(settings_ns_open("hvac_cfg", 1, NULL, &h) == ESP_OK);
    CHECK(schema_of(h) == 1);
    CHECK(fake_nvs_count("hvac_cfg") == 1);

    setup();
    CHECK(settings_ns_open("hvac_cfg", 1, migrate_keep, &h) == ESP_OK);
    CHECK(s_migrate_calls == 1 && s_from == 0 && s_to == 1);       /* "no schema key" counts as version 0 */
    CHECK(schema_of(h) == 1);
}

static void test_older_schema_calls_migrate(void)
{
    setup();
    seed("hvac_cal", 1);
    nvs_handle_t h = 0;
    CHECK(settings_ns_open("hvac_cal", 2, migrate_keep, &h) == ESP_OK);
    CHECK(s_migrate_calls == 1 && s_from == 1 && s_to == 2);
    CHECK(schema_of(h) == 2);
    int32_t v = 0;
    CHECK(nvs_get_i32(h, "keep_me", &v) == ESP_OK && v == 42);
    CHECK(nvs_get_i32(h, "added_in_v2", &v) == ESP_OK && v == 7);
}

static void test_null_migrate_erases(void)
{
    setup();
    seed("hvac_cal", 1);
    nvs_handle_t h = 0;
    CHECK(settings_ns_open("hvac_cal", 2, NULL, &h) == ESP_OK);
    int32_t v = 0;
    CHECK(nvs_get_i32(h, "keep_me", &v) == ESP_ERR_NVS_NOT_FOUND);
    CHECK(schema_of(h) == 2);
    CHECK(fake_nvs_count("hvac_cal") == 1);

    setup();                                    /* unversioned data from before the scheme */
    seed("hvac_cal", 0);
    CHECK(settings_ns_open("hvac_cal", 1, NULL, &h) == ESP_OK);
    CHECK(nvs_get_i32(h, "keep_me", &v) == ESP_ERR_NVS_NOT_FOUND);
    CHECK(schema_of(h) == 1);
}

static void test_failed_migrate_erases(void)
{
    setup();
    seed("hvac_cal", 1);
    s_migrate_ret = ESP_FAIL;
    nvs_handle_t h = 0;
    CHECK(settings_ns_open("hvac_cal", 2, migrate_keep, &h) == ESP_OK);
    int32_t v = 0;
    CHECK(nvs_get_i32(h, "keep_me", &v) == ESP_ERR_NVS_NOT_FOUND);
    CHECK(nvs_get_i32(h, "added_in_v2", &v) == ESP_ERR_NVS_NOT_FOUND);
    CHECK(schema_of(h) == 2);
}

static void test_current_schema_untouched(void)
{
    setup();
    seed("hvac_cal", 2);
    nvs_handle_t h = 0;
    CHECK(settings_ns_open("hvac_cal", 2, migrate_keep, &h) == ESP_OK);
    CHECK(s_migrate_calls == 0);
    int32_t v = 0;
    CHECK(nvs_get_i32(h, "keep_me", &v) == ESP_OK && v == 42);
    CHECK(fake_nvs_count("hvac_cal") == 2);

    setup();                                    /* newer data than this firmware: read, never erase or downgrade */
    seed("hvac_cal", 5);
    CHECK(settings_ns_open("hvac_cal", 2, NULL, &h) == ESP_OK);
    CHECK(nvs_get_i32(h, "keep_me", &v) == ESP_OK && v == 42);
    CHECK(schema_of(h) == 5);
}

static void test_open_error_propagates(void)
{
    setup();
    fake_nvs_fail_open(ESP_ERR_NO_MEM);
    nvs_handle_t h = 0;
    CHECK(settings_ns_open("hvac_cfg", 1, NULL, &h) == ESP_ERR_NO_MEM);
}

int main(void)
{
    RUN(test_fresh_namespace_gets_schema);
    RUN(test_older_schema_calls_migrate);
    RUN(test_null_migrate_erases);
    RUN(test_failed_migrate_erases);
    RUN(test_current_schema_untouched);
    RUN(test_open_error_propagates);
    T_DONE();
}
