# CarHvacController touch UI redesign

**Approved direction:** A — OEM Minimal, with the owner changes recorded below.
**Implementation branch:** `ui/direction-a-oem-minimal`
**Baseline branch:** `main` (the unmodified source snapshot extracted from `CarHvacController-main.zip`).

The ZIP did not contain its original `.git` directory or upstream commit history. The local `main` branch is therefore a committed baseline snapshot, not a reconstruction of the original remote repository history. The redesign is committed separately on the feature branch.

## 1. Source audit: current UI

### What the current UI does well

- `main/hvac_ui.c::create_center_temp_display()` already makes the setpoint prominent with `lv_font_montserrat_28` and includes separate temperature `−` / `+` controls.
- Existing controller setters handle user actions and `hvac_controller_get_state()` is the source of current state. The redesign preserves that boundary; the UI never assumes that a request is already reflected in controller state.
- The current diagnostics page already provides sensor readings, valve position readouts, uptime, free heap, thermal learning status, a 1-Wire read action and calibration save/reset actions.
- `main.c` already owns the LVGL task and recursive mutex; periodic UI calls occur inside that lock. The redraw path and hardware integration remain unchanged.
- Controller settings already persist to NVS in `main/hvac_controller.c` under the `hvac` namespace and `settings` key.

### Problems verified in source

- `create_fan_control()` uses a 220×22 px slider for a discrete fan enum. This is a small precision interaction rather than a stepped thumb control.
- `create_left_buttons()` makes A/C and recirculation only 48 px wide; the right-side mode buttons are also 48 px wide. They only meet the requested minimum, with poor spacing and edge placement rather than comfortable thumb margins.
- The old main layout combines a 150×195 px central panel with side controls and a fan panel. It leaves the climate status distributed across the screen instead of using the approved fixed rows.
- `hvac_ui_update()` calls `lv_label_set_text()` for many values every update even when the displayed text is unchanged. With `disp_drv.full_refresh = 1`, redundant invalidation is costly.
- The old UI checks the A/C button from `state.ac_compressor_on`, although the button action changes `state.ac_enabled`. That confuses requested A/C state with the compressor's actual state.
- Faults can cause an error message box to appear from the periodic UI update. This can obscure the controls, and a fault is not persistently readable as a main-screen badge.
- The old debug reset-calibration button resets immediately on click. That action now needs an explicit confirmation.
- The old debug page only shows the cached actual valve-position fields; it does not pair each actual position with the current TB6612 target.
- `remote_io_binary.c` already collects TX/RX/CRC-error/timeout counters, but `remote_io.h` did not declare the stats getter and the display had no explicit “connected recently” API.

## 2. Approved direction A — OEM Minimal

### Main screen, 320×240

The status/fault row is deliberately at the top, above the control row, so the fault badge is not wedged between two rows of buttons. Five control targets are **56×48 px**, separated by **8 px**, with 4 px at each horizontal edge. Main-screen labels use at least 16 px Montserrat. A/C and RECIRC use a two-line label with an explicit `ON`/`OFF` line, so state is not conveyed by colour alone. RECIRC uses tighter letter spacing to fit at 16 px within its fixed-width target.

```text
x=0                 160                320
┌──────────────────────────────────────────┐ y=0
│ AUTO | HEATING              [ ! SENSOR ] │ 24 px status/fault row
├──────────────────────────────────────────┤ y=24
│[ MODE ] [ AUTO ] [ DEF ]  [A/C]  [RECIRC]│
│                           ON      OFF    │ 48 px control row
├───────┬────────────────────────┬─────────┤ y=72
│       │                        │         │
│ [ − ] │       22.0°C           │  [ + ]  │ 72 px
│ 64×72 │    Montserrat 44       │ 64×72   │
│       │   value + unit together│         │
├───────┴────────────────────────┴─────────┤ y=144
│ CABIN 21.5°C     RH 46%       COMP ON     │ 48 px
├───────────┬────────────────────┬─────────┤ y=192
│   [ − ]   │   FAN MED | 65%     │   [ + ] │
│   56×48   │       184×48        │  56×48  │
└──────────────────────────────────────────┘ y=240
```

