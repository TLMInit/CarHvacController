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
| Electric compressor output | 10 | display `ac_out_pin`; low at boot. On/off (GPIO_SET), PWM, or 0–3.3 V through an RC filter; see [Compressor output](#compressor-output-gpio10) |

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

From 2.2 on the node uses its own partition table (`partitions.csv`, two OTA app
slots) and a bootloader with rollback. The first time a 2.2 build goes onto a board
that ran 2.1 or older, flash everything over USB with `idf.py -p COM7 flash`
(bootloader, partition table, otadata, app); `app-flash` alone is not enough. After
that, updates can go over the link (see [Node OTA](#node-ota)).

| Partition | Offset | Size |
| --- | --- | --- |
| nvs | 0x9000 | 16 KB |
| otadata | 0xD000 | 8 KB |
| phy_init | 0xF000 | 4 KB |
| ota_0 | 0x10000 | 960 KB |
| ota_1 | 0x100000 | 960 KB |

The map ends at 1.94 MB, so the same image boots on 2, 8 and 16 MB modules (the
N16R8 simply leaves the rest unused). The app is about 300 KB.

`dependencies.lock` is committed on purpose so the `onewire_bus` component stays at
the version that was built and tested.

## Bench test without the display

Unplug the display's link cable, then connect a USB-UART adapter: adapter TX → node
GPIO21, adapter RX ← node GPIO2, GND ↔ GND. `pip install pyserial`, then:

```
python tools/ionode_cli.py -p COM8 ping           # PONG
python tools/ionode_cli.py -p COM8 version        # ionode 2.4 (<git describe>) up=<s> rst=<reason>
python tools/ionode_cli.py -p COM8 hello          # protocol 2, caps 0x07 (search ota pwm_hz), ionode 2.4 (<git describe>)
python tools/ionode_cli.py -p COM8 search         # 1 ROM(s), then e.g. 28 ff 64 1e 0f 21 03 5a
python tools/ionode_cli.py -p COM8 ota-status     # IDLE, 0 bytes
python tools/ionode_cli.py -p COM8 ota build/CarHvacIoNode.bin   # ~15 s, then reboot and confirm
python tools/ionode_cli.py -p COM8 stress 200     # round trip; expect ~1 ms median
python tools/ionode_cli.py -p COM8 ads-all        # 8 pot readings, raw / mV / %
python tools/ionode_cli.py -p COM8 ds18b20        # duct temperature
python tools/ionode_cli.py -p COM8 bme280         # chip id 0x60 at 0x76 or 0x77
python tools/ionode_cli.py -p COM8 valve foot fwd 60   # motor moves, pot reading changes
python tools/ionode_cli.py -p COM8 valve foot stop
python tools/ionode_cli.py -p COM8 blower 60      # blower spins
python tools/ionode_cli.py -p COM8 pwm-hz 10 20000 8   # compressor output: 20 kHz, 8 bit
python tools/ionode_cli.py -p COM8 pwm-set16 10 128    # raw counts: 128/256 = 50 % (1.65 V after the RC filter)
python tools/ionode_cli.py -p COM8 i2c-write 0x60 0x08 0x00   # MCP4725 fast write 0x800 = Vdd/2
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

### Numbers shared with the display

These live in both repos and must change together; each site names the other.

| What | Node | Display | Rule, enforcement |
| --- | --- | --- | --- |
| Link window / watchdog | `LINK_WATCHDOG_MS` 3000 until HELLO | `REMOTE_IO_LINK_WINDOW_MS` 3000 (`remote_io.h`) | equal: sent in HELLO; bounds `LINK_WINDOW_MIN_MS`/`_MAX_MS` asserted on both sides |
| Frame timeout | `LINK_FRAME_TIMEOUT_MS` 20 | `FRAME_TIMEOUT_MS` 20 (`remote_io_binary.c`) | equal, comment |
| Reply budgets | ADS poll ≤ ~20 ms (`io_i2c.c`), DS18B20 3 tries, search budget 100 ms (`io_onewire.c`) | `CMD_TIMEOUT_MS` 200, `SEARCH_TIMEOUT_MS` 500 | node budget below the display timeout, comment |
| OTA confirm | `OTA_CONFIRM_TIMEOUT_MS` 60 s | `OTA_REBOOT_WAIT_MS` 30 s | display wait below the confirm time, comment |
| Node image size | slot 0xF0000 (`partitions.csv`) | `NODE_OTA_SLOT_SIZE` (`remote_io.h`) | equal; both refuse a larger image |
| Project name | `project(CarHvacIoNode)` | `NODE_PROJECT_NAME` | equal, comment; the OTA image check uses it |
| Baud, link pins | `LINK_BAUD` 921600 | `REMOTE_IO_BAUD_RATE` (`hvac_hw_config.h`) | equal, comment |
| Pin map | `NODE_BOOT_*_PINS`, `NODE_ALLOWED_PINS` | `hvac_hw_config.h` | same pins, comment |
| Blower PWM | `{48, 25}` in `NODE_BOOT_PWM_PINS` | `remote_pwm_configure(..., 25000, 10)` (`bts7960_driver.c`) | 25 kHz both, comment |
| ADS1115 full scale | `ADS_CFG_PGA_4V096` (`io_i2c.c`) | mV maths 4096/32767 (`ads1115_driver.c`) | same range, comment |
| Protocol header | `main/cobs_protocol.h`, `main/ota_image.h` | same files | byte-identical; `tools/ionode_cli.py` `ERR_NAMES` mirrors the error codes |

## Protocol

Frames: COBS-encoded `[type][payload][CRC16-CCITT big-endian]` followed by `0x00`.
CRC16 polynomial 0x1021, initial value 0xFFFF, no reflection, no final XOR. Payload
at most 63 bytes. The node answers every command with exactly one frame.

| Command | Payload | Reply |
| --- | --- | --- |
| PING 0x01 | — | PONG 0x82 |
| VERSION 0x02 | — | 0x83, version string, NUL |
| HELLO 0x03 | display protocol version, optional link window ms u16 BE | 0x88 protocol version (2), capabilities (0x01 search, 0x02 OTA, 0x04 PWM_HZ), version string, NUL |
| GPIO_SET 0x10 | pin, level | OK 0x80 |
| GPIO_GET 0x11 | pin | 0x84 pin, level |
| GPIO_MODE 0x12 | pin, mode (0 in, 1 out, 2 in-pullup, 3 in-pulldown, 4 out-od) | OK |
| PWM_SET 0x20 | pin, duty 0–255 (scaled to the channel's resolution, see [Compressor output](#compressor-output-gpio10)) | OK |
| PWM_SET 0x20 (3 bytes) | pin, duty u16 BE in raw counts of the channel, clamped to 2^bits = full on (capability 0x04) | OK |
| PWM_INIT 0x21 | pin, kHz 1–200; 8 bit | OK |
| PWM_INIT_HZ 0x22 | pin, freq Hz u32 BE (1–40000), resolution bits 1–14 (capability 0x04) | OK; error 2 bad freq/bits, 3 pin not allowed, 8 no LEDC timer/channel left |
| I2C_WRITE 0x30 | addr, reg, len, data | OK |
| I2C_READ 0x31 | addr, reg, len | 0x85 len, data |
| ADS_READ 0x40 | i2c addr, channel 0–3 | 0x86 channel, hi, lo (raw int16, PGA ±4.096 V) |
| DS18B20_START 0x50 | optional 8-byte ROM | OK |
| DS18B20_READ 0x51 | optional 8-byte ROM | 0x87 temperature × 100, int16 big-endian |
| ONEWIRE_SEARCH 0x52 | — | 0x89 count, count × 8-byte ROM (family code first), at most 4 |
| OTA_BEGIN 0x60 | size u32 BE, sha256 (32) | 0x8A OTA_ACK: state, bytes received u32 BE |
| OTA_DATA 0x61 | offset u32 BE, 1–56 bytes | OTA_ACK |
| OTA_END 0x62 | — | OTA_ACK (READY) |
| OTA_STATUS 0x63 | — | OTA_ACK |
| RESET 0xF0 | — | OK, then the node restarts |

Errors come back as `0x81` with one code byte: 1 invalid command, 2 invalid
arguments, 3 invalid pin, 4 I2C failure, 5 timeout or no device, 6 CRC mismatch,
8 no LEDC timer or channel left (PWM_INIT, PWM_INIT_HZ) or no I2C device slot, 9 OTA command out of order, 10 OTA write failed or offset not contiguous, 11 OTA
size or sha256 mismatch, 12 OTA image is not an ESP32-S3 `CarHvacIoNode` build.

### Protocol 2

- **Handshake.** A protocol 2 display sends HELLO with its protocol version on every
  connect (from display v2.3 followed by its link window, see Link watchdog). HELLO_RESP
  carries the node's protocol version, the capability bits and the same
  `ionode 2.4 (<git describe>)` text as VERSION. A 2.1 node answers HELLO
  with error 1, which tells the display it is talking to protocol 1.
- **Pad rule.** Until the first HELLO since boot, replies whose CRC ends in 0x00
  get the compatibility pad described below. From that HELLO on (HELLO_RESP
  included) nothing is padded. `LINK_PAD_TRAILING_ZERO 0` turns the pad off entirely.
- **ONEWIRE_SEARCH** runs the 1-Wire ROM search on GPIO7. One ROM takes roughly
  20–30 ms; no new ROM is started after 100 ms, so the reply comes well inside the
  display's 500 ms search timeout (`SEARCH_TIMEOUT_MS`). An empty bus gives count 0;
  error 5 if the bus driver never started.

### Node OTA

1. `OTA_BEGIN` size (at least 288) + sha256: all outputs go to their safe state.
   Nothing is opened or written yet. Reply OTA_ACK(RECEIVING, 0). BEGIN in any state
   except PENDING starts over.
2. `OTA_DATA` offset + up to 56 bytes, offsets contiguous from 0. The first 288 bytes
   (image header, first segment header, `esp_app_desc_t`) are held in RAM until they
   show an ESP32-S3 app whose project name is `CarHvacIoNode` (`ota_image.c`, shared
   with the display). Otherwise error 12 and state ERROR, and nothing has touched
   flash. Once they pass, the inactive slot is opened without an up-front erase and
   the held bytes are written. Each 4 KB sector is erased when the write first reaches it. Reply OTA_ACK(RECEIVING, bytes so far). A
   wrong offset gets error 10 and leaves the transfer intact: ask OTA_STATUS and
   resume from its byte count. A flash failure gets error 10 and state ERROR.
3. `OTA_END`: byte count and sha256 must match (else error 11, state ERROR), then
   the image is checked (`esp_ota_end`) and made the boot slot. Reply OTA_ACK(READY, size).
4. `RESET`: the node reboots into the new image.
5. **Confirmation and rollback.** The new image boots unconfirmed; OTA_STATUS
   reports PENDING. It confirms itself once it has answered at least one PING and
   one VERSION. If that has not happened 60 s after boot, it marks itself invalid
   and reboots into the previous image. A crash or reset before confirmation rolls
   back too (bootloader). OTA_BEGIN is refused (error 9) while PENDING.

The link watchdog keeps running during a transfer; the outputs are already safe.
Worst-case reply times: DATA with a sector erase ~50 ms (typical flash), the DATA that
opens the slot ~50 ms (otadata sector erase), END ~100 ms (image check plus otadata write).
A lost link mid-transfer leaves the node in RECEIVING with its outputs safe and the
running image and boot slot untouched; the display resumes from OTA_STATUS or starts
over with a new BEGIN.

The firmware is built at `-O2` (`CONFIG_COMPILER_OPTIMIZATION_PERF` in `sdkconfig.defaults`).
Only the carrier's outputs are on the allow-list in `node_config.h` (valve direction
and PWM pins, blower PWM, relay, compressor output 10); every other pin is refused with code 3.

### Compressor output (GPIO10)

GPIO10 is a plain GPIO on the DevKitC-1 N16R8: the module's quad flash uses GPIO26–32 and
its octal PSRAM GPIO33–37, and 10 is no strapping pin. (The display's own GPIO10 is its
link TX; that is the other board.) It boots low like the other outputs and goes low / duty 0
in the safe state (link watchdog, RESET, OTA_BEGIN): `io_outputs_safe_state` sets every
attached LEDC channel to duty 0 and every plain output pin low.

**PWM_INIT_HZ** (`io_pwm_init_hz`, rules in `main/io_pwm_calc.c`): the pin must be on the
allow-list (else error 3); freq 1–40000 Hz, resolution 1–14 bits, and freq × 2^bits must be
reachable from the 80 MHz APB clock with an LEDC divider of 1 to below 1024, i.e.
78 126 Hz ≤ freq × 2^bits ≤ 80 MHz (else error 2). So 20 and 25 kHz take at most 11
bits, 40 kHz at most 10, 14 bits needs 5–4882 Hz, and 8 bits needs at least 306 Hz. Every timer is
configured with `LEDC_USE_APB_CLK`: the S3's LEDC timers share one clock source, so none may
pick another.

**Timers.** The ESP32-S3 has 4 LEDC timers and 8 channels. At boot two timers are taken
(20 kHz 8 bit: valves 17, 15, 40, 47, 13; 25 kHz 8 bit: blower 48) and six channels. A timer
is shared only by channels with the same frequency *and* resolution; a timer no channel uses
any more is free again. The display's analog_rc init (pin 10, 20000 Hz, 8 bit) therefore
shares the valves' timer and takes the seventh channel; any other frequency or resolution
takes one of the two free timers. None left, or no channel left: error 8 (`ERR_OVERFLOW`,
the code PWM_INIT already gives for this). PWM_INIT_HZ again on the same pin re-times it; if
the resolution changes, the duty goes to 0 (old counts mean something else at the new
resolution). The reply is immediate (an LEDC register setup, well under a millisecond).

**Duty.** A PWM_INIT channel (and one auto-attached by PWM_SET) takes the 8-bit duty byte
unscaled, exactly as before (255 = 255/256). On a PWM_INIT_HZ channel the 2-byte PWM_SET is
scaled: `counts = (duty × 2^bits + 127) / 255`, so 0 = off and 255 = 2^bits = full on. The
3-byte PWM_SET writes raw counts, clamped to 2^bits (full on), on either kind of channel.

**analog_rc (0–3.3 V from PWM).** The display sends PWM_INIT_HZ 10, 20000 Hz, 8 bit, then
the duty. Filter: GPIO10 — 10 kΩ — output, 10 µF from output to GND.

- Cutoff 1/(2π · 10 kΩ · 10 µF) = 1.59 Hz; time constant 100 ms, about 0.5 s to settle within 1 %.
- Ripple at 20 kHz: worst at 50 % duty, ΔV ≈ 3.3 V · 0.25 / (20 kHz · 0.1 s) ≈ 0.4 mV p-p,
  far below one 8-bit step (3.3 V / 256 = 12.9 mV), which sets the real resolution.
- The output has 10 kΩ source impedance: it must feed a high-impedance input (≥ 1 MΩ for
  < 1 % error) or a rail-to-rail op-amp follower. A 10 kΩ load halves the voltage.
- Full scale is the GPIO's high level (about 3.3 V); calibrate on the display side if the
  compressor input needs better than a few percent.

**MCP4725 DAC (I2C).** On the node's I2C bus: SDA GPIO8, SCL GPIO9, 400 kHz, shared with
the ADS1115s (0x48/0x49) and the BME280 (0x76/0x77). Address pin A0: GND = 0x60, Vdd = 0x61
(MCP4725A0 parts; A1/A2 variants are 0x62–0x65). The display drives it with the existing
I2C_WRITE, unchanged: the node always sends `reg` first, then the `len` data bytes, in one
transaction. The MCP4725 fast write (two bytes, `[0 0 PD1 PD0 D11 D10 D9 D8] [D7..D0]`, no
register byte) is therefore framed as

    I2C_WRITE  addr = 0x60 or 0x61,  reg = (PD << 4) | (value >> 8),  len = 1,  data = value & 0xFF
    payload    60 0H 01 LL           (PD = 0: normal; value 0..4095; e.g. 0x800 -> 60 08 01 00)

The top bits C2 C1 = 00 of that first byte select fast mode, so `reg` stays ≤ 0x3F (0x00–0x0F
with PD = 0). Output = value / 4096 × Vdd.

- **Vdd 3.3 V** (recommended): logic levels match the node; full scale 3.3 V. For 0–5 V, add a
  non-inverting op-amp stage with gain 1.5 (e.g. 10 kΩ / 5.1 kΩ, rail-to-rail output, 5 V supply).
- **Vdd 5 V**: full scale 5 V, but the MCP4725's VIH is 0.7 × Vdd = 3.5 V, above the node's
  3.3 V logic, so a 3.3 V high on SDA/SCL is not guaranteed to read as high. Put an I2C level
  shifter (e.g. PCA9306 or a BSS138 module) between the node's bus and the DAC. Most MCP4725
  breakout boards also pull SDA/SCL up to their own Vdd: at 5 V that puts 5 V on GPIO8/9, which
  are not 5 V tolerant. Remove those pull-ups or keep them on the 5 V side of the shifter.
- The node's safe state does not touch I2C: a link loss, RESET or OTA_BEGIN leaves the DAC at
  its last value, and at power-up the MCP4725 outputs its EEPROM value (factory default 0x800,
  half scale). The display should store 0 in the EEPROM once (write DAC and EEPROM:
  `reg = 0x60, len = 2, data = [D11..D4] [D3..D0 0000]`, i.e. payload `60 60 02 00 00`) and must
  not rely on the node to zero the DAC; use the relay or another enable for a hard stop.

### Behaviour worth knowing

- **Boot state.** All valve direction pins, the relay and the compressor output (10) are driven low and all six
  PWM channels set to duty 0 before the link task starts. The display never sends
  GPIO_MODE or PWM_INIT for the valve drivers, so the node must come up ready.
  Between power-on and this point (~300 ms: bootloader and app start) the pins float.
  The TB6612 inputs have internal pull-downs; whether the BTS7960 module and the relay
  driver do is not yet measured, so assume they do not.
- **GPIO_SET on an unconfigured pin** makes it an output; **PWM_SET on a pin without
  PWM_INIT** attaches a 20 kHz channel. Both exist for the same reason.
- **Link watchdog.** After the link window without a valid frame (only once a frame
  has been seen), every PWM goes to 0 and every output pin low. A display from v2.3 on
  sends its link window (the time it calls the link "connected", 3000 ms) in HELLO and
  the node uses that value, clamped to 2000..10000 ms (`LINK_WINDOW_MIN_MS` / `_MAX_MS` in
  `cobs_protocol.h`), so both ends share one number.
  Before any HELLO, or with an older display, it is `LINK_WATCHDOG_MS` (3000) in
  `node_config.h`; 0 disables the watchdog. A 2.3 node answers the 3-byte HELLO with
  error 2 and the display repeats HELLO without the window. The check runs from an esp_timer, so it still fires
  if the link task hangs; the task watchdog then restarts the node (5 s, panic on).
  When the link returns, the display re-sends the relay and blower every second and
  every valve's last command (direction and duty, a stop included) once a second
  (display v2.1.1 on; from v2.2.1 all valves in the first pass after the link returns),
  so an idle valve's stop is re-asserted too.
- **RGB LED.** The DevKitC-1's on-board LED is on GPIO48 (board v1.0, blower PWM) or
  GPIO38 (v1.1, foot valve AIN1). Read the revision off the silkscreen before trusting
  either pin's behaviour on a scope.
- **I2C recovery.** After 3 bus errors in a row the node resets the I2C bus, and
  re-creates it if the reset fails.
- **Corrupt frames** are dropped without a reply; the display's 200 ms timeout
  handles it. `LINK_REPLY_ON_CRC_ERROR 1` answers with error 6 instead.

### The display's trailing-zero bug and the compatibility pad

`cobs_decode()` in the protocol 1 display firmware strips one trailing 0x00 from
every decoded message. That is wrong whenever the CRC's low byte is 0x00 (1 frame
in 256). Worse than a rejection: with this CRC (no final XOR) the shortened frame
still passes the CRC check, so the display sees a payload one byte short. Two
measures in this node keep the pair working:

1. The node's own `cobs_decode()` does not strip, so commands from the display with
   a CRC ending in 0x00 are parsed correctly.
2. Until a HELLO arrives, the node appends one extra 0x00 to a reply whose CRC ends
   in 0x00 (`LINK_PAD_TRAILING_ZERO 1`, the default). The display strips the pad,
   not the CRC.

The mirror case matters for protocol 2 displays: a padded frame decoded without
stripping also passes the CRC, one byte too long. So a decoder must know whether
the sender pads; a frame that decodes to `... 00 00` is padded (unpadded only when
the CRC is 0x0000, 1 in 65536). `tools/ionode_cli.py` uses that rule.

## What the original node had, and what this one does with it

The flash dump of the lost firmware (`CarHvacIONode`, ESP-IDF, 28 Jan 2026) shows
modules IO_UART_BIN, IO_GPIO, IO_PWM, IO_I2C, IO_ADS1115, IO_ONEWIRE and
IO_STATUS_LED. Everything the display uses is covered by this node. Two things it
had on top are back in 2.1:

- **Scratchpad retry.** A DS18B20 read with a CRC mismatch is tried up to three times
  in all (`DS18B20_READ_ATTEMPTS`) before an error goes back to the display (`io_onewire.c`).
- **Status LED.** The original drove the DevKitC's on-board RGB LED through
  `led_strip` on **GPIO48** (read out of its flash dump), which on this carrier is
  also the blower PWM input: each refresh pulsed the BTS7960 for 30 µs until the
  display's `PWM_INIT(48)` took the pin, then the LED went dark. Here it is
  `NODE_STATUS_LED_GPIO` in `node_config.h`, off by default, and it always yields
  the pin when the display claims it:
  - **48** reproduces the original: boot (white), waiting for the display (blue
    blink), link up (green), dark once the blower pin is claimed. The blower driver
    sees two 30 µs bursts per second while waiting, as before.
  - **38** is the LED pin on a DevKitC-1 v1.1 and the foot valve's AIN1 here: harmless
    while PWMA is 0, dark the first time the foot valve moves.
  - An external WS2812 on any free GPIO keeps the LED alive permanently.
  Patterns: white = booting, blue blink = waiting for the display, green = link up,
  red blink = link lost after it was up, amber flash = an error reply or a bad frame.

## Files

```
main/node_config.h      pin map, link and bus settings, allow-list
main/main.c             bring-up order and the 10 s status line
main/protocol_server.c  UART task, frame parsing, command dispatch, watchdog
main/io_hw.c            GPIO and LEDC PWM (timer/channel allocation, safe state)
main/io_pwm_calc.c      PWM_INIT_HZ argument check, duty scaling and clamp (pure, host-tested)
main/io_i2c.c           I2C bus, register access, ADS1115 single-shot reads
main/io_onewire.c       DS18B20 over the RMT 1-Wire bus, 3-attempt scratchpad retry, ROM search
main/io_ota.c           node OTA into the inactive slot, sha256 check, confirm/rollback
partitions.csv          nvs, otadata, phy_init, ota_0, ota_1 (2 MB map)
main/io_status_led.c    WS2812 status LED, optional, yields its pin to the display
main/cobs_protocol.*    copied from the display repo; two marked changes
tools/ionode_cli.py     PC-side tester speaking the same protocol
```

## Tests

Host unit tests for `main/cobs_protocol.c` live in `test/host/` (round trip, trailing-zero handling, reply
padding only while `cobs_pad_trailing_zero` is set; built with `LINK_PAD_TRAILING_ZERO` 1 and 0),
`test_pwm_node.c` for PWM_INIT_HZ / 3-byte PWM_SET framing and `main/io_pwm_calc.c`, and
`test_ota_image.c` for the OTA image header check.

```
cmake -S test/host -B ../tbuild-node
cmake --build ../tbuild-node
ctest --test-dir ../tbuild-node --output-on-failure
```

CI (`.github/workflows/ci.yml`) runs these tests, then builds with `espressif/idf:v6.1` and fails on
any compiler warning from `main/`.
