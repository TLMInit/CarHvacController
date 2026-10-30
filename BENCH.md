# Bench and car check — v2.2-display with v2.3-node

Flash sets with SHA-256 sums are in `../releases/v2.2-display` and `../releases/v2.3-node`.
Check the sums first (`Get-FileHash *.bin`) against `SHA256SUMS.txt`.

Coming from v2.1/v2.2 or v2.1.1/v2.2.1 the partition tables are unchanged: the phone page or
the full `@flash_args` set both work. Coming from v2.0 or earlier, flash the full set once
(section 1). The checks below go in this order: the safety fixes (also valid for v2.1.1), the
-O2 build, then the general v2.1 procedure in sections 1–8.

## Hotfix v2.2.1-display (items 1–4)

The node is unchanged in this hotfix (`v2.3.1-node` is the same code as `v2.3-node`).

**Link outage (items 1, 2).** System in AUTO, valves settled, display console open.
1. Pull the node's RX wire (display TX → node RX) and leave it out for 30 s, then reconnect.
2. Expect, in this order:
   - `Link down: valve commands paused` within about 0.2 s of pulling;
   - while it is out: the 10 s status lines `Valve task: longest pass N ms` with N under 100,
     apart from the 10 s window in which the link broke (that pass can take one command
     timeout, up to about 0.4 s with another task's timeout in front of it); no `V<n>` lines
     and no valve events in the event log; after 3 s the node's own watchdog stops its outputs;
   - on reconnecting: `Link back after N ms: every commanded valve re-sent in this pass`,
     and the valves resume. The event log has no `stop resent` entry unless a stop was in
     flight at the moment of pulling.
3. Record for the report: N of every status line during the outage, and whether any
   `Valve pass ... ms` event appeared (it must not).

**Wi-Fi (item 4).** Settings → Connect → Phone Wi-Fi: Always.
1. The console prints `Free internal heap with AP on: N (largest block M)`. Write N and M
   here: ____ / ____ (needed for the report; this cannot be measured off the board).
2. Diagnostics tab, last row: `WI-FI ON 0 PHONE(S)`; join with a phone: `1 PHONE(S)`.
3. A failed start (not provoked on purpose) would show `WI-FI FAILED <step>: <error>` on that
   row, an `AP start failed` event at most every 10 s, and a retry every second.

## Safety fixes (findings 1, 2, 3, 5, 6, 9; first shipped as v2.1.1-display / v2.2.1-node)

1. **Lost stop (1).** System in AUTO, Diagnostics → Valve service open on one valve. Change the
   setpoint so that valve moves; the instant it reaches its target, pull the node's RX wire
   (display TX → node RX) and reconnect it within 2 s. The motor must stop within one second
   of reconnecting, and the event log shows `V<n> stop resent after a lost command`. Keep the
   wire out for less than 3 s: after 3 s of silence the node's own link watchdog (the display's
   `REMOTE_IO_LINK_WINDOW_MS` from HELLO; `LINK_WATCHDOG_MS` on a node that has not had one)
   stops every output, which would hide whether the display resent the stop.
2. **Stored calibration (2).** Mode OFF so every butterfly valve closes, wait until they have
   stopped, reboot. Repeat once. Each boot logs `V<n>: accepted stored (...)` for V0–V3 and no
   valve rotates.
3. **Wrong image (3).** Mode OFF, blower stopped, phone page → Update → *display* box: upload
   `CarHvacIoNode.bin`. Refused with `Wrong firmware: image is CarHvacIoNode, this board runs
   CarHvacController`; the display keeps running, the event log shows `Display OTA refused`.
   The reverse (display image in the node box) is refused with the same kind of message.
4. **Calibration getter (5).** Service → Valve service → Learn direction on a valve, and keep
   the page open: the console shows `Calibration mode enabled` once at the start and
   `disabled` once at the end, nothing in between.
5. **READ 1W (6).** The diagnostics tab no longer has the button; Service → Sensor roles shows
   the live temperatures.
6. **UI cadence (9).** With nothing touched for a minute, change the setpoint from the phone
   page: the display shows it within half a second.
7. **Valve rows in open % (4).** Diagnostics tab with the system settled: each valve's `ACT`
   is within a few percent of its `TARGET`; a closed valve reads `ACT 0..3% TARGET 0%`.

