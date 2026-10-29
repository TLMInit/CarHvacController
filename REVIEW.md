# Car HVAC firmware — review and change plan

Scope: the display firmware (`main/`, ESP-IDF 6.1, LVGL 8.4) and the new IO node
(`CarHvacIoNode/CarHvacIoNode/main/`, called `node/` below). Every file in both `main/`
folders was read, plus `sdkconfig.defaults`, `sdkconfig`, `idf_component.yml`,
`dependencies.lock`, `build/project_description.json` and `node/../tools/ionode_cli.py`.
Nothing was edited. Line numbers are against the files as they are on disk today (2026-10-29).

Corrections to the brief, found while reading:
- The workspace is **not a git repository** (it looks like a GitHub zip), so I cannot confirm
  which files are tracked. `build/`, `sdkconfig.old` and `.vscode/` are present on disk.
- There **is** a `README.md`, but it is the unmodified ESP-IDF sample-project template.
- The node is nested one level deeper than stated: `CarHvacIoNode/CarHvacIoNode/`.
- "What the owner has noticed" is empty in the brief. This review is code-only; see Questions.

---

## Summary

1. **Make a stuck or blind valve stop itself (S-01, S-02, S-03).** A failed ADS read is
   written into the control loop as position **0**. The stall timeout exists but is switched
   off. Nothing limits how long a motor may drive. Together these are the realistic way a
   valve motor runs into its end stop indefinitely while the link stays alive (so the node's
   3 s watchdog never fires). Small, display-only fixes.
2. **Fix the link layer on the display (L-01..L-04).** It has a use-after-free when the node
   is slow or absent. It also spends ~3.5 ms per round trip on two fixed tick waits, for a
   frame that takes ~0.15 ms on the wire. Five moving valves cost 15 round trips per loop
   iteration, all while `valve_mutex` is held. A display-only rewrite of
   `execute_command_internal` cuts round-trip time roughly 7× without touching the protocol.
3. **Stop the sensor task blocking 750 ms every cycle (SN-01), then fix the A/C interlocks and
   target flapping (C-01, C-02).** Valve feedback is meant to refresh every 100 ms. It actually
   refreshes every ~0.9 s, so a valve moves ~9 % between samples against a 2 % tolerance. The
   compressor can run with the blower off, and it chatters at the humidity threshold.

---

## Status

Display hashes are in this repo; node hashes (marked *node*) are in `CarHvacIoNode/`.
"Authorised" = in scope of the go-ahead of 2026-10-29, not started yet.

| ID | Title | Phase | Status |
|---|---|---|---|
| S-01 | Failed position read fed to the loop as 0 | 1 | **done** b9840e9 |
| S-02 | Stall detection disabled | 1 | **done** 6f7c36c |
| S-03 | Motor run time unbounded while link stays up | 1 (step 1), 2b (lease) | step 1 **done** 82feebd (10 s, see commit); step 2 held |
| S-04 | Display reboot does not stop running motors | 1 | **done** 4f29e75 |
| S-05 | Blower and relay never re-commanded | 1 | **done** 0d0bb86 |
| S-06 | Use-after-free in the link queue | 1 | **done** 957b16a |
| S-07 | A/C without blower interlock | 1 (via C-01) | **done** via C-01 936b1bc |
| S-08 | Controller steers on frozen sensor value | 1 | **done** 3b6ca2f |
| S-09 | Calibration runs blind with node absent | — | partly: calibration now gives up when feedback is lost (b9840e9); re-calibrating when the node appears later is open |
| S-10 | Two tasks commanding one motor | 1 | **done** 9e2e069 |
| S-11 | Trailing-zero bug | 2b | held |
| L-01 | Fixed tick waits per round trip | 2a | authorised |
| L-02 | Unchanged commands re-sent under `valve_mutex` | 2a | authorised |
| L-03 | Semaphore churn | 1 (via S-06) | **done** via S-06 957b16a |
| L-04 | Replies not matched to command | 2a | authorised |
| L-05 | Dead or misleading link API | 2a (stats getter in 1) | stats getter declared 99ca320; rest authorised (2a) |
| L-06 / L-07 | Bulk read, full-state exchange | 2b | held |
| C-01 | A/C interlocks | 1 | **done** 936b1bc |
| C-02 | Valve targets flap within a cycle | 1 | **done** e4c4acf |
| C-03 | Valve positioning resolution and units | 3 | held |
| C-04 | Calibration at every boot | 3 | held |
| C-05 | Thermal adaptation / cascade PI | 3 | held |
| C-06 | AUTO fan unreachable | 1 (selection only) | **done** 6e32f15 |
| C-07 | Saved settings half-applied | 1 | **done** e4a1acc (no OFF button: layout, for the UI branch) |
| C-08 | Recirc / rear defrost without actuator | 1 (remove rear defrost) | **done** 5b55235 (rear defrost removed; recirc kept as a flag) |
| C-09 | AUTO cannot cool | 3 | held (owner: default yes) |
| SN-01 | Sensor task blocks 750 ms | 2a | authorised |
| SN-02 | No plausibility checks | 1 | **done** 70f977e |
| SN-03 | BME280 driver details | 3 | held |
| SN-04 | Dead local path | 4 | held |
| SN-05 | Sensor fault pop-up at every boot | 1 | **done** e438b8b |
| U-01 | UI thread does link I/O | 1 (keep last snapshot only) | minimal form **done** 2a85ec6; queue refactor held |
| U-02 | Update cadence and drawing cost | 4 | held |
| U-03 | Diagnostics page | 1 (link stats, node uptime only) | link stats + node info **done** a465c59; rest held |
| U-04 | Fault pop-ups | 5 | held |
| U-05 | LVGL mutex discipline | — | no action |
| M-01 / M-02 | Module layout, shared protocol code | 4 | held |
| B-01 | `sdkconfig.defaults` does not reproduce the build | 0 | **done** da769b8 |
| B-02 | Toolchain and dependency pinning | 0 | **done** 7f6c77c; *node* df86a8d |
| B-03 | Repo hygiene, baseline release, version strings | 0 | **done** 82831fc, fe699cd, tag `v1.0-baseline`; *node* 512353c, 1f9f754 (CI, README: step 4–5 held) |
| N-01 | Node watchdog inside the supervised task | 1 | **done** *node* c7160c7 |
| N-02 | README claim about re-commanding | 1 (with S-05) | **done** *node* c90571f (README); display side S-05 |
| N-03 | Watchdog timeout 500 ms | 2b | held |
| N-04 | I2C bus recovery | 1 | **done** *node* 500f78a |
| N-05 | Error-code mapping | 1 | **done** *node* a2965c1 |
| N-06 | ADS background scan | 2b | held |
| N-07 | Reply to corrupt frames | 2b | held |
| N-08 | DS18B20 85 °C on the node | — | display side **done** via SN-02 70f977e |
| N-09 | Pin allow-list, LED note | 1 | **done** *node* 09e98e0, README c90571f |
| N-10 | Missing `dependencies.lock` | 0 | **done** *node* df86a8d |
| N-11 | `watch` CLI command | 5 | held |
| INST | Instrumentation for the "everything drops" reports | 1 | **done** 6a0fe99 (timeout lines), 99ca320 (node restarts), a465c59 (debug tab); *node* d83c7fd (uptime + reset reason) |

---

## Task layout (as built)

| Task | File:line | Core | Prio | Period | Holds while doing UART I/O |
|---|---|---|---|---|---|
| `remote_io` | `main/remote_io_binary.c:335` | 1 (pinned) | 10 | on demand | — (does the I/O) |
| `valve_ctrl` | `main/tb6612fng_driver.c:1203` | any | 5 | 25 ms + I/O | `valve_mutex` (tb6612) for the whole iteration (`:461-842`) |
| `blower_ctrl` | `main/bts7960_driver.c:346` | any | 5 | 20 ms + I/O | `blower_mutex` (`:311-337`) |
| `hvac_ctrl` | `main/hvac_controller.c:1036` | any | 5 | 100 ms + I/O | `state_mutex` (`:1025-1027`), then `blower_mutex` inside |
| `sensors` | `main/hvac_sensors.c:818` | any | 4 | 100 ms + ~800 ms I/O | `ads1115` mutex per read |
| `lvgl` | `main/main.c:436` | 1 (pinned) | 5 | 1–500 ms | LVGL recursive mutex, then `state_mutex` via `hvac_ui_update` |
| `main` | `main/main.c:443` | 0 | 1 | 1 s log | — |
| node `link` | `node/protocol_server.c:357` | 1 | 10 | event-driven | — (single task owns all node I/O) |

The `remote_io_binary.c` header says "Core 0 runs main app". Not true: every application task
except `remote_io` and `lvgl` is created with `xTaskCreate` and floats between cores.

---

## Findings

Format for each finding: **Where**, **Evidence**, **Impact**, **Change**, **Effort / risk**,
**Node**. *Node* says whether the IO node must change too.

### 1. Correctness and safety

#### S-01 A failed position read is fed to the valve loop as "position 0"
- **Where:** `main/hvac_sensors.c:500-521`, used at `:533-537`; consumed raw at `main/tb6612fng_driver.c:478`.
- **Evidence:** each `ads1115_read_valve_position(...) != ESP_OK` sets `positions[i] = 0`,
  which then goes to `tb6612fng_update_feedback()`. That stores it unfiltered in
  `valve_adc_pos[]` (`:1616`), and the valve loop works directly on `valve_adc_pos`. A pot
  wiper that comes loose reads <50 mV, which is also mapped to 0 (`main/ads1115_driver.c:295`).
