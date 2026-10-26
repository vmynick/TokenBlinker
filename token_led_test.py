#!/usr/bin/env python3
"""
Token-LED teszt, telepítés és beállítás-módosítás nélkül.
Csak a Python beépített könyvtárait használja, nem ír fájlt, és csak 127.0.0.1-en figyel.

  python token_led_test.py           -> LED-ablak + Claude Code proxy (127.0.0.1:8787)
  python token_led_test.py --demo    -> LED-ablak + szimulált Claude-forgalom

Claude Code teszt, csak abban az egy terminálban (bezárás után nyoma sincs):
  bash/zsh:   ANTHROPIC_BASE_URL=http://127.0.0.1:8787 claude
  PowerShell: $env:ANTHROPIC_BASE_URL="http://127.0.0.1:8787"; claude
  cmd:        set ANTHROPIC_BASE_URL=http://127.0.0.1:8787 && claude
"""
import http.client, json, random, socket, sys, threading, time
import tkinter as tk
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

BIND = "127.0.0.1"
UDP_PORT, HTTP_PORT, PROXY_PORT = 4210, 8080, 8787
UPSTREAM = "api.anthropic.com"
DECAY = 0.90
CHARS_PER_TOKEN = 4
HOP = {"host", "content-length", "transfer-encoding", "connection",
       "keep-alive", "content-encoding", "accept-encoding"}

# ---------- Valódi ESP32 LED (UDP) ----------
ESP_HOST = "tokenled.local"   # vagy az ESP IP-je, pl. "192.168.1.50"
ESP_PORT = 4210
esp_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
esp_addr = None


def esp_resolver():
    global esp_addr
    while True:
        try:
            esp_addr = (socket.gethostbyname(ESP_HOST), ESP_PORT)
        except OSError:
            esp_addr = None
        time.sleep(30)

# ---------- LED-állapot (ugyanaz a logika, mint a firmware-ben) ----------
lock = threading.Lock()
ch = {"W": {"e": 0.0, "rgb": (255, 90, 0), "n": 0},
      "C": {"e": 0.0, "rgb": (0, 180, 255), "n": 0}}
CODE_COLORS = {"t": (0, 180, 255), "k": (150, 0, 255), "j": (0, 255, 60)}


def pulse(src, typ, n):
    if n <= 0:
        return
    src = "C" if src.upper() == "C" else "W"
    with lock:
        c = ch[src]
        if src == "C":
            c["rgb"] = CODE_COLORS.get(typ, CODE_COLORS["t"])
        c["e"] = min(c["e"] + 40 + n * 8, 255)
        c["n"] += n
    if esp_addr:
        try:
            esp_sock.sendto(f"{src} {typ} {n}".encode(), esp_addr)
        except OSError:
            pass


# ---------- ESP32-protokoll (UDP + HTTP), hogy a valódi szkriptek is tesztelhetők legyenek ----------
def udp_loop():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind((BIND, UDP_PORT))
    while True:
        data, _ = s.recvfrom(64)
        try:
            a, b, n = data.decode().split()
            pulse(a, b, int(n))
        except ValueError:
            pass


class LedHttp(BaseHTTPRequestHandler):
    def do_GET(self):
        u = urlparse(self.path)
        if u.path == "/p":
            q = parse_qs(u.query)
            try:
                pulse(q.get("s", ["w"])[0], q.get("t", ["t"])[0], int(q.get("n", ["0"])[0]))
            except ValueError:
                pass
        self.send_response(204)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()

    def log_message(self, *a):
        pass


# ---------- Claude Code proxy (stdlib, pip nélkül) ----------
pending = {"t": 0, "k": 0, "j": 0}
pend_lock = threading.Lock()


def add(kind, chars=0, tokens=None):
    with pend_lock:
        pending[kind] += tokens if tokens is not None else max(1, round(chars / CHARS_PER_TOKEN))


def flusher():
    while True:
        time.sleep(0.05)
        with pend_lock:
            items = [(k, n) for k, n in pending.items() if n]
            for k, _ in items:
                pending[k] = 0
        for k, n in items:
            pulse("C", k, n)


def on_sse(raw):
    try:
        ev = json.loads(raw)
    except ValueError:
        return
    if ev.get("type") != "content_block_delta":
        return
    d = ev.get("delta", {})
    t = d.get("type")
    if t == "text_delta":
        add("t", len(d.get("text", "")))
    elif t == "thinking_delta":
        add("k", len(d.get("thinking", "")))
    elif t == "input_json_delta":
        add("j", len(d.get("partial_json", "")))


