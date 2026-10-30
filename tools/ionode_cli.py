#!/usr/bin/env python3
"""
ionode_cli.py - bench-test the HVAC IO Node from a PC.

Speaks the display firmware's link protocol (cobs_protocol.h): COBS framing,
[type][payload][CRC16-CCITT big-endian], 0x00 delimiter, 921600 8N1.

Wiring: USB-UART adapter TX -> node GPIO21 (RX), adapter RX <- node GPIO2 (TX),
GND to GND. Unplug the display's cable first; two masters on one RX line
will not work.

Examples:
  ionode_cli.py -p COM7 ping
  ionode_cli.py -p COM7 version
  ionode_cli.py -p COM7 hello                   # protocol 2 handshake: version, capabilities
  ionode_cli.py -p COM7 search                  # 1-Wire ROM codes on the bus
  ionode_cli.py -p COM7 ota build/CarHvacIoNode.bin   # flash the inactive slot over the link, reboot, confirm
  ionode_cli.py -p COM7 ads-all                 # all 8 ADS1115 inputs in raw, mV and %
  ionode_cli.py -p COM7 ds18b20                 # duct temperature
  ionode_cli.py -p COM7 bme280                  # chip id of the cabin sensor
  ionode_cli.py -p COM7 valve foot fwd 60       # drive the foot valve forward at duty 60 (of 255)
  ionode_cli.py -p COM7 valve foot stop
  ionode_cli.py -p COM7 blower 80               # blower duty 80/255 (motor spins!)
  ionode_cli.py -p COM7 relay 1                 # A/C clutch relay on (compressor!)
  ionode_cli.py -p COM7 stress 200              # 200 pings, round-trip statistics
  ionode_cli.py -p COM7 safe                    # everything off
"""

import argparse
import hashlib
import statistics
import sys
import time

try:
    import serial  # pyserial
except ImportError:  # pragma: no cover - only matters on the PC
    serial = None

# --- protocol constants (cobs_protocol.h) ---------------------------------
MSG_PING, MSG_VERSION, MSG_HELLO = 0x01, 0x02, 0x03
MSG_GPIO_SET, MSG_GPIO_GET, MSG_GPIO_MODE = 0x10, 0x11, 0x12
MSG_PWM_SET, MSG_PWM_INIT = 0x20, 0x21
MSG_I2C_WRITE, MSG_I2C_READ = 0x30, 0x31
MSG_ADS_READ = 0x40
MSG_DS18B20_START, MSG_DS18B20_READ, MSG_ONEWIRE_SEARCH = 0x50, 0x51, 0x52
MSG_OTA_BEGIN, MSG_OTA_DATA, MSG_OTA_END, MSG_OTA_STATUS = 0x60, 0x61, 0x62, 0x63
PROTOCOL_VERSION = 2
OTA_DATA_MAX = 56
OTA_STATES = {0: "IDLE", 1: "RECEIVING", 2: "READY", 3: "ERROR", 4: "PENDING"}
MSG_RESET = 0xF0

RESP_NAMES = {
    0x80: "OK", 0x81: "ERROR", 0x82: "PONG", 0x83: "VERSION", 0x84: "GPIO_VALUE",
    0x85: "I2C_DATA", 0x86: "ADS_VALUE", 0x87: "TEMP_VALUE",
    0x88: "HELLO_RESP", 0x89: "ROM_LIST", 0x8A: "OTA_ACK",
}
ERR_NAMES = {
    0: "none", 1: "invalid command", 2: "invalid arguments", 3: "invalid pin",
    4: "I2C failure", 5: "timeout / no device", 6: "CRC mismatch", 7: "frame error", 8: "overflow",
    9: "OTA state", 10: "OTA write", 11: "OTA verify",
}

# Carrier pin map (hvac_hw_config.h / node_config.h): name -> (IN1, IN2, PWM, ADS address, ADS channel)
VALVES = {
    "c1":   (4, 16, 17, 0x48, 0),
    "c2":   (5, 18, 15, 0x48, 1),
    "foot": (38, 39, 40, 0x48, 2),
    "ws":   (41, 42, 47, 0x48, 3),
    "cool": (12, 14, 13, 0x49, 0),
}
BLOWER_PWM_PIN = 48
AC_RELAY_PIN = 6


