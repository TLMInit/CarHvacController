# Decisions — v2.1-display / v2.2-node

Decisions taken without stopping, per the brief. Newest at the bottom of each section.

## Owner answers (2026-10-30)
- rc2: implement from the brief's four points (done in `52f83e7`).
- Node board: ARCELI ESP32-S3 WROOM-1 N16R8 DevKitC-1 (16 MB flash, 8 MB octal PSRAM). Status LED stays off (`NODE_STATUS_LED_GPIO = -1`).
- AP password may be shown on the settings page and the phone page.
- No recirculation flap on this vehicle: new setting `SET_RECIRC_FITTED` (default **no**). When no, the recirc button is hidden, presets leave recirc alone, and recirc never changes control.
- Cascade: free design; classic stays the default and is selectable in the settings.

## Base
- Display base = `v2.0-display-rc1` + rc2 (`52f83e7`) + Phase 2a (L-01 `ba77250`, L-04 `c1858c9`, L-05 `c5fdc46`, L-02 `41132d2`, SN-01 `9911146`) on branch `v2.1/integration`. L-05 conflicts resolved toward the UI redesign's `remote_io_is_connected()`; the 2.0 no-op functions are gone.
- Node base = `v2.1-node`, branch `v2.2/integration`.

## Interfaces
- `main/CMakeLists.txt` uses `SRC_DIRS "."` so packages add files without touching it (parallel packages would otherwise all conflict there).
- Interfaces not yet implemented are satisfied by one placeholder file per package (`stub_wpa.c`, `stub_wpc.c`, ...), deleted by that package. Not weak symbols: in a static library a weak definition can be linked instead of the strong one.
- `event_log.c` is implemented by the lead with the interfaces (40 lines; every package logs into it).
- settings: two types beyond the brief — `SET_TYPE_STRING` (AP password) and `SET_TYPE_ACTION` (calibrate-now, factory reset, node reboot). Per-valve settings are one id per valve.
- Defaults chosen so v2.1 behaves like v2.0 until the owner changes a setting: AUTO-requests-A/C **off**, heater delay **off**, dew-point defog **off**, control mode **classic**, quiet-night cap **off**, recirc fitted **no**, DEF-restores-A/C **on** (fixes the reported issue that quick DEF left the user A/C on).
- Settings that 2.0 kept elsewhere (power-up mode/fan/setpoint in namespace `hvac`, DAY/NIGHT in the UI) stay where they are as runtime state; the settings table holds preferences. `hvac_thermal` adopts the versioned scheme when WP-E touches it.
- sensors: the role logic lives in `hvac_sensors.c` and is owned by WP-C (the brief lists no owner); WP-C already owns the link it runs on. First boot with no assignments and exactly one ROM found: that ROM becomes DUCT.
- `cascade.c` (pure PI) is added to WP-E's files next to `comfort.c`, so WP-T can test both on the host.
- `main.c` is not in any package: the lead adds each package's init calls at merge time.

## Protocol
- `PROTOCOL_VERSION` 1 -> 2. `HELLO_RESP` = proto(1), caps(1), version string. `OTA_ACK` = state(1), bytes u32 BE. New error codes 9-11 for OTA. Big-endian, like the existing temperature reply.
- Pad handling: the node pads only until it has seen HELLO (per boot). The display's decoder no longer strips (S-11) but tolerates one trailing pad byte when the CRC fails without it, for the window before HELLO and for 2.1 nodes.

- Correction found during WP-B/WP-C: with this CRC (init 0xFFFF, no final XOR) a padded reply `M|crc_hi|00|00` also passes the CRC check, so "retry without the pad when the CRC fails" never fires. Rule used instead (display, `strip_pad()` in `remote_io_binary.c`): while padding is possible (before a protocol-2 HELLO_RESP, for a protocol-1 node, and after any reset or link recovery until the next HELLO) a reply whose CRC field is 0x0000 and whose one-byte-shorter form also checks out loses one trailing byte. Cost: 1 genuine reply in 65536 mis-stripped, only in that window. After HELLO nothing is stripped. The node's `ionode_cli.py` uses the same rule.

