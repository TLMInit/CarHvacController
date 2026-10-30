/* Phone page request parsers (main/web_req.c): every POST body is validated here before any
 * controller, valve or settings call, so a bad body can only ever produce a 400 reason. */
#include <string.h>
#include "web_req.h"
#include "t.h"

static void test_control_valid(void)
{
    web_ctl_req_t c;
    CHECK(!web_parse_control("{\"mode\":3}", &c) && c.what == WCTL_MODE && c.value == 3);
    CHECK(!web_parse_control("{ \"setpoint\" : 22.5 }", &c) && c.what == WCTL_SETPOINT && c.value == 22.5f);
    CHECK(!web_parse_control("{\"setpoint_step\":-1}", &c) && c.what == WCTL_SETPOINT_STEP && c.value == -1);
    CHECK(!web_parse_control("{\"fan\":6}", &c) && c.what == WCTL_FAN && c.value == 6);
    CHECK(!web_parse_control("{\"fan_step\":1}", &c) && c.what == WCTL_FAN_STEP && c.value == 1);
    CHECK(!web_parse_control("{\"ac\":true}", &c) && c.what == WCTL_AC && c.value == 1);
    CHECK(!web_parse_control("{\"recirc\":false}", &c) && c.what == WCTL_RECIRC && c.value == 0);
    CHECK(!web_parse_control("{\"preset\":4}", &c) && c.what == WCTL_PRESET && c.value == 4);
    CHECK(!web_parse_control("{\"quick\":\"auto\"}", &c) && c.what == WCTL_QUICK_AUTO);
    CHECK(!web_parse_control("{\"quick\":\"def\"}", &c) && c.what == WCTL_QUICK_DEF);
}

static void test_control_rejected(void)
{
    web_ctl_req_t c;
    CHECK(web_parse_control("{}", &c));
    CHECK(web_parse_control("{\"mode\":7}", &c));
    CHECK(web_parse_control("{\"mode\":-1}", &c));
    CHECK(web_parse_control("{\"mode\":1.5}", &c));
    CHECK(web_parse_control("{\"mode\":\"1\"}", &c));
    CHECK(web_parse_control("{\"mode\":1,\"fan\":2}", &c));         /* one control at a time */
    CHECK(web_parse_control("{\"setpoint\":15.5}", &c));
    CHECK(web_parse_control("{\"setpoint\":31}", &c));
    CHECK(web_parse_control("{\"setpoint\":nan}", &c));
    CHECK(web_parse_control("{\"setpoint\":22x}", &c));
    CHECK(web_parse_control("{\"setpoint_step\":0}", &c));
    CHECK(web_parse_control("{\"fan_step\":2}", &c));
    CHECK(web_parse_control("{\"fan\":7}", &c));
    CHECK(web_parse_control("{\"ac\":1}", &c));
    CHECK(web_parse_control("{\"preset\":0}", &c));
    CHECK(web_parse_control("{\"preset\":5}", &c));
    CHECK(web_parse_control("{\"quick\":\"max\"}", &c));
}

static void test_valve(void)
{
    int v;
    web_valve_act_t a;
    CHECK(!web_parse_valve("{\"valve\":0,\"action\":\"jog_plus\"}", &v, &a) && v == 0 && a == WVALVE_JOG_PLUS);
    CHECK(!web_parse_valve("{\"action\":\"jog_minus\",\"valve\":4}", &v, &a) && v == 4 && a == WVALVE_JOG_MINUS);
    CHECK(!web_parse_valve("{\"valve\":2,\"action\":\"stop\"}", &v, &a) && a == WVALVE_STOP);
    CHECK(!web_parse_valve("{\"valve\":2,\"action\":\"recalibrate\"}", &v, &a) && a == WVALVE_RECAL);
    CHECK(!web_parse_valve("{\"valve\":2,\"action\":\"learn\"}", &v, &a) && a == WVALVE_LEARN);
    CHECK(web_parse_valve("{\"valve\":5,\"action\":\"stop\"}", &v, &a));
    CHECK(web_parse_valve("{\"valve\":1,\"action\":\"jog\"}", &v, &a));
    CHECK(web_parse_valve("{\"action\":\"stop\"}", &v, &a));
}

static void test_sensor(void)
{
    bool search;
    uint8_t rom[8];
    int role;
    CHECK(!web_parse_sensor("{\"action\":\"search\"}", &search, rom, &role) && search);
    CHECK(!web_parse_sensor("{\"action\":\"assign\",\"rom\":\"28aB01020304051F\",\"role\":3}", &search, rom, &role));
    CHECK(!search && role == 3 && rom[0] == 0x28 && rom[1] == 0xAB && rom[7] == 0x1F);
    CHECK(web_parse_sensor("{\"action\":\"assign\",\"rom\":\"28AB0102030405\",\"role\":1}", &search, rom, &role));
    CHECK(web_parse_sensor("{\"action\":\"assign\",\"rom\":\"28AB01020304051G\",\"role\":1}", &search, rom, &role));
    CHECK(web_parse_sensor("{\"action\":\"assign\",\"rom\":\"28AB01020304051F\",\"role\":5}", &search, rom, &role));
    CHECK(web_parse_sensor("{\"action\":\"erase\"}", &search, rom, &role));
}

static void test_compressor(void)
{
    bool stop, blower;
    int d;
    CHECK(!web_parse_compressor("{\"stop\":true}", &stop, &d, &blower) && stop);
    CHECK(!web_parse_compressor("{\"demand\":40,\"blower\":true}", &stop, &d, &blower) && !stop && d == 40 && blower);
    CHECK(web_parse_compressor("{\"stop\":false}", &stop, &d, &blower));
    CHECK(web_parse_compressor("{\"demand\":101,\"blower\":false}", &stop, &d, &blower));
    CHECK(web_parse_compressor("{\"demand\":50}", &stop, &d, &blower));
}

static void test_action(void)
{
    web_act_t a;
    int id = -1;
    CHECK(!web_parse_action("{\"action\":\"log_clear\"}", &a, &id) && a == WACT_LOG_CLEAR);
    CHECK(!web_parse_action("{\"action\":\"thermal_reset\"}", &a, &id) && a == WACT_THERMAL_RESET);
    CHECK(!web_parse_action("{\"action\":\"thermal_save\"}", &a, &id) && a == WACT_THERMAL_SAVE);
    CHECK(!web_parse_action("{\"action\":\"setting\",\"id\":42}", &a, &id) && a == WACT_SETTING && id == 42);
    CHECK(web_parse_action("{\"action\":\"setting\"}", &a, &id));
    CHECK(web_parse_action("{\"action\":\"reboot\"}", &a, &id));
    CHECK(web_parse_action("{}", &a, &id));
}

int main(void)
{
    RUN(test_control_valid);
    RUN(test_control_rejected);
    RUN(test_valve);
    RUN(test_sensor);
    RUN(test_compressor);
    RUN(test_action);
    T_DONE();
}