- **Impact:** safety and correctness. One lost reply looks like a 40–60 % jump:
  - The wrap-around check (`tb6612fng_driver.c:487`, threshold 40) brakes the valve and
    **blocks a direction** until the next target change.
  - The coolant valve (linear) sees "0 %" and drives open.
  - During calibration, a single 0 sets `global_min = 0` (`:937`) and silently skews
    `adc_center`.
  - With the node absent, all five feedbacks are 0 every cycle.
- **Change:** on read failure, keep the last good value and count consecutive failures per
  channel. After 3 failures, mark the valve's feedback invalid: stop the motor, refuse to move
  it, raise `valve_fault`. Reject an implausible single-sample jump before it reaches
  `valve_adc_pos`, not only in the EMA. Abort calibration on any read failure instead of
  folding the value into min/max.
- **Effort / risk:** S / low. **Node:** no.

#### S-02 Stall detection is disabled; `stalled` is never set
- **Where:** `main/tb6612fng_driver.c:810` ("Stall detection disabled - just keep trying");
  `VALVE_STALL_TIMEOUT_MS` (`:86`) is never used. No `stalled = true` exists anywhere.
- **Evidence:** `last_motion_ms` is still kept up to date (`:803-809`), so the data is there.
  The UI's "Valve stall detected" pop-up (`main/hvac_ui.c:781`) can never fire.
- **Impact:** safety. A jammed valve, a slipped coupling, or a pot stuck in a dead band is
  driven at ≥30/255 duty (coolant ≥100/255) forever.
- **Change:** if `is_moving` and `now - last_motion_ms > VALVE_STALL_TIMEOUT_MS`: brake,
  `stalled = true`, keep the target, and retry at most N times with back-off (stiction). After
  that, latch the fault until the next user target change. Add a hard cap per move as well:
  `max_move_ms = 1.5 × full-travel time`.
- **Effort / risk:** S / low. Bench-tune the 6 s value; a valve slowed by cold may legitimately
  take longer. **Node:** no.

#### S-03 Nothing on either side bounds how long a motor runs if the display's valve task stops but the link stays up
- **Where:** node watchdog `node/protocol_server.c:251-265` resets on *any* valid frame. On the
  display, the valve task is not subscribed to the task watchdog (no `esp_task_wdt_add`
  anywhere). `CONFIG_ESP_TASK_WDT_PANIC` is not set (`sdkconfig:2014`).
- **Evidence:** the sensors task alone sends ≥7 frames per second (S-01 path, SN-01). If the
  valve task hangs (a deadlock, or a hang in the S-06 corruption) after commanding FWD, the
  node keeps seeing traffic and never trips.
- **Impact:** safety. A motor can drive into its stop until the ignition is switched off.
- **Change (two steps):**
  1. Display: subscribe `valve_ctrl`, `hvac_ctrl` and `sensors` to the task watchdog with
     panic enabled (`CONFIG_ESP_TASK_WDT_PANIC=y`, a 3–5 s timeout). A hang then reboots, and
     S-04 makes the reboot safe.
  2. Node and display, phase 2: a per-output lease. Valve PWM drops to 0 unless refreshed
     within 1 s. The display already re-sends direction and duty every iteration while moving,
     but not during calibration (`tb6612fng_driver.c:913-919` sends once, then rotates for
     ~10 s), so calibration must refresh too.
- **Effort / risk:** S for step 1, M for step 2 / low. **Node:** step 2 only.

#### S-04 A display reboot does not stop motors that were running
- **Where:** `main/main.c:348-358` pings the node, then spends LCD/LVGL init time before
  `tb6612fng_start_task()` (`:422`). Nothing in the boot path stops valves or turns the relay
  off before calibration starts (`:433`).
- **Evidence:** after a panic reboot (`CONFIG_ESP_SYSTEM_PANIC_PRINT_REBOOT=y`) the node still
  holds the last outputs. The first PING counts as a valid frame, so the node's 3 s watchdog
  is pushed back. The blower is zeroed early (`bts7960_init` → `set_pwm_duty(0)`); valves and
  the A/C relay are not.
- **Impact:** safety. Valve motors and the clutch can stay on for the length of display init.
  The coolant valve is not calibrated and is only commanded once it leaves the hysteresis
  band.
- **Change:** right after `remote_io_wait_ready()`, call the existing but unused
  `remote_io_reset_node()` (`main/remote_io_binary.c:688`). The node replies OK, goes to its
  safe state and restarts (`node/protocol_server.c:198-205`). Then ping again. As an
  alternative with no node restart, send STOP and PWM 0 for all five valves plus relay off.
- **Effort / risk:** S / low. **Node:** no.

#### S-05 After a node reset or watchdog trip, blower and relay are never re-commanded
- **Where:** relay is only sent on change (`main/hvac_controller.c:139-141`, `static last_state`).
  Blower is sent on ramp steps or `set_speed` calls only (`main/bts7960_driver.c:164-168`,
  `:331-332`).
- **Evidence:** the node README says "the display re-commands everything when it returns".
  That holds for moving valves only. After a node brown-out (blower inrush on a shared supply
  is the classic cause) or a >3 s link gap, the node sets PWM 0 and relay low, but the display
  still believes blower = Medium and compressor = ON. The UI keeps showing both as running.
  This is the likely cause if the owner has seen "blower stops on its own".
- **Impact:** correctness (comfort, defog). The display and the node disagree about the
  outputs until the user touches a control.
- **Change:** in `hvac_ctrl`, re-assert relay level and blower duty every 1 s whether or not
  they changed (2 round trips per second). Phase 2's full-state command (L-07) makes this
  automatic.
- **Effort / risk:** S / low. **Node:** no.

#### S-06 Use-after-free when a command waits in the queue longer than `timeout + 500 ms`
- **Where:** `main/remote_io_binary.c:263-277` (caller side), `:200-215` (IO task side).
- **Evidence:** the caller gives up after `CMD_TIMEOUT_MS + 500 = 700 ms`, deletes `done_sem`
  and returns. The request is still queued with pointers to the caller's stack (`result`,
  `resp_type`, `resp_data`, `resp_len`) and to the deleted semaphore. When the IO task gets to
  it, it writes into a dead stack frame and gives a freed semaphore.
  - With the node absent, each command takes ≈ 2 + 200 + 2 = 204 ms.
  - Callers that can be outstanding at once: valve, sensors, blower ramp, `hvac_ctrl` (fan AUTO
    calls `bts7960_set_speed` every 100 ms), and the UI (OFF → `stop_all_valves`).
  - The fifth in line waits 4 × 204 = 816 ms, then runs its own 204 ms. That is past 700 ms.
- **Impact:** safety and correctness. Silent memory corruption, exactly when the node is
  missing or rebooting.
- **Change:** delete the queue and the IO task. Guard `execute_command_internal` with one
  mutex (priority inheritance comes free) and run it in the caller. Every caller already
  blocks, so the task buys nothing. The mutex wait is bounded by the per-command timeout and
  no pointer outlives its frame. This also removes the per-call semaphore churn (L-03).
- **Effort / risk:** S / low; keep the 921600-baud UART config untouched. **Node:** no.

#### S-07 A/C compressor has no blower interlock
- See C-01. Listed here because the clutch can stay engaged with no airflow: fan slider at
  OFF, mode DEFROST or AUTO (`main/hvac_controller.c:605-607`, `:613-618`) → evaporator icing.

#### S-08 On cabin-sensor loss, the controller keeps steering on a frozen value
- **Where:** `main/hvac_sensors.c:678-686` copies `.value` regardless of `.status`. The error
  paths (`:376-378`, `:476-481`) change only the status, so the last good value persists.
  `run_temperature_control` only bails on NaN (`main/hvac_controller.c:399`).
- **Evidence:** after the first successful read, `cabin_temp_c` is never NaN again. With the
  BME280 disconnected, AUTO keeps the coolant valve at `|error| × gain` and the fan at its
  last speed, indefinitely. Compressor state is also frozen when the early return is hit.
- **Impact:** correctness and safety (A/C stays as-is).
- **Change:** `hvac_sensors_get_all` returns NaN for any sensor not OK, or older than 5 s
  (`last_update_ms` exists but is never checked). Define a fallback in the controller: coolant
  valve at a fixed mid position, A/C only on user request, fan at the manual level, and a
  visible fault.
- **Effort / risk:** S / low. **Node:** no.

#### S-09 `ESP_ERROR_CHECK` audit: no runtime abort with the node absent (good), but quiet degradation
- **Evidence:** of the boot calls in `main/main.c:348-433`, none that talk to the node can
  return an error. `remote_io_init` fails only on local UART setup. `bts7960_init` ignores its
  remote failure (`main/bts7960_driver.c:109-113`). `hvac_controller_init` logs relay failure
  and continues (`:676-678`). `tb6612fng_auto_calibrate_all` always returns OK (`:1528`).
- **Consequence:** with the node absent at boot, calibration runs on 0-feedback (S-01). It
  never finds the marker, times out after 120 s (`CALIB_MAX_ROTATION_TIME`) and falls back to
  defaults. If the node then appears (separate supply, later ignition stage), the butterfly
  valves run uncalibrated until the next reboot.
- **Change:** gate calibration on a successful `wait_ready`. If the node shows up later,
  start calibration then (or load persisted calibration, C-04). Show "IO node offline" on the
  UI.
- **Effort / risk:** S / low. **Node:** no.