## Wave 1 merge notes (rc1)
- WP-A: settings are written 2 s after the last change (one write per burst); a power cut inside that window loses that change. A key set back to its default is erased, so a future default change reaches users who never touched it.
- Lead change after WP-A: the empty AP password no longer expands to a value derived from the softAP MAC (the BSSID broadcasts it). A random 10-character password is drawn once with the hardware RNG and kept in `hvac_cfg/ap_pass_gen`; factory reset draws a new one. It is shown on the settings page (owner answer 2).
- WP-D: a butterfly valve whose pot rests in the dead zone at boot fails the stored-record check and recalibrates that boot; its position is unknown there anyway. A valve without any position reading stays held (no timeout), as in 2.0 without feedback. Coolant valve: recalibrate/learn return NOT_SUPPORTED (linear, direction setting only).
- WP-E: `quick_cool` still switches recirc on as in 2.0; recirc has no effect when `SET_RECIRC_FITTED` is off and the button is hidden (WP-F). A quiet-night cap below 30 % lets the A/C interlock drop the compressor. The AUTO fan now goes through the soft-start ramp with change-only sends. Cascade default gains are first guesses (toy-model check only): tune on the car.
- WP-H: the self-test runs once per boot after calibration; a link that is down in that window reports the blower FAIL until the next boot.
- WP-C: p95 RTT is computed from six 10 s histograms (100 µs buckets to 2 ms, 1 ms to 30 ms) instead of a sample ring (would be ~24 KB). Unassigned ROMs are read too, at most 4/s, so the role page can show which sensor is which. Commanded resets (boot, OTA, node reboot) do not count as node restarts.
- Merge order kept: A → D, E, H → rc1. B and C wait for F (rc2), as the brief orders.

## rc2 (WP-F)
- Page order main <-> diagnostics <-> settings. Inside a settings sub-page swipes do nothing and BACK is explicit, so a held valve jog cannot be cut off by a swipe.
- Presets from a long press on AUTO and from the first settings row.
- Danish needs æ/ø/å, which the built-in Montserrat lacks: Montserrat-Medium 14 and 16 px subsets (ASCII, °, æøåÆØÅ, the symbols used) generated with lv_font_conv 1.5.2 from the TTF shipped in the LVGL package, ~19 KB flash. Diagnostics, valve service and sensor pages stay English; the main page is unchanged.
- DAY/NIGHT now toggles `SET_NIGHT_MODE`; the 2.0 private "night_mode" key is not migrated (one tap after updating). Brightness, auto-dim and night screen-off settings are applied by the UI; a transparent overlay swallows the touch that wakes a dark screen.
- Button-context pool 32 -> 64 (the Valves group alone has 17 rows).

## rc3 (WP-B, WP-C, WP-G)
- Display partition table: nvs kept at 0x9000/0x6000 so settings, calibration and thermal learning survive; otadata 0x10000, ota_0 0x20000 and ota_1 0x420000 (4 MB each). First flash of rc3 must write bootloader, table and otadata (the archived flash_args does).
- Node partition table on 2 MB: ota_0 0x10000, ota_1 0x100000 (960 KB each). Same first-flash rule.
- Internal RAM: with Wi-Fi linked, static DIRAM was 78 % used. LVGL's 128 KB pool now comes from one malloc in PSRAM (lv_conf macros via build-wide compile definitions; LVGL 8 has no Kconfig entry). Static DIRAM 267 KB -> 136 KB. Wi-Fi IRAM optimisations off (WP-G, 18 KB). `wifi_ap.c` logs the free internal heap on every AP start.
- `wifi_ap_init()` failing is a warning, never a reboot: the phone page is optional, the climate control is not.
- Phone access: anyone with the WPA2 password can read and change settings (not the action rows) and upload firmware, the latter only with mode OFF and the blower stopped. No separate web login: the WPA2 password is the gate. In 10 min mode an AP that timed out stays off until the setting is chosen again or the next boot.
- Display OTA is kept after 30 s of running UI with the node link up; with no link at all it reboots after 5 min and the bootloader rolls back. Node OTA: kept after one PING and one VERSION within 60 s, else rollback.
- Node OTA_DATA with a wrong offset returns ERR_OTA_WRITE but keeps the transfer, so the display resumes from OTA_STATUS.
- Archived flash sets: `flash_args` rewritten to the flat file names of the archive (earlier sets fixed in place, sums regenerated).

## Final (WP-T, WP-K)
- WP-T's host test found a latent node bug: a full-size reply that needs the compatibility pad did not fit `send_frame`'s buffer and was dropped. Unreachable today (no reply that long is sent while padding is on) but fixed (*node* 3a6a1e8); the test runs by default.
- `strip_pad()` is static in the display link layer, so the host test checks a reference copy of the rule plus the real `cobs_parse_frame` on the documented byte sequences. The node's HELLO handling is not host-tested (needs ESP-IDF).
- CI is written for GitHub Actions (host tests on ubuntu, firmware build in `espressif/idf:v6.1` failing on any `main/` warning) but has not run: neither repo is pushed.

## v2.2-display / v2.3-node — the eleven review findings

Step A (safety; tags v2.1.1-display / v2.2.1-node):
- 1: the bookkeeping moved into `motor_link.c` (pure) so the requested host test exercises the real code instead of a copy. The 1 s re-assert lives in the valve task (owner of the motors) rather than in the controller's S-05 re-assert. Calibration and service sends are tracked too, because they also drive the node directly. While the link is down a retry waits for the 1 s re-assert: the link layer does not fail fast, and five valves retrying every 25 ms would hold the valve task in 200 ms timeouts. Cost: 10 small frames per second for idle valves.
- 2: rule exactly as specified; reasons in the log say which branch fired.
- 3: both boards check the same 288 header bytes with the same pure `ota_image.c`. `esp_ota_begin` (the slot erase) moved behind the check on both boards, so "rejected before any flash write" holds literally. The display also checks a node image before sending it; the node returns the new `ERR_OTA_IMAGE` (12) if it ever receives one anyway (e.g. from `ionode_cli.py`).
- 5: the calibration tick switches fast reads on for learn-direction moves too, which the scattered calls did as well.
- 6: the diagnostics tab keeps RESET CAL and SAVE CAL at their positions; the gap stays (layout untouched).
- 9: as specified.