# --- framing ----------------------------------------------------------------
def crc16_ccitt(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def cobs_encode(data: bytes) -> bytes:
    out = bytearray()
    block = bytearray()
    code = 1
    for b in data:
        if b == 0:
            out.append(code)
            out += block
            block = bytearray()
            code = 1
        else:
            block.append(b)
            code += 1
            if code == 0xFF:
                out.append(code)
                out += block
                block = bytearray()
                code = 1
    out.append(code)
    out += block
    return bytes(out)


def cobs_decode(enc: bytes):
    out = bytearray()
    i = 0
    while i < len(enc):
        code = enc[i]
        i += 1
        if code == 0 or i + code - 1 > len(enc):
            return None
        out += enc[i:i + code - 1]
        i += code - 1
        if code < 0xFF and i < len(enc):
            out.append(0)
    return bytes(out)


def build_frame(msg_type: int, payload: bytes = b"") -> bytes:
    raw = bytes([msg_type]) + payload
    raw += crc16_ccitt(raw).to_bytes(2, "big")
    return cobs_encode(raw) + b"\x00"


def parse_frame(frame: bytes):
    """frame without the 0x00 delimiter -> (type, payload) or None.

    Until it sees a HELLO the node pads replies whose CRC ends in 0x00 with one
    more 0x00 (for the protocol 1 display's decoder). Trying the CRC both ways
    cannot tell padded from unpadded: with this CRC both pass, one byte apart.
    A frame ending in 00 00 is padded (unpadded only if the CRC is 0x0000, 1 in 65536).
    """
    raw = cobs_decode(frame)
    if raw is None or len(raw) < 3:
        return None
    if len(raw) >= 4 and raw[-1] == 0 and raw[-2] == 0:
        raw = raw[:-1]
    if crc16_ccitt(raw[:-2]) == int.from_bytes(raw[-2:], "big"):
        return raw[0], raw[1:-2]
    return None


# --- transport --------------------------------------------------------------
class Node:
    def __init__(self, port: str, baud: int = 921600, timeout: float = 0.3):
        if serial is None:
            sys.exit("pyserial is missing: pip install pyserial")
        self.ser = serial.Serial(port, baud, timeout=0.01)
        self.timeout = timeout

    def transact(self, msg_type: int, payload: bytes = b"", timeout=None):
        self.ser.reset_input_buffer()
        self.ser.write(build_frame(msg_type, payload))
        deadline = time.monotonic() + (timeout or self.timeout)
        buf = bytearray()
        while time.monotonic() < deadline:
            chunk = self.ser.read(64)
            if not chunk:
                continue
            for b in chunk:
                if b == 0:
                    if buf:
                        parsed = parse_frame(bytes(buf))
                        if parsed is None:
                            raise IOError("corrupt reply %s" % buf.hex())
                        return parsed
                else:
                    buf.append(b)
        raise TimeoutError("no reply to 0x%02X" % msg_type)

    def expect(self, msg_type, payload=b"", want=0x80, timeout=None):
        t, data = self.transact(msg_type, payload, timeout)
        if t == 0x81:
            code = data[0] if data else -1
            raise IOError("node ERROR %d (%s)" % (code, ERR_NAMES.get(code, "?")))
        if t != want:
            raise IOError("unexpected reply %s" % RESP_NAMES.get(t, hex(t)))
        return data

    # convenience wrappers
    def ping(self):
        self.expect(MSG_PING, want=0x82)

    def version(self) -> str:
        return self.expect(MSG_VERSION, want=0x83).split(b"\x00")[0].decode(errors="replace")

    def gpio_mode(self, pin, mode):
        self.expect(MSG_GPIO_MODE, bytes([pin, mode]))

    def gpio_set(self, pin, level):
        self.expect(MSG_GPIO_SET, bytes([pin, 1 if level else 0]))

    def gpio_get(self, pin) -> int:
        return self.expect(MSG_GPIO_GET, bytes([pin]), want=0x84)[1]

    def pwm_init(self, pin, khz):
        self.expect(MSG_PWM_INIT, bytes([pin, khz]))

    def pwm_set(self, pin, duty):
        self.expect(MSG_PWM_SET, bytes([pin, duty]))

    def i2c_write(self, addr, reg, data: bytes):
        self.expect(MSG_I2C_WRITE, bytes([addr, reg, len(data)]) + data)

    def i2c_read(self, addr, reg, n) -> bytes:
        d = self.expect(MSG_I2C_READ, bytes([addr, reg, n]), want=0x85)
        return d[1:1 + d[0]]

    def ads_read(self, addr, ch) -> int:
        d = self.expect(MSG_ADS_READ, bytes([addr, ch]), want=0x86)
        return int.from_bytes(d[1:3], "big", signed=True)

    def hello(self):
        d = self.expect(MSG_HELLO, bytes([PROTOCOL_VERSION]), want=0x88)
        return d[0], d[1], d[2:].split(b"\x00")[0].decode(errors="replace")

    def search(self):
        d = self.expect(MSG_ONEWIRE_SEARCH, want=0x89)
        return [d[1 + 8 * i:9 + 8 * i] for i in range(d[0])]

    def ota_ack(self, msg_type, payload=b""):
        d = self.expect(msg_type, payload, want=0x8A)
        return d[0], int.from_bytes(d[1:5], "big")

    def ds18b20(self) -> float:
        self.expect(MSG_DS18B20_START)
        time.sleep(0.8)
        d = self.expect(MSG_DS18B20_READ, want=0x87)
        return int.from_bytes(d, "big", signed=True) / 100.0


# --- commands ---------------------------------------------------------------
def ads_to_mv(raw: int) -> float:
    return raw * 4096.0 / 32767.0  # same scaling as the display's ads1115_driver.c


def cmd_ads_all(node: Node):
    for addr in (0x48, 0x49):
        for ch in range(4):
            try:
                raw = node.ads_read(addr, ch)
                mv = ads_to_mv(raw)
                print("ADS 0x%02X A%d: raw %6d  %7.1f mV  %5.1f %%" % (addr, ch, raw, mv, mv / 33.0))
            except Exception as e:  # noqa: BLE001
                print("ADS 0x%02X A%d: %s" % (addr, ch, e))


def cmd_valve(node: Node, name: str, action: str, duty: int):
    in1, in2, pwm, ads_addr, ads_ch = VALVES[name]
    if action == "stop":
        node.pwm_set(pwm, 0)
        node.gpio_set(in1, 0)
        node.gpio_set(in2, 0)
    else:
        fwd = action == "fwd"
        node.gpio_set(in1, 1 if fwd else 0)
        node.gpio_set(in2, 0 if fwd else 1)
        node.pwm_set(pwm, duty)
    raw = node.ads_read(ads_addr, ads_ch)
    print("valve %s %s duty %d | position raw %d (%.1f mV)" % (name, action, duty, raw, ads_to_mv(raw)))


def cmd_safe(node: Node):
    for name in VALVES:
        cmd_valve(node, name, "stop", 0)
    node.pwm_set(BLOWER_PWM_PIN, 0)
    node.gpio_set(AC_RELAY_PIN, 0)
    print("all outputs off")


def cmd_stress(node: Node, count: int):
    times = []
    failures = 0
    for _ in range(count):
        t0 = time.perf_counter()
        try:
            node.ping()
            times.append((time.perf_counter() - t0) * 1000)
        except Exception:  # noqa: BLE001
            failures += 1
    if times:
        print("%d pings: median %.2f ms, max %.2f ms, failures %d" %
              (count, statistics.median(times), max(times), failures))
    else:
        print("no successful pings (%d failures)" % failures)


def cmd_ota(node: Node, path: str):
    image = open(path, "rb").read()
    print("%s: %d bytes, sha256 %s" % (path, len(image), hashlib.sha256(image).hexdigest()))
    node.ota_ack(MSG_OTA_BEGIN, len(image).to_bytes(4, "big") + hashlib.sha256(image).digest())
    off, t0, retries = 0, time.monotonic(), 0
    while off < len(image):
        chunk = image[off:off + OTA_DATA_MAX]
        try:
            _, off = node.ota_ack(MSG_OTA_DATA, off.to_bytes(4, "big") + chunk)
        except (TimeoutError, IOError) as e:
            # a lost reply or a rejected offset: ask the node where it is and resume there
            retries += 1
            if retries > 20:
                raise
            state, off = node.ota_ack(MSG_OTA_STATUS)
            print("\n%s, node %s at %d, resuming" % (e, OTA_STATES.get(state, state), off))
            if state != 1:
                raise IOError("transfer aborted by the node")
        if off % 16384 < OTA_DATA_MAX or off == len(image):
            print("\r%6.1f %%  %d bytes  %.1f s" % (100.0 * off / len(image), off, time.monotonic() - t0), end="", flush=True)
    print()
    state, n = node.ota_ack(MSG_OTA_END)
    print("END: %s, %d bytes" % (OTA_STATES.get(state, state), n))
    node.expect(MSG_RESET)
    print("RESET sent, waiting for the new image")
    deadline = time.monotonic() + 15
    while True:
        time.sleep(0.25)
        try:
            node.ping()
            break
        except (TimeoutError, IOError):
            if time.monotonic() > deadline:
                raise TimeoutError("node did not come back within 15 s")
    print("PONG, " + node.version())
    state, _ = node.ota_ack(MSG_OTA_STATUS)
    print("OTA state %s (IDLE = new image confirmed)" % OTA_STATES.get(state, state))


def cmd_bme280(node: Node):
    for addr in (0x76, 0x77):
        try:
            cid = node.i2c_read(addr, 0xD0, 1)[0]
            print("BME280 at 0x%02X: chip id 0x%02X (%s)" % (addr, cid, "BME280" if cid == 0x60 else "unexpected"))
            return
        except Exception as e:  # noqa: BLE001
            print("0x%02X: %s" % (addr, e))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-p", "--port", required=True, help="COM7, /dev/ttyUSB0 ...")
    ap.add_argument("-b", "--baud", type=int, default=921600)
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("ping")
    sub.add_parser("version")
    sub.add_parser("hello")
    sub.add_parser("search")
    sub.add_parser("ota-status")
    s = sub.add_parser("ota"); s.add_argument("file")
    s = sub.add_parser("gpio-mode"); s.add_argument("pin", type=int); s.add_argument("mode", type=int, help="0 in, 1 out, 2 in-pullup, 3 in-pulldown, 4 out-od")
    s = sub.add_parser("gpio-set"); s.add_argument("pin", type=int); s.add_argument("level", type=int)
    s = sub.add_parser("gpio-get"); s.add_argument("pin", type=int)
    s = sub.add_parser("pwm-init"); s.add_argument("pin", type=int); s.add_argument("khz", type=int)
    s = sub.add_parser("pwm-set"); s.add_argument("pin", type=int); s.add_argument("duty", type=int)
    s = sub.add_parser("i2c-write"); s.add_argument("addr", type=lambda x: int(x, 0)); s.add_argument("reg", type=lambda x: int(x, 0)); s.add_argument("data", nargs="*", type=lambda x: int(x, 0))
    s = sub.add_parser("i2c-read"); s.add_argument("addr", type=lambda x: int(x, 0)); s.add_argument("reg", type=lambda x: int(x, 0)); s.add_argument("len", type=int)
    s = sub.add_parser("ads"); s.add_argument("addr", type=lambda x: int(x, 0)); s.add_argument("ch", type=int)
    sub.add_parser("ads-all")
    sub.add_parser("ds18b20")
    sub.add_parser("bme280")
    s = sub.add_parser("valve"); s.add_argument("name", choices=sorted(VALVES)); s.add_argument("action", choices=["fwd", "rev", "stop"]); s.add_argument("duty", type=int, nargs="?", default=60)
    s = sub.add_parser("blower"); s.add_argument("duty", type=int)
    s = sub.add_parser("relay"); s.add_argument("level", type=int)
    s = sub.add_parser("stress"); s.add_argument("count", type=int, nargs="?", default=100)
    sub.add_parser("safe")
    sub.add_parser("reset")
    args = ap.parse_args()

    node = Node(args.port, args.baud)
    c = args.cmd
    if c == "ping":
        node.ping(); print("PONG")
    elif c == "version":
        print(node.version())
    elif c == "hello":
        ver, caps, text = node.hello()
        print("protocol %d, caps 0x%02X (%s), %s" % (ver, caps, " ".join(
            n for b, n in ((1, "search"), (2, "ota")) if caps & b) or "-", text))
    elif c == "search":
        roms = node.search()
        print("%d ROM(s)" % len(roms))
        for r in roms:
            print("  " + r.hex(" "))
    elif c == "ota-status":
        state, n = node.ota_ack(MSG_OTA_STATUS)
        print("%s, %d bytes" % (OTA_STATES.get(state, state), n))
    elif c == "ota":
        cmd_ota(node, args.file)
    elif c == "gpio-mode":
        node.gpio_mode(args.pin, args.mode); print("OK")
    elif c == "gpio-set":
        node.gpio_set(args.pin, args.level); print("OK")
    elif c == "gpio-get":
        print("GPIO%d = %d" % (args.pin, node.gpio_get(args.pin)))
    elif c == "pwm-init":
        node.pwm_init(args.pin, args.khz); print("OK")
    elif c == "pwm-set":
        node.pwm_set(args.pin, args.duty); print("OK")
    elif c == "i2c-write":
        node.i2c_write(args.addr, args.reg, bytes(args.data)); print("OK")
    elif c == "i2c-read":
        print(node.i2c_read(args.addr, args.reg, args.len).hex(" "))
    elif c == "ads":
        raw = node.ads_read(args.addr, args.ch)
        print("raw %d  %.1f mV" % (raw, ads_to_mv(raw)))
    elif c == "ads-all":
        cmd_ads_all(node)
    elif c == "ds18b20":
        print("%.2f degC" % node.ds18b20())
    elif c == "bme280":
        cmd_bme280(node)
    elif c == "valve":
        cmd_valve(node, args.name, args.action, max(0, min(255, args.duty)))
    elif c == "blower":
        node.pwm_set(BLOWER_PWM_PIN, max(0, min(255, args.duty))); print("blower duty %d" % args.duty)
    elif c == "relay":
        node.gpio_set(AC_RELAY_PIN, args.level); print("relay %d" % args.level)
    elif c == "stress":
        cmd_stress(node, args.count)
    elif c == "safe":
        cmd_safe(node)
    elif c == "reset":
        node.expect(MSG_RESET); print("node restarting")


if __name__ == "__main__":
    main()
