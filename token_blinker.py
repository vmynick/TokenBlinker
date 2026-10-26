#!/usr/bin/env python3
"""
Token-LED segédprogram: WiFi-beállítás, színteszt, és napló alapú számolás a terminálos munkamenetekhez.
A VS Code-os Claude élőben, a token_tap.exe-n keresztül villogtat (README); ez a program ahhoz nem kell.

  python token_blinker.py --wifi       # egyszer, USB-n: WiFi-név és jelszó beírása az ESP-be
  python token_blinker.py --test 3     # típusonként 3 villanás, a színek ellenőrzéséhez
  python token_blinker.py              # terminálos Claude Code: a naplókból (~/.claude/projects) számol,
                                       # válaszonként; a VS Code-os munkameneteket kihagyja (azokat a token_tap küldi)
  --host 192.168.0.42                  # ha a tokenled.local nem oldható fel
  --usb [COM9]                         # WiFi helyett USB soros

A PC nyers tokenszámokat küld ("T <típus> <token>"); hány token egy villanás, a LED webes felületén
állítható: http://tokenled.local
A --wifi és a --usb a pyserial csomagot igényli (pip install pyserial), a WiFi-s mód csak stdlib.
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


# ---------- LED WiFi-n (UDP) ----------
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
            except OSError:   # a legutóbbi címet megtartja, az mDNS alvó ESP-vel néha akadozik
                if not self.addr and not warned:
                    print(f"{self.host} nem található; be van kapcsolva és a WiFi-n van? (vagy --host <IP>)")
                    warned = True
            time.sleep(30 if self.addr else 5)

    def send(self, line):
        if self.addr:
            try:
                self.sock.sendto(line.encode(), self.addr)
            except OSError:
                pass


# ---------- LED USB-n (soros) ----------
def need_serial():
    if serial is None:
        sys.exit("Ehhez kell a pyserial: pip install pyserial")


def find_port():
    for p in list_ports.comports():
        if p.vid == ESPRESSIF_VID:
            return p.device
    return None


def open_port(name):
    s = serial.Serial()
    s.port, s.baudrate, s.write_timeout = name, 115200, 1
    s.dtr = s.rts = False   # a natív USB-n a DTR/RTS reseteli vagy bootloaderbe teszi a C3-at
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
                print(f"LED csatlakoztatva: {name}")
            except serial.SerialException as e:
                print(f"{name} nem nyitható meg ({e}); fut a PlatformIO soros monitor?")
                return
        try:
            self.ser.write(line.encode())
        except (serial.SerialException, OSError):
            print("LED leválasztva, újracsatlakozás...")
            self.ser.close()
            self.ser = None


def setup_wifi(port):
    need_serial()
    name = port or find_port()
    if not name:
        sys.exit("Nem találok USB-re dugott Espressif eszközt; add meg: --wifi --usb COMx")
    ssid = input("WiFi-név (SSID, 2,4 GHz-es hálózat): ").strip()
    pw = getpass.getpass("Jelszó (nem látszik gépelés közben): ")
    with open_port(name) as s:
        s.timeout = 0.5
        s.reset_input_buffer()
        s.write(f"W {ssid}\t{pw}\n".encode())
        print("Csatlakozás...")
        end = last_ask = time.monotonic()
        end += 25
        while time.monotonic() < end:
            if time.monotonic() - last_ask > 2:
                last_ask = time.monotonic()
                s.write(b"?\n")
            line = s.readline().decode(errors="replace").strip()
            if line.startswith("IP "):
                print(f"Kész: a LED a WiFi-n van, IP {line[3:]} — beállítások: http://tokenled.local")
                return
    sys.exit("25 s alatt nem csatlakozott. Jó a jelszó, és 2,4 GHz-es a hálózat? (--wifi újra)")


# ---------- Claude Code naplók (terminálos munkamenetek) ----------
offsets = {}    # fájl -> eddig feldolgozott bájt
partial = {}    # fájl -> befejezetlen utolsó sor
seen = {}       # már beszámolt message.id-k (egy üzenet több sorban, ugyanazzal a usage-dzsel)


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
    """Új tokenek a legutóbbi hívás óta. Induláskor a meglévő naplók végére ugrik."""
    got = {"t": 0, "k": 0, "i": 0, "r": 0}
    for p in ROOT.glob("**/*.jsonl"):
        try:
            size = p.stat().st_size
        except OSError:
            continue
        off = offsets.get(p)
        if off is None:
            off = offsets[p] = size if first else 0
        if size < off:   # felülírt fájl
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


# ---------- Fő ciklus ----------
def fmt(n):
    return f"{n:,}".replace(",", " ")


def main():
    ap = argparse.ArgumentParser(description="Claude Code token-LED segédprogram")
    ap.add_argument("--host", default="tokenled.local", help="a LED címe WiFi-n (alap: tokenled.local)")
    ap.add_argument("--usb", nargs="?", const="", metavar="PORT", help="WiFi helyett USB soros, pl. --usb COM9")
    ap.add_argument("--wifi", action="store_true", help="WiFi beállítása USB-n keresztül, majd kilép")
    ap.add_argument("--test", type=int, metavar="N", help="típusonként N villanás, majd kilép")
    a = ap.parse_args()

    if a.wifi:
        setup_wifi(a.usb or None)
        return

    led = SerialLed(a.usb or None) if a.usb is not None else UdpLed(a.host)

    if a.test:
        end = time.monotonic() + 10
        while time.monotonic() < end and not getattr(led, "addr", None) and not getattr(led, "ser", None):
            led.send("P\n")   # USB-n ez nyitja meg a portot
            time.sleep(0.2)
        led.send("".join(f"B {t} {a.test}\n" for t in "tkjir"))
        print(f"Elküldve: típusonként {a.test} villanás (cián, lila, zöld, narancs, sötétkék).")
        return

    total = {"t": 0, "k": 0, "i": 0, "r": 0}
    last_ping = 0.0
    scan(first=True)
    print(f"Figyelem (VS Code nélkül): {ROOT}")

    while True:
        time.sleep(POLL_S)
        got = scan(first=False)
        if any(got.values()):
            led.send("".join(f"T {t} {n}\n" for t, n in got.items() if n))
            for t, n in got.items():
                total[t] += n
            print(time.strftime("%H:%M:%S"),
                  f"kimenet +{fmt(got['t'])}  thinking +{fmt(got['k'])}  bemenet +{fmt(got['i'])}"
                  f"  cache +{fmt(got['r'])}   | össz.: {fmt(total['t'] + total['k'])} ki, {fmt(total['i'])} be")

        if time.monotonic() - last_ping > PING_S:
            last_ping = time.monotonic()
            led.send("P\n")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
