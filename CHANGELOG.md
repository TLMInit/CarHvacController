# Changelog

## v2.2-display (with node v2.3)

The eleven findings of the second review; details in `REVIEW.md` (status) and `DECISIONS.md`.
v2.1.1-display / v2.2.1-node carried the safety fixes early.

- A stop the node never acknowledged is resent on the next tick, and every valve's command
  is re-asserted once a second (1).
- A valve closed at power-off keeps its stored calibration (2).
- Both boards refuse an OTA image of the other project, before anything is written (3).
- Diagnostics shows valve positions in open %, the unit of the target (4).
- Fast sensor reads during calibration and direction learning no longer drop out when a
  page polls the driver (5); the blocking READ 1W button is gone (6).
- UI refresh every 100 ms of time instead of every 10 loop passes (9).
- Dead code removed: the local-IO build path, two components and ~50 unused functions;
  duplicate helpers merged (7, 8, 11). About 1,800 lines fewer.
- Both boards built at -O2 (10).

## v2.1-display (with node v2.2)

Defaults keep v2.0 behaviour except that leaving DEF restores the A/C choice. Details and
reasons in `DECISIONS.md`; bench procedure in `BENCH.md`.

- Settings: 66 rows in eight groups (English/Danish), saved to NVS with a schema number,
  editable on the display, the USB console and the phone page.
- Valves: calibration stored per valve and reused after a plausibility check; per-valve
  speed and direction overrides; service page with jog, stop, recalibrate, learn direction.
- Control: presets (Defog, Max heat, Max cool, Eco); heater delay; dew-point defog with an
  outside sensor; reheat while dehumidifying; cascade control (duct target -> coolant valve)
  selectable next to classic, which stays the default. Recirc only when fitted (setting).
- Link: protocol 2 with HELLO, 1-Wire search with sensor roles (duct, coolant, outside),
  node OTA, link health (RTT last/p95, timeouts per minute). S-11 trailing-zero fix.
- Diagnostics: event log, link health, boot self-test, 10-minute trend chart.
- Phone: Wi-Fi access point with a status/settings/log page and firmware upload for both
  boards; updates roll back unless the new image proves itself.
- Memory: LVGL pool in PSRAM, leaving room for Wi-Fi.
- Host unit tests and GitHub Actions CI.

## Unreleased — OEM Minimal touch UI

- Replaced the slider-based Main screen with five large top-row controls (including explicit A/C/recirculation ON/OFF text), a dominant 44 px setpoint, repeatable temperature/fan steps and an actual blower percentage readout.
- Added one-tap AUTO and quick DEF, a tap-to-AUTO fan label, controller-reported A/C/compressor separation, persistent main-screen fault status and scrollable diagnostics with named valve targets.
- Added persisted DAY/NIGHT backlight selection and startup restoration, instant tab changes with native content scrolling disabled, short-click/long-press separation, mode selection and confirmed thermal-calibration reset.
- Added remote IO link-liveness reporting and exposed the existing link counter getter in `remote_io.h`.
- Added LVGL heap and `hvac_ui_update()` timing logs. Montserrat 44 is enabled in `sdkconfig` and `sdkconfig.defaults`; estimated incremental flash is 80–90 KiB from generated font source data (map-file delta pending). Build/hardware timing values remain pending because ESP-IDF 6.1 and the ESP32-S3 toolchain are unavailable in the authoring environment.

- Fixed LVGL 8.4 gesture dispatch by registering swipes on `scr_main`, using the event's input device, and suppressing page changes while modal/message overlays are open. Added bench coverage for swipes starting on controls and in diagnostics without accidental activation.
- Added a shared 10 px per-axis tap-slop dispatcher for all custom buttons, including picker/calibration/message dialogs; long-press repeats stop after the press drifts beyond slop. Configured touch gestures with `gesture_limit = 30` and `gesture_min_velocity = 1`, and added the four requested bench cases.