Step B:
- 4: the five `hvac_sensors_get_*_valve_pos()` getters had no callers and went with the array. `HARDWARE_PHOTO_CHECKLIST.md` item 8 is the bench checklist that names these rows.

Step C:
- 7: the old local path no longer compiled anyway (it referenced a removed `ds18b20_coolant`). `managed_components/` is tracked in this repo, so the two vendored components were removed with `git rm` in a second commit for the same finding. `dependencies.lock` changed only in those two entries and the manifest hash; every remaining component keeps its version and hash. `sdkconfig` did not change.
- 8: dead = found by grep only at its declaration, definition and comments, repeated until stable (47 public functions plus `calibrate_valve_internal`, the callback type and storage nothing could register, one table and one define). Kept: a few unused `#define`s that were never local-path code, and the unused `i2c_bus` parameters of two init functions (API and log output unchanged).
- 11: `cobs_crc16()` is the same CRC as the store's copy (CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflection, no xor; equal on "123456789" and 20 000 random buffers), so stored calibration records stay valid.
- Diff for step C: 31 files, +37 / −1,826 lines (the brief estimated about −1,200; finding 8 had more dead functions than counted). The linker had already dropped most of it: app −208 bytes.

Step D:
- 10: `sdkconfig` regenerated from the defaults; it differs from the -Og one only in the optimisation choice and `CONFIG_FREERTOS_TASK_FUNCTION_WRAPPER`, which IDF offers only at -Og. Assertions stay on. Display app 0x169f70 → 0x1662c0 (−1.0 %), node 0x4a780 → 0x46840 (−5 %). No new warnings at -O2. `hvac_ui_update` timing needs the board; BENCH.md asks for the log line from both builds.
- The node's VERSION/HELLO prefix is now `ionode 2.3`, matching the release.

## v2.3-display / v2.4-node — hotfix and settings release

- The brief named the starting points `v2.2-display (74b89da)` and `v2.3-node (3a6a1e8)`; those hashes are `v2.1-display` and `v2.2-node`. The tags were used: `v2.2-display` = d068015, `v2.3-node` = 630a6e4.

Step A (hotfix; tags v2.2.1-display / v2.3.1-node):
- 1: while the link is down no valve command is due, not even the 1 s re-assert; on the first tick it is back, both parts of every commanded valve are sent once. Skipped refreshes neither mark a stop lost nor write an event. Idle cost: 15 frames/s with the link up (two GPIO_SET and one PWM_SET per valve per second), 0 while it is down. "Down" for the valve task is the link layer's 3 s window **or** the last command on the link (any task) having timed out (`remote_io_last_command_timed_out()`); the sensors task polls every 100 ms, so the valve task notices a broken link within one command instead of after 3 s, and sees it back with the first answered poll. A direction send stops after its first failed GPIO command and the speed is not tried in that pass, so the pass in which the link breaks costs at most one command timeout (200 ms) plus a wait for the link mutex if another task's command is timing out at that moment. The bench criterion "longest pass under 100 ms" therefore applies to every pass while the link is down except the one in which it breaks; that one is bounded at roughly 0.4 s, far from the 1 s event threshold and the 10 s task watchdog. Failing fast for every task would change the sensors' and controller's probing, which this hotfix leaves alone. The valve task logs `Link down: valve commands paused` / `Link back after N ms: every commanded valve re-sent in this pass` so the bench can see the refresh.
- 2: the longest pass is printed on main.c's 10 s status line; the event threshold is 1000 ms (one tenth of the task watchdog).
- 3: the sweep is a script (non-static definitions in `main/*.c` of both repos, uses searched in `main/` and `test/host/` with comments stripped). It found exactly the two named functions; afterwards nothing in either repo.
- 4: the failed-start text stays on the diagnostics tab's new last row until a start succeeds or the mode goes to OFF. The free internal heap with the AP running can only be read on the board: BENCH.md asks for it. (Static internal RAM at link time is 136 KB used of 342 KB.)
- The node has no change in step A; `v2.3.1-node` tags the same code as `v2.3-node` so the pair stays named together.

## Models
- Strongest available model (Opus) for WP-A, WP-B, WP-C, WP-G, WP-H; faster model (Sonnet) for WP-D, WP-E, WP-F, WP-T. WP-A is not named in the brief; it is the framework every package depends on, so it gets the stronger model.