## Release build at -O2 (finding 10)

-O2 can surface timing and uninitialised-variable bugs that -Og hid, so run this as a whole,
on the bench with the car's valves, blower and sensors connected:

1. Before flashing, note the `hvac_ui_update avg=... max=...` line (every 5 s on the display
   console) from the build you run now (v2.1.1 or older, -Og).
2. Flash both boards. Full boot: no panic, no `Guru Meditation`, no task watchdog line; self-test OK.
3. Calibration: Settings → Valves → Recalibrate at boot: On → reboot. Every butterfly valve
   calibrates and logs `CALIBRATED`; reboot once more, every valve logs `accepted stored`.
4. AUTO for ten minutes at a setpoint a few degrees from cabin: valves settle without hunting,
   the event log has no `stall` and no node reset, Link health shows 0 timeouts/min at rest.
5. Note the `hvac_ui_update avg=... max=...` line again and compare it with step 1.
6. Pull the node RX wire as in safety check 1; the stop is re-asserted within a second.
7. One OTA round trip: node image through the phone page, then the display image; both
   confirm (`OTA: New image confirmed` on the node, `New display firmware validated` in the
   display event log).

## 1. Flash (both boards change partition table: full flash once)

```
cd ..\releases\v2.3-node
python -m esptool --chip esp32s3 -p COM_NODE -b 921600 write_flash "@flash_args"
cd ..\v2.2-display
python -m esptool --chip esp32s3 -p COM_DISPLAY -b 921600 write_flash "@flash_args"
```

Settings, valve calibration and thermal learning on the display survive (NVS keeps its place).
Later updates can go over the phone page.

## 2. First boot (display console)

| Expect | Means |
|---|---|
| `SETTINGS: hvac_cfg schema 1: N keys loaded, 0 rejected, 141 rows total` | settings loaded |
| `HELLO: node protocol 2, caps 0x03 (search ota), ionode 2.3 (...)` | handshake; pad off from here |
| `1-Wire search: 1 ROM(s)` and `Sensor roles: single ROM assigned DUCT (migration from 2.0)` | existing duct sensor kept its role |
| `V0: recalibrating: no stored calibration (...)` then `CALIBRATED`, same for V1–V3; `V4: fixed calibration (linear valve)` | first boot stores calibration |
| `SELFTEST: PASS ...` lines, then `EVENT: INFO: Self-test OK` | all checks pass |

Node console: `LINK: HELLO from display protocol 2: reply padding off`.

## 3. Second boot

Every butterfly valve logs `accepted stored (...)` and does not rotate. Boot to
control is seconds instead of a full calibration round.

## 4. Display checks

1. Swipe main → diagnostics → settings. No button fires during a swipe.
2. Settings → Display → Language: Dansk. Labels switch, æ/ø/å render.
3. Climate → Recirc fitted: No (default) — no recirc button on the main page.
4. Long press AUTO → presets. Pick Max heat: foot, 30 °C, fan high, A/C off; touching the
   setpoint clears the preset highlight.
5. A/C off, quick DEF, leave DEF: A/C is off again.
6. Diagnostics → Link health: RTT last/p95 in the low ms, 0 timeouts/min at rest.
7. Diagnostics → Trend chart: one point every 10 s (default of Display → Trend point every, 5..60 s).
8. Mode OFF → Service → Valve service: hold + on a valve, it moves; release, it stops. With
   mode AUTO the jog buttons are greyed with the reason.
9. Service → Sensor roles: Search lists each DS18B20 with its temperature; assign a second
   sensor to Coolant or Outside if fitted.

## 5. Console

`list fan` · `set fan_max 80` (reboot after 2 s: `1 keys loaded`) · `set hum_on 55` → `note:` set to 60, at
least 2 %RH above Humidity A/C off (the other setting is never moved), so `get hum_off` → 58 ·
`reset all`.

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
overshoots by more than 1 °C or the coolant valve swings, lower Control → Cabin loop Kp (`outer_kp`) first.

## What to send back

Console log of the first and second boot, the `Free internal heap with AP on` number, the
event log after the drive (phone page Log tab), and anything that surprised you.
