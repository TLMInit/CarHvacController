# Inventory of hard-coded tunables not on the settings page

Read-only review of the display repo at `v2.2-display` (d068015) and the node repo at
`v2.3-node` (630a6e4). The brief named `v2.2.1-display` / `v2.3.1-node`; those tags do not
exist in either repo, and nothing has changed since the heads above, so these are what was read.

Classes: **USER** (settings page), **SERVICE** (service page, car stopped), **ADVANCED** (phone
page only), **KEEP** (design constant; the reason says why). Defaults in the proposals equal
today's constants. "Today" means the constant as it is in the code; where a value is in the
header of a settings row already, that row is referenced instead.

Node paths are relative to `CarHvacIoNode/`.

## 1. Inventory

| # | file:line | name | value | what it changes | class | proposal (group, label, min, max, step, default) |
|---|---|---|---|---|---|---|
| 1 | main/bts7960_driver.h:19 | BTS7960_FAN_PERCENT_MEDIUM_LOW | 45 % | Blower duty of the manual Med-low level | USER | Fan, "Fan level Med-low", 10, 100, 5, 45 |
| 2 | main/bts7960_driver.h:20 | BTS7960_FAN_PERCENT_MEDIUM | 60 % | Duty of Medium (also the Eco cap, #15) | USER | Fan, "Fan level Medium", 10, 100, 5, 60 |
| 3 | main/bts7960_driver.h:21 | BTS7960_FAN_PERCENT_MEDIUM_HIGH | 80 % | Duty of Med-high | USER | Fan, "Fan level Med-high", 10, 100, 5, 80 |
| 4 | main/bts7960_driver.h:23 | BTS7960_FAN_PERCENT_AUTO_DEFAULT | 50 % | AUTO fan duty while the cabin sensor is lost (hvac_controller.c:692) | USER | Fan, "AUTO fan, no sensor", 0, 100, 5, 50 |
| 5 | main/hvac_controller.c:36 | TEMP_DEADBAND | 0.5 °C | Band around the setpoint with no heating or cooling; also the lower knee of the AUTO fan curve (:767, :775) | USER | Climate, "Setpoint deadband", 0.2, 2.0, 0.1, 0.5 |
| 6 | main/hvac_controller.c:59 | WARMUP_COLD_CABIN_THRESHOLD | 15 °C | Cabin below this when AUTO starts = cold-start warm-up (max heat, windscreen, A/C) | USER | Comfort, "Cold start below", 0, 20, 1, 15 |
| 7 | main/hvac_controller.c:60 | WARMUP_MIN_DURATION_MS | 60000 ms | Warm-up cannot end before this | USER | Comfort, "Cold start minimum", 0, 600, 30, 60 (s) |
| 8 | main/hvac_controller.c:61 | WARMUP_MAX_DURATION_MS | 300000 ms | Warm-up always ends after this | USER | Comfort, "Cold start maximum", 1, 15, 1, 5 (min) |
| 9 | main/hvac_controller.c:62 | WARMUP_EXIT_CABIN_TEMP | 18 °C | Cabin temperature that lets warm-up end | USER | Comfort, "Cold start ends at", 10, 25, 0.5, 18 |
| 10 | main/hvac_controller.c:63 | WARMUP_EXIT_HUMIDITY | 60 %RH | Humidity below which warm-up may end | USER | Comfort, "Cold start ends below", 30, 90, 5, 60 |
| 11 | main/hvac_controller.c:64 | WARMUP_DUCT_TEMP_READY | 40 °C | Duct temperature that counts as "heater warm" for ending warm-up | USER | Comfort, "Cold start duct warm", 20, 70, 1, 40 |
| 12 | main/hvac_controller.c:67 | SENSOR_FALLBACK_COOLANT_PCT | 50 % | Coolant valve while the cabin sensor is lost (S-08) | USER | Climate, "Heat w/o cabin sensor", 0, 100, 5, 50 |
| 13 | main/hvac_controller.c:69 | DEFOG_EXIT_HYSTERESIS_C | 2.0 °C | How far below the trigger the dew-point defog releases (sibling of SET_DEFOG_MARGIN) | USER | Comfort, "Defog release band", 0.5, 5, 0.5, 2 |
| 14 | main/hvac_controller.c:70 | DEFOG_MIN_WINDSCREEN_PCT | 50 % | Windscreen valve floor while defog is active | USER | Comfort, "Defog windscreen air", 30, 100, 10, 50 |
| 15 | main/hvac_controller.c:71 | ECO_FAN_CAP_PCT | 60 % (= Medium) | AUTO blower cap under the Eco preset | USER | Fan, "Eco preset fan cap", 30, 100, 5, 60 |
| 16 | main/hvac_controller.c:500-501 | FACE_FOOT split | 70 / 70 % | Centre and foot valve in Face+Foot mode | USER | Climate, "Face+Foot split", 10, 100, 10, 70 |
| 17 | main/hvac_controller.c:513 | DEFROST_FOOT foot share | 50 % | Foot valve in Defrost+Foot mode (windscreen stays 100) | USER | Climate, "Defrost+Foot foot air", 0, 100, 10, 50 |
| 18 | main/hvac_controller.c:752 | coolant at setpoint | 10 % | Coolant trickle when the setpoint is reached (classic law) | USER | Control, "Coolant at setpoint", 0, 30, 1, 10 |
| 19 | main/hvac_controller.c:770, :775 | AUTO fan full-speed knee | 5.0 °C | Cabin error at which the AUTO fan reaches 100 % | USER | Fan, "AUTO full fan at error", 2, 10, 0.5, 5 |
| 20 | main/hvac_controller.c:869 | feet-first threshold | 18.0 °C | AUTO heating with cabin below this sends 100 % to the feet | USER | Climate, "Feet-first below", 10, 25, 0.5, 18 |
| 21 | main/hvac_controller.c:890-891 | AUTO windscreen on humidity | > 70 %RH → 50 % | Windscreen valve share added in AUTO when the cabin is humid | USER | Climate, "Windscreen air above RH", 50, 95, 5, 70 |
| 22 | main/presets.c:6 | MAX_HEAT setpoint | 30 °C | Setpoint applied by the Max heat preset | USER | Climate, "Max heat preset", 25, 30, 0.5, 30 |
| 23 | main/presets.c:7 | MAX_COOL setpoint | 16 °C | Setpoint applied by the Max cool preset | USER | Climate, "Max cool preset", 16, 22, 0.5, 16 |
| 24 | main/hvac_ui.c:36 | UI_SCREEN_OFF_DEFAULT_S | 30 s | Idle time before the night screen-off when auto-dim is 0 | USER | Display, "Night screen off after", 10, 600, 10, 30 |
| 25 | main/hvac_ui_diag.c:247 (:246) | CHART_PERIOD_MS, CHART_POINTS | 10000 ms × 60 | Trend chart sample interval; span = 60 points | USER | Display, "Trend point every", 5, 60, 5, 10 (s) |
| 26 | main/wifi_ap.c:30 | AP_IDLE_TIMEOUT_US | 10 min | "10 min" mode: AP off after this long with no phone | USER | Connect, "Wi-Fi auto-off after", 1, 60, 1, 10 (min) |
| 27 | LVGL default (lv_hal_indev.c:64), main.c:265-271 | long_press_time | 400 ms | Hold time before AUTO opens presets, MODE opens diagnostics, +/- start repeating | USER | Display, "Hold delay", 200, 1000, 50, 400 (ms) |
| 28 | LVGL default (lv_hal_indev.c:65), main.c:265-271 | long_press_repeat_time | 100 ms | Repeat rate of held +/- buttons | USER | Display, "Hold repeat", 50, 500, 10, 100 (ms) |
| 29 | main/hvac_controller.c:845-851 | warm-up distribution | coolant 100, windscreen 100, foot 70, centres 30 % | Valve set during the cold-start phase | SERVICE | Service, "Warm-up foot %", 0, 100, 10, 70 and "Warm-up centre %", 0, 100, 10, 30 |
| 30 | main/hvac_controller.c:871-874 | AUTO cold heating | centres 30, foot 100, windscreen 30 % | Distribution while heating a cold cabin | SERVICE | Service, "Cold heat centre %", 0, 100, 10, 30 and "Cold heat windscreen %", 0, 100, 10, 30 |
| 31 | main/hvac_controller.c:877-880 | AUTO cooling | centres 100, foot 30, windscreen 0 % | Distribution while cooling | SERVICE | Service, "Cooling foot %", 0, 100, 10, 30 |
| 32 | main/hvac_controller.c:883-886, :519-521 | AUTO balanced / AUTO entry | centres 50, foot 50, windscreen 0 % | Distribution at setpoint and on entering AUTO | SERVICE | Service, "Balanced centre/foot %", 0, 100, 10, 50 |
| 33 | main/hvac_hw_config.h:181-185 | VALVE_*_ENABLED | 1 ×5 | A valve set to 0 is never driven, calibrated or self-tested | SERVICE | Service, "Fitted: centre 1 / centre 2 / foot / windscreen / coolant", bool, 1 |
| 34 | main/tb6612fng_driver.c:79 (:581) | VALVE_POSITION_TOLERANCE | 2 % | Error window that counts as "at target" (pot % in the loop, open % in set-target: C-03) | SERVICE | Service, "Position tolerance", 1, 10, 1, 2 |
| 35 | main/tb6612fng_driver.c:80 | VALVE_HYSTERESIS | 2 % | Extra error before a stopped valve restarts | SERVICE | Service, "Restart hysteresis", 0, 10, 1, 2 |
| 36 | main/tb6612fng_driver.c:81 | VALVE_STALL_TIMEOUT_MS | 6000 ms | No pot movement for this long while driving = stall | SERVICE | Service, "Stall timeout", 2, 20, 1, 6 (s) |
| 37 | main/tb6612fng_driver.c:82 (:76) | VALVE_MAX_MOVE_MS | 20000 ms (2 × 10000) | One move in one direction longer than this = stall | SERVICE | Service, "Max move time", 5, 60, 5, 20 (s) |
| 38 | main/tb6612fng_driver.c:85 | VALVE_STALL_RETRIES | 3 | Retries after a stall before the valve latches until a mode change | SERVICE | Service, "Stall retries", 0, 10, 1, 3 |
| 39 | main/tb6612fng_driver.c:86 (:1090) | VALVE_STALL_RETRY_MS | 10000 ms × attempt | Back-off between stall retries | SERVICE | Service, "Stall retry wait", 5, 60, 5, 10 (s) |
| 40 | main/tb6612fng_driver.c:88 | VALVE_MIN_SPEED | 30 PWM | Lowest butterfly PWM near the target (stiction) | SERVICE | Service, "Butterfly min speed", 10, 150, 5, 30 |
| 41 | main/tb6612fng_driver.c:89 | VALVE_RAMP_START_ERROR | 40 % | Error below which butterfly speed ramps down | SERVICE | Service, "Butterfly slow band", 5, 60, 5, 40 |
| 42 | main/tb6612fng_driver.c:92 | VALVE_EMA_ALPHA | 128/256 | Position feedback filter strength | SERVICE | Service, "Feedback filter", 32, 255, 16, 128 |
| 43 | main/tb6612fng_driver.c:104 | COOLANT_MIN_SPEED | 100 PWM | Lowest coolant-valve PWM near the target | SERVICE | Service, "Coolant min speed", 10, 255, 5, 100 |
| 44 | main/tb6612fng_driver.c:105 | COOLANT_RAMP_START_ERROR | 20 % | Error below which coolant speed ramps down | SERVICE | Service, "Coolant slow band", 5, 60, 5, 20 |
| 45 | main/tb6612fng_driver.c:203 | SERVICE_JOG_SPEED | 40 PWM | Jog speed on the valve service page | SERVICE | Service, "Jog speed", 20, 255, 5, 40 |
| 46 | main/tb6612fng_driver.c:205 | SERVICE_LEARN_MS | 800 ms | Length of the learn-direction move | SERVICE | Service, "Learn-direction move", 300, 3000, 100, 800 (ms) |
| 47 | main/tb6612fng_driver.c:1166 | CALIB_MAX_ROTATION_TIME | 120000 ms | Calibration gives up after this per phase | SERVICE | Service, "Calibration timeout", 30, 300, 10, 120 (s) |
| 48 | main/tb6612fng_driver.c:1168 | CALIB_MOTOR_SPEED | 50 PWM | Motor speed during calibration | SERVICE | Service, "Calibration speed", 20, 255, 5, 50 |
| 49 | main/hvac_controller.c:46 | BASE_COOLANT_GAIN | 20 %/°C | Classic law: coolant valve % per °C of cabin error (before adaptation) | ADVANCED | Advanced, "Classic heat gain", 5, 50, 1, 20 |
| 50 | main/hvac_controller.c:47 | MIN_COOLANT_GAIN | 10 %/°C | Lower clamp of the adapted gain | ADVANCED | Advanced, "Heat gain minimum", 5, 50, 1, 10 |
| 51 | main/hvac_controller.c:48 | MAX_COOLANT_GAIN | 50 %/°C | Upper clamp of the adapted gain | ADVANCED | Advanced, "Heat gain maximum", 10, 100, 1, 50 |
| 52 | main/hvac_controller.c:53 | AC_MIN_OFF_MS | 10000 ms | Clutch stays off at least this long (compressor protection) | ADVANCED | Advanced, "A/C minimum off", 5, 60, 5, 10 (s) |
| 53 | main/hvac_controller.c:54 | AC_MIN_ON_MS | 30000 ms | Humidity-only clutch stays on at least this long | ADVANCED | Advanced, "A/C min on (humidity)", 10, 300, 10, 30 (s) |
| 54 | main/wifi_ap.c:28 | AP_CHANNEL | 1 | Wi-Fi channel of the phone AP | ADVANCED | Advanced, "Wi-Fi channel", 1, 13, 1, 1 |
| 55 | main/io_hw.c:173 (node) | PWM_INIT range | 1..200 kHz | Accepted PWM_INIT frequencies | KEEP | Protocol guard, not a behaviour knob |
| 56 | main/io_i2c.c:28 (node) | I2C_FAILS_BEFORE_RESET | 3 | Bus errors in a row before an I2C reset | KEEP | Sized to the display's two BME280 probes per cycle; must change with hvac_sensors.c:573-583 |
| 57 | main/io_i2c.c:35, :37 (node) | ADS PGA / data rate | ±4.096 V, 860 SPS | ADC range and speed | KEEP | The display's mV maths (ads1115_driver.c:102) assumes 4.096 V; 860 SPS keeps the reply inside 200 ms |
| 58 | main/io_i2c.c:39-41 (node) | ADS wait/poll | 1 ms + 100 × 100 µs | Conversion wait (≤ 20 ms worst) | KEEP | Bounded by the display's 200 ms command timeout |
| 59 | main/io_onewire.c:22 (node) | DS18B20_READ_ATTEMPTS | 3 | Scratchpad retries on CRC error | KEEP | ~45 ms worst case inside the 200 ms timeout |
| 60 | main/io_onewire.c:25 (node) | ONEWIRE_SEARCH_BUDGET_US | 100 ms | No new ROM started after this during a search | KEEP | Pairs with the display's 500 ms search timeout (remote_io_binary.c:48) and the README |
| 61 | main/io_status_led.c:22-25 (node) | LED tick, flash, task | 100 ms, 300 ms | Status LED timing | KEEP | Board option, LED disabled (NODE_STATUS_LED_GPIO -1) |
| 62 | main/node_config.h:22 (node) | OTA_CONFIRM_TIMEOUT_MS | 60000 ms | New node image rolls back unless PING + VERSION arrive in time | KEEP | Rollback safety; documented in both READMEs and DECISIONS.md:55 |
| 63 | main/node_config.h:33 (node) | LINK_FRAME_TIMEOUT_MS | 20 ms | Partial frame discarded after this silence | KEEP | Same value as the display's FRAME_TIMEOUT_MS; both ends must agree |
| 64 | main/node_config.h:34 (node) | LINK_WATCHDOG_MS | 3000 ms | All outputs safe after this silence from the display | KEEP | Safety. Equals the display's "connected" window (remote_io_binary.c:733) and must stay above the 1 s re-assert (motor_link.h:23). Settable over the link only with a new command |
| 65 | main/node_config.h:35 (node) | LINK_REPLY_ON_CRC_ERROR | 0 | Answer corrupt frames with error 6 | KEEP | N-07 held; a reply to a corrupt frame can collide with the next command |
| 66 | main/node_config.h:36 (node) | LINK_PAD_TRAILING_ZERO | 1 | Pad replies until HELLO (protocol 1 displays) | KEEP | Protocol compatibility; the display's strip rule depends on it |
| 67 | main/node_config.h:42-44 (node) | I2C 400 kHz, 20 ms, 8 devices | | Bus speed, transaction timeout, device cache | KEEP | Hardware; the display assumes nothing else |
| 68 | main/node_config.h:55, :60, :88 (node) | boot output / PWM / allowed pins | pin tables | Which pins the node drives and accepts | KEEP | Copy of the display's hvac_hw_config.h pin map; must change together |
| 69 | main/node_config.h:61 (node) | NODE_DEFAULT_PWM_KHZ | 20 kHz | Frequency for a PWM_SET without PWM_INIT (all valve PWMs) | KEEP | Motor-driver and audible-noise choice; the display never sends PWM_INIT for valves |
| 70 | main/node_config.h:77-78 (node) | NODE_STATUS_LED_GPIO, brightness | -1, 24 | Optional status LED | KEEP | Board option |
| 71 | main/protocol_server.c:41-46 (node) | task stack/prio, queue, read chunk, watchdog period | 6144, 10, 20, 128, 100 ms | Link task sizing | KEEP | Timing/RAM |
| 72 | main/protocol_server.c:465 (node) | uart_set_rx_timeout | 4 symbols | How soon received bytes are delivered | KEEP | Link latency |
| 73 | sdkconfig (node) | CONFIG_ESP_TASK_WDT_TIMEOUT_S | 5 s | Node reboots if the link task hangs | KEEP | Safety; README:193 |
| 74 | main/ads1115_driver.c:102 | mV conversion | 4096 / 32767 | Raw ADC to mV | KEEP | Matches the node's PGA (#57) |
| 75 | main/ads1115_driver.c:106, :120 | full scale | 3300 mV = 100 % | Pot supply voltage | KEEP | Hardware |
| 76 | main/ads1115_driver.c:115 | noise floor | < 50 mV = 0 % | Floating channel reads 0 | KEEP | Hardware noise |
| 77 | main/bts7960_driver.c:35 | BLOWER_TASK_PERIOD_MS | 20 ms | Ramp step period | KEEP | Timing |
| 78 | main/bts7960_driver.c:36 (:188) | DEFAULT_RAMP_TIME_MS | 500 ms | Ramp used by bts7960_stop (mode OFF) | KEEP | Duplicate of SET_SOFT_START_MS's default; the stop path should read the setting (see section 3) |
| 79 | main/bts7960_driver.c:87 | blower PWM | 25 kHz, 10-bit | PWM_INIT sent to the node for GPIO48 | KEEP | Same number in node_config.h:60 and node README:26 |
| 80 | main/bts7960_driver.h:18 | BTS7960_FAN_PERCENT_LOW | 30 % | Manual Low duty and the A/C blower interlock threshold (hvac_controller.c:986) | KEEP | Coupled to SET_FAN_MIN_DUTY's default and the compressor interlock; see section 3 |
| 81 | main/bts7960_driver.h:22 | BTS7960_FAN_PERCENT_HIGH | 100 % | Manual High | KEEP | SET_FAN_MAX_DUTY already caps it |
| 82 | main/cobs_protocol.h:31, :38, :44, :45 | payload 64, PROTOCOL_VERSION 2, 4 ROMs, OTA_DATA_MAX 56 | | Wire protocol | KEEP | Identical copy in the node repo |
| 83 | main/comfort.c:6-7 | MAGNUS_B, MAGNUS_C | 17.62, 243.12 | Dew-point formula | KEEP | Physics |
| 84 | main/hvac_controller.c:35 | HVAC_TASK_PERIOD_MS | 100 ms | Control loop period | KEEP | Cascade dt and thermal sampling assume it |
| 85 | main/hvac_controller.c:37-38 | TEMP_MIN_C, TEMP_MAX_C | 16, 30 °C | Setpoint limits | KEEP | Also the SET_DEFAULT_SETPOINT range (settings_table.c:77) and the preset setpoints; change in code, all three together |
| 86 | main/hvac_controller.c:44-45, :49 | THERMAL_SAMPLE_PERIOD, LEARN_SAMPLES, LEARNING_RATE | 2 s, 30, 0.1 | Thermal adaptation sampling | KEEP | C-05 held; the model's shape, not a user knob |
| 87 | main/hvac_controller.c:55-56, :997 | AC_FREEZE_CUTOUT/CUTIN, coolant ≤ 5 % | 3 / 6 °C | Evaporator freeze protection | KEEP | Safety of the A/C hardware |
| 88 | main/hvac_controller.c:85 | SETTINGS_SAVE_DELAY_MS | 5000 ms | Mode/setpoint saved to NVS 5 s after a change | KEEP | Flash wear vs loss on power cut |
| 89 | main/hvac_controller.c:224-226 | curve_k | 0.6 / 1.0 / 1.4 | Meaning of the Eco/Normal/Max fan-curve choice | KEEP | Definitions of an existing enum setting |
| 90 | main/hvac_controller.c:342 | reassert_outputs | 1000 ms | Relay and blower re-sent once a second | KEEP | Link; below the node watchdog |
| 91 | main/hvac_controller.c:423-455, :433, :472 | airflow normaliser 150, +0.15, 0.2, 0.4, 0.9, 400; rate window 0.05..5; save every 50 | | Thermal adaptation internals | KEEP | Interdependent; tune in code |
| 92 | main/hvac_controller.c:589 | cfg refresh | 1000 ms | Safety-net re-read of settings | KEEP | Timing |
| 93 | main/hvac_controller.c:615 | boot grace | 10000 ms | Sensors "not found" are not a fault in the first 10 s | KEEP | Matches sensor start-up |
| 94 | main/hvac_controller.c:860 | warm-up log | 10 s | Console only | KEEP | Logging |
| 95 | main/hvac_controller.c:1066 | initial setpoint | 22 °C | Overwritten by SET_DEFAULT_SETPOINT two lines later | KEEP | Dead literal; delete |
| 96 | main/hvac_controller.c:1274 | temp_step rounding | 0.01 | Setpoint rounded to 2 decimals | KEEP | Numeric hygiene |
| 97 | main/hvac_hw_config.h:66 | BACKLIGHT_FREQ_HZ | 10 kHz | Backlight PWM | KEEP | Hardware |
| 98 | main/hvac_hw_config.h:121 | AC_RELAY_ACTIVE_HIGH | 1 | Relay polarity | KEEP | Wiring; a wrong value runs the compressor at boot |
| 99 | main/hvac_hw_config.h:150 | REMOTE_IO_BAUD_RATE | 921600 | Link speed | KEEP | Both ends |
| 100 | main/hvac_sensors.c:42, :44 | sensor_interval_ms, CALIB_SENSOR_INTERVAL_MS | 100 / 25 ms | Valve feedback rate, normal / calibrating | KEEP | Control-loop timing |
| 101 | main/hvac_sensors.c:45 | SENSOR_STALE_MS | 5000 ms | Reading older than this is "no reading" | KEEP | S-08 fallback timing |
| 102 | main/hvac_sensors.c:130-131 | SENSOR_MAX_RATE_C_PER_S, SENSOR_REJECT_LIMIT | 5 °C/s, 3 | Plausibility filter | KEEP | SN-02; sensor physics |
| 103 | main/hvac_sensors.c:174 | DS18B20_CONVERSION_MS | 750 ms | Wait after Convert T | KEEP | Sensor specification |
| 104 | main/hvac_sensors.c:216-217, :646 | role ranges, BME range | -40..100/125 °C, -40..85 °C | Accepted temperatures | KEEP | Sensor specifications |
| 105 | main/hvac_sensors.c:341, :375, :574 | boot grace | 5000 ms | Read errors in the first 5 s are "not found" | KEEP | Start-up |
| 106 | main/hvac_sensors.c:351, :377 | 85 °C power-on check | 85 unless last > 75 | DS18B20 reset value ignored | KEEP | Sensor behaviour |
| 107 | main/hvac_sensors.c:600, :611, :619 | BME280 oversampling, forced mode, 10 ms wait | x1 | Sensor setup | KEEP | Hardware |
| 108 | main/hvac_sensors.c:681 | VALVE_FEEDBACK_FAIL_LIMIT | 3 | Failed reads before "feedback lost" | KEEP | Link noise tolerance |
| 109 | main/hvac_sensors.c:832, :840 | temperature cycle, node poll | 1000 / 10000 ms | Sensor cadence | KEEP | Timing |
| 110 | main/hvac_sensors.h:117 | doc "(50ms)" | 50 | Comment for the fast read mode | KEEP | Comment is wrong: code is 25 ms (section 4) |
| 111 | main/hvac_ui.c:30-32 | UI_DIAG_REFRESH_MS, UI_STATS_LOG_MS, UI_TAP_SLOP | 750 ms, 5 s, 10 px | Diagnostics refresh, timing log, tap slop | KEEP | UI timing; slop tuned to the panel |
| 112 | main/hvac_ui_diag.c:21, :117, :246, :270 | page refresh, log rebuild, chart points, default range | 750 ms, 2 s, 60, 15..30 °C | Diagnostics pages | KEEP | UI |
| 113 | main/hvac_ui_service.c:25 | JOG_REPEAT_MS | 150 ms | Jog resend period | KEEP | Must stay below the driver's 300 ms dead-man (tb6612fng_driver.c:204) |
| 114 | main/hvac_ui_service.c:26, :241; main/hvac_ui_settings.c:19 | page refresh | 250 / 500 / 500 ms | UI | KEEP | UI |
| 115 | main/main.c:62 | LVGL_TICK_PERIOD_MS | 2 ms | LVGL tick | KEEP | Timing |
| 116 | main/main.c:246-248 | draw buffers | 2 × full frame in PSRAM | Rendering | KEEP | Memory layout (DESIGN.md:307 says internal: section 4) |
| 117 | main/main.c:269-270 | gesture_limit, gesture_min_velocity | 30 px, 1 | Swipe detection | KEEP | Tuned for the panel (DESIGN.md:68) |
| 118 | main/main.c:304 | UI update period | 100 ms | Main screen refresh | KEEP | Equals the control period |
| 119 | main/main.c:364, :373 | remote_io_wait_ready | 3000 ms ×2 | Boot wait for the node, before and after its reset | KEEP | Boot |
| 120 | main/main.c:485 | status print | 10 s | Console only | KEEP | Logging |
| 121 | main/motor_link.h:23 | MOTOR_LINK_REFRESH_MS | 1000 ms | Every commanded valve re-asserted once a second | KEEP | Must stay well below the node watchdog (#64) |
| 122 | main/ota_display.c:25-26 | VALIDATE_LINK_UP_S, VALIDATE_GIVE_UP_S | 30 s, 5 min | New display image kept after 30 s of link; rollback after 5 min | KEEP | Rollback safety; README:43, DECISIONS.md:55 |
| 123 | main/remote_io_binary.c:45-48 | CMD_TIMEOUT 200, FRAME_TIMEOUT 20, LINK_LOCK 1000, SEARCH 500 ms | | Link timeouts | KEEP | Sized to the node's worst replies (#58-60); the node README quotes 200 ms |
| 124 | main/remote_io_binary.c:49-51, :886, :912 | OTA timeouts, retries, 200 ms settle | 1 s, 5 s, 30 s, 3 | Node OTA transfer | KEEP | Flash timings on the node |
| 125 | main/remote_io_binary.c:52, :58-60 | timeout event period, RTT histogram | 10 s; 6 × 10 s, 49 buckets | Diagnostics | KEEP | Logging/RAM |
| 126 | main/remote_io_binary.c:246, :714 | read slice, ping retry | 2 ms, 100 ms | Link polling | KEEP | Timing |
| 127 | main/remote_io_binary.c:733 | connected window | 3000 ms | "Link up" means a reply within this | KEEP | Equals the node watchdog (#64) |
| 128 | main/selftest.c:27-29 | settle, max wait, poll | 3 s, 60 s, 250 ms | Boot self-test timing | KEEP | 60 s is below the 120 s calibration timeout (#47): a slow calibration is reported as "stalled" (section 3) |
| 129 | main/settings.c:32 | PERSIST_DELAY_US | 2 s | Settings written 2 s after the first unwritten change | KEEP | Flash wear vs loss on power cut (DECISIONS.md:34 wording: section 4) |
| 130 | main/settings.c:184-190, :608-609 | humidity hysteresis gap | 2 %RH | OFF kept ≥ 2 below ON | KEEP | The controller uses 1 (hvac_controller.c:207): section 3 |
| 131 | main/settings.c:529 | AP password length | 10 chars | Generated default password | KEEP | Security floor |
| 132 | main/settings.h:36, main/valve_cal_store.h:23 | SETTINGS_SCHEMA, VALVE_CAL_SCHEMA | 1 | NVS migration versions | KEEP | Bump only with a migration |
| 133 | main/event_log.h:20-21; main/selftest.h:30-31 | 50 events × 48 chars; 12 items × 40 | | Log and self-test sizes | KEEP | RAM |
| 134 | main/sensor_roles.h:29 | SENSOR_ROLES_MAX_ROMS | 4 | DS18B20s tracked | KEEP | Equals the protocol's ROM list (cobs_protocol.h:44) |
| 135 | main/tb6612fng_driver.c:77 | VALVE_TASK_PERIOD_MS | 25 ms | Valve loop period | KEEP | Timing |
| 136 | main/tb6612fng_driver.c:78, :103 | VALVE_MOTOR_SPEED, COOLANT_MOTOR_SPEED | 80 / 255 PWM | Default max speed when SET_VALVE_SPEED_n is 0 | KEEP | Already exposed per valve (0 = this) |
| 137 | main/tb6612fng_driver.c:83-84 | VALVE_WRAP_BLOCK_MS, VALVE_WRAP_CLEAR_NEAR_PCT | 5 s, 9 % | Pot wrap-around handling | KEEP | Internal to the butterfly logic |
| 138 | main/tb6612fng_driver.c:87, :95, :99 | movement threshold, outlier threshold, wrap threshold | 2, 15, 40 % | Feedback noise handling | KEEP | Tied to the EMA filter and pot noise |
| 139 | main/tb6612fng_driver.c:90 | VALVE_DEBUG_LOG_MS | 300 ms | Console only | KEEP | Logging |
| 140 | main/tb6612fng_driver.c:91, :96 | VALVE_PLATEAU_ZONE, VALVE_DEADZONE_ADC | 5, 5 | Nothing: defined only | KEEP | Dead constants; delete |
| 141 | main/tb6612fng_driver.c:94, :124-129, :134-139, :146 | default centre 50, default calibration table, default inversion, last-stable 50 | | Used only before the first calibration | KEEP | Replaced by the stored calibration |
| 142 | main/tb6612fng_driver.c:97-98 | VALVE_DEADZONE_LOW/HIGH_PCT | 2 % | Targets clamped to 2..98 % open | KEEP | Overlaps SET_SAFE_MARGIN_PCT |
| 143 | main/tb6612fng_driver.c:100 | VALVE_BOUNDARY_SAFE_MARGIN | 1 % | Initial value of the safe margin | KEEP | SET_SAFE_MARGIN_PCT's default |
| 144 | main/tb6612fng_driver.c:204 | SERVICE_JOG_TIMEOUT_MS | 300 ms | Jog dead-man | KEEP | Must stay above the UI's 150 ms resend (#113) |
| 145 | main/tb6612fng_driver.c:833, :836-838, :878, :1342, :1665-1667 | side latch ≥ 98, side hysteresis ±3, raw/filtered 20, post-cal position 50, outlier drift 64 | | Butterfly V-shape internals | KEEP | Interdependent |
| 146 | main/tb6612fng_driver.c:1163-1165, :1167, :1169, :1316 | settle 500 ms, marker 60 ± 2, min samples 200, min range 20, record dead zone 3 | | Calibration internals | KEEP | Interdependent; the dead zone is no longer used by the plausibility check |
| 147 | main/valve_cal_store.c:124 | plausibility slack | ± 3 % | Stored calibration accepted within this of its range | KEEP | Finding 2 of the second review |
| 148 | main/web_server.c:37, :39, :594; main/web/index.html:139, :153, :158 | node image max 1 MB, 3 retries × 10 s, page polls 1 s / 2 s | | Phone page | KEEP | 1 MB appears three times (web_server.c:37, index.html:139, node slot 960 KB in partitions.csv:6-7) |
| 149 | main/wifi_ap.c:29 | AP_MAX_CLIENTS | 2 | Phones at once | KEEP | Internal RAM |
| 150 | sdkconfig (display) | CONFIG_ESP_TASK_WDT_TIMEOUT_S 10, CONFIG_LV_DISP_DEF_REFR_PERIOD 30, CONFIG_LV_INDEV_DEF_READ_PERIOD 30, CONFIG_FREERTOS_HZ 1000 | | Watchdog, LVGL frame and touch rate, tick | KEEP | Timing; the valve task's link-down pass (second review, finding 1) is the one that approaches 10 s |

Not in any table because they do not exist: a cabin or duct temperature offset (no constant,
no setting; the only correction is the BME280's own compensation), and a per-valve
end-stop/travel time (the driver uses stalls instead).

## 2. Ready-to-paste settings entries

Two new groups keep SERVICE and ADVANCED rows off the main list; the display's
`k_groups[]` (hvac_ui_settings.c:27) and the console's `k_group_names[]` (console_cmds.c)
then need two more names, and the display should not list `SET_GROUP_ADVANCED`.
Ids are appended before `SET_COUNT` (append only). Every default below equals today's constant.
The apply callbacks named exist; the modules must read the new ids for them to take effect.

```c
/* settings.h: in setting_group_t, before SET_GROUP_COUNT */
    SET_GROUP_SERVICE,          /* valve service page only, mode OFF */
    SET_GROUP_ADVANCED,         /* phone page only */

/* settings.h: in setting_id_t, before SET_COUNT */
    /* USER */
    SET_TEMP_DEADBAND,          /* float degC, 0.2..2.0 step 0.1, default 0.5 (TEMP_DEADBAND) */
    SET_FEET_FIRST_BELOW,       /* float degC, 10..25 step 0.5, default 18 */
    SET_AUTO_WS_RH,             /* int %RH, 50..95 step 5, default 70 */
    SET_SENSOR_FALLBACK_HEAT,   /* int %, 0..100 step 5, default 50 */
    SET_FACE_FOOT_SPLIT,        /* int %, 10..100 step 10, default 70 */
    SET_DEF_FOOT_PCT,           /* int %, 0..100 step 10, default 50 */
    SET_PRESET_HEAT_C,          /* float degC, 25..30 step 0.5, default 30 */
    SET_PRESET_COOL_C,          /* float degC, 16..22 step 0.5, default 16 */
    SET_COLD_START_BELOW,       /* float degC, 0..20 step 1, default 15 */
    SET_COLD_START_MIN_S,       /* int s, 0..600 step 30, default 60 */
    SET_COLD_START_MAX_MIN,     /* int min, 1..15 step 1, default 5 */
    SET_COLD_START_END_C,       /* float degC, 10..25 step 0.5, default 18 */
    SET_COLD_START_END_RH,      /* int %RH, 30..90 step 5, default 60 */
    SET_COLD_START_DUCT_C,      /* float degC, 20..70 step 1, default 40 */
    SET_DEFOG_RELEASE_C,        /* float degC, 0.5..5 step 0.5, default 2 */
    SET_DEFOG_WS_PCT,           /* int %, 30..100 step 10, default 50 */
    SET_ECO_FAN_CAP,            /* int %, 30..100 step 5, default 60 */
    SET_AUTO_FAN_FULL_C,        /* float degC, 2..10 step 0.5, default 5 */
    SET_FAN_LEVEL_MLOW,         /* int %, 10..100 step 5, default 45 */
    SET_FAN_LEVEL_MED,          /* int %, 10..100 step 5, default 60 */
    SET_FAN_LEVEL_MHIGH,        /* int %, 10..100 step 5, default 80 */
    SET_AUTO_FAN_NO_SENSOR,     /* int %, 0..100 step 5, default 50 */
    SET_COOLANT_IDLE_PCT,       /* int %, 0..30 step 1, default 10 */
    SET_HOLD_DELAY_MS,          /* int ms, 200..1000 step 50, default 400 */
    SET_HOLD_REPEAT_MS,         /* int ms, 50..500 step 10, default 100 */
    SET_NIGHT_OFF_S,            /* int s, 10..600 step 10, default 30 */
    SET_CHART_INTERVAL_S,       /* int s, 5..60 step 5, default 10 */
    SET_AP_IDLE_MIN,            /* int min, 1..60 step 1, default 10 */
    /* SERVICE */
    SET_VALVE_FITTED_0,         /* bool, default yes; one per valve */
    SET_VALVE_FITTED_1,
    SET_VALVE_FITTED_2,
    SET_VALVE_FITTED_3,
    SET_VALVE_FITTED_4,
    SET_WARMUP_FOOT_PCT,        /* int %, 0..100 step 10, default 70 */
    SET_WARMUP_CENTRE_PCT,      /* int %, 0..100 step 10, default 30 */
    SET_COLD_HEAT_CENTRE_PCT,   /* int %, 0..100 step 10, default 30 */
    SET_COLD_HEAT_WS_PCT,       /* int %, 0..100 step 10, default 30 */
    SET_COOLING_FOOT_PCT,       /* int %, 0..100 step 10, default 30 */
    SET_BALANCED_PCT,           /* int %, 0..100 step 10, default 50 */
    SET_VALVE_TOL_PCT,          /* int %, 1..10 step 1, default 2 */
    SET_VALVE_HYST_PCT,         /* int %, 0..10 step 1, default 2 */
    SET_VALVE_STALL_S,          /* int s, 2..20 step 1, default 6 */
    SET_VALVE_MOVE_MAX_S,       /* int s, 5..60 step 5, default 20 */
    SET_VALVE_STALL_RETRIES,    /* int, 0..10 step 1, default 3 */
    SET_VALVE_RETRY_S,          /* int s, 5..60 step 5, default 10 */
    SET_VALVE_MIN_PWM,          /* int PWM, 10..150 step 5, default 30 */
    SET_VALVE_RAMP_PCT,         /* int %, 5..60 step 5, default 40 */
    SET_VALVE_FILTER,           /* int /256, 32..255 step 16, default 128 */
    SET_COOLANT_MIN_PWM,        /* int PWM, 10..255 step 5, default 100 */
    SET_COOLANT_RAMP_PCT,       /* int %, 5..60 step 5, default 20 */
    SET_JOG_PWM,                /* int PWM, 20..255 step 5, default 40 */
    SET_LEARN_MS,               /* int ms, 300..3000 step 100, default 800 */
    SET_CAL_SPEED_PWM,          /* int PWM, 20..255 step 5, default 50 */
    SET_CAL_TIMEOUT_S,          /* int s, 30..300 step 10, default 120 */
    /* ADVANCED */
    SET_HEAT_GAIN,              /* float %/degC, 5..50 step 1, default 20 */
    SET_HEAT_GAIN_MIN,          /* float %/degC, 5..50 step 1, default 10 */
    SET_HEAT_GAIN_MAX,          /* float %/degC, 10..100 step 1, default 50 */
    SET_AC_MIN_OFF_S,           /* int s, 5..60 step 5, default 10 */
    SET_AC_MIN_ON_S,            /* int s, 10..300 step 10, default 30 */
    SET_AP_CHANNEL,             /* int, 1..13 step 1, default 1 */
```

```c
/* settings_table.c: next to the other group defines */
#define SV SET_GROUP_SERVICE
#define AD SET_GROUP_ADVANCED

/* settings_table.c: inside g_settings_table[], after SET_NODE_REBOOT */
    /* USER (findings: INVENTORY.md section 1) */
    FLT (SET_TEMP_DEADBAND,     CL, "Setpoint deadband", "Setpunkt dødbånd", 0.2f, 2.0f, 0.1f, 0.5f, "°C", "deadband", apply_controller),
    FLT (SET_FEET_FIRST_BELOW,  CL, "Feet-first below", "Fod først under", 10, 25, 0.5f, 18, "°C", "feet_below", apply_controller),
    INT (SET_AUTO_WS_RH,        CL, "Windscreen air above", "Rudeluft over", 50, 95, 5, 70, "%RH", "auto_ws_rh", apply_controller),
    INT (SET_SENSOR_FALLBACK_HEAT, CL, "Heat w/o cabin sensor", "Varme u. kabinesensor", 0, 100, 5, 50, "%", "fallback_heat", apply_controller),
    INT (SET_FACE_FOOT_SPLIT,   CL, "Face+Foot split", "Ansigt+fod fordeling", 10, 100, 10, 70, "%", "ff_split", apply_controller),
    INT (SET_DEF_FOOT_PCT,      CL, "Defrost+Foot foot air", "Afrim+fod fodluft", 0, 100, 10, 50, "%", "def_foot", apply_controller),
    FLT (SET_PRESET_HEAT_C,     CL, "Max heat preset", "Maks varme forvalg", 25, 30, 0.5f, 30, "°C", "preset_heat", NULL),
    FLT (SET_PRESET_COOL_C,     CL, "Max cool preset", "Maks kold forvalg", 16, 22, 0.5f, 16, "°C", "preset_cool", NULL),
    FLT (SET_COLD_START_BELOW,  CF, "Cold start below", "Koldstart under", 0, 20, 1, 15, "°C", "cold_below", apply_controller),
    INT (SET_COLD_START_MIN_S,  CF, "Cold start minimum", "Koldstart minimum", 0, 600, 30, 60, "s", "cold_min_s", apply_controller),
    INT (SET_COLD_START_MAX_MIN, CF, "Cold start maximum", "Koldstart maksimum", 1, 15, 1, 5, "min", "cold_max_min", apply_controller),
    FLT (SET_COLD_START_END_C,  CF, "Cold start ends at", "Koldstart slut ved", 10, 25, 0.5f, 18, "°C", "cold_end_c", apply_controller),
    INT (SET_COLD_START_END_RH, CF, "Cold start ends below", "Koldstart slut under", 30, 90, 5, 60, "%RH", "cold_end_rh", apply_controller),
    FLT (SET_COLD_START_DUCT_C, CF, "Cold start duct warm", "Koldstart kanal varm", 20, 70, 1, 40, "°C", "cold_duct_c", apply_controller),
    FLT (SET_DEFOG_RELEASE_C,   CF, "Defog release band", "Afdug slip-bånd", 0.5f, 5, 0.5f, 2, "°C", "defog_release", apply_controller),
    INT (SET_DEFOG_WS_PCT,      CF, "Defog windscreen air", "Afdug rudeluft", 30, 100, 10, 50, "%", "defog_ws", apply_controller),
    INT (SET_ECO_FAN_CAP,       FN, "Eco preset fan cap", "Eco-blæsergrænse", 30, 100, 5, 60, "%", "eco_cap", apply_controller),
    FLT (SET_AUTO_FAN_FULL_C,   FN, "AUTO full fan at", "AUTO fuld blæser ved", 2, 10, 0.5f, 5, "°C", "auto_full_c", apply_controller),
    INT (SET_FAN_LEVEL_MLOW,    FN, "Fan level Med-low", "Blæser mellem-lav", 10, 100, 5, 45, "%", "fan_mlow", apply_controller),
    INT (SET_FAN_LEVEL_MED,     FN, "Fan level Medium", "Blæser mellem", 10, 100, 5, 60, "%", "fan_med", apply_controller),
    INT (SET_FAN_LEVEL_MHIGH,   FN, "Fan level Med-high", "Blæser mellem-høj", 10, 100, 5, 80, "%", "fan_mhigh", apply_controller),
    INT (SET_AUTO_FAN_NO_SENSOR, FN, "AUTO fan, no sensor", "AUTO-blæser u. sensor", 0, 100, 5, 50, "%", "auto_fan_nosens", apply_controller),
    INT (SET_COOLANT_IDLE_PCT,  CT, "Coolant at setpoint", "Kølevand ved setpunkt", 0, 30, 1, 10, "%", "coolant_idle", apply_controller),
    INT (SET_HOLD_DELAY_MS,     D,  "Hold delay", "Holdeforsinkelse", 200, 1000, 50, 400, "ms", "hold_delay_ms", NULL),
    INT (SET_HOLD_REPEAT_MS,    D,  "Hold repeat", "Holdegentagelse", 50, 500, 10, 100, "ms", "hold_rep_ms", NULL),
    INT (SET_NIGHT_OFF_S,       D,  "Night screen off after", "Nat: skærm fra efter", 10, 600, 10, 30, "s", "night_off_s", NULL),
    INT (SET_CHART_INTERVAL_S,  D,  "Trend point every", "Kurvepunkt hver", 5, 60, 5, 10, "s", "chart_s", NULL),
    INT (SET_AP_IDLE_MIN,       CN, "Wi-Fi auto-off after", "Wi-Fi auto-fra efter", 1, 60, 1, 10, "min", "ap_idle_min", apply_wifi),

    /* SERVICE: valve service page, mode OFF */
    BOOL(SET_VALVE_FITTED_0,    SV, "Fitted: center 1", "Monteret: midt 1", 1, "vfit0", apply_valves),
    BOOL(SET_VALVE_FITTED_1,    SV, "Fitted: center 2", "Monteret: midt 2", 1, "vfit1", apply_valves),
    BOOL(SET_VALVE_FITTED_2,    SV, "Fitted: foot", "Monteret: fod", 1, "vfit2", apply_valves),
    BOOL(SET_VALVE_FITTED_3,    SV, "Fitted: windscreen", "Monteret: rude", 1, "vfit3", apply_valves),
    BOOL(SET_VALVE_FITTED_4,    SV, "Fitted: coolant", "Monteret: kølevand", 1, "vfit4", apply_valves),
    INT (SET_WARMUP_FOOT_PCT,   SV, "Warm-up foot air", "Opvarmning fodluft", 0, 100, 10, 70, "%", "warm_foot", apply_controller),
    INT (SET_WARMUP_CENTRE_PCT, SV, "Warm-up centre air", "Opvarmning midtluft", 0, 100, 10, 30, "%", "warm_centre", apply_controller),
    INT (SET_COLD_HEAT_CENTRE_PCT, SV, "Cold heat centre air", "Kold varme midtluft", 0, 100, 10, 30, "%", "cold_centre", apply_controller),
    INT (SET_COLD_HEAT_WS_PCT,  SV, "Cold heat windscreen", "Kold varme rude", 0, 100, 10, 30, "%", "cold_ws", apply_controller),
    INT (SET_COOLING_FOOT_PCT,  SV, "Cooling foot air", "Køling fodluft", 0, 100, 10, 30, "%", "cool_foot", apply_controller),
    INT (SET_BALANCED_PCT,      SV, "Balanced centre/foot", "Balanceret midt/fod", 0, 100, 10, 50, "%", "balanced", apply_controller),
    INT (SET_VALVE_TOL_PCT,     SV, "Position tolerance", "Positionstolerance", 1, 10, 1, 2, "%", "v_tol", apply_valves),
    INT (SET_VALVE_HYST_PCT,    SV, "Restart hysteresis", "Genstart-hysterese", 0, 10, 1, 2, "%", "v_hyst", apply_valves),
    INT (SET_VALVE_STALL_S,     SV, "Stall timeout", "Stall-tid", 2, 20, 1, 6, "s", "v_stall_s", apply_valves),
    INT (SET_VALVE_MOVE_MAX_S,  SV, "Max move time", "Maks bevægelsestid", 5, 60, 5, 20, "s", "v_move_s", apply_valves),
    INT (SET_VALVE_STALL_RETRIES, SV, "Stall retries", "Stall-forsøg", 0, 10, 1, 3, NULL, "v_retries", apply_valves),
    INT (SET_VALVE_RETRY_S,     SV, "Stall retry wait", "Stall-ventetid", 5, 60, 5, 10, "s", "v_retry_s", apply_valves),
    INT (SET_VALVE_MIN_PWM,     SV, "Butterfly min speed", "Spjæld min. fart", 10, 150, 5, 30, NULL, "v_min_pwm", apply_valves),
    INT (SET_VALVE_RAMP_PCT,    SV, "Butterfly slow band", "Spjæld bremsebånd", 5, 60, 5, 40, "%", "v_ramp", apply_valves),
    INT (SET_VALVE_FILTER,      SV, "Feedback filter", "Positionsfilter", 32, 255, 16, 128, NULL, "v_filter", apply_valves),
    INT (SET_COOLANT_MIN_PWM,   SV, "Coolant min speed", "Kølevand min. fart", 10, 255, 5, 100, NULL, "c_min_pwm", apply_valves),
    INT (SET_COOLANT_RAMP_PCT,  SV, "Coolant slow band", "Kølevand bremsebånd", 5, 60, 5, 20, "%", "c_ramp", apply_valves),
    INT (SET_JOG_PWM,           SV, "Jog speed", "Jog-fart", 20, 255, 5, 40, NULL, "jog_pwm", apply_valves),
    INT (SET_LEARN_MS,          SV, "Learn-direction move", "Lær-retning bevægelse", 300, 3000, 100, 800, "ms", "learn_ms", apply_valves),
    INT (SET_CAL_SPEED_PWM,     SV, "Calibration speed", "Kalibreringsfart", 20, 255, 5, 50, NULL, "cal_pwm", apply_valves),
    INT (SET_CAL_TIMEOUT_S,     SV, "Calibration timeout", "Kalibreringstimeout", 30, 300, 10, 120, "s", "cal_timeout_s", apply_valves),

    /* ADVANCED: phone page only */
    FLT (SET_HEAT_GAIN,         AD, "Classic heat gain", "Klassisk varmegain", 5, 50, 1, 20, "%/°C", "heat_gain", apply_controller),
    FLT (SET_HEAT_GAIN_MIN,     AD, "Heat gain minimum", "Varmegain minimum", 5, 50, 1, 10, "%/°C", "heat_gain_min", apply_controller),
    FLT (SET_HEAT_GAIN_MAX,     AD, "Heat gain maximum", "Varmegain maksimum", 10, 100, 1, 50, "%/°C", "heat_gain_max", apply_controller),
    INT (SET_AC_MIN_OFF_S,      AD, "A/C minimum off", "A/C minimum fra", 5, 60, 5, 10, "s", "ac_min_off_s", apply_controller),
    INT (SET_AC_MIN_ON_S,       AD, "A/C min on (humidity)", "A/C min til (fugt)", 10, 300, 10, 30, "s", "ac_min_on_s", apply_controller),
    INT (SET_AP_CHANNEL,        AD, "Wi-Fi channel", "Wi-Fi-kanal", 1, 13, 1, 1, NULL, "ap_channel", apply_wifi),
```

Notes on the block: `SET_HEAT_GAIN_MAX` must stay ≥ `SET_HEAT_GAIN_MIN` and `SET_AC_MIN_*`
need no cross-check. The preset setpoints have no callback because `presets.c` holds a
`const` table today. A new Wi-Fi channel takes effect on the next AP start, like the
password. Danish labels are 22 characters or fewer, as `test_settings_table` requires.

## 3. Dead or mismatched settings

No row in `settings_table.c` is unreferenced; every id is read somewhere (the per-valve and
calibrate-action rows through `SET_..._0 + i`). These rows do not do what their range or
label promises:

1. **SET_FAN_MIN_DUTY** (settings_table.c:87, range 0..100): the controller reads it with a
   floor of 1 (hvac_controller.c:216), so a stored 0 silently becomes 30. Manual Low is the
   constant 30 (hvac_controller.c:268, bts7960_driver.h:18) and the A/C interlock is the
   same constant (hvac_controller.c:986), so below 30 the setting lowers only the AUTO fan
   floor, and the compressor drops out there.
2. **SET_SOFT_START_MS** (settings_table.c:91) is ignored by the mode-OFF ramp:
   `bts7960_stop()` uses the fixed 500 ms (bts7960_driver.c:188).
3. **SET_VALVE_SPEED_0..4** (settings_table.c:113-117) apply to normal moves only. Learn
   direction uses `VALVE_MOTOR_SPEED` (tb6612fng_driver.c:442), jog uses
   `SERVICE_JOG_SPEED` (:479) and calibration uses `CALIB_MOTOR_SPEED` (:1168). Values 5..25
   are allowed but sit below the 30 PWM stiction floor; the driver then lowers the floor to
   the override (:1048) and the valve may not move at all.
4. **SET_HUMIDITY_OFF** gap: the setter keeps OFF 2 %RH below ON (settings.c:184-190, :608),
   the controller's own fallback uses 1 %RH (hvac_controller.c:207). Two rules for one pair.
5. **SET_DUCT_MIN / SET_DUCT_MAX** (settings_table.c:109-110): both ranges include 30, so
   min = max = 30 is accepted and the cascade then holds its last output forever
   (cascade.c:29). The max of DUCT_MIN should be 29 or the setter should keep a gap.
6. **SET_HEATER_DELAY_FAN** (settings_table.c:96) includes Off; with it the heater delay runs
   the AUTO fan at 0 % and the compressor interlock trips. Legal, but worth a note on the row.
7. Self-test waits at most 60 s for calibration (selftest.c:28) while a calibration may run
   120 s per phase (tb6612fng_driver.c:1166): a slow first boot reports valves as
   "uncalibrated" or "stalled" that finish a minute later.

## 4. Documentation that differs from the code

| doc | says | code |
|---|---|---|
| DESIGN.md:99, :236 | NIGHT brightness 18 %, stored under `night_mode` in namespace `hvac` | 20 % (settings_table.c:71), key `night_mode` in `hvac_cfg` (settings.h:79) |
| DESIGN.md:307 | draw buffers "stay internal for DMA" | both full-frame buffers are in PSRAM (main.c:246-248) |
| DECISIONS.md:34 | settings written "2 s after the last change" | 2 s after the first unwritten change; later changes in that window do not restart the timer (settings.c:132-137) |
| DECISIONS.md:67 | idle re-assert "10 small frames per second" | 15: two GPIO_SET and one PWM_SET per valve (tb6612fng_driver.c, motor_send) |
| CarHvacIoNode/main/protocol_server.c:7 | display "waits (2 ms grace, then up to 200 ms)" | no grace since L-01 (remote_io_binary.c:232) |
| main/hvac_sensors.h:117 | fast reads "(50ms)" | 25 ms (hvac_sensors.c:44) |
| this brief | tags `v2.2.1-display`, `v2.3.1-node` | not present; heads are `v2.2-display`, `v2.3-node` (the safety-fix tags that exist are `v2.1.1-display`, `v2.2.1-node`) |

## 5. Constants that exist twice and must change together

- Link liveness: node `LINK_WATCHDOG_MS` 3000 (node_config.h:34) = display "connected"
  window 3000 (remote_io_binary.c:733) > `MOTOR_LINK_REFRESH_MS` 1000 (motor_link.h:23)
  = controller re-assert 1000 (hvac_controller.c:342); BENCH.md:17 and node README:190.
- Command timeout 200 ms (remote_io_binary.c:45) vs the node's reply budgets: ADS poll
  ≤ 20 ms (io_i2c.c:39-41), search 100 ms (io_onewire.c:25), DS18B20 3 retries
  (io_onewire.c:22); node README:144, :202 and protocol_server.c:7.
- Frame timeout 20 ms: remote_io_binary.c:46 and node_config.h:33.
- `cobs_protocol.h` (payload 64, protocol 2, 4 ROMs, OTA_DATA_MAX 56, error codes) and
  `ota_image.h`: byte-identical copies in both repos; `ionode_cli.py` ERR_NAMES mirrors the
  error codes.
- `SENSOR_ROLES_MAX_ROMS` 4 (sensor_roles.h:29) = `ONEWIRE_SEARCH_MAX_ROMS` 4 (cobs_protocol.h:44).
- Blower PWM 25 kHz: bts7960_driver.c:87 and node_config.h:60 `{48, 25}`; node README:26.
- Pin map: hvac_hw_config.h vs node_config.h:55, :60, :88 and the node README table.
- `NODE_PROJECT_NAME` "CarHvacIoNode" (remote_io.h) vs `project(CarHvacIoNode)` in the node CMakeLists.txt:4.
- Node image size: 1 MB in web_server.c:37 and index.html:139; the node slot is 960 KB (partitions.csv:6-7).
- ADC full scale: display 4096/32767 mV (ads1115_driver.c:102) assumes the node's PGA ±4.096 V (io_i2c.c:35).
- Fan Low 30: bts7960_driver.h:18, SET_FAN_MIN_DUTY default 30 (settings_table.c:87), the
  A/C interlock (hvac_controller.c:986) and DECISIONS.md:37.
- Deadband 0.5 °C: TEMP_DEADBAND (hvac_controller.c:36) and the AUTO fan lower knee (:767, :775).
- Jog: UI resend 150 ms (hvac_ui_service.c:25) must stay below the driver dead-man 300 ms (tb6612fng_driver.c:204).
- Humidity gap: 2 %RH (settings.c:184-190) vs 1 %RH (hvac_controller.c:207).
- `cfg_*` fallback defaults (hvac_controller.c:204-243) repeat every table default; a changed
  table default without the fallback changed is only visible when the table is broken.
- Self-test maximum wait 60 s (selftest.c:28, DESIGN.md:293) vs calibration timeout 120 s (tb6612fng_driver.c:1166).
- `DEFAULT_RAMP_TIME_MS` 500 (bts7960_driver.c:36) vs SET_SOFT_START_MS default 500.
- Setpoint range 16..30: TEMP_MIN/MAX_C (hvac_controller.c:37-38), SET_DEFAULT_SETPOINT
  range (settings_table.c:77), presets 30 and 16 (presets.c:6-7).
- `VALVE_BOUNDARY_SAFE_MARGIN` 1 (tb6612fng_driver.c:100) vs SET_SAFE_MARGIN_PCT default 1;
  `VALVE_MOTOR_SPEED` 80 / `COOLANT_MOTOR_SPEED` 255 vs SET_VALVE_SPEED_n "0 = default".
- Node task watchdog 5 s (node sdkconfig) and README:193; OTA confirm 60 s (node_config.h:22),
  both READMEs and DECISIONS.md:55; display validation 30 s / 5 min (ota_display.c:25-26),
  README.md:43 and DECISIONS.md:55; chart 10 s (hvac_ui_diag.c:247) and BENCH.md:92.

## Not checked

- Nothing was built or run on hardware; line numbers are from the sources at the heads above.
- LVGL's long-press defaults (400 ms and 100 ms) are LVGL 8.4's documented defaults, set by
  `lv_indev_drv_init` (lv_hal_indev.c:64-65) because `sdkconfig` has no
  `CONFIG_LV_INDEV_DEF_LONG_PRESS_*`; the generated `lv_conf_internal.h` in this tree was not
  parsed for them.
- `ionode_cli.py` beyond its error table; the node's status-LED path; sdkconfig keys beyond
  those listed in row 150 and row 73.
- Whether every proposed Danish label renders in the generated 16 px font (æ/ø/å are in the subset; other glyphs were not checked).

## Risk

- Exposing a SERVICE or ADVANCED row is not free: each one needs the owning module to read
  the id and a bench check that the range cannot stall a valve or ice the evaporator. The
  ranges above are proposals, not tested limits.
- The KEEP rows that pair across the link (#64, #121, #123, #127) are the ones where a change
  on one board without the other silently breaks the safe state.

Skipped: a line-by-line re-read of unchanged files; constants were located by pattern
search and checked against the earlier full read. Risk: a literal that is neither a
`#define`, a table nor inside a comparison could have slipped the search.