Illustrative readings only. The first row shows the controller-reported operating mode/thermal status on the left and the fault badge on the right (`LINK LOST`, `SENSOR`, `VALVE`, or `FAULT`). Its position gives faults priority and makes the status row visible before the control row. The displayed temperature including `°C`/`°F` stays on one 44 px label, at x=76, y=80, width 168 and height 56.

- **MODE:** a short tap (`LV_EVENT_SHORT_CLICKED`) opens the airflow picker; a long-press opens diagnostics. The long-press cannot also invoke the picker on release. A horizontal swipe left opens diagnostics and swipe right returns to Main; the gesture handler calls `lv_indev_wait_release()` to suppress an accidental click on release.
- **AUTO:** one tap calls `hvac_controller_set_mode(HVAC_MODE_AUTO)`.
- **DEF:** one tap calls `hvac_controller_quick_defrost()`; it does not pretend to control a rear-window defroster.
- **A/C:** the tile renders `A/C` with a second-line `ON`/`OFF` from `ac_enabled`. `COMP ON/OFF` separately reflects `ac_compressor_on`.
- **RECIRC:** the tile renders `RECIRC` with a second-line `ON`/`OFF` from `recirculate`.
- **Temperature:** the two 64×72 px controls use 22 px symbols and call `hvac_controller_temp_down/up` on `LV_EVENT_SHORT_CLICKED` and `LV_EVENT_LONG_PRESSED_REPEAT` while held. They do not register the release-time `LV_EVENT_CLICKED`, avoiding an extra step after a held repeat. The central Montserrat 44 setpoint is rendered from `target_temp_c` and `temp_unit`; the number and unit use the same 168×56 px label.
- **Fan:** 56×48 px `−` / `+` controls run their one-shot action only on `LV_EVENT_SHORT_CLICKED` and also support `LV_EVENT_LONG_PRESSED_REPEAT`; release after a repeat does not add a click step. They step through Off, Low, Med-Low, Medium, Med-High, High and clamp at both ends. From AUTO, `−` exits to Off and `+` exits to Low. Tapping the 184×48 px level label sets fan AUTO. The label renders the actual `fan_level` and `blower_speed` percentage.
- Every click gets LVGL pressed-state feedback. Checked state is updated on the next controller-state render, not from the requested action.
- Tap slop applies to every button and dialog action: `UI_TAP_SLOP` is 10 px independently on X and Y. A press is invalidated as soon as `LV_EVENT_PRESSING`, release, long-press or repeat observes either axis beyond that limit; returning to the origin does not re-enable the press. `SHORT_CLICKED` and `CLICKED` share one per-button dispatcher and one physical tap is consumed only once. Swipes are the only movement gesture; non-swipe drags beyond slop activate nothing.
- Gesture events are registered on `scr_main`, the first ancestor without `LV_OBJ_FLAG_GESTURE_BUBBLE`; child/tabview registration is insufficient because LVGL 8.4 bubbles the gesture past those objects. `main.c` configures `indev_drv.gesture_limit = 30` and `indev_drv.gesture_min_velocity = 1` to make swipes easier to trigger. The handler reads the event's input device and calls `lv_indev_wait_release()` before returning, including when a modal/message overlay is open.
- Every touch button, including picker cells, calibration-dialog actions, and message-overlay OK, uses a fixed per-button context through `make_button()`. `LV_EVENT_PRESSED` records the input point; `LV_EVENT_PRESSING`, release dispatch and long-press repeat compare each axis against `UI_TAP_SLOP = 10` px. Exceeding either axis invalidates that press even if the finger returns to the origin. LVGL 8.4 emits `SHORT_CLICKED` followed by `CLICKED` for a tap, so both are routed to one dispatcher and consumed once. A long press suppresses its release-time click; repeat steps stop as soon as the press exceeds slop. The built-in LVGL message-box close button is intentionally replaced with a `make_button()` OK control so message dismissal obeys the same rule.

### Diagnostics screen

