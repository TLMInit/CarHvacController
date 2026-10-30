/* Settings table sanity (main/settings_table.c) plus the defaults DECISIONS.md promises. */
#include <string.h>
#include "settings.h"
#include "t.h"

extern const setting_def_t g_settings_table[SET_COUNT];

static int chars(const char *s)     /* UTF-8 code points: the display counts glyphs, not bytes */
{
    int n = 0;
    for (; *s; s++) n += ((unsigned char)*s & 0xC0) != 0x80;
    return n;
}

static int expected_enum_count(setting_id_t id)
{
    switch (id) {
    case SET_TEMP_UNIT: case SET_LANGUAGE: case SET_CONTROL_MODE: return 2;
    case SET_POWER_UP_STATE: case SET_AUTO_CURVE: case SET_WIFI_AP_MODE:
    case SET_VALVE_DIR_0: case SET_VALVE_DIR_1: case SET_VALVE_DIR_2: case SET_VALVE_DIR_3: case SET_VALVE_DIR_4: return 3;
    case SET_HEATER_DELAY_FAN: return 6;      /* OFF, LOW, MED-LOW, MEDIUM, MED-HIGH, HIGH */
    default: return -1;
    }
}

static void test_rows_in_order(void)
{
    for (int i = 0; i < SET_COUNT; i++) {
        const setting_def_t *d = &g_settings_table[i];
        CHECKM(d->id == (setting_id_t)i, "row %d has id %d", i, d->id);
        CHECKM(settings_def((setting_id_t)i) == d, "settings_def(%d)", i);
        CHECKM(d->group < SET_GROUP_COUNT, "row %d group", i);
    }
}

static void test_nvs_keys(void)
{
    for (int i = 0; i < SET_COUNT; i++) {
        const setting_def_t *d = &g_settings_table[i];
        if (d->type == SET_TYPE_ACTION) {
            CHECKM(d->nvs_key == NULL, "action row %d has a key", i);
            continue;
        }
        CHECKM(d->nvs_key && d->nvs_key[0], "row %d: empty key", i);
        if (!d->nvs_key) continue;
        CHECKM(strlen(d->nvs_key) <= 15, "row %d: key %s > 15", i, d->nvs_key);
        CHECKM(strcmp(d->nvs_key, "schema") != 0, "row %d uses reserved key", i);
        CHECKM(strcmp(d->nvs_key, "ap_pass_gen") != 0, "row %d collides with ap_pass_gen", i);
        for (int j = 0; j < i; j++) {
            if (g_settings_table[j].nvs_key) {
                CHECKM(strcmp(g_settings_table[j].nvs_key, d->nvs_key) != 0, "rows %d and %d share key %s", j, i, d->nvs_key);
            }
        }
    }
}

static void test_ranges_and_steps(void)
{
    for (int i = 0; i < SET_COUNT; i++) {
        const setting_def_t *d = &g_settings_table[i];
        if (d->type == SET_TYPE_ACTION) continue;
        if (d->type == SET_TYPE_STRING) {
            CHECK(d->def_str != NULL);
            CHECK(d->min <= d->max);
            CHECK(d->max < SETTINGS_STR_MAX);
            continue;
        }
        CHECKM(d->min <= d->def && d->def <= d->max, "row %d (%s): min %g def %g max %g", i, d->nvs_key, d->min, d->def, d->max);
        CHECKM(d->min < d->max || d->type == SET_TYPE_ENUM, "row %d: empty range", i);
        CHECKM(d->step > 0.0f, "row %d (%s): step %g", i, d->nvs_key, d->step);
    }
}

static void test_labels_and_enums(void)
{
    for (int i = 0; i < SET_COUNT; i++) {
        const setting_def_t *d = &g_settings_table[i];
        CHECKM(d->label_en && d->label_en[0] && chars(d->label_en) <= 22, "row %d en label", i);
        CHECKM(d->label_da && d->label_da[0] && chars(d->label_da) <= 22, "row %d da label", i);
        int want = expected_enum_count(d->id);
        if (d->type != SET_TYPE_ENUM) {
            CHECKM(want < 0, "row %d expected to be an enum", i);
            continue;
        }
        CHECKM(want > 0, "enum row %d not in the test's expected-count table", i);
        CHECKM((int)d->max + 1 == want, "row %d: max+1 = %d, labels = %d", i, (int)d->max + 1, want);
        CHECKM(d->min == 0, "enum row %d min", i);
        CHECK(d->enum_en && d->enum_da);
        if (!d->enum_en || !d->enum_da) continue;
        for (int k = 0; k <= (int)d->max; k++) {
            CHECKM(d->enum_en[k] && d->enum_en[k][0] && chars(d->enum_en[k]) <= 22, "row %d en label %d", i, k);
            CHECKM(d->enum_da[k] && d->enum_da[k][0] && chars(d->enum_da[k]) <= 22, "row %d da label %d", i, k);
        }
    }
}

static void test_decision_defaults(void)
{
    CHECK(settings_def(SET_AUTO_REQUESTS_AC)->def == 0);
    CHECK(settings_def(SET_HEATER_DELAY)->def == 0);
    CHECK(settings_def(SET_DEFOG)->def == 0);
    CHECK(settings_def(SET_CONTROL_MODE)->def == SET_CONTROL_CLASSIC);
    CHECK(settings_def(SET_RECIRC_FITTED)->def == 0);
    CHECK(settings_def(SET_DEF_RESTORES_AC)->def == 1);
    CHECK(settings_def(SET_QUIET_NIGHT_CAP)->def == 0);          /* "quiet-night cap off" */
    CHECK(settings_def(SET_HUMIDITY_OFF)->def <= settings_def(SET_HUMIDITY_ON)->def - SETTINGS_HUMIDITY_GAP);
}

int main(void)
{
    RUN(test_rows_in_order);
    RUN(test_nvs_keys);
    RUN(test_ranges_and_steps);
    RUN(test_labels_and_enums);
    RUN(test_decision_defaults);
    T_DONE();
}
