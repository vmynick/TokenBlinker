#!/usr/bin/env python3
"""
Token LED helper: WiFi setup, colour test, and log-based counting for terminal sessions.
The VS Code Claude flashes live through token_tap.exe (see README); this program is not needed for that.

  python token_blinker.py --wifi       # over USB: type the WiFi name and password into the ESP
  python token_blinker.py --test 3     # 3 flashes per type, to check the colours
  python token_blinker.py              # terminal Claude Code: counts from the logs (~/.claude/projects),
                                       # per response; skips VS Code sessions (token_tap sends those)
  --host 192.168.0.42                  # if tokenled.local does not resolve
  --usb [COM9]                         # USB serial instead of WiFi

The PC sends raw token counts ("T <type> <tokens>"); how many tokens make one flash is set on the
LED's web page: http://tokenled.local
--wifi and --usb need pyserial (pip install pyserial); the WiFi mode uses the standard library only.
"""
import argparse, getpass, json, os, socket, sys, threading, time
from pathlib import Path

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    serial = None

ROOT = Path(os.getenv("CLAUDE_CONFIG_DIR", Path.home() / ".claude")) / "projects"
POLL_S, PING_S = 0.5, 2.0
UDP_PORT = 4210
ESPRESSIF_VID = 0x303A
SKIP_ENTRYPOINT = b'"entrypoint":"claude-vscode"'


# ---------- LED over WiFi (UDP) ----------
class UdpLed:
    def __init__(self, host):
        self.host, self.addr = host, None
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        threading.Thread(target=self._resolver, daemon=True).start()

    def _resolver(self):
        warned = False
        while True:
            try:
                addr = (socket.gethostbyname(self.host), UDP_PORT)
                if addr != self.addr:
                    print(f"LED: {self.host} = {addr[0]}")
                self.addr, warned = addr, False
            except OSError:   # keeps the last address; mDNS sometimes fails with a sleeping ESP
                if not self.addr and not warned:
                    print(f"{self.host} not found; is it powered and on the WiFi? (or use --host <IP>)")
                    warned = True
            time.sleep(30 if self.addr else 5)

    def send(self, line):
        if self.addr:
            try:
                self.sock.sendto(line.encode(), self.addr)
            except OSError:
                pass


# ---------- LED over USB (serial) ----------
def need_serial():
    if serial is None:
        sys.exit("This needs pyserial: pip install pyserial")


def find_port():
    for p in list_ports.comports():
        if p.vid == ESPRESSIF_VID:
            return p.device
    return None


def open_port(name):
    s = serial.Serial()
    s.port, s.baudrate, s.write_timeout = name, 115200, 1
    s.dtr = s.rts = False   # on native USB, DTR/RTS would reset the C3 or put it into the bootloader
    s.open()
    return s


class SerialLed:
    def __init__(self, port):
        need_serial()
        self.port, self.ser, self.last_try = port, None, 0.0

    def send(self, line):
        if self.ser is None:
            if time.monotonic() - self.last_try < 2:
                return
            self.last_try = time.monotonic()
            name = self.port or find_port()
            if not name:
                return
            try:
                self.ser = open_port(name)
                print(f"LED connected: {name}")
            except serial.SerialException as e:
                print(f"cannot open {name} ({e}); is the PlatformIO serial monitor running?")
                return
        try:
            self.ser.write(line.encode())
        except (serial.SerialException, OSError):
            print("LED disconnected, reconnecting...")
            self.ser.close()
            self.ser = None


def setup_wifi(port):
    need_serial()
    name = port or find_port()
    if not name:
        sys.exit("No Espressif device found on USB; give the port: --wifi --usb COMx")
    ssid = input("WiFi name (SSID, 2.4 GHz network): ").strip()
    pw = getpass.getpass("Password (not shown while typing): ")
    with open_port(name) as s:
        s.timeout = 0.5
        s.reset_input_buffer()
        s.write(f"W {ssid}\t{pw}\n".encode())
        print("Connecting...")
        end = last_ask = time.monotonic()
        end += 25
        while time.monotonic() < end:
            if time.monotonic() - last_ask > 2:
                last_ask = time.monotonic()
                s.write(b"?\n")
            line = s.readline().decode(errors="replace").strip()
            if line.startswith("IP "):
                print(f"Done: the LED is on the WiFi, IP {line[3:]} - settings: http://tokenled.local")
                return
    sys.exit("Did not connect within 25 s. Is the password right and the network 2.4 GHz? (run --wifi again)")


