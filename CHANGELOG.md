# Changelog

## Unreleased — OEM Minimal touch UI

- Replaced the slider-based Main screen with five large top-row controls (including explicit A/C/recirculation ON/OFF text), a dominant 44 px setpoint, repeatable temperature/fan steps and an actual blower percentage readout.
- Added one-tap AUTO and quick DEF, a tap-to-AUTO fan label, controller-reported A/C/compressor separation, persistent main-screen fault status and scrollable diagnostics with named valve targets.
- Added persisted DAY/NIGHT backlight selection and startup restoration, instant tab changes with native content scrolling disabled, short-click/long-press separation, mode selection and confirmed thermal-calibration reset.
- Added remote IO link-liveness reporting and exposed the existing link counter getter in `remote_io.h`.
- Added LVGL heap and `hvac_ui_update()` timing logs. Montserrat 44 is enabled in `sdkconfig` and `sdkconfig.defaults`; estimated incremental flash is 80–90 KiB from generated font source data (map-file delta pending). Build/hardware timing values remain pending because ESP-IDF 6.1 and the ESP32-S3 toolchain are unavailable in the authoring environment.

- Fixed LVGL 8.4 gesture dispatch by registering swipes on `scr_main`, using the event's input device, and suppressing page changes while modal/message overlays are open. Added bench coverage for swipes starting on controls and in diagnostics without accidental activation.
- Added a shared 10 px per-axis tap-slop dispatcher for all custom buttons, including picker/calibration/message dialogs; long-press repeats stop after the press drifts beyond slop. Configured touch gestures with `gesture_limit = 30` and `gesture_min_velocity = 1`, and added the four requested bench cases.