```text
┌──────────────────────────────────────────┐ y=0
│ DIAGNOSTICS                     [ DAY ]  │ 48 px
├──────────────────────────────────────────┤ y=48
│ NO ACTIVE FAULT / readable fault details │ 36 px
├──────────────────────────────────────────┤ y=84
│ CABIN / RH / DUCT                         │
│ MODE / FAN / BLOWER %                     │
│ A/C REQUEST / COMPRESSOR / RECIRC         │
│ Center 1  ACT xx% TARGET xx%              │
│ Center 2  ACT xx% TARGET xx%              │
│ Foot      ACT xx% TARGET xx%              │
│ Windscreen ACT xx% TARGET xx%             │
│ Coolant   ACT xx% TARGET xx%              │
│ HEAT / COOL / REACHED flags               │ 108 px scrollable
│ thermal rates / gain / samples            │
│ uptime / free heap                        │
│ remote TX / RX / CRC / timeout counters   │
│ link connected / lost                     │
├────────────┬────────────┬─────────────────┤ y=192
│  READ 1W   │ RESET CAL  │     SAVE CAL    │ 48 px
└──────────────────────────────────────────┘ y=240
```

The fault explanation wraps rather than silently clipping. The scrolling content lists all five valves in the specified order. Actual valve positions use the corresponding `hvac_state_t` fields; target positions use `tb6612fng_get_valve_target()` for the matching `hvac_valve_id_t`. Any valve whose bit is set in `valve_fault_mask` is highlighted and named in the fault explanation.

The `DAY/NIGHT` target is 100×48 px. It switches the backlight through the existing `hvac_ui_set_brightness()` API, persists `night_mode` in NVS namespace `hvac` next to the existing controller settings, and restores it at boot. Defaults are DAY 80% and NIGHT 18%. These are proposed starting values and should be adjusted after night driving tests.

`RESET CAL` opens a modal; no calibration reset happens until `RESET CAL` is confirmed. Both `KEEP MODEL` and `RESET CAL` are 144×48 px targets. `READ 1W` and `SAVE CAL` are each 96×48 px.

### Airflow picker

The mode picker uses a 72×48 px, 8 px-gap, 4-column grid. All seven controller modes and Cancel use Montserrat 16. Compact labels `F+F` and `D+F` mean Face+Foot and Defrost+Foot. `MODE` remains the one-tap entry point; `AUTO` and `DEF` are direct actions on the main screen.

```text
┌──────────────────────────────────────────┐
│                AIRFLOW MODE              │
│ [ OFF ] [ FACE ] [ FOOT ] [ F+F ]        │
│ [ DEF ] [ D+F  ] [ AUTO ] [ CANCEL ]     │
└──────────────────────────────────────────┘
```

### Calibration confirmation

```text
┌──────────────────────────────────────────┐
│           RESET THERMAL CAL?             │
│          Clears the learned model        │
│ [     KEEP MODEL     ] [    RESET CAL   ]│
└──────────────────────────────────────────┘
```

## 3. Direction B — Split Cluster (alternate, not selected)

This was the second concept compared during approval. It preserves the same controls and safety rules but trades the single large setpoint cluster for a persistent cabin-information column.

```text
┌──────────────────────────────────────────┐
│ COOLING                         ! SENSOR │ 24 px
├─────────────┬────────────────────────────┤
│ CABIN       │ [ − ]      22.0°C     [ + ] │
│ 21.5°C      │      large setpoint        │
│ RH 46%      │                            │
├─────────────┴────────┬────────┬──────┬────┤
│ MODE AUTO            │ A/C ON │RECIRC│ DEF│ 48 px
├────────────┬─────────┴────────┴──────┴────┤
│ [ − FAN ]  │ FAN MED | 65%       [ + FAN ]│ 48 px
├──────────────────────────────────────────┤
│ COMPRESSOR ON | SETPOINT REACHED         │ 24 px
└──────────────────────────────────────────┘
```

Diagnostics in Direction B uses separate, compact measurement cards and the same scrollable valve/state list, DAY/NIGHT button and confirmed calibration action. Its narrower zones and more numerous groups make it slightly denser than Direction A. Direction A was selected because the large central setpoint and consistent row controls are easier to locate at a glance.

## 4. State-to-visual mapping

