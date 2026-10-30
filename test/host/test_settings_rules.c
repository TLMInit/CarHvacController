/* Cross-field rules (settings_table.c chains, enforced by settings.c at set time and at load). */
#include <string.h>
#include "settings.h"
#include "bts7960_driver.h"
#include "t.h"

void fake_nvs_reset(void);

static char s_note[SETTINGS_NOTE_MAX];

static bool noted(setting_id_t id, const char *part)
{
    return settings_last_note(id, s_note, sizeof(s_note)) && strstr(s_note, part) != NULL;
}

static void fresh(void)
{
    fake_nvs_reset();
    CHECK(settings_init() == ESP_OK);
}

/* Boot with these i32 keys already stored (as older firmware may have left them). */
static void boot_with_i32(const char *k1, int32_t v1, const char *k2, int32_t v2)
{
    fake_nvs_reset();
    nvs_handle_t h;
    nvs_open(SETTINGS_NAMESPACE, NVS_READWRITE, &h);
    nvs_set_u16(h, "schema", SETTINGS_SCHEMA);
    if (k1) nvs_set_i32(h, k1, v1);
    if (k2) nvs_set_i32(h, k2, v2);
    CHECK(settings_init() == ESP_OK);
}

/* Boot with these float keys stored (u32 bit pattern, as settings.c writes them). */
static void boot_with_f(const char *k1, float v1, const char *k2, float v2)
{
    fake_nvs_reset();
    nvs_handle_t h;
    uint32_t b;
    nvs_open(SETTINGS_NAMESPACE, NVS_READWRITE, &h);
    nvs_set_u16(h, "schema", SETTINGS_SCHEMA);
    memcpy(&b, &v1, 4); nvs_set_u32(h, k1, b);
    memcpy(&b, &v2, 4); nvs_set_u32(h, k2, b);
    CHECK(settings_init() == ESP_OK);
}

static void test_fan_chain_pure(void)
{
    float vals[SET_COUNT] = { 0 };
    vals[SET_FAN_MIN_DUTY] = 30;
    vals[SET_FAN_MAX_DUTY] = 100;
    setting_rule_hit_t hit;
    CHECK(settings_rules_clamp(SET_FAN_MIN_DUTY, 40, vals, &hit) == 40 && hit.chain == NULL);
    CHECK(settings_rules_clamp(SET_FAN_MIN_DUTY, 70, vals, &hit) == BTS7960_FAN_PERCENT_MEDIUM_LOW);
    CHECK(hit.chain && hit.other_above && hit.other == &g_fan_levels[1]);
    CHECK(settings_rules_clamp(SET_FAN_MAX_DUTY, 50, vals, &hit) == BTS7960_FAN_PERCENT_MEDIUM_HIGH);
    CHECK(hit.chain && !hit.other_above && hit.other == &g_fan_levels[3]);
}

static void test_defaults_obey_rules(void)
{
    float vals[SET_COUNT];
    for (int i = 0; i < SET_COUNT; i++) vals[i] = settings_def((setting_id_t)i)->def;
    CHECK(settings_rules_fix(vals) == 0);
}

static void test_fan_set_time(void)
{
    fresh();
    CHECK(settings_set_int(SET_FAN_MIN_DUTY, 60) == ESP_OK);
    CHECK(settings_get_int(SET_FAN_MIN_DUTY) == BTS7960_FAN_PERCENT_MEDIUM_LOW);
    CHECKM(noted(SET_FAN_MIN_DUTY, "must not be above Med-low (45 %)"), "note '%s'", s_note);
    CHECK(settings_set_int(SET_FAN_MIN_DUTY, 40) == ESP_OK);
    CHECK(settings_get_int(SET_FAN_MIN_DUTY) == 40);
    CHECK(!noted(SET_FAN_MIN_DUTY, ""));                        /* a legal set clears the note */

    CHECK(settings_set_int(SET_FAN_MAX_DUTY, 50) == ESP_OK);
    CHECK(settings_get_int(SET_FAN_MAX_DUTY) == BTS7960_FAN_PERCENT_MEDIUM_HIGH);
    CHECKM(noted(SET_FAN_MAX_DUTY, "must not be below Med-high (80 %)"), "note '%s'", s_note);
    CHECK(!noted(SET_FAN_MIN_DUTY, ""));                        /* the note belongs to the last set only */

    CHECK(settings_set_int(SET_FAN_MIN_DUTY, 0) == ESP_OK);     /* below the range: plain clamp, no rule note */
    CHECK(settings_get_int(SET_FAN_MIN_DUTY) == 5);
    CHECK(!noted(SET_FAN_MIN_DUTY, ""));
}

static void test_fan_load_time(void)
{
    boot_with_i32("fan_min", 70, "fan_max", 40);
    CHECK(settings_get_int(SET_FAN_MIN_DUTY) == BTS7960_FAN_PERCENT_MEDIUM_LOW);
    CHECK(settings_get_int(SET_FAN_MAX_DUTY) == BTS7960_FAN_PERCENT_MEDIUM_HIGH);

    boot_with_i32("fan_min", 0, NULL, 0);                      /* older firmware ran a stored 0 as 30 */
    CHECK(settings_get_int(SET_FAN_MIN_DUTY) == 30);
}