#### S-10 Concurrent commands to the same motor from two tasks
- **Where:** `tb6612fng_stop_valve` sends BRAKE and PWM 0 **without** `valve_mutex`
  (`main/tb6612fng_driver.c:421-423`), then takes it with a 100 ms timeout. It is called from
  the LVGL task through `hvac_controller_set_mode(OFF)` (`hvac_controller.c:743`).
- **Evidence:** the valve task holds `valve_mutex` for its whole iteration (≥ 3 round trips
  per moving valve). The two command streams interleave on the link, so the valve task can
  re-send FWD right after the user's BRAKE. If the 100 ms mutex take then times out, the stop
  is lost and the valve continues to its old target.
- **Impact:** correctness (OFF does not reliably stop valves).
- **Change:** `stop_valve` only sets `target = position` and a `stop_requested` flag under
  the mutex. The valve task is the only writer of motor outputs.
- **Effort / risk:** S / low. **Node:** no.

#### S-11 Known trailing-zero bug — assessed consequences
- **Where:** `main/cobs_protocol.c:149-153`.
- **Evidence:** computed with the protocol's own CRC (script in the session scratchpad):
  - The fixed replies are **not** affected: OK = 0x7078, PONG = 0x503A, ERROR 1..8 low bytes
    are all non-zero.
  - Data replies are affected at exactly 1/256 (ADS replies: 413 of 105,600 combinations).
  - Commands with a CRC ending in 0x00 are **deterministic**. Every `PWM_SET(pin 13, duty 202)`
    has a CRC ending in 0x00, and 202 is a reachable coolant-valve speed (ramp 100–255,
    `tb6612fng_driver.c:777-796`). Others: `(17,25) (15,234) (40,190) (47,206) (48,45)`.
    Any node that shares the display's decoder rejects these every time. The new node does
    not, because its decoder is fixed.
  - Under a node without the pad, a lost ADS reply triggers S-01 (phantom 0, wrap-around
    stop). Per calibration rotation (~120 position reads per valve at the measured cadence,
    SN-01): P(at least one hit) = 1 − (255/256)^120 ≈ 37 % per valve.
- **Change:** keep `LINK_PAD_TRAILING_ZERO 1` until the display is fixed. Then delete the
  strip block, rebuild the display, and set the pad to 0 in the **same** bench session.
  A fixed display with a padding node rejects 1/256 replies. Fixing S-01 first makes either
  mismatch harmless.
- **Effort / risk:** S / medium (the two ends must change together). **Node:** yes, flag flip.

### 2. Link performance

#### L-01 Every round trip pays two fixed tick waits
- **Where:** `main/remote_io_binary.c:121` (`vTaskDelay(pdMS_TO_TICKS(2))`), `:133`
  (`uart_read_bytes(..., 64, pdMS_TO_TICKS(2))`).
- **Evidence:** with `CONFIG_FREERTOS_HZ=1000`:
  - `vTaskDelay(2)` waits 1–2 ms.
  - `uart_read_bytes` returns only when it has 64 bytes **or** the 2 ticks have passed.
    Replies are 5–9 bytes, so it always waits out the 2 ticks: another 1–2 ms.
  - Frame airtime at 921600 baud is 10.85 µs per byte: an 8-byte command plus a 6-byte reply
    ≈ 0.15 ms. Node processing for GPIO/PWM is tens of µs; ADS ≈ 1.3–1.5 ms
    (`node/io_i2c.c:141` waits 1 ms, then polls).
  - So a round trip costs ≈ 2–4 ms (≈ 3.5 ms typical) where ≈ 0.3 ms would do (≈ 1.7 ms for
    ADS).
- **Change:** drop the grace delay. Read with `uart_get_buffered_data_len()` plus a short
  blocking read, or wait on the UART event queue already installed (`:304`), and feed the
  COBS receiver until the delimiter arrives. Return immediately on a full frame.
- **Effort / risk:** S / low. **Node:** no.

#### L-02 Valve loop re-sends unchanged commands every iteration, under `valve_mutex`
- **Where:** `main/tb6612fng_driver.c:798-799` sends 2 × `GPIO_SET` + 1 × `PWM_SET` per
  moving valve per iteration, even when direction and duty are unchanged. `set_motor_speed`
  also logs at INFO level on every call (`:262`).
- **Evidence:** see the timing table. `tb6612fng_set_valve_position` and
  `tb6612fng_update_feedback` take `valve_mutex` with a 50 ms timeout (`:344`, `:1655`). Once
  the loop holds the mutex for more than 50 ms (≥ 5 round trips, i.e. two moving valves), new
  targets are dropped with "mutex timeout" and feedback updates are skipped.
- **Change:** remember the last direction and duty per valve and send only on change.
  Refresh once per second, for the S-03 lease. Do the UART I/O **after** releasing
  `valve_mutex`: compute the outputs under the lock, then apply them. Remove the per-call
  INFO log.
- **Effort / risk:** S / low. **Node:** no.

#### L-03 Per-call semaphore churn and a 100-byte queue copy
- **Where:** `main/remote_io_binary.c:230` creates a binary semaphore on every call, `:276`
  deletes it. The request struct (~110 bytes) is copied into and out of the queue.
- **Impact:** small CPU and heap churn; the real cost is S-06. Fixed by S-06's mutex design.

#### L-04 Replies are not matched to the command
- **Where:** `main/remote_io_binary.c:148-166` accepts the first valid frame. Callers with
  `resp_type == NULL` (GPIO_SET, PWM_SET, GPIO_MODE, DS18B20_START) treat any non-ERROR frame
  as success. `remote_ads1115_read_bulk` ignores the echoed channel byte (`:554`).
- **Impact:** correctness, low probability. The input is flushed before each command
  (`:113`), so only a reply that arrives after the flush can be mistaken. That would take a
  node that is slower than the timeout.
- **Change:** pass the expected reply type into `send_command_binary` (OK for setters, the
  matching type for getters) and check `resp_data[0] == channel` for ADS. A sequence byte
  needs a protocol change (L-07).
- **Effort / risk:** S / low. **Node:** no.

#### L-05 Dead or misleading link API
- `remote_io_flush`, `remote_io_reinit` and `remote_io_stop_all_pwm` are no-ops that report
  success (`main/remote_io_binary.c:660-711`). `DS18B20_TIMEOUT_MS` is unused (`:45`).
  `remote_io_get_stats`, `remote_io_ping` and `remote_io_reset` are defined but not declared
  in `remote_io.h`. Delete the no-ops; declare `remote_io_get_stats` and show it on the UI
  (U-03). **Effort:** S.

#### L-06 What a bulk read and asynchronous replies would buy
- **Bulk ADS read** (one command, 5 channels): the node converts 5 × ~1.4 ms ≈ 7 ms; one
  round trip ≈ 7.5 ms after L-01, against 5 × 1.7 ≈ 8.5 ms. **Small gain after L-01.**
  A node task that scans the ADS continuously and returns cached values answers in ≈ 0.3 ms:
  5 pots for the price of one GPIO_SET.
- **Asynchronous / pipelined replies** (several commands in flight, matched by sequence
  number): saves the per-frame turnaround. After L-01 that is ≈ 0.3 ms per command, so
  pipelining 15 commands saves ≈ 4 ms per loop. **Not worth it on its own.**
- **What is worth it — L-07, idempotent full-state exchange.** One `SET_OUTPUTS` frame
  carries 5 × (dir, duty) + blower duty + relay (12 bytes payload). One `GET_STATUS` reply
  carries 5 pots (raw int16), the latest DS18B20 and BME280 raw values, node uptime, a boot
  counter and fault flags (~30 bytes, within the 63-byte limit). Sent every control tick
  (50–100 ms), this:
  - is the keepalive, which makes the node lease (S-03) natural;
  - re-asserts every output, which fixes S-05 by construction;
  - shows node resets through the boot counter;
  - cuts link traffic to 2 round trips per tick, ≈ 1 ms of link time per 100 ms.
- **Cost:** both ends change together.
  - Add new message types (e.g. 0x60/0x61) **next to** the existing ones; nothing is removed.
  - The display sends `MSG_VERSION` at boot. The node already answers it
    (`node/protocol_server.c:103-107`) but the display never asks.
  - If the reply contains `proto=2`, use the new path; otherwise stay on single commands.
  - Put `PROTOCOL_VERSION` into the version string (it is defined but unused).
  - `ionode_cli.py` gets two subcommands; the host COBS tests get vectors for the new frames.
  - **Effort / risk:** M / medium. Rollback means flashing the previous display build; an
    old display keeps working against a new node.

### 3. Control logic quality

#### C-01 A/C clutch: no blower interlock, no hysteresis, no minimum on/off time, no freeze protection
- **Where:** `main/hvac_controller.c:590-645`.
- **Evidence:**
  - `should_run_ac` never looks at the blower. With the fan slider at OFF in DEFROST it stays
    true.
  - The AUTO humidity rule is `> 65.0f` with no hysteresis (`:613-618`). Humidity updates
    about once per sensor cycle (SN-01), so the relay can toggle about every 0.9 s around
    65 %.
  - There is no minimum off time. A production clutch typically gets ≥ 5–10 s off after a
    disengage.
  - No evaporator temperature is measured. The duct DS18B20 sits after the heater core, so it
    is only a proxy.
  - AUTO warm-up forces the compressor on at a cabin temperature below 15 °C (`:598-601`)
    with no ambient lockout. Most systems lock the compressor out below ~2–5 °C ambient.