| `hvac_state_t` field | Representation |
|---|---|
| `mode` | Main header mode name; picker selection; AUTO/DEF checked state where applicable; diagnostics enum name |
| `fan_level` | Main fan-level label and step actions; diagnostic fan label |
| `ac_enabled` | Main A/C request button; diagnostic `A/C REQ ON/OFF` |
| `recirculate` | Main recirculation checked state; diagnostic ON/OFF |
| `rear_defrost` | Intentionally not presented as an actionable feature: source has no setter or hardware output. The top-row DEF action is `hvac_controller_quick_defrost()` and is not a rear-defrost control. |
| `target_temp_c` | Largest main-screen value; diagnostics/controller unit formatting |
| `temp_unit` | All main/diagnostic sensor and setpoint temperature suffixes |
| `cabin_temp_c` | Main cabin reading; diagnostics |
| `cabin_humidity` | Main RH reading; diagnostics |
| `duct_temp_c` | Diagnostics |
| `center_valve_pos` | Center 1 actual position row |
| `center2_valve_pos` | Center 2 actual position row |
| `foot_valve_pos` | Foot actual position row |
| `windscreen_valve_pos` | Windscreen actual position row |
| `coolant_valve_pos` | Coolant actual position row |
| `blower_speed` | Main fan label and diagnostics as a percentage |
| `is_heating` | Main operating status; explicit diagnostics flag |
| `is_cooling` | Main operating status; explicit diagnostics flag |
| `temp_reached` | Main operating status when not heating/cooling; explicit diagnostics flag |
| `sensor_fault` | Persistent main badge; diagnostic plain-language fault summary |
| `valve_fault` | Persistent main badge; diagnostic fault summary |
| `valve_fault_mask` | Bits 0–4 map in order: Center 1, Center 2, Foot, Windscreen, Coolant; names shown in diagnostics and affected rows highlighted |
| `ac_compressor_on` | Main `COMP ON/OFF`, separate from A/C request; diagnostics |

The main operating strip prioritizes the reported state as HEATING, COOLING, REACHED or READY for a concise glance. Diagnostics shows the three flags independently, so none is inferred from another.

### Link and stale readings

The new `remote_io_is_connected()` returns true only after at least one CRC-valid response and while its age is under 3 seconds. Any valid response proves link liveness, including a protocol-level error response. `remote_io_get_stats()` exposes the existing TX/RX/CRC-error/timeout counters through its newly declared header prototype.

The current controller/sensor state has no sensor-reading timestamp or age field. The UI therefore reports `sensor_fault`, NaN readings and remote `LINK LOST`, but does **not** claim that an unchanged, valid number is stale. Robust stale-sensor detection would require an existing timestamp/status source or a separate, explicitly approved sensor API addition.

## 5. Theme and fonts

| Token | RGB hex | Use |
|---|---|---|
| Background | `#101419` | Screen background |
| Surface | `#1D242C` | Buttons |
| Surface pressed | `#252E38` | Immediate pressed feedback |
| Primary text | `#F5F7FA` | Setpoint and labels |
| Secondary text | `#B7C1CC` | Supporting readings |
| Divider | `#37414B` | Button outlines |
| Active accent | `#52D3CC` | Checked state, with ON/OFF text or labels |
| Warning | `#FFC857` | Calibration reset confirmation |
| Fault | `#FF5B61` | Fault status and affected valves |
| Heating | `#FF9A52` | Reported heating indicator |
| Cooling | `#65C8F2` | Reported cooling indicator |

Fonts were verified against **`sdkconfig`** and then updated in both `sdkconfig` and `sdkconfig.defaults`. The setpoint now uses `lv_font_montserrat_44`; Montserrat 44 is explicitly enabled in both files. Main-screen labels remain at least 16 px. Montserrat 44's generated source contains 78,670 bytes of glyph bitmap literals before glyph descriptors, kerning tables and alignment, so expect roughly **80–90 KiB additional flash** as a source-based estimate. A linker map file is not available in this environment, so this is not an observed flash delta.

Colour is supplementary rather than the sole on/off or fault signal. Normal and checked buttons have different fills/borders, with labels unchanged or displaying explicit status text.

## 6. Memory, redraws and timing

