/* Settings export / import (settings.c): all or nothing, refusal reasons. */
#include <string.h>
#include "settings.h"
#include "t.h"

void fake_nvs_reset(void);

static char s_buf[8192], s_why[160];
static int s_changed;

static void fresh(void)
{
    fake_nvs_reset();
    CHECK(settings_init() == ESP_OK);
}

static esp_err_t imp(const char *json, bool service_ok)
{
    s_changed = -1;
    return settings_import_json(json, service_ok, &s_changed, s_why, sizeof(s_why));
}

/* A refused import changes nothing: same sequence number before and after. */
static void refused(const char *json, bool service_ok, esp_err_t want, const char *part)
{
    uint32_t seq = settings_seq();
    esp_err_t err = imp(json, service_ok);
    CHECKM(err == want, "err %d want %d (%s)", err, want, s_why);
    CHECKM(strstr(s_why, part) != NULL, "why '%s' lacks '%s'", s_why, part);
    CHECK(settings_seq() == seq && s_changed == 0);
}

#define FILE_OF(settings) "{\"format\":\"hvac-settings\",\"schema\":1,\"firmware\":\"x\",\"settings\":{" settings "}}"

static void test_export_defaults_reimports_unchanged(void)
{
    fresh();
    int n = settings_export_json(s_buf, sizeof(s_buf), "v1.2-3", false);
    CHECK(n > 0 && n < (int)sizeof(s_buf));
    CHECK(strstr(s_buf, "\"format\":\"hvac-settings\"") && strstr(s_buf, "\"firmware\":\"v1.2-3\""));
    CHECK(strstr(s_buf, "\"fan_min\":30") && strstr(s_buf, "\"show_blower\":true") && strstr(s_buf, "\"sp_step\":0.5"));
    CHECK(strstr(s_buf, "ap_pass") == NULL);                       /* left out by default */
    CHECKM(imp(s_buf, false) == ESP_OK && s_changed == 0, "changed %d why '%s'", s_changed, s_why);
    CHECK(settings_export_json(s_buf, 100, "v", false) == -1);     /* too small: no partial file */

    CHECK(settings_export_json(s_buf, sizeof(s_buf), "v", true) > 0);
    CHECK(strstr(s_buf, "\"ap_pass\":\""));                        /* the generated password */
    CHECKM(imp(s_buf, false) == ESP_OK && s_changed == 0, "changed %d why '%s'", s_changed, s_why);
}

static void test_changed_values_import(void)
{
    fresh();
    CHECK(imp(FILE_OF("\"fan_min\":35"), false) == ESP_OK && s_changed == 1);
    CHECK(settings_get_int(SET_FAN_MIN_DUTY) == 35);
    CHECK(imp(FILE_OF("\"sp_step\":0.3,\"vfit0\":true,\"temp_unit\":1"), false) == ESP_OK && s_changed == 2);
    CHECK(settings_get_float(SET_SETPOINT_STEP) > 0.29f && settings_get_float(SET_SETPOINT_STEP) < 0.31f);
    CHECK(settings_get_int(SET_TEMP_UNIT) == SET_UNIT_F);

    /* Lowering both humidity values one at a time would clamp ON against the old OFF (58): the
     * set is applied as a whole */
    CHECK(imp(FILE_OF("\"hum_on\":52,\"hum_off\":50"), false) == ESP_OK && s_changed == 2);
    CHECK(settings_get_int(SET_HUMIDITY_ON) == 52 && settings_get_int(SET_HUMIDITY_OFF) == 50);

    CHECK(imp(FILE_OF("\"ap_pass\":\"secret123\""), false) == ESP_OK && s_changed == 1);
    char pw[SETTINGS_STR_MAX];
    settings_get_str(SET_AP_PASSWORD, pw, sizeof(pw));
    CHECK(strcmp(pw, "secret123") == 0);
}