- **Production comparison:**
  - The compressor is allowed only with the blower running.
  - An evaporator thermistor cuts out at ~1–2 °C and back in at ~4–5 °C.
  - There is an ambient lockout and a minimum cycle time.
  - The refrigerant pressure switches are hardware (see Questions).
- **Change, all display-only:**
  - require actual blower duty ≥ LOW;
  - humidity hysteresis (on > 65 %, off < 58 %);
  - minimum off 10 s and minimum on 30 s;
  - if the duct temperature is below 3 °C with the coolant valve closed, open the clutch
    (crude freeze protection);
  - compressor off whenever the cabin sensor is invalid and the user has not asked for A/C.
- **Effort / risk:** S / low. **Node:** no.

#### C-02 Valve targets flap within one control cycle
- **Where:** `main/hvac_controller.c:555-572` and `:605-610`, `:519-531`.
- **Evidence:** in AUTO cooling, `WINDSCREEN` is set to 0 (`:560`), and then to 50 when
  RH > 70 % (`:571`). Both happen every 100 ms. Same pattern for the coolant valve:
  - `err × gain` (`:417`), then 100 in DEFROST (`:609`);
  - `err × gain`, then 100 in warm-up (`:524`).

  Each change exceeds the 2 % tolerance, so `tb6612fng_set_valve_position` accepts both. Every
  call also resets the V-side latch, the boundary flag and the wrap-around block
  (`tb6612fng_driver.c:351-360`) and logs at INFO. When the valve task holds the mutex, either
  call can time out (50 ms), so which value wins is random.
- **Impact:** correctness. Valves may hunt or pick a side from a reset latch; log spam at
  20 lines/s.
- **Change:** compute one `desired_targets[5]` struct per cycle (mode → warm-up → humidity
  override → defrost override), then apply once. This is also the seam for host tests (M-01).
- **Effort / risk:** S / low. **Node:** no.

#### C-03 Valve positioning: coarse, slow feedback; tolerance in the wrong units; stiction handling by luck
- **Where:** `main/ads1115_driver.c:282-299`, `main/tb6612fng_driver.c:84-100`, `:515-591`.
- **Evidence:**
  - The 16-bit ADS value (~26,000 counts over 0–3.3 V) is quantised to **1 %** before any
    filtering (`ads1115_voltage_to_position`).
  - The butterfly error is in ADC-% while the target is open-%. With `half_range ≈ 50`,
    1 ADC-% ≈ 2 open-%. Tolerance 2 + hysteresis 2 therefore means "stop within ±4 % open,
    restart only beyond ±8 % open".
  - The EMA (α = 0.5) and the outlier gate (15 %) run at the feedback rate (≈ 1.1 Hz,
    SN-01). The outlier path crawls 0.25 % per sample (`:1636`), so a real 15 % step takes
    ~60 samples ≈ 1 minute to be believed.
  - Butterfly control uses the **unfiltered** `valve_adc_pos`; the filtered value only feeds
    the coolant path and the UI.
  - Stiction: `VALVE_MIN_SPEED 30` is a fixed floor with no kick. A short full-duty pulse at
    start (~100 ms) is the usual fix.
  - Dead constants: `VALVE_PLATEAU_ZONE`, `VALVE_DEADZONE_ADC` and `adc_low/high_deadzone`
    are computed but not used for control.
- **Change:**
  - keep positions in raw counts or 0.1 %;
  - one filter, run where the samples arrive;
  - tolerance in open-% converted to ADC per valve;
  - a start kick;
  - time-based stall (S-02);
  - delete the unused fields.
- **Effort / risk:** M / medium. Bench-tune each actuator with `ionode_cli.py valve ... fwd`
  and `ads-all`. **Node:** no.

#### C-04 Auto-calibration at every boot
- **Where:** `main/main.c:433` → `tb6612fng_driver.c:1138-1168`.
- **Evidence:**
  - All four butterfly valves do a full revolution at power-up at duty 50, timeout 120 s.
    Air distribution sweeps through every outlet, including the windscreen, at each start.
  - The sample counter counts valve-task ticks, not sensor samples
    (`CALIB_MIN_SAMPLES 200` ≈ 5 s of ticks).
  - Sensor cadence in calibration mode has a ~0.8 s gap every 20 cycles (SN-01). At ~10 %/s
    pot speed, the ±2 % marker window (4 % wide) can be crossed inside a gap, which costs an
    extra revolution or a timeout.
  - The result lives only in RAM.
- **Recommendation:** yes, persist it.
  - Store `{min, max, center, inverted, crc, fw_version}` per valve in NVS.
  - At boot, load it and validate cheaply: the current reading must be inside
    `[min−3, max+3]`, and a short ±5 % jog must move the pot in the stored direction.
  - Re-calibrate only on validation failure, on a stall (S-02), or from a UI button.
  - Fix the gaps first (SN-01) so a stored calibration is trustworthy.
- **Effort / risk:** M / low. Keep a "force recalibration" button as rollback. **Node:** no.

#### C-05 The "thermal adaptation" model learns sensor noise, and its output is a pure P-gain
- **Where:** `main/hvac_controller.c:224-305`, used at `:414-417`.
- **Evidence:**
  - The rate is `|ΔT| / Δt` over 2 s windows (`:243-245`). A 0.02 °C change between two
    samples, i.e. BME280 noise or quantisation, gives 0.6 °C/min, 12× the 0.05 °C/min
    acceptance threshold (`:258`).
  - `fabsf` throws away the sign, so a cabin cooling while "heating" (door open) counts as
    heating rate.
  - Cabin temperature only changes every ~0.9 s (SN-01), so samples are stale.
  - The learned value only scales a **proportional** controller:
    `coolant% = |error| × gain`, clamped 10–50 × factors.
  - Inside ±0.5 °C the valve is forced to a fixed 10 % (`:436`). There is no integral term, so
    the cabin settles at whatever error makes `err × gain` match the heat loss, or limit-cycles
    around the deadband edge.
  - The duct temperature, the natural inner-loop variable, is not used for control at all.
  - The blob saved to NVS includes `last_sample_time` and `last_cabin_temp` from the previous
    boot (`:201`), and has no version field.
- **Observability:** the debug tab shows only heating/cooling rate and CAL/LRN
  (`main/hvac_ui.c:816-820`). The gain actually applied and the sample count are not shown.
  The owner cannot tell what it learned or whether it helped.
- **Production comparison:** cascade control.
  - Cabin error → PI → target outlet (duct) temperature, with anti-windup.
  - Duct-temperature error → PI → coolant valve.
  - Optional feed-forward from ambient and solar. Neither sensor exists here.
- **Change:**
  - Replace the model with a cascade PI using the DS18B20 as the inner loop.
  - Gains as constants, tunable from the debug tab and stored in NVS.
  - Keep the "learned" code behind a flag until the PI is proven, then delete it.
  - Log one line per minute: cabin, target, duct target, duct, valve.
- **Effort / risk:** M / medium. Bench-test as a host test against a first-order cabin model;
  tune in the car. **Node:** no.

#### C-06 The AUTO fan curve is unreachable from the UI
- **Where:** the fan slider covers 0–5 only (`main/hvac_ui.c:371`) and `slider_fan_handler`
  never selects `HVAC_FAN_AUTO` (`:151-172`). AUTO mode only disables the slider
  (`:432-435`).
- **Evidence:** `fan_level` stays at the manual level that was active, or Medium if it was OFF
  (`hvac_controller.c:756-759`). The auto curve (`:444-467`) and the warm-up fan boost
  (`:533-536`) therefore never run unless NVS holds AUTO, and the UI cannot put it there. In
  AUTO mode the blower is fixed and the user cannot change it.
- **Production comparison:** auto blower with a warm-up delay. The blower is held low until
  the duct/coolant is warm, except in defrost.
- **Change:** entering AUTO mode selects `HVAC_FAN_AUTO`; touching the slider in AUTO drops
  the fan to manual (as production does). Add a warm-up limit: max 30 % until
  duct > 30 °C, unless defrost. In the auto curve, send the blower duty only on change; it
  currently does a UART round trip every 100 ms (`:465`).
- **Effort / risk:** S / low. **Node:** no.

#### C-07 Saved settings are half-applied at boot; there is no way to switch the system OFF
- **Evidence:**
  - `main.c:414` forces AUTO after `hvac_controller_load_settings()`, so the saved mode is
    never used, and NVS is written on every boot.
  - A saved manual fan level is loaded into `hvac_state.fan_level` but never sent to the
    blower: `set_mode` only acts when it is OFF (`hvac_controller.c:756`). The UI shows "Med"
    while the blower is at 0.
  - `temp_up/down` (the UI path) never save (`:850-872`); only `set_target_temp` does.
  - `load_settings` indexes `mode_names[settings.mode]` without a range check (`:1113`).
  - There is no OFF mode button. `hvac_controller_off` is unused, so with the fan at OFF the
    A/C can still run (C-01).
- **Change:** apply the loaded state through the same setter path; validate enum ranges;
  save the setpoint on a 5 s debounce; add OFF (long-press AUTO, or a button).
- **Effort / risk:** S / low. **Node:** no.

#### C-08 Recirculation and rear defrost have no actuator
- `hvac_state.recirculate` only toggles a flag and a factor in the adaptation maths
  (`hvac_controller.c:248-251`, `:277-279`). There is no recirc output in `hvac_hw_config.h`.
  `rear_defrost` is never used. The UI button gives the impression something moves.
  See Questions.

#### C-09 AUTO cannot cool
- In AUTO, "cooling" only closes the coolant valve and opens face vents (`:423-430`, `:555-560`).
  A/C is requested only by humidity, defrost, warm-up or the user. With hot ambient, AUTO
  ventilates with outside air. That may be intended for this vehicle; see Questions.