# ---------- Claude Code logs (terminal sessions) ----------
offsets = {}    # file -> bytes processed so far
partial = {}    # file -> unfinished last line
seen = {}       # message ids already counted (one message spans several lines with the same usage)


def parse(line, got):
    if b'"usage"' not in line or SKIP_ENTRYPOINT in line:
        return
    try:
        d = json.loads(line)
    except ValueError:
        return
    m = d.get("message") or {}
    u, mid = m.get("usage"), m.get("id")
    if d.get("type") != "assistant" or not u or not mid or mid in seen:
        return
    seen[mid] = None
    if len(seen) > 20000:
        for k in list(seen)[:10000]:
            del seen[k]
    out = u.get("output_tokens") or 0
    think = min(out, (u.get("output_tokens_details") or {}).get("thinking_tokens") or 0)
    got["t"] += out - think
    got["k"] += think
    got["i"] += (u.get("input_tokens") or 0) + (u.get("cache_creation_input_tokens") or 0)
    got["r"] += u.get("cache_read_input_tokens") or 0


def scan(first):
    """New tokens since the last call. At startup it skips to the end of the existing logs."""
    got = {"t": 0, "k": 0, "i": 0, "r": 0}
    for p in ROOT.glob("**/*.jsonl"):
        try:
            size = p.stat().st_size
        except OSError:
            continue
        off = offsets.get(p)
        if off is None:
            off = offsets[p] = size if first else 0
        if size < off:   # file was overwritten
            off, partial[p] = 0, b""
        if size == off:
            continue
        try:
            with open(p, "rb") as f:
                f.seek(off)
                data = f.read(size - off)
        except OSError:
            continue
        offsets[p] = off + len(data)
        *lines, partial[p] = (partial.get(p, b"") + data).split(b"\n")
        for line in lines:
            parse(line, got)
    return got


# ---------- Main loop ----------
def fmt(n):
    return f"{n:,}".replace(",", " ")


def main():
    ap = argparse.ArgumentParser(description="Claude Code token LED helper")
    ap.add_argument("--host", default="tokenled.local", help="LED address over WiFi (default: tokenled.local)")
    ap.add_argument("--usb", nargs="?", const="", metavar="PORT", help="USB serial instead of WiFi, e.g. --usb COM9")
    ap.add_argument("--wifi", action="store_true", help="set up WiFi over USB, then exit")
    ap.add_argument("--test", type=int, metavar="N", help="N flashes per type, then exit")
    a = ap.parse_args()

    if a.wifi:
        setup_wifi(a.usb or None)
        return

    led = SerialLed(a.usb or None) if a.usb is not None else UdpLed(a.host)

    if a.test:
        end = time.monotonic() + 10
        while time.monotonic() < end and not getattr(led, "addr", None) and not getattr(led, "ser", None):
            led.send("P\n")   # over USB this opens the port
            time.sleep(0.2)
        led.send("".join(f"B {t} {a.test}\n" for t in "tkjir") + "A 1\n")
        print(f"Sent: {a.test} flashes per type (cyan, purple, green, orange, dark blue), then an approval blink.")
        return

    total = {"t": 0, "k": 0, "i": 0, "r": 0}
    last_ping = 0.0
    scan(first=True)
    print(f"Watching (without VS Code sessions): {ROOT}")

    while True:
        time.sleep(POLL_S)
        got = scan(first=False)
        if any(got.values()):
            led.send("".join(f"T {t} {n}\n" for t, n in got.items() if n))
            for t, n in got.items():
                total[t] += n
            print(time.strftime("%H:%M:%S"),
                  f"output +{fmt(got['t'])}  thinking +{fmt(got['k'])}  input +{fmt(got['i'])}"
                  f"  cache +{fmt(got['r'])}   | total: {fmt(total['t'] + total['k'])} out, {fmt(total['i'])} in")

        if time.monotonic() - last_ping > PING_S:
            last_ping = time.monotonic()
            led.send("P\n")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