- `CONFIG_LV_MEM_SIZE_KILOBYTES=128`; the existing two full-frame buffers remain in PSRAM and `disp_drv.full_refresh = 1` remains untouched.
- Approximately 60 retained LVGL objects are expected across the two screens and controls, plus modal objects allocated only when opened. The incremental LVGL heap cost is an estimate only; it is not reported as measured.
- The update path uses persistent per-label buffers and `lv_label_set_text_static()`. It compares text and colour values before touching widgets, and only runs extended diagnostics refresh when the diagnostics tab is active (750 ms interval). The periodic `hvac_ui_update()` path uses fixed stack buffers and no explicit heap allocation. All state reads in `hvac_ui.c` use a static last-good `hvac_state_t` snapshot: the local destination is seeded from that snapshot before the void getter, then saved back afterward, so the getter's mutex-timeout path cannot expose uninitialised stack data.
- Page changes use `lv_tabview_set_act(..., LV_ANIM_OFF)`. Both the tabview and `lv_tabview_get_content(tabview)` have `LV_OBJ_FLAG_SCROLLABLE` cleared; in LVGL 8.4 the content object owns the native horizontal scroll/snap behavior. Swipe gestures are converted into an immediate page change, and `lv_indev_wait_release()` prevents the touch release from activating a button underneath the gesture.
- `lv_mem_monitor()` logs total/free/used/fragmentation/largest block immediately after UI initialization. `hvac_ui_update()` logs average and maximum function duration over five-second windows. These timings may spike on Diagnostics while the valve task holds `valve_mutex` during valve motion: `tb6612fng_get_valve_target()` can wait up to 100 ms per valve (five serial calls can therefore stall the UI path for as much as 500 ms). This is existing TB6612 driver behavior, not a UI rendering defect; review item L-02 is expected to remove that driver-side blocking.
- No usable build output or ESP-IDF toolchain is available in this environment: `idf.py build` exits 127 because `idf.py` is not installed and `IDF_PATH` is unset. A new build and runtime measurements could not be executed here; the code's log messages are instrumentation, not measured results.

## 7. API and integration changes

- Preserved all existing `hvac_ui.h` public functions and added `hvac_ui_apply_saved_brightness()` so `main.c` can restore the selected backlight setting after display initialization. `main.c` no longer overwrites DAY/NIGHT with a hard-coded 80% value.
- Added `remote_io_is_connected()` in `remote_io_binary.c` and declared it in `remote_io.h`. Its 3-second liveness window is based on a CRC-valid response timestamp.
- Declared the already-implemented `remote_io_get_stats()` in `remote_io.h`. Its four output arguments are TX frames, RX frames, CRC errors and timeouts; each may be `NULL`.
- No `hvac_controller.h` API, controller logic, hardware drivers, display/touch drivers, protocol frame definitions or node firmware were changed.

## 8. Verification and in-car test checklist

### Automated/source checks

- [ ] Run `idf.py build` with ESP-IDF 6.1 and the committed `sdkconfig` (not runnable in the current authoring environment: `idf.py`/`IDF_PATH` are unavailable).
- [ ] Confirm no compiler warnings about missing prototypes, format strings, LVGL API names or event-callback signatures.
- [ ] Verify the common `SHORT_CLICKED`/`CLICKED` dispatcher invokes each button action at most once, drops releases with `abs(dx) > 10 px` or `abs(dy) > 10 px`, and invalidates a press once it crosses the slop boundary.
- [ ] Verify `main.c` uses `gesture_limit = 30` and `gesture_min_velocity = 1`, and `lv_indev_wait_release()` runs before overlay-related gesture early returns.
- [ ] Compare the linker map against the baseline to measure the Montserrat 44 flash delta; 80–90 KiB is only a current source-based estimate.
- [ ] Read the post-init `lv_mem_monitor` log and record total/free/used/fragmentation/largest block.
- [ ] Record average/max `hvac_ui_update()` duration after several five-second windows, both on Main and Diagnostics.
- [ ] Verify changes only touch the value that changed; monitor invalidation/redraw frequency while values are steady.
- [ ] Capture physical-panel photos listed in `HARDWARE_PHOTO_CHECKLIST.md`.

### Hardware/car checks