### 4. Sensors

#### SN-01 The sensor task blocks 750 ms every cycle, so valve feedback runs at ~1.1 Hz instead of 10 Hz
- **Where:** `main/hvac_sensors.c:810` → `read_remote_ds18b20_duct` (`:367`) →
  `remote_onewire_read_ds18b20`, which contains `vTaskDelay(750)`
  (`main/remote_io_binary.c:632`).
- **Evidence:** cycle = DS18B20 (start RT + 750 + read ≈ 10 ms) + BME280 (~5 RT + 10 ms) +
  5 ADS RT + 100 ms delay ≈ **0.9 s** (timing table). `sensor_interval_ms = 100` is commented
  "for responsive valve control" (`:62`). At the nominal 10 %/s
  (`VALVE_FULL_TRAVEL_TIME_MS 10000`), a valve moves ~9 % between position samples against a
  2 % tolerance. That produces overshoot and hunting, which the ramp-down (`:782-790`) only
  partly hides.
- **Change:** use the async API that already exists
  (`remote_ds18b20_start_conversion` / `remote_ds18b20_read_temp`, `remote_io.h:154-163`).
  Read the previous conversion and start the next one, once per second. Run positions every
  100 ms and temperatures every 1 s from one loop with counters, not delays. Better still,
  move position reads into the valve task, so the controller and its feedback share a cadence
  (M-01).
- **Effort / risk:** S / low. **Node:** no.

#### SN-02 No plausibility checks
- DS18B20: the power-on scratchpad value **85.00 °C** has a valid CRC. It appears after a node
  reset or a lost START (`node/io_onewire.c:112-117` passes it through), and the display
  accepts it. That falsely satisfies `duct_warm` (`hvac_controller.c:499-500`) and ends
  warm-up early.
- BME280: no range check (−40..85 °C, 0..100 %RH) and no rate limit.
- **Change:** reject exactly 85.0 unless the previous reading was > 75 °C. Reject values out
  of range or changing more than 5 °C/s; three rejects in a row → status ERROR.
- **Effort:** S. **Node:** optional (the node could return an error for 85.0 without a
  preceding START).

#### SN-03 BME280 driver details
- The chip ID is probed on every cycle (1–2 extra round trips, `hvac_sensors.c:396-403`).
- Calibration is loaded once and never reloaded after a sensor reset.
- `dig_H4`/`dig_H5` use unsigned shifts where Bosch sign-extends the MSB (`:332-333`);
  harmless for typical parts.
- Forced mode at ×1 oversampling with no IIR filter: fine for cabin air.
- **Change:** probe once, retry the probe only after 3 failures; reload calibration on
  re-detect. **Effort:** S.

#### SN-04 The local (non-remote) path is dead and no longer compiles
- `USE_REMOTE_IO 0` references `ds18b20_coolant` and `read_ds18b20_coolant`, which do not
  exist (`hvac_sensors.c:593-596`, `:625`, `:802`). The SHT30 path, the local branches of
  `hvac_hw_config.c:319-345` and `:394-485`, the local ADS code and the
  `espressif/ds18b20` + `onewire_bus` dependencies only serve that path.
- **Change:** delete the local path and its two dependencies. **Effort:** S / low.

#### SN-05 Sensor status is "NOT_FOUND" for 5 s after boot
- With `grace`, the sensor fault pop-up flashes at every boot (U-04).
  `hvac_sensors_all_healthy` and `read_all`'s return value are unused.

### 5. UI

#### U-01 UI thread does blocking link I/O
- **Evidence:**
  - "Read 1W" calls `hvac_sensors_debug_read_onewire` from an LVGL event
    (`hvac_ui.c:131`): ~760 ms frozen. It also interleaves with the sensor task's own
    conversion.
  - Mode OFF → `tb6612fng_stop_all_valves` is 15 round trips ≈ 52 ms, and ≈ 3.1 s with the
    node absent (15 × 204 ms), all inside the LVGL mutex.
  - `hvac_ui_update` → `hvac_controller_get_state` waits up to 100 ms for `state_mutex`. The
    controller holds that mutex during its UART work (`hvac_controller.c:1025-1027`). On
    timeout the UI quietly shows the zeroed struct.
- **Change:**
  - UI handlers post commands to a queue (or set requested-state fields) and return.
  - The controller publishes a snapshot (copy under a short lock, no I/O inside).
  - The 1-Wire button reads the cached value.
- **Effort / risk:** M / low. **Node:** no.

#### U-02 Update cadence and drawing cost
- `hvac_ui_update` runs every 10 `lv_timer_handler` calls (`main.c:298`). The cadence follows
  LVGL's sleep hint, ~0.1–0.3 s.
- Every label is re-set on every update even when unchanged. With `full_refresh = 1`
  (`main.c:257`), each invalidation redraws 320 × 240 × 2 B = 153.6 kB from PSRAM over 80 MHz
  SPI: ≥ 15.4 ms per frame.
- **Change:** an `lv_timer` at 200 ms; set text only on change; partial refresh with two
  1/10-screen buffers in internal DMA RAM.
- **Effort:** S.

#### U-03 Diagnostics page shows too little to troubleshoot with
- **Today:** cabin, duct, humidity, uptime, heap, and two thermal rates. The valve column
  shows **raw ADC %** from the sensors module, not open-% (`hvac_ui.c:761-774` ←
  `hvac_sensors.c:691-695`), without targets. The "Coolant: --" placeholder is a leftover
  (`:509`).
- **It should show (two swipe pages):**
  - Per valve: target, open-%, ADC-%, calibration min/center/max, moving/stalled/feedback-fault,
    last move time.
  - Link: tx, rx, CRC errors and timeouts (`remote_io_get_stats` exists), node version string
    and uptime, last error code.
  - Outputs: blower duty actually commanded, relay state and the reason it is on
    (`ac_reason`, already computed at `hvac_controller.c:591`), warm-up state and remaining
    time.
  - Control: cabin error, duct target, coolant command (once C-05 lands).
  - Sensor ages in seconds.
  - Firmware version of both boards.
  - A "recalibrate valves" button (C-04).
- **Effort:** M.

#### U-04 Fault pop-ups
- `sensor_fault` is true at every boot (SN-05), so an error box flashes up and closes.
- A modal message box is the wrong place for a persistent fault. Use a status icon in the
  header with tap-for-details.
- **Effort:** S.

#### U-05 LVGL mutex discipline
- Good: nothing outside the `lvgl` task calls `lv_*`. `hvac_sensors.c` includes `hvac_ui.h`
  but calls nothing from it, and `hvac_controller_register_callback` is never used. Keep it
  that way. The mutex is effectively private, so it can be removed if U-01 makes the UI a pure
  reader.

### 6. Structure and maintainability

#### M-01 Proposed layout (host-testable core, thin ESP-IDF shells)
```
components/
  link/          cobs.c crc16.c frame.c          pure C, host tests (shared verbatim with node)
  ionode/        ionode_client.c                 typed API over link + UART; one mutex (S-06)
  valve_ctl/     valve_ctl.c                     pure: step(cfg, cal, sample, target, now) -> {dir, duty, flags}
                 valve_cal.c                     pure: calibration state machine on samples
  hvac_logic/    hvac_logic.c                    pure: inputs{temps, rh, mode, setpoints, faults} -> outputs{targets[5], blower, ac, reason}
  sensors/       bme280.c ds18b20_check.c        pure compensation + plausibility
main/
  app.c          tasks, timing, NVS (settings, valve calibration)
  ui.c           LVGL only; reads a snapshot, posts commands
```
- `tb6612fng_driver.c` (1,692 lines) splits into `valve_ctl` (~300), `valve_cal` (~200) and
  a ~150-line glue task. About 400 lines are deleted outright: the `#if !USE_REMOTE_IO`
  branches, `calibrate_valve_internal` (marked unused, `:1257`), `tb6612fng_calibrate_valve`
  (unused), and the legacy `valve_dir_inverted`.
- `hvac_controller.c` (1,133 lines) becomes `hvac_logic` (pure, ~400) plus the setter/NVS
  shell.
- Host tests: Unity or plain asserts, built with host CMake or the ESP-IDF `linux` target.
  Cases:
  - COBS round-trip, including CRC-low-byte-0 vectors from S-11;
  - valve step on recorded pot traces;
  - A/C interlock truth table;
  - the C-02 "one target per cycle" invariant.
- **Do this after phases 1–3, one module at a time, behaviour-preserving, each move landing
  with its tests.**
- **Effort:** L in total, M per module / medium.

#### M-02 Shared protocol code drifts
- `node/cobs_protocol.c` is a copy with two marked changes (diff confirmed). Once S-11 is
  done, make it one file. Use a shared component, or a CI check that both copies hash equal,
  and keep `ionode_cli.py`'s codec under the same test vectors.

### 7. Build and repo hygiene

#### B-01 `sdkconfig.defaults` does not reproduce the build
- `hvac_ui.c:228` uses `lv_font_montserrat_28`. `sdkconfig` enables it (`:3790`), but
  `sdkconfig.defaults` stops at 22, so a clean checkout that regenerates `sdkconfig` fails to
  link.
- Also only in `sdkconfig`: `LV_FONT_MONTSERRAT_24/26`; `CONFIG_COMPILER_OPTIMIZATION_DEBUG`
  (-Og; fine but undocumented); `CONFIG_ESP_TASK_WDT_PANIC` unset (S-03).
