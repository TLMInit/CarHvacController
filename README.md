# CarHvacController — display firmware

Touch display and controller for a retrofitted car HVAC. Runs on a Waveshare
ESP32-S3-Touch-LCD-2 (320×240, 16 MB flash, 8 MB PSRAM) with ESP-IDF 6.1 and LVGL 8.4.
All actuators and the 1-Wire sensors sit on the IO node (`CarHvacIoNode/`, its own repo
and README), reached over UART at 921600 baud with COBS + CRC16 frames.

| Doc | What is in it |
|---|---|
| `BENCH.md` | How to flash and check a release on the bench and in the car |
| `DECISIONS.md` | Every decision taken for v2.1 to v2.3 and why |
| `DESIGN.md` | UI design, page map, module and task layout |
| `REVIEW.md` | The review findings and their status |
| `CHANGELOG.md` | What changed per release |

## Build

ESP-IDF v6.1, target esp32s3. Managed components are pinned in `dependencies.lock`.

```
idf.py set-target esp32s3     # once
idf.py build
idf.py -p COMx flash monitor
```

Since v2.1 the partition table has two app slots (`partitions.csv`). Coming from v2.0 or
earlier, the first flash must write bootloader, partition table and otadata: `idf.py flash`
does, or `esptool ... write_flash "@flash_args"` from a release folder. NVS keeps its old
place, so settings, valve calibration and thermal learning survive.

## Using it

- **Pages**: swipe main ↔ diagnostics ↔ settings. Settings holds the presets, the eight
  setting groups (Valves starts with the air distribution grid), diagnostics (event log, link
  health, self-test, trend chart) and service (valve jog/recalibrate/learn direction, the
  Service settings, sensor roles). Valve service and the Service settings need mode OFF. The
  Advanced group is on the phone page only.
- **Presets**: long press AUTO, or the first settings row.
- **Console** (USB, same port as the log): `list [group]`, `get <key>`, `set <key> <value>`,
  `reset <key|all>`, `help`. Example: `set fan_max 80`.
- **Phone page**: Settings → Connect → Phone Wi-Fi (Off / Auto off / Always; Auto off after
  Connect → Wi-Fi auto-off after, default 10 min). Join
  `HVAC-xxxx` with the password shown there and open `http://192.168.4.1`: status, settings,
  event log, diagnostics and firmware upload for the display and the node. Uploads are
  refused unless the system is OFF and the blower has stopped.
- **Electric compressor** (v2.4, needs node v2.5): Settings → A/C → Compressor type =
  Electric shows the output rows. Speed output PWM (GPIO10, 100–20000 Hz), Analog (RC) (GPIO10
  through the node README's 10 kΩ / 10 µF filter, 0–3.3 V) or DAC MCP4725 (node I2C, 0x60/0x61).
  The A/C relay (GPIO6) is the compressor's enable: wire it to the enable input. The compressor
  runs at Minimum speed or more, ramps at Speed ramp, and switches off after Off after no demand
  without a request. Assign a sensor the Evaporator role (Service → Sensor roles) for the freeze
  limits and the evaporator loop; with the clutch type the same role replaces the duct freeze
  cutout. Diagnostics shows `A/C ELEC dem .. out .. EN ev ..`. Service → Compressor test runs it
  with mode OFF: start the blower with BLOWER LOW, set the demand with − / +; it stops after 60 s
  without a touch or when you leave the page.
- **Updates**: a new display image is kept only after it has run the UI for 30 s with the
  node link up; otherwise the bootloader rolls back. The node does the same after one PING
  and one VERSION within 60 s.

## Tests

Host unit tests (no ESP-IDF, no hardware) live in `test/host/`: settings table and cross-field rules,
NVS schema/migration (`settings_ns_open` against a fake NVS), dew point and defog, cascade control,
COBS + CRC and the pad rule, valve motor bookkeeping, valve calibration records, OTA image check,
the electric compressor law and its output bytes (PWM_INIT_HZ, 16-bit PWM_SET, MCP4725).
Firmware sources are compiled as they are; ESP-IDF headers are faked under `test/host/fakes/`.

```
cmake -S test/host -B ../tbuild-display
cmake --build ../tbuild-display
ctest --test-dir ../tbuild-display --output-on-failure
```

CI (`.github/workflows/ci.yml`) runs these tests, then builds with `espressif/idf:v6.1` and fails on any
compiler warning from `main/`.
