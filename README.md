# CarHvacController — display firmware

Touch display and controller for a retrofitted car HVAC. Runs on a Waveshare
ESP32-S3-Touch-LCD-2 (320×240, 16 MB flash, 8 MB PSRAM) with ESP-IDF 6.1 and LVGL 8.4.
All actuators and the 1-Wire sensors sit on the IO node (`CarHvacIoNode/`, its own repo
and README), reached over UART at 921600 baud with COBS + CRC16 frames.

| Doc | What is in it |
|---|---|
| `BENCH.md` | How to flash and check a release on the bench and in the car |
| `DECISIONS.md` | Every decision taken for v2.1 / v2.2 and why |
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
  setting groups, diagnostics (event log, link health, self-test, trend chart) and service
  (valve jog/recalibrate/learn direction, sensor roles). Valve service needs mode OFF.
- **Presets**: long press AUTO, or the first settings row.
- **Console** (USB, same port as the log): `list [group]`, `get <key>`, `set <key> <value>`,
  `reset <key|all>`, `help`. Example: `set fan_max 80`.
- **Phone page**: Settings → Connect → Phone Wi-Fi (off / 10 min / always). Join
  `HVAC-xxxx` with the password shown there and open `http://192.168.4.1`: status, settings,
  event log, diagnostics and firmware upload for the display and the node. Uploads are
  refused unless the system is OFF and the blower has stopped.
- **Updates**: a new display image is kept only after it has run the UI for 30 s with the
  node link up; otherwise the bootloader rolls back. The node does the same after one PING
  and one VERSION within 60 s.

## Tests

Host unit tests (no ESP-IDF, no hardware) live in `test/host/`: settings table, NVS schema/migration
(`settings_ns_open` against a fake NVS), dew point and defog, cascade control, COBS + CRC and the pad rule.
Firmware sources are compiled as they are; ESP-IDF headers are faked under `test/host/fakes/`.

```
cmake -S test/host -B ../tbuild-display
cmake --build ../tbuild-display
ctest --test-dir ../tbuild-display --output-on-failure
```

CI (`.github/workflows/ci.yml`) runs these tests, then builds with `espressif/idf:v6.1` and fails on any
compiler warning from `main/`.