- **Change:** run `idf.py save-defconfig` and commit the result; keep `sdkconfig` out of git.

#### B-02 Toolchain and dependency pinning
- The build used `v6.1-dev-1713-g7e3df61a97` (`build/project_description.json:14`), a
  development snapshot.
- The devcontainer points at an `idf5.4` Python env (`.devcontainer/devcontainer.json`).
- `idf_component.yml` says `idf: ">=4.4"` but the code needs ≥ 5.2 (`i2c_master`).
- `ds18b20: "*"` and `onewire_bus: "*"` are unpinned.
- The node README says `dependencies.lock` is committed; **it is not in the node folder**.
- **Change:** pin IDF to a release tag in both devcontainers (e.g. `v6.1`); pin component
  versions; commit both `dependencies.lock` files.

#### B-03 Minimum to prevent another loss and catch regressions
1. A real git repo per project, or one monorepo with `display/` and `node/`.
   - `git rm -r --cached build sdkconfig.old .vscode` if they are tracked (unverifiable here).
   - Keep only `.vscode/settings.json` stripped of COM ports, if anything.
2. **Releases:** tag both projects together, e.g. `v1.3.0-display` + `v1.3.0-node`, and
   attach `*.bin` + `sdkconfig` + `flasher_args.json`. That is what was missing when the node
   firmware was lost.
3. **Version string:**
   - display: `set(PROJECT_VER ...)` in `CMakeLists.txt`, log it at boot, show it on the
     debug tab;
   - display queries `MSG_VERSION` from the node and shows it;
   - the node already has `NODE_VERSION_STRING`.
4. **CI** (GitHub Actions, `espressif/idf:v6.1` container):
   - `idf.py build` for both projects;
   - host tests (M-01);
   - `python -m pytest` on `ionode_cli.py`'s codec against the C vectors.
5. **README** (replacing the template): what each board does, the wiring table (the node
   README already has one), build/flash, how to bench-test with `ionode_cli.py`, and the
   pairing rule "display vX needs node ≥ vY".

### 8. The new node (`CarHvacIoNode`)

Overall: small, readable and faithful to the display's contract. I checked each command
against the display's payloads:
- `I2C_WRITE`: the display sends `len-1` data bytes with `p[2] = len-1`; the node checks
  `len == 3 + p[2]`. ✓
- `I2C_READ` ≤ 26 bytes requested, node limit 58. ✓
- `ADS_READ` raw int16 at ±4.096 V on both sides. ✓
- `GPIO_VALUE` as `[pin, level]`. ✓
- `TEMP_VALUE` ×100 big-endian. ✓
- `PWM_INIT(48, 25)` for the blower matches the boot table. ✓
- Valve PWM is pre-configured because the display never sends `PWM_INIT` for it. ✓
- Relay pin 6 is in the boot table and goes low. ✓

What remains:

#### N-01 The link watchdog lives inside the task it supervises
- **Where:** `node/protocol_server.c:314` (`watchdog_check` inside `link_task`).
- **Evidence:** if `link_task` blocks in a driver call (a stuck I2C bus despite the 20 ms
  timeout, an RMT 1-Wire hang), no watchdog runs and the outputs freeze in their last state.
  `link_task` is not subscribed to the task watchdog either.
- **Change:** run the watchdog check from an `esp_timer` callback, which only calls the
  ISR-safe-enough `ledc_set_duty`/`gpio_set_level` on a flag. Or subscribe `link_task` to the
  task watchdog with panic on; the boot path is already safe.
- **Effort / risk:** S / low. **Display:** no.

#### N-02 The README's "display re-commands everything" is not true today
- See S-05. Fix it on the display side, then reword the README.

#### N-03 Watchdog timeout
- 3 s is right for **today's** display. With SN-01 unfixed, the longest normal silence is
  ≈ 0.8 s (the DS18B20 wait, while no valve moves and the fan is manual).
- After L-07 (full state every 100 ms), drop it to 500 ms.
- **Display:** dependent.

#### N-04 I2C bus recovery
- `node/io_i2c.c` never calls `i2c_master_bus_reset`. An ADS1115 or BME280 reset during a
  read can leave SDA held low. Every following read then fails, and on the display that means
  S-01.
- **Change:** after 3 consecutive I2C errors, reset the bus (and re-create the bus if that
  fails).
- **Effort:** S.

#### N-05 Error-code mapping
- `ESP_ERR_INVALID_ARG` maps to `ERR_INVALID_PIN` (`node/protocol_server.c:80`), so an ADS
  channel > 3 reports "invalid pin". The display does not branch on codes today, so this is
  cosmetic. Map by command context.
- **Effort:** S.

#### N-06 ADS1115 read timing
- Single-shot at 860 SPS: 1 ms busy-wait, then 100 µs polls, worst case ~20 ms
  (`node/io_i2c.c:141-155`). That fits well inside the display's 200 ms, but the busy-wait
  holds core 1 at priority 10 for ~1.3 ms per read. Fine today.
- For L-06/L-07, move the ADS into a background scanning task (continuous 5-channel round
  robin, ~7 ms per sweep) and answer from the cache.

#### N-07 `LINK_REPLY_ON_CRC_ERROR 0`
- A corrupted command costs the display a full 200 ms timeout.
- Enabling it brings that to ~3 ms, but an `ERROR(6)` sent for line noise could be taken as
  the reply to the next command (L-04).
- **Recommendation:** enable it together with the sequence byte in L-07, not before.

#### N-08 DS18B20 85 °C
- See SN-02. Optional node-side guard: remember whether a START succeeded since boot; if not,
  answer READ with an error.

#### N-09 Pin allow-list
- It correctly excludes straps, USB, flash/PSRAM, console, the link pins and the bus pins.
- It allows 1, 10 and 11, which the carrier does not use. Harmless, but tighten to exactly the
  carrier's outputs plus relay.
- On some ESP32-S3-DevKitC-1 revisions the on-board RGB LED data line is **GPIO48**
  (blower PWM); on v1.1 it is **GPIO38** (foot valve AIN1). See Questions.

#### N-10 Missing `dependencies.lock`
- See B-02.

#### N-11 Bench tool
- `ionode_cli.py` covers every command, including `stress` for round-trip statistics.
- Add a `watch` subcommand: poll `ads-all` at 10 Hz and print min/max per channel. It doubles
  as a pot-noise and wiring check before calibration work (C-03, C-04).

---

## Timing analysis

Assumptions:
- `CONFIG_FREERTOS_HZ=1000`; 921600 baud = 10.85 µs/byte.
- One round trip (RT) today = 2 ms grace (`vTaskDelay(2)` = 1–2 ms) + read slice (1–2 ms)
  + airtime ≈ **2–4 ms, 3.5 ms used below**. An ADS RT finishes inside the grace window.
- A timed-out RT = 2 + 200 + ≤ 2 ≈ **204 ms**.
- After L-01: GPIO/PWM RT ≈ 0.3 ms, ADS RT ≈ 1.7 ms. Estimated, not measured.

Measure on the bench before and after: `ionode_cli.py stress 200` gives the PC-side baseline;
a `tx_frames`-per-second line on the display console gives the real cadence.

| Path | Round trips | Today, node present | Today, node absent (all time out) | After L-01 + L-02 (display only) |
|---|---|---|---|---|
| Valve loop, 0 valves moving | 0 | 25 ms | 25 ms | 25 ms |
| Valve loop, 1 moving | 3 | 25 + 10.5 = **35.5 ms** | 25 + 612 = **637 ms** | 25 + 0 (unchanged cmds suppressed) ≈ 25 ms; ≈ 26 ms on a change |
| Valve loop, 5 moving | 15 | 25 + 52.5 = **77.5 ms** | 25 + 3,060 = **3.1 s** (mutex held throughout) | ≈ 25–30 ms |
| `valve_mutex` hold per iteration (5 moving) | — | ≈ 52 ms (> the 50 ms timeouts at `:344`, `:1655`) | ≈ 3.06 s | < 1 ms (I/O moved outside the lock) |
| Sensor cycle, normal mode | 1 + 1 (DS18B20) + 4–5 (BME280) + 5 (ADS) | 3.5 + 750 + 10 + ~24 + 17.5 + 100 ≈ **0.9 s** → feedback at **1.1 Hz** | 204 (DS start) + 408 (2 BME probes) + 1,020 (ADS) + 100 ≈ **1.7 s**, every position = 0 (S-01) | with SN-01: 5 × 1.7 + 100 ≈ **108 ms** (9 Hz); temperatures every 1 s off the critical path |
| Sensor cycle, calibration mode | 5 ADS | 17.5 + 25 = 42.5 ms × 19 cycles, then one ~0.83 s cycle: **a 0.8 s gap every ~1.6 s** | as above | 5 × 1.7 + 25 ≈ 34 ms, no gaps |
| Valve travel between two feedback samples (10 %/s nominal) | — | **≈ 9 %** (tolerance 2 %) | n/a | ≈ 1 % |
| Blower ramp (500 ms, 25 steps) | 25 | 25 × (20 + 3.5) ≈ 590 ms | 25 × 224 ≈ 5.6 s | ≈ 510 ms |
| Fan AUTO, per 100 ms control tick | 1 (always) | 3.5 ms, every tick | 204 ms, every tick | 0 unless duty changes (C-06) |
| UI "OFF" press | 15 | ≈ 52 ms UI freeze | ≈ 3.1 s UI freeze | ≈ 5 ms (and 0 after U-01) |
| Caller wait before S-06 use-after-free | — | not reachable | 5th queued caller: 4 × 204 + 204 = 1,020 ms > 700 ms limit | n/a (queue removed) |
| L-07 full-state exchange, per control tick | 2 | — | — | ≈ 0.6 ms + cached ADS ≈ **< 1 ms per 100 ms** |

