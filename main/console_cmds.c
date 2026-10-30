/**
 * @file console_cmds.c
 * @brief esp_console REPL on USB-Serial-JTAG with settings commands.
 *
 * Commands run in the REPL task ("console_repl", priority 2); apply_cb of a
 * changed setting therefore runs there too. Values are typed in stored units (°C).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "esp_console.h"
#include "esp_log.h"
#include "settings.h"
#include "hvac_controller.h"
#include "console_cmds.h"

static const char *TAG = "CONSOLE";

static const char *const k_group_names[SET_GROUP_COUNT] = {
    "display", "climate", "fan", "comfort", "control", "valves", "connect", "system",
    "service", "advanced", "air", "ac",
};

static const setting_def_t *find_key(const char *key)
{
    for (int i = 0; i < SET_COUNT; i++) {
        const setting_def_t *d = settings_def((setting_id_t)i);
        if (d->nvs_key && strcmp(d->nvs_key, key) == 0) {
            return d;
        }
    }
    printf("unknown key '%s' (see 'list')\n", key);
    return NULL;
}

/* The display and phone page refuse SERVICE rows unless mode is OFF; the console (a service
 * technician) may change them anyway, with a warning. */
static void warn_service(const setting_def_t *d)
{
    if (d->group == SET_GROUP_SERVICE && hvac_controller_get_mode() != HVAC_MODE_OFF) {
        printf("warning: service setting changed while mode is not OFF\n");
    }
}

static void print_row(const setting_def_t *d)
{
    char val[48];
    settings_format(d->id, val, sizeof(val));
    printf("%3d %-16s %-20s ", (int)d->id, d->nvs_key ? d->nvs_key : "-", val);
    switch (d->type) {
    case SET_TYPE_BOOL:
        printf("on/off");
        break;
    case SET_TYPE_ENUM:
        for (int i = 0; i <= (int)d->max; i++) {
            printf("%s%d=%s", i ? " " : "", i, d->enum_en[i]);
        }
        break;
    case SET_TYPE_STRING:
        printf("%d..%d chars, empty = device default", (int)d->min, (int)d->max);
        break;
    case SET_TYPE_ACTION:
        printf("action: %s", d->label_en);
        break;
    default:
        printf("%g..%g step %g%s%s", (double)d->min, (double)d->max, (double)d->step,
               d->unit ? " " : "", d->unit ? d->unit : "");
        break;
    }
    printf("\n");
}

static int cmd_list(int argc, char **argv)
{
    int group = -1;
    if (argc > 1) {
        for (int g = 0; g < SET_GROUP_COUNT; g++) {
            if (strcasecmp(argv[1], k_group_names[g]) == 0) {
                group = g;
            }
        }
        if (group < 0) {
            printf("groups:");
            for (int g = 0; g < SET_GROUP_COUNT; g++) {
                printf(" %s", k_group_names[g]);
            }
            printf("\n");
            return 1;
        }
    }
    for (int i = 0; i < SET_COUNT; i++) {
        const setting_def_t *d = settings_def((setting_id_t)i);
        if (group < 0 || (int)d->group == group) {
            print_row(d);
        }
    }
    return 0;
}

static int cmd_get(int argc, char **argv)
{
    if (argc != 2) {
        printf("usage: get <key>\n");
        return 1;
    }
    const setting_def_t *d = find_key(argv[1]);
    if (!d) {
        return 1;
    }
    print_row(d);
    return 0;
}

/* Parse text into the row's value; returns false (with a message) if it does not fit the type. */
static bool parse_value(const setting_def_t *d, const char *s, float *out)
{
    char *end;
    if (d->type == SET_TYPE_BOOL) {
        if (!strcasecmp(s, "on") || !strcasecmp(s, "1") || !strcasecmp(s, "true")) {
            *out = 1;
            return true;
        }
        if (!strcasecmp(s, "off") || !strcasecmp(s, "0") || !strcasecmp(s, "false")) {
            *out = 0;
            return true;
        }
        printf("expected on/off/1/0\n");
        return false;
    }
    if (d->type == SET_TYPE_ENUM) {
        for (int i = 0; i <= (int)d->max; i++) {
            if (!strcasecmp(s, d->enum_en[i]) || !strcasecmp(s, d->enum_da[i])) {
                *out = (float)i;
                return true;
            }
        }
        long idx = strtol(s, &end, 10);
        if (*s && !*end && idx >= 0 && idx <= (long)d->max) {
            *out = (float)idx;
            return true;
        }
        printf("expected 0..%d or a label (see 'get %s')\n", (int)d->max, d->nvs_key);
        return false;
    }
    *out = strtof(s, &end);
    if (!*s || *end) {
        printf("expected a number\n");
        return false;
    }
    return true;
}

static int cmd_set(int argc, char **argv)
{
    if (argc != 3) {
        printf("usage: set <key> <value>\n");
        return 1;
    }
    const setting_def_t *d = find_key(argv[1]);
    if (!d) {
        return 1;
    }
    esp_err_t err;
    if (d->type == SET_TYPE_STRING) {
        err = settings_set_str(d->id, argv[2]);
    } else {
        float v;
        if (!parse_value(d, argv[2], &v)) {
            return 1;
        }
        err = settings_set_float(d->id, v);
    }
    if (err != ESP_OK) {
        printf("rejected: %s\n", esp_err_to_name(err));
        return 1;
    }
    warn_service(d);
    print_row(d);      /* shows the value after clamping/snapping */
    char note[SETTINGS_NOTE_MAX];
    if (settings_last_note(d->id, note, sizeof(note))) {
        printf("note: %s\n", note);
    }
    return 0;
}

static int cmd_reset(int argc, char **argv)
{
    if (argc != 2) {
        printf("usage: reset <key|all>\n");
        return 1;
    }
    if (strcmp(argv[1], "all") == 0) {
        settings_reset_all();
        printf("all settings back to defaults\n");
        return 0;
    }
    const setting_def_t *d = find_key(argv[1]);
    if (!d) {
        return 1;
    }
    settings_reset(d->id);
    warn_service(d);
    print_row(d);
    return 0;
}

void console_cmds_start(void)
{
    static const esp_console_cmd_t cmds[] = {
        { .command = "list",  .help = "List settings: id key value range", .hint = "[group]", .func = cmd_list },
        { .command = "get",   .help = "Show one setting",                  .hint = "<key>",   .func = cmd_get },
        { .command = "set",   .help = "Change a setting (stored units, °C)", .hint = "<key> <value>", .func = cmd_set },
        { .command = "reset", .help = "Back to the default",               .hint = "<key|all>", .func = cmd_reset },
    };

    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "hvac>";
    esp_console_dev_usb_serial_jtag_config_t dev_cfg = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();

    esp_err_t err = esp_console_new_repl_usb_serial_jtag(&dev_cfg, &repl_cfg, &repl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "REPL not started: %s", esp_err_to_name(err));
        return;
    }
    esp_console_register_help_command();
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
    }
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