class Proxy(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def _proxy(self):
        n = int(self.headers.get("Content-Length", 0) or 0)
        body = self.rfile.read(n) if n else None
        conn = http.client.HTTPSConnection(UPSTREAM, timeout=900)
        try:
            conn.putrequest(self.command, self.path, skip_host=True, skip_accept_encoding=True)
            conn.putheader("Host", UPSTREAM)
            for k, v in self.headers.items():
                if k.lower() not in HOP:
                    conn.putheader(k, v)
            conn.putheader("Accept-Encoding", "identity")
            if body is not None:
                conn.putheader("Content-Length", str(len(body)))
            conn.endheaders(body)
            r = conn.getresponse()

            self.send_response(r.status)
            for k, v in r.getheaders():
                if k.lower() not in HOP:
                    self.send_header(k, v)
            self.send_header("Connection", "close")
            self.end_headers()
            self.close_connection = True

            ctype = r.getheader("Content-Type", "")
            sse = "event-stream" in ctype
            buf = b""
            while True:
                chunk = r.read1(65536)
                if not chunk:
                    break
                self.wfile.write(chunk)
                self.wfile.flush()
                if sse:
                    buf += chunk
                    *lines, buf = buf.split(b"\n")
                    for line in lines:
                        if line.startswith(b"data:"):
                            on_sse(line[5:])
                elif "json" in ctype:
                    buf += chunk
            if not sse and "json" in ctype and r.status == 200:
                try:
                    out = json.loads(buf).get("usage", {}).get("output_tokens", 0)
                    if out:
                        add("t", tokens=out)
                except ValueError:
                    pass
        except (BrokenPipeError, ConnectionResetError):
            pass
        finally:
            conn.close()

    do_GET = do_POST = do_PUT = do_PATCH = do_DELETE = _proxy

    def log_message(self, *a):
        pass


# ---------- Szimulált forgalom ----------
def stream(src, typ, seconds, tok_per_s):
    end = time.time() + seconds
    while time.time() < end:
        time.sleep(0.1)
        pulse(src, typ, max(1, int(random.gauss(tok_per_s / 10, 2))))


def demo_code():
    while True:
        stream("C", "k", 2.0, 60)    # thinking
        stream("C", "j", 0.6, 80)    # tool input
        time.sleep(random.uniform(0.5, 1.5))  # tool fut
        stream("C", "t", 3.0, 70)    # válasz
        time.sleep(random.uniform(2, 4))


def demo_web():
    while True:
        time.sleep(random.uniform(1, 3))
        stream("W", "t", random.uniform(2, 5), 50)


# ---------- Ablak ----------
root = tk.Tk()
root.title("Token LED teszt")
root.configure(bg="#111")
cv = tk.Canvas(root, width=300, height=170, bg="#111", highlightthickness=0)
cv.pack()
leds = {"W": cv.create_oval(40, 30, 120, 110, outline="#333", width=2),
        "C": cv.create_oval(180, 30, 260, 110, outline="#333", width=2)}
cv.create_text(80, 125, text="claude.ai", fill="#aaa")
cv.create_text(220, 125, text="Claude Code", fill="#aaa")
counters = {"W": cv.create_text(80, 148, text="", fill="#666"),
            "C": cv.create_text(220, 148, text="", fill="#666")}


def tick():
    with lock:
        for k, c in ch.items():
            s = c["e"] / 255
            r, g, b = (max(17, int(v * s)) for v in c["rgb"])
            cv.itemconfig(leds[k], fill=f"#{r:02x}{g:02x}{b:02x}")
            cv.itemconfig(counters[k], text=f"~{c['n']} token")
            c["e"] = 0 if c["e"] < 1 else c["e"] * DECAY
    root.after(10, tick)


def bg(fn):
    threading.Thread(target=fn, daemon=True).start()


bg(esp_resolver)
bg(udp_loop)
bg(ThreadingHTTPServer((BIND, HTTP_PORT), LedHttp).serve_forever)

if "--demo" in sys.argv:
    bg(demo_code)
    bg(demo_web)
    print("Demó mód: szimulált forgalom.")
else:
    bg(flusher)
    bg(ThreadingHTTPServer((BIND, PROXY_PORT), Proxy).serve_forever)
    print(f"Proxy: http://{BIND}:{PROXY_PORT}  ->  https://{UPSTREAM}")
    print("Másik terminálban, csak arra a munkamenetre:")
    print(f"  PowerShell: $env:ANTHROPIC_BASE_URL=\"http://{BIND}:{PROXY_PORT}\"; claude")
    print(f"  bash/zsh:   ANTHROPIC_BASE_URL=http://{BIND}:{PROXY_PORT} claude")
print(f"Kézi teszt: http://{BIND}:{HTTP_PORT}/p?s=w&n=20")

tick()
root.mainloop()