---

## Roadmap

Each phase is a few small, separately revertible commits. Every step is bench-tested with the
display + node + actuators on the bench supply before it goes in the car. **Rollback:** reflash
the previous tagged release of both boards (B-03 step 2 comes first for exactly this reason).

### Phase 0 — prerequisites (½ day)
- **Touches:** B-03 steps 1–3 (git, release tag of the current display build as
  `v1.0-baseline`, version strings), B-01 `save-defconfig`.
- **Bench:** a clean clone builds; the boot log shows the version.

### Phase 1 — safety and correctness, no change to behaviour the owner relies on
- **Touches (display):** S-01, S-02, S-04, S-05, S-06, S-08, S-10, C-01 (interlocks only),
  C-02, C-07 (apply saved fan level, range checks), SN-02, SN-05, the S-03 step 1 task
  watchdog.
- **Touches (node):** N-01, N-04, and the `dependencies.lock` commit.
- **Bench:**
  - Pull the node's UART RX wire mid-move. The motor stops within 3 s (node watchdog); the
    display shows link-down; reconnect restores blower and relay within 1 s (S-05).
  - Hold a valve shaft by hand. Stall within 6 s, fault shown, no further drive.
  - Unplug one pot connector. Feedback-fault after 3 reads, motor stopped.
  - Kill the display (reset button) while a valve moves. The node stops it at reboot (S-04).
  - Set the fan to OFF in DEFROST. The relay drops.
  - Breathe on the BME280 around 65 %RH. The relay does not chatter (`ionode_cli.py` not
    needed: watch the node's `link` line and a scope or LED on GPIO6).
  - Run `ionode_cli.py stress 1000` on the node alone: no failures.
- **Car:** one cold start and one warm drive; compare the logged valve and A/C behaviour with
  the baseline. AUTO should look the same except for the absence of chatter.

### Phase 2 — link performance
- **Touches:**
  - 2a, display only: L-01, L-02, L-04, L-05, SN-01.
  - 2b, both ends together: S-11 (decoder fix + pad off), L-07 full-state exchange behind
    `MSG_VERSION` detection, S-03 step 2 lease, N-03 watchdog 500 ms, N-06 ADS scan task.
- **Bench:**
  - Log RT/s and the valve-loop period; target the "after" column of the timing table.
  - 2b: a new display against the old node falls back; an old display against the new node
    still works; `ionode_cli.py` passes on both message sets; a 10-minute soak shows 0 CRC
    errors and 0 timeouts in the stats (U-03).
- **Car:** valve settling without overshoot; no "mutex timeout" lines in the log.

### Phase 3 — control logic
- **Touches:** C-03 (resolution, tolerance in open-%, start kick), C-04 (persist calibration
  + validation + recalibrate button), C-05 (cascade PI on duct temperature, adaptation behind
  a flag), C-06 (auto fan reachable, warm-up limit), C-09 (if the owner wants it), U-03
  diagnostics.
- **Bench:**
  - Host test of the PI against a first-order cabin + heater model (step response, no limit
    cycle).
  - Each valve: 20 random targets, settle within ±2 % open, no more than one reversal.
  - Power-cycle 10×: no full-revolution calibration after the first; a deliberately moved
    valve is re-validated.
- **Car:** cold start to setpoint, logged once a minute; compare overshoot and time-to-setpoint
  with the Phase 1 log.

### Phase 4 — restructuring
- **Touches:** M-01 one module at a time (link → valve_ctl → hvac_logic → sensors → UI
  shell), M-02, U-01, U-02, CI (B-03 step 4), SN-04 dead-code removal.
- **Bench:** host tests green; on-target behaviour identical to the end of Phase 3. Diff the
  console logs of the same bench script before and after.

### Phase 5 — nice-to-haves
- Delete the adaptation model if the PI holds.
- U-04 status icon.
- The N-11 `watch` CLI command.
- Node replies with error codes to corrupt frames (N-07).
- Log a 1-minute CSV line to the console (or to a small ring in flash) for in-car tuning.
- Brightness auto-dim at night (if a light input exists).

---

## Questions for the owner

1. **What have you noticed?** The section in the brief is empty. These symptoms map directly
   to findings:
   - the blower stopping on its own (S-05);
   - valves hunting or overshooting (SN-01, C-03);
   - the A/C clicking on and off (C-01);
   - "Valve 3 wraparound detected" in the log (S-01);
   - calibration taking much longer than 10 s, or failing (C-04).
2. Are the display and the node on the **same supply and ignition feed**? Can the node brown
   out when the blower starts? (S-05, S-09)
3. Are the valve actuators **continuous-rotation** (pot wraps) or end-stopped? What is the real
   0→100 % travel time at duty 80 and duty 255 (coolant)? `VALVE_FULL_TRAVEL_TIME_MS` says
   10 s. (S-02, C-03, timing table)
4. Is there a **refrigerant pressure switch or engine-running interlock** in series with the
   clutch relay, independent of the firmware? Should A/C ever run below ~3 °C ambient? (C-01)
5. Is the duct DS18B20 **after the heater core and before the outlets** (usable as the inner
   control variable), or somewhere else? (C-05)
6. **Recirculation and rear defrost:** is there a flap, cable or relay these should drive?
   (C-08)
7. Should AUTO request A/C for **cooling**, or is this vehicle heating-only in practice?
   (C-09)
8. Which **ESP32-S3-DevKitC-1 revision** sits on the carrier? Its RGB LED shares GPIO48
   (blower PWM) or GPIO38 (foot valve AIN1). Is the LED populated? (N-09)
9. Do the BTS7960 module and the relay driver have **pull-downs** on their inputs? This
   matters for the ~300 ms between node power-up and `io_hw_init`, when pins float.
10. Is the BTS7960's current-sense (IS) output wired anywhere? It would allow blower stall
    detection.
11. Should the saved **mode** be restored at power-up, or is "always start in AUTO" intentional
    (`main.c:414`)?
12. Is the current display build still the one from the last Jan 2026 commit
    (`project_version 505be1d-dirty`)? If so, what were the uncommitted ("dirty") changes?

---

## Found during Phase 0

- **P0-1 The workspace sources are exactly GitHub `main` (`2725189`).** The zip matched byte for
  byte, apart from three object files under `build/` whose paths exceed the Windows limit.
  The tracked `build/CarHvacController.bin` reports `505be1d-dirty`: it was built from the
  uncommitted state that became `2725189`, with IDF master on another PC
  (`C:/Users/Admin/esp/master/esp-idf`). Tag `v1.0-baseline` points at `2725189`. Its `.bin`,
  bootloader, partition table, `flasher_args.json` and `sdkconfig` are copied, hash-checked
  against the tag, to `../releases/v1.0-baseline/` for attaching to a GitHub release.
- **P0-2 The baseline does not build with a released IDF.** The unpinned `espressif/ds18b20: "*"`
  makes the IDF 6.1 component solver move to 0.4.0, which fails on its `DS18B20_SENSOR_HUB`
  Kconfig condition. `onewire_bus: "*"` would also drift from the vendored 1.0.4 to 1.1.x.
  Both are now pinned to the versions that were running (B-02).
- **P0-3 `managed_components/` is tracked (1,285 files).** That was not on the untrack list, so
  it was left. It makes component bumps noisy diffs; untracking it is safe once the lock file
  is trusted (it is now).
- **P0-4 The running build uses comprehensive heap poisoning**, not the light poisoning the old
  `sdkconfig.defaults` claimed. It costs CPU on every allocation. The new defaults record what
  ran; changing it is a separate decision (Phase 4).
- **P0-5 Display and node use different `onewire_bus` versions** (1.0.4 and 1.1.2). The display
  only uses it in the dead local path, so this does not matter until SN-04 removes it.
- **P0-6 Correction to the review:** the node's `dependencies.lock` only exists after the first
  build; it is now generated and committed (N-10). The nested `CarHvacIoNode/CarHvacIoNode/`
  was an unzip artefact and is flattened. The node is its own git repository (no remote yet),
  ignored by the display repo.
- **P0-7 Windows long paths.** Checking out any commit that still tracks `build/` needs
  `core.longpaths=true` (now set in this repo's config).

---

## Found during Phase 1

- **P1-1 A third mechanism for "everything drops", needing no lost frame.** The UI drew an
  unfilled `hvac_state_t` whenever `hvac_controller_get_state()` could not take
  `state_mutex` within 100 ms (fixed for the screen in U-01, `2a85ec6`). The controller
  holds that mutex for its whole cycle, and inside it each `tb6612fng_set_valve_position()`
  waits up to 50 ms for `valve_mutex`. The valve task holds `valve_mutex` for its whole
  iteration: ≈ 3 round trips × 3.5 ms per moving valve, so ≈ 52 ms with five valves moving.
  In AUTO, two or three target writes per cycle can each wait up to 50 ms, which pushes the
  controller past the UI's 100 ms. That needs only valves moving in AUTO, no timeout and no
  node reset. The bench log tells the three mechanisms apart:
  - `Controller busy ... kept last screen` alone → this one;
  - `TIMEOUT ...` lines → lost replies;
  - `IO NODE RESTARTED` → a node reset.

  L-02 (Phase 2a) takes the UART I/O out from under `valve_mutex`, which removes the cause.
- **P1-2 `hvac_sensors_get_all()` left the caller's struct uninitialised on a mutex
  timeout.** The controller then used stack garbage as temperatures for one cycle. Fixed
  with S-08 (`3b6ca2f`).
- **P1-3 `bts7960_set_speed`/`_ramp` call `bts7960_enable()` while holding the
  non-recursive `blower_mutex`.** If the blower is disabled, that call blocks the caller for
  100 ms and then fails silently. No current code path disables the blower
  (`bts7960_disable` is unused), so this is latent. Not changed.
- **P1-4 Fan slider after AUTO fan (C-06).** The slider keeps its last position while the fan
  is AUTO. Choosing that same level raises no `VALUE_CHANGED`, so the user must move off it
  and back. Left for the UI redesign branch.
- **P1-5 S-03 deviation.** The task-watchdog timeout stays 10 s, not 3–5 s. With the node
  absent and all tasks contending for the link, one sensors cycle can take ~6.5 s
  (`82feebd`).
- **P1-6 Behaviour now live in the car (C-06).** The AUTO warm-up fan boost was dead code
  until now:
  - With a cabin below 15 °C, AUTO runs fan High, windscreen 100 %, coolant 100 % and A/C.
  - The A/C waits at least 10 s after boot (minimum off-time) and needs the blower ≥ LOW.
  - Worth watching on the first cold start.
- **P1-7 Stall threshold vs pot noise.** S-02 calls a valve "moving" when its raw reading
  changes by ≥ 2 %. Pot noise above 2 % would hide a stall. Check with `ionode_cli.py ads-all`
  repeated on a stationary valve.
- **P1-8 S-04 needs the new node.** `MSG_RESET` handling is part of the rewrite. Against a
  node without it, the display logs that the node did not come back and carries on.
- **P1-9 The display image grew 11.8 KB** (Montserrat 12 is now used by the debug tab).
  59 % of the app partition is still free.

---

## Bench checklist — Phase 1

Setup:
- Display, node and all five actuators, blower and relay on the bench supply.
- Display console: its USB port (USB-Serial-JTAG).
- Node console: the DevKitC "UART" USB-C port, 115200 baud.
- Build both from branch `phase-1` and flash: `idf.py -p <port> flash monitor`, in the
  display repo and in `CarHvacIoNode/`.

Console lines are shown as `TAG: text`. "Within N s" counts from the action. Tick each box.

1. **Boot (B-03, S-04, INST, SN-05).** Power both.
   - [ ] Display: `HVAC_MAIN:   Firmware v1.0-baseline-…`
   - [ ] Display: `HVAC_MAIN: IO Node connected!`, then `Resetting IO Node to its safe boot state...`.
     A few `REMOTE_IO: TIMEOUT PING …` lines follow while the node reboots, then
     `IO Node back after reset`, then `REMOTE_IO: IO node: ionode 2.0 (…) up=0 rst=SW`.
   - [ ] Node: `LINK: Reset requested by the display`, then the boot banner again, then
     `Ready, waiting for the display`.
   - [ ] No "Sensor fault" box appears at boot.
   - [ ] Debug tab (swipe left): the bottom lines read `Link tx … timeout …` and
     `Node ionode 2.0 (…) up …s restarts 0`.
2. **Display reboot with a valve moving (S-04).** Press AUTO, then the down arrow (foot) so
   valves start moving, then reset the display.
   - [ ] Every valve motor stops within about 1 s of the display starting to boot (node:
     `Reset requested by the display`). Valves then calibrate as before.
3. **Saved settings (C-07).**
   - Select face (up arrow), fan slider to M-Hi, setpoint 24.0. Wait 6 s.
   - [ ] Display: `HVAC_CTRL: Settings saved to NVS`, once, about 5 s after the last touch.
   - Power-cycle the display.
   - [ ] Face selected, fan M-Hi, 24.0 °C.
   - [ ] The blower actually runs at M-Hi after boot (it stayed off before this build).
4. **AUTO fan (C-06).** Press AUTO.
   - [ ] Display: `Entered AUTO mode, warmup state reset, fan AUTO`; the fan label shows `Auto`.
   - [ ] Move the slider to Low: `Fan level set to: Low` and the blower slows. The mode
     stays AUTO.
5. **Link loss and recovery (S-05, N-01, S-01, INST).** Face mode, fan Medium, A/C on.
   Wait 30 s so the A/C minimum on-time has passed. Pull the wire from display TX to node
   GPIO21.
   - [ ] Display, at once: `REMOTE_IO: TIMEOUT <cmd> [..] from <task>: link wait … +
     command 20x ms (1 in a row, …)`, at most 10 such lines, then every 50th.
   - [ ] Display, within ~2 s: `TB6612FNG: V0: position feedback LOST - motor stopped,
     valve held`, and the same for V1 to V4.
   - [ ] Node, after 3 s: `LINK: No frames for 3000 ms: outputs forced to safe state`;
     the blower stops and the relay drops.
   - Reconnect the wire.
   - [ ] Display: `Link recovered after N timed-out command(s)` and
     `V0..V4: position feedback back - control resumes`.
   - [ ] Node: `Link restored`.
   - [ ] Within ~1 s, without touching the display, the blower runs again and the relay
     re-engages (A/C needs 10 s minimum off-time, so the relay comes back after 10 s).
6. **Node reset seen by the display (INST, S-05).** Press EN/RST on the node.
   - [ ] Within 10 s, display: `REMOTE_IO: IO NODE RESTARTED: uptime … s -> … s, reset
     reason POWERON (1 since boot)` (an EN reset may also report `EXT`).
   - [ ] The blower and relay come back within ~1 s of the node answering.
   - [ ] Debug tab: `restarts 1`.
7. **Stall (S-02).** Change mode so one butterfly valve must move, and hold its shaft
   firmly (low torque, but keep fingers clear of the linkage).
   - [ ] After 6 s: `TB6612FNG: V<n>: STALL (no movement for 6 s) at pos=…% adc=…%
     tgt=…%, will retry`, and the "Valve fault" box appears.
   - [ ] Retries at +10 s, +20 s, +30 s: `retrying after stall (k of 3)`.
   - [ ] On the fourth stall: `… latched until mode change`, and the motor stays off.
   - [ ] Release the shaft and press a mode button: the valve moves to its target.
   - [ ] Pot noise check (P1-7): with that valve idle, run `ionode_cli.py ads <0x48|0x49>
     <ch>` ten times on a PC connected to the node in place of the display. The spread
     must be < 2 % (< ~520 raw).
8. **I2C failure (N-04, S-01).** Disconnect VDD of ADS1115 #1 (0x48) for 5 s, then reconnect.
   - [ ] Node: `IO_I2C: 3 I2C errors in a row: resetting the bus`.
   - [ ] Display: `V0..V3: position feedback LOST …`, then `… back …` after reconnect.
     V4 (coolant, on 0x49) is unaffected.
9. **A/C interlocks (C-01).** DEFROST mode, fan Medium; the relay engages.
   - Move the fan slider to OFF.
   - [ ] At once: `HVAC_CTRL: A/C REQUEST: OFF - Reason: Interlock: blower below LOW`,
     and the relay drops immediately.
   - Move the slider back to Low.
   - [ ] `A/C REQUEST: ON - Reason: DEFROST mode`. The relay engages no sooner than 10 s
     after it dropped.
   - AUTO mode: breathe on the BME280 until humidity is over 65 %.
   - [ ] `A/C REQUEST: ON - Reason: High humidity`.
   - [ ] The relay must not drop before humidity is below 58 % **and** it has been on 30 s.
10. **Sensor loss (S-08, SN-02).** Unplug the BME280.
    - [ ] Within ~5 s: cabin shows `--`, the "Sensor fault" box appears, and
      `TB6612FNG: Valve 4 target: …% -> 50%` (fallback coolant).
    - [ ] Replug: values return; the coolant valve follows AUTO again.
    - Unplug and replug the DS18B20.
    - [ ] `HVAC_SENSORS: DS18B20 power-on value 85.00 C ignored` appears at most once, and
      no 85 °C ever shows on the debug tab.
11. **The owner's "everything drops" (P1-1, U-01).** AUTO, valves moving, 30 min.
    - [ ] The main screen never shows 0.0 / OFF.
    - Note any `HVAC_UI: Controller busy for 100 ms: kept last screen (n …)` lines, and the
      `UI busy`, `timeout` and `restarts` counts on the debug tab, and send them in. They
      say which mechanism it was.
12. **Node alone, with `tools/ionode_cli.py` on a PC** (display link cable unplugged).
    - [ ] `version` prints `ionode 2.0 (…) up=<s> rst=POWERON`.
    - [ ] `gpio-set 1 1` → `ERROR invalid pin` (N-09).
    - [ ] `ads 0x48 5` → `ERROR invalid arguments` (N-05).
    - [ ] `stress 200`: 0 failures.

Not testable from the bench UI: S-10 (no OFF button; `stop_valve` is reached only through a
restored OFF mode) and the S-03 / N-01 task-watchdog paths (no way to hang a task on
purpose). These were checked by reading the code only.

Car (after the bench passes): one cold start below 15 °C cabin in AUTO. Expect:
- the warm-up phase: fan High, windscreen 100 %, coolant 100 %;
- A/C from about 10 s after boot;
- the warm-up ends at ≥ 18 °C with humidity < 60 % (after at least 60 s);
- no relay chatter.

Rollback: flash `../releases/v1.0-baseline/` with the offsets in its `flasher_args.json`.
The node has no earlier build to go back to.