static void test_valve_speed_floor(void)
{
    fresh();
    CHECK(settings_set_int(SET_VALVE_SPEED_0, 5) == ESP_OK);    /* + from 0 reaches the floor */
    CHECK(settings_get_int(SET_VALVE_SPEED_0) == 30);
    CHECKM(noted(SET_VALVE_SPEED_0, "0 = default"), "note '%s'", s_note);
    CHECK(settings_step(SET_VALVE_SPEED_0, -1) == ESP_OK);     /* - from the floor reaches 0 */
    CHECK(settings_get_int(SET_VALVE_SPEED_0) == 0);
    CHECK(settings_set_int(SET_VALVE_SPEED_4, 60) == ESP_OK);   /* coolant floor is 100 */
    CHECK(settings_get_int(SET_VALVE_SPEED_4) == 100);
    CHECK(settings_set_int(SET_VALVE_SPEED_1, 120) == ESP_OK);
    CHECK(settings_get_int(SET_VALVE_SPEED_1) == 120 && !noted(SET_VALVE_SPEED_1, ""));

    boot_with_i32("vspeed2", 10, "vspeed3", 25);
    CHECK(settings_get_int(SET_VALVE_SPEED_2) == 0);
    CHECK(settings_get_int(SET_VALVE_SPEED_3) == 30);
}

static void test_humidity_gap(void)
{
    fresh();                                                    /* ON 65, OFF 58 */
    CHECK(settings_set_float(SET_HUMIDITY_OFF, 64) == ESP_OK);  /* ON - 1 */
    CHECK(settings_get_float(SET_HUMIDITY_OFF) == 65 - SETTINGS_HUMIDITY_GAP);
    CHECKM(noted(SET_HUMIDITY_OFF, "must be at least 2 %RH below Humidity A/C on (65 %RH)"), "note '%s'", s_note);

    /* Lowering ON stops at OFF + gap; OFF is never moved */
    CHECK(settings_set_float(SET_HUMIDITY_ON, 60) == ESP_OK);
    CHECK(settings_get_float(SET_HUMIDITY_ON) == 63 + SETTINGS_HUMIDITY_GAP);
    CHECK(settings_get_float(SET_HUMIDITY_OFF) == 63);
    CHECKM(noted(SET_HUMIDITY_ON, "must be at least 2 %RH above Humidity A/C off (63 %RH)"), "note '%s'", s_note);
    CHECK(settings_step(SET_HUMIDITY_ON, -1) == ESP_OK);
    CHECK(settings_get_float(SET_HUMIDITY_ON) - settings_get_float(SET_HUMIDITY_OFF) >= SETTINGS_HUMIDITY_GAP);

    /* reset_all lands on the defaults although ON is reset while OFF is still high */
    CHECK(settings_set_float(SET_HUMIDITY_ON, 85) == ESP_OK && settings_set_float(SET_HUMIDITY_OFF, 80) == ESP_OK);
    CHECK(settings_get_float(SET_HUMIDITY_OFF) == 80);
    CHECK(settings_reset_all() == ESP_OK);
    CHECK(settings_get_float(SET_HUMIDITY_ON) == 65 && settings_get_float(SET_HUMIDITY_OFF) == 58);

    boot_with_f("hum_on", 60, "hum_off", 70);                  /* stored by older firmware */
    CHECK(settings_get_float(SET_HUMIDITY_ON) == 60);
    CHECK(settings_get_float(SET_HUMIDITY_OFF) == 60 - SETTINGS_HUMIDITY_GAP);
}

static void test_duct_gap(void)
{
    fresh();                                                    /* min 10, max 60 */
    CHECK(settings_set_float(SET_DUCT_MIN, 30) == ESP_OK);
    CHECK(settings_set_float(SET_DUCT_MAX, 32) == ESP_OK);      /* below min + 5 */
    CHECK(settings_get_float(SET_DUCT_MAX) == 30 + SETTINGS_DUCT_GAP);
    CHECKM(noted(SET_DUCT_MAX, "must be at least 5 °C above Duct temp minimum (30 °C)"), "note '%s'", s_note);
    CHECK(settings_set_float(SET_DUCT_MIN, 10) == ESP_OK);
    CHECK(settings_set_float(SET_DUCT_MAX, 33) == ESP_OK);
    CHECK(settings_set_float(SET_DUCT_MIN, 31) == ESP_OK);      /* max - 2 */
    CHECK(settings_get_float(SET_DUCT_MIN) == 33 - SETTINGS_DUCT_GAP);
    CHECKM(noted(SET_DUCT_MIN, "must be at least 5 °C below Duct temp maximum (33 °C)"), "note '%s'", s_note);
    CHECK(settings_get_float(SET_DUCT_MAX) == 33);

    boot_with_f("duct_min", 30, "duct_max", 30);               /* accepted by older firmware */
    CHECK(settings_get_float(SET_DUCT_MIN) == 30 - SETTINGS_DUCT_GAP);
    CHECK(settings_get_float(SET_DUCT_MAX) == 30);
}

int main(void)
{
    RUN(test_fan_chain_pure);
    RUN(test_defaults_obey_rules);
    RUN(test_fan_set_time);
    RUN(test_fan_load_time);
    RUN(test_valve_speed_floor);
    RUN(test_humidity_gap);
    RUN(test_duct_gap);
    T_DONE();
}
