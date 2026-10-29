# CarHvacIoNode

Firmware for the carrier-board ESP32-S3 of the car HVAC system. The other half,
[TLMInit/CarHvacController](https://github.com/TLMInit/CarHvacController), runs on a
Waveshare ESP32-S3-Touch-LCD-2 and contains the HVAC logic and the LVGL user
interface. That firmware calls this board a "Remote IO Node": it sends every GPIO,
PWM, I2C and 1-Wire operation over a UART link, and this node executes them.

The original node firmware was lost; this is a rewrite against the protocol and pin
map found in the display repo (`cobs_protocol.h`, `remote_io_binary.c`,
`hvac_hw_config.h`). The display firmware needs no change.

## Hardware (ESP32-S3 DevKitC-1 on the carrier PCB)

| Function | GPIO | Notes |
| --- | --- | --- |
| UART link to display | TX 2, RX 21 | 921600 8N1. Display side: TX GPIO10, RX GPIO11 |
| I2C: 2x ADS1115, BME280 | SDA 8, SCL 9 | 400 kHz. ADS1115 at 0x48 (ADDR→GND) and 0x49 (ADDR→VDD), BME280 at 0x76/0x77 |
| 1-Wire: DS18B20 duct sensor | 7 | 4.7 kΩ pull-up on the carrier |
| TB6612 #1, center valve 1 | AIN1 4, AIN2 16, PWMA 17 | feedback pot on ADS 0x48 A0 |
| TB6612 #1, center valve 2 | BIN1 5, BIN2 18, PWMB 15 | ADS 0x48 A1 |
| TB6612 #2, foot valve | AIN1 38, AIN2 39, PWMA 40 | ADS 0x48 A2 |
| TB6612 #2, windscreen valve | BIN1 41, BIN2 42, PWMB 47 | ADS 0x48 A3 |
| TB6612 #3, coolant valve | AIN1 12, AIN2 14, PWMA 13 | ADS 0x49 A0; motor B unused |
| TB6612 STBY (all) | — | tied to 3V3 |
| BTS7960 blower | RPWM 48 | R_EN→3V3, L_EN→GND, LPWM not connected. 25 kHz |
| A/C compressor clutch relay | 6 | active high |

Valve PWM runs at 20 kHz, 8-bit. Everything above lives in `main/node_config.h`.

## Build and flash

The project is plain ESP-IDF (5.2 or newer; the display repo is on 6.1). Either open
the folder in VS Code with the ESP-IDF extension and the included devcontainer
(same one as the display repo), or use a local ESP-IDF install:

```
idf.py set-target esp32s3
idf.py build
idf.py -p COM7 flash monitor
```

Use the DevKitC's **UART** USB-C port (CP2102) for flashing and the console; the
boot log should end with `Ready, waiting for the display`. If flashing does not
start, hold BOOT while pressing RST, then retry.

`dependencies.lock` is committed on purpose so the `onewire_bus` component stays at
the version that was built and tested.

## Bench test without the display

Unplug the display's link cable, then connect a USB-UART adapter: adapter TX → node
GPIO21, adapter RX ← node GPIO2, GND ↔ GND. `pip install pyserial`, then:

```
python tools/ionode_cli.py -p COM8 ping           # PONG
python tools/ionode_cli.py -p COM8 version        # ionode 2.0 (...)
python tools/ionode_cli.py -p COM8 stress 200     # round trip; expect ~1 ms median
python tools/ionode_cli.py -p COM8 ads-all        # 8 pot readings, raw / mV / %
python tools/ionode_cli.py -p COM8 ds18b20        # duct temperature
python tools/ionode_cli.py -p COM8 bme280         # chip id 0x60 at 0x76 or 0x77
python tools/ionode_cli.py -p COM8 valve foot fwd 60   # motor moves, pot reading changes
python tools/ionode_cli.py -p COM8 valve foot stop
python tools/ionode_cli.py -p COM8 blower 60      # blower spins
python tools/ionode_cli.py -p COM8 safe           # everything off
```

`valve`, `blower` and `relay` really drive the hardware; keep fingers and the
compressor in mind.

## Integration with the display

Reconnect the link cable (node GPIO21 ← display GPIO10, node GPIO2 → display GPIO11,
common GND), power both boards. On the display's console you should see
`IO Node connected!`, then the valve auto-calibration drives each actuator to both
end stops (about 10 s per valve). On the node's console the 10-second status line
shows `link up` and growing frame counters.

## Protocol

Frames: COBS-encoded `[type][payload][CRC16-CCITT big-endian]` followed by `0x00`.
CRC16 polynomial 0x1021, initial value 0xFFFF, no reflection, no final XOR. Payload
at most 63 bytes. The node answers every command with exactly one frame.

| Command | Payload | Reply |
| --- | --- | --- |
| PING 0x01 | — | PONG 0x82 |
| VERSION 0x02 | — | 0x83, version string, NUL |
| GPIO_SET 0x10 | pin, level | OK 0x80 |
| GPIO_GET 0x11 | pin | 0x84 pin, level |
| GPIO_MODE 0x12 | pin, mode (0 in, 1 out, 2 in-pullup, 3 in-pulldown, 4 out-od) | OK |
| PWM_SET 0x20 | pin, duty 0–255 | OK |
| PWM_INIT 0x21 | pin, kHz | OK |
| I2C_WRITE 0x30 | addr, reg, len, data | OK |
| I2C_READ 0x31 | addr, reg, len | 0x85 len, data |
| ADS_READ 0x40 | i2c addr, channel 0–3 | 0x86 channel, hi, lo (raw int16, PGA ±4.096 V) |
| DS18B20_START 0x50 | optional 8-byte ROM | OK |
| DS18B20_READ 0x51 | optional 8-byte ROM | 0x87 temperature × 100, int16 big-endian |
| RESET 0xF0 | — | OK, then the node restarts |

Errors come back as `0x81` with one code byte: 1 invalid command, 2 invalid
arguments, 3 invalid pin, 4 I2C failure, 5 timeout or no device, 6 CRC mismatch.
Pins not in the allow-list in `node_config.h` (strapping, USB, flash, console, the
link and the bus pins) are refused with code 3.

### Behaviour worth knowing

- **Boot state.** All valve direction pins and the relay are driven low and all six
  PWM channels set to duty 0 before the link task starts. The display never sends
  GPIO_MODE or PWM_INIT for the valve drivers, so the node must come up ready.
- **GPIO_SET on an unconfigured pin** makes it an output; **PWM_SET on a pin without
  PWM_INIT** attaches a 20 kHz channel. Both exist for the same reason.
- **Link watchdog.** After 3 s without a valid frame (only once a frame has been
  seen), every PWM goes to 0 and every output pin low; the display re-commands
  everything when it returns. `LINK_WATCHDOG_MS` in `node_config.h`, 0 disables it.
- **Corrupt frames** are dropped without a reply; the display's 200 ms timeout
  handles it. `LINK_REPLY_ON_CRC_ERROR 1` answers with error 6 instead.

### The display's trailing-zero bug and the compatibility pad

`cobs_decode()` in the display firmware strips one trailing 0x00 from every decoded
message. That is wrong whenever the CRC's low byte is 0x00 (1 frame in 256): the
display then loses a CRC byte and rejects the frame, and waits out its 200 ms
timeout. Two measures in this node keep the pair working:

1. The node's own `cobs_decode()` does not strip, so commands from the display with
   a CRC ending in 0x00 are parsed correctly.
2. With `LINK_PAD_TRAILING_ZERO 1` (default) the node appends one extra 0x00 to a
   reply whose CRC ends in 0x00. The display strips the pad, not the CRC.

`tools/ionode_cli.py` understands both. To fix the display properly, delete the
"Remove the trailing zero" block at the end of its `cobs_decode()`, rebuild it, and
set `LINK_PAD_TRAILING_ZERO` to 0 here. Until then leave the pad on; a fixed display
with a padding node would reject 1 reply in 256.

## Files

```
main/node_config.h      pin map, link and bus settings, allow-list
main/main.c             bring-up order and the 10 s status line
main/protocol_server.c  UART task, frame parsing, command dispatch, watchdog
main/io_hw.c            GPIO and LEDC PWM (timer/channel allocation, safe state)
main/io_i2c.c           I2C bus, register access, ADS1115 single-shot reads
main/io_onewire.c       DS18B20 over the RMT 1-Wire bus
main/cobs_protocol.*    copied from the display repo; two marked changes
tools/ionode_cli.py     PC-side tester speaking the same protocol
```