static void test_bad_values_refused(void)
{
    fresh();
    refused(FILE_OF("\"fan_max\":90,\"fan_min\":120"), true, ESP_ERR_INVALID_ARG, "fan_min: 120 is not allowed (5..100 in steps of 5)");
    CHECK(settings_get_int(SET_FAN_MAX_DUTY) == 100);              /* the valid value was not applied either */
    refused(FILE_OF("\"fan_min\":33"), true, ESP_ERR_INVALID_ARG, "fan_min: 33 is not allowed");
    refused(FILE_OF("\"vfit0\":1"), true, ESP_ERR_INVALID_ARG, "vfit0: expected true or false");
    refused(FILE_OF("\"fan_min\":\"35\""), true, ESP_ERR_INVALID_ARG, "fan_min: expected a number");
    refused(FILE_OF("\"fan_min\":35,\"fan_min\":40"), true, ESP_ERR_INVALID_ARG, "fan_min: listed twice");
    refused(FILE_OF("\"ap_pass\":\"short\""), true, ESP_ERR_INVALID_ARG, "ap_pass: 8..32 printable");
    refused(FILE_OF("\"fan_min\":0x20"), true, ESP_ERR_INVALID_ARG, "could not be read");
}

static void test_unknown_keys_refused(void)
{
    fresh();
    refused(FILE_OF("\"fan_min\":35,\"bogus\":1,\"old_key\":true"), true, ESP_ERR_INVALID_ARG, "Unknown settings: bogus, old_key");
    CHECK(settings_get_int(SET_FAN_MIN_DUTY) == 30);
}

static void test_format_and_schema_refused(void)
{
    fresh();
    refused("{\"format\":\"hvac-settings\",\"schema\":2,\"settings\":{\"fan_min\":35}}", true,
            ESP_ERR_INVALID_VERSION, "newer firmware (settings schema 2");
    refused("{\"format\":\"hvac-settings\",\"schema\":0,\"settings\":{\"fan_min\":35}}", true,
            ESP_ERR_INVALID_VERSION, "cannot convert");
    refused("{\"format\":\"other\",\"schema\":1,\"settings\":{\"fan_min\":35}}", true, ESP_ERR_INVALID_ARG, "format is not");
    refused("{\"format\":\"hvac-settings\",\"schema\":1}", true, ESP_ERR_INVALID_ARG, "no \"settings\"");
    refused("{\"format\":\"hvac-settings\",\"schema\":1,\"settings\":{\"fan_min\":35}", true, ESP_ERR_INVALID_ARG, "could not be read");
    refused("[1,2]", true, ESP_ERR_INVALID_ARG, "could not be read");
    CHECK(settings_get_int(SET_FAN_MIN_DUTY) == 30);
}

static void test_cross_field_refused(void)
{
    fresh();
    refused(FILE_OF("\"fan_min\":50"), true, ESP_ERR_INVALID_ARG, "Fan minimum (50 %) must not be above Fan level Med-low (45 %)");
    refused(FILE_OF("\"hum_off\":64"), true, ESP_ERR_INVALID_ARG,
            "Humidity A/C off (64 %RH) must be at least 2 %RH below Humidity A/C on (65 %RH)");
    refused(FILE_OF("\"vspeed0\":20"), true, ESP_ERR_INVALID_ARG, "Speed center 1 (20): 0 = default, else at least Butterfly min speed (60)");
    CHECK(settings_get_int(SET_HUMIDITY_OFF) == 58);
}

static void test_service_needs_mode_off(void)
{
    fresh();
    refused(FILE_OF("\"fan_min\":35,\"v_tol\":3"), false, ESP_ERR_INVALID_STATE, "Service settings change only with mode OFF (v_tol)");
    CHECK(settings_get_int(SET_FAN_MIN_DUTY) == 30);
    CHECK(imp(FILE_OF("\"fan_min\":35,\"v_tol\":2"), false) == ESP_OK && s_changed == 1);     /* unchanged service row: fine */
    CHECK(imp(FILE_OF("\"v_tol\":3"), true) == ESP_OK && s_changed == 1);
    CHECK(settings_get_int(SET_VALVE_TOL_PCT) == 3);
}

int main(void)
{
    RUN(test_export_defaults_reimports_unchanged);
    RUN(test_changed_values_import);
    RUN(test_bad_values_refused);
    RUN(test_unknown_keys_refused);
    RUN(test_format_and_schema_refused);
    RUN(test_cross_field_refused);
    RUN(test_service_needs_mode_off);
    T_DONE();
}
