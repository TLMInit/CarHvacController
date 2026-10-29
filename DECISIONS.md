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

## Models
- Strongest available model (Opus) for WP-A, WP-B, WP-C, WP-G, WP-H; faster model (Sonnet) for WP-D, WP-E, WP-F, WP-T. WP-A is not named in the brief; it is the framework every package depends on, so it gets the stronger model.
