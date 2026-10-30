# Bench and car check — v2.1-display with v2.2-node

Flash sets with SHA-256 sums are in `../releases/v2.1-display` and `../releases/v2.2-node`.
Check the sums first (`Get-FileHash *.bin`) against `SHA256SUMS.txt`.

## 1. Flash (both boards change partition table: full flash once)

```
cd ..\releases\v2.2-node
python -m esptool --chip esp32s3 -p COM_NODE -b 921600 write_flash "@flash_args"
cd ..\v2.1-display
python -m esptool --chip esp32s3 -p COM_DISPLAY -b 921600 write_flash "@flash_args"
```

Settings, valve calibration and thermal learning on the display survive (NVS keeps its place).
Later updates can go over the phone page.

## 2. First boot (display console)

| Expect | Means |
|---|---|
| `SETTINGS: hvac_cfg schema 1: N keys loaded, 0 rejected, 66 rows total` | settings loaded |
| `HELLO: node protocol 2, caps 0x03 (search ota), ionode 2.2 (...)` | handshake; pad off from here |
| `1-Wire search: 1 ROM(s)` and `Sensor roles: single ROM assigned DUCT (migration from 2.0)` | existing duct sensor kept its role |
| `V0: calibrating (no stored calibration ...)` then `CALIBRATED`, same for V1–V3; `V4: fixed calibration (linear valve)` | first boot stores calibration |
| `SELFTEST: PASS ...` lines, then `EVENT: INFO: Self-test OK` | all checks pass |

Node console: `LINK: HELLO from display protocol 2: reply padding off`.

## 3. Second boot

Every butterfly valve logs `using stored calibration (...)` and does not rotate. Boot to
control is seconds instead of a full calibration round.

## 4. Display checks

1. Swipe main → diagnostics → settings. No button fires during a swipe.
2. Settings → Display → Language: Dansk. Labels switch, æ/ø/å render.
3. Climate → Recirc fitted: No (default) — no recirc button on the main page.
4. Long press AUTO → presets. Pick Max heat: foot, 30 °C, fan high, A/C off; touching the
   setpoint clears the preset highlight.
5. A/C off, quick DEF, leave DEF: A/C is off again.
6. Diagnostics → Link health: RTT last/p95 in the low ms, 0 timeouts/min at rest.
7. Diagnostics → Trend chart: one point every 10 s.
8. Mode OFF → Service → Valve service: hold + on a valve, it moves; release, it stops. With
   mode AUTO the jog buttons are greyed with the reason.
9. Service → Sensor roles: Search lists each DS18B20 with its temperature; assign a second
   sensor to Coolant or Outside if fitted.

## 5. Console

`list fan` · `set fan_max 80` (reboot after 2 s: `1 keys loaded`) · `set hum_on 55` then
`get hum_off` → 53 · `reset all`.

## 6. Phone page and updates

1. Settings → Connect → Phone Wi-Fi: Always. Console: `Free internal heap with AP on: N` —
   note N (expect tens of KB; report it if under 20 000).
2. Join `HVAC-xxxx` with the password on the Connect page, open `http://192.168.4.1`.
   Status refreshes every 2 s; change a setting there and see it on the display.
3. Update tab with mode AUTO: upload is refused with "Switch the system OFF first".
4. Mode OFF, blower stopped: upload `CarHvacIoNode.bin` from the node release folder.
   Progress runs, node reboots, node console `OTA: New image confirmed`.
5. Upload `CarHvacController.bin`: display restarts; after 30 s with the link up the event
   log shows `New display firmware validated`.
6. Rollback check (optional): unplug the link cable, upload the display image again, leave
   the link unplugged: after 5 min the event log shows `rolling back`, the display reboots into
   the previous image and logs `Rolled back: image in ota_x was rejected`.

## 7. Link loss

Unplug the link cable for 5 s with the system running: `LINK LOST` badge, event log
`N link timeout(s)`; plug back: `Link up again: HELLO queued`, then a HELLO line, control
resumes. Outputs on the node go safe while the link is down (node watchdog).

## 8. In the car, 20 minutes

Cold start in AUTO, then drive. Check and note:
- valves settle without hunting; no `stall` lines in the event log;
- AUTO reaches the setpoint and holds it within about 1 °C;
- event log after the drive: number of timeouts and node resets (expect 0 resets);
- Diagnostics → Link health: timeouts/min and p95 while driving.

Optional, one at a time, each on its own drive: heater delay on; dew-point defog on (needs an
outside sensor); Control mode: Cascade. Cascade gains are first guesses — if the cabin
overshoots by more than 1 °C or the coolant valve swings, lower `CASCADE_OUTER_KP` first.

## What to send back

Console log of the first and second boot, the `Free internal heap with AP on` number, the
event log after the drive (phone page Log tab), and anything that surprised you.