- [ ] Sunlight: read setpoint, cabin temperature, fan, mode, A/C request and compressor state in under one second.
- [ ] Night: switch to NIGHT, power-cycle, verify it stays at the stored 18% default, and verify DAY restores 80%; adjust values based on actual visibility.
- [ ] One-handed: tap temperature and fan controls; hold them to verify repeat, boundaries and safe behaviour.
- [ ] AUTO: verify both requested mode/fan and displayed readback; ensure the UI follows state changes made by the controller.
- [ ] DEF: verify one tap calls quick defrost, with the mode reflecting controller state.
- [ ] A/C: verify request state and actual compressor state can differ and remain separately readable.
- [ ] Fault injection: unplug a sensor and verify the main SENSOR badge plus diagnostic explanation; restore it and verify the badge clears when controller state clears.
- [ ] Link fault: disconnect the remote IO UART/node and verify LINK LOST within the connection freshness window; reconnect and verify recovery.
- [ ] Valve fault: trigger a known stall and verify the correct bit/name mapping and row highlight.
- [ ] Calibration safety: tap RESET CAL and cancel; verify no reset occurs. Confirm only on the second explicit action.
- [ ] Gesture/animation: swipe to Diagnostics and back; verify the tabview content does not natively scroll/snap, page changes are instant, and the release does not trigger the button under the finger.
- [ ] Tap slop: slow-drag across AUTO and verify nothing happens; perform a fast swipe starting on DEF and verify the page changes without starting defrost; swipe inside the picker and verify no mode is selected and the picker stays open; hold `+` and slide more than 10 px off-origin and verify stepping stops. Repeat with the message-overlay OK button and calibration dialog controls.
- [ ] Diagnostic responsiveness: observe the UI while valves move. The current `tb6612fng_get_valve_target()` may block on `valve_mutex` for up to 100 ms per valve; attribute any multi-valve stall to the existing driver path pending driver review L-02, not to LVGL layout changes.

## 9. Photo list when a compatible PC simulator is unavailable

See `HARDWARE_PHOTO_CHECKLIST.md`. The required set is Main normal, AUTO, DEF, night mode, each fault class (sensor/valve/link), Diagnostics including valve actual/target and link stats, the mode picker, and both calibration-confirmation outcomes.

## 10. v2.1 additions (as built)

### Page map

```
main  <-swipe->  diagnostics  <-swipe->  settings home
                                           |- Presets (also: long press AUTO)
                                           |- Display / Climate / Fan / Comfort / Control / Valves / Connect / System
                                           |- Diagnostics: Event log, Link health, Self-test, Trend chart
                                           '- Service: Valve service (mode OFF only), Sensor roles
```

Settings rows are 48 px, label left and value right. BOOL toggles, ENUM steps, INT/FLOAT open
an editor (−/+ with repeat, DEFAULT, OK), ACTION rows confirm in a modal. Inside a sub-page
swipes do nothing and BACK is explicit, so a held jog cannot be interrupted. New pages use
generated Montserrat 14/16 subsets with æ/ø/å; the main page keeps its fonts.

### Modules

| Module | Role |
|---|---|
| `settings.c`, `settings_table.c` | One table of all preferences; RAM cache, debounced NVS writes (2 s), apply callbacks, schema per namespace |
| `console_cmds.c` | USB console `list/get/set/reset` |
| `event_log.c` | 50-entry ring, newest first, fed by link, valves, controller, self-test, OTA |
| `valve_cal_store.c` | Per-valve calibration records with CRC in `hvac_cal` |
| `presets.c`, `comfort.c`, `cascade.c` | Presets; dew point, defog and heater-delay logic; cascade PI (pure, host-tested) |
| `selftest.c` | One-shot boot check after calibration |
| `hvac_ui_settings.c`, `hvac_ui_diag.c`, `hvac_ui_service.c` | The new pages |
| `wifi_ap.c`, `web_server.c`, `web/index.html` | Phone access point and page |
| `ota_display.c` | Display update into the inactive slot, 30 s validation |

### New tasks

| Task | Prio | Period | Note |
|---|---|---|---|
| `settings_nvs` | 1 | event + 2 s debounce | only task that writes `hvac_cfg` values |
| `console_repl` | 2 | on input | runs apply callbacks of what it sets |
| `selftest` | 2 | once, then deleted | waits for calibration, max 60 s |
| `wifi_ap` | 3 | event | starts/stops the AP and the HTTP server |
| `httpd` | IDF default | on request | thread-safe getters only; OTA uploads stream in chunks |
| `node_ota` | — | per upload | streams a node image over the link, one command per mutex hold |
| `ota_valid` | — | until validated | marks a fresh display image valid after 30 s UI + link up |

The sensors task now does one broadcast Convert T per second and, on the next cycle, one
scratchpad read per ROM (at most four); searches run in the same task.

### Memory

LVGL's 128 KB pool is one malloc in PSRAM (see the root `CMakeLists.txt`); draw buffers stay
internal for DMA. Static internal RAM: 136 KB used of 342 KB. `wifi_ap.c` logs the free
internal heap each time the AP starts.
