#!/usr/bin/env python3
"""Claude token LED - launcher for the VS Code Claude extension on macOS and Linux (claudeCode.claudeProcessWrapper).
The same as token_tap.cs (Windows). The extension calls it as: token_tap.py <claude> <arguments...>
It starts claude unchanged, passes its output and input through byte for byte, and meanwhile reads the
stream_event lines (the live token stream) and tells the LED over UDP. It also notices when Claude asks the
user for approval (control_request / can_use_tool), when a turn ends ("result": finished / error) and what
the usage limits are (rate_limit_event). No proxy, no background process: if the LED or the network is
unreachable, Claude works exactly the same.

Setup: chmod +x token_tap.py, then in VS Code settings.json: "claudeCode.claudeProcessWrapper": "/path/to/token_tap.py"
LED address: TOKENLED_HOST environment variable, default tokenled.local
(UDP 4210, "T <type> <tokens>", "A 1", "D <0|1>", "U <5h%> <7d%> <5h reset> <7d reset>")
Standard library only (Python 3.8+)."""

import json
import os
import signal
import socket
import subprocess
import sys
import threading
import time

LED_PORT = 4210
MAX_LINE = 65536        # stream_event lines are small; longer ones (tool results) are not parsed
CHARS_PER_TOKEN = 4     # live estimate; the real output_tokens at the end of a message fixes the difference


def die_with_parent():
    """Linux: claude gets SIGTERM if we die. (Elsewhere it still ends when its input closes.)"""
    try:
        import ctypes
        ctypes.CDLL("libc.so.6", use_errno=True).prctl(1, signal.SIGTERM)   # PR_SET_PDEATHSIG
    except Exception:
        pass


def write_all(fd, data):
    view = memoryview(data)
    while view:
        view = view[os.write(fd, view):]


# ---------- Live token stream from the output ----------
class Msg:
    """Characters of one message and the tokens already sent for them, per type
    (t/et: text, k/ek: thinking, j/ej: tool input)."""
    def __init__(self):
        self.t = self.k = self.j = self.et = self.ek = self.ej = self.sent = 0
        self.thinking = False


def get(o, *keys):
    for k in keys:
        o = o.get(k) if isinstance(o, dict) else None
    return o


def num(o, key):
    v = get(o, key)
    return int(v) if isinstance(v, (int, float)) else 0


def length(o, key):
    v = get(o, key)
    return len(v) if isinstance(v, str) else 0


class Tap:
    def __init__(self):
        self.line = bytearray()
        self.too_long = False
        self.in_line = bytearray()
        self.msgs = {}                 # main thread and subagents separately
        self.waiting = False           # Claude waits for the user's approval
        self.done_req = -1             # turn finished: 0 ok, 1 error, -1 nothing to send
        self.use5 = self.use7 = -1     # usage limits in percent
        self.reset5 = self.reset7 = 0  # when those windows reset (Unix time, 0 = unknown)
        self.gate = threading.Lock()
        self.pending = {}
        self.sender = None

    # Input to claude: the user's answer to an approval ends the wait.
    def scan_input(self, b):
        for c in b:
            if c == 10:
                if b'"control_response"' in self.in_line:
                    self.waiting = False   # approved or denied
                self.in_line.clear()
            elif len(self.in_line) < 400:
                self.in_line.append(c)

    def feed(self, b):
        start = 0
        while True:
            nl = b.find(b"\n", start)
            part = b[start:] if nl < 0 else b[start:nl]
            room = MAX_LINE - len(self.line)
            if len(part) > room:
                self.too_long = True
                part = part[:max(room, 0)]
            self.line += part
            if nl < 0:
                return
            self.handle(self.line.decode("utf-8", "replace"), not self.too_long)
            self.line.clear()
            self.too_long = False
            start = nl + 1

    def handle(self, s, complete):
        # Approval: Claude asks (control_request / can_use_tool); anything it prints afterwards means it was answered.
        head = s[:300]
        if '"control_request"' in head and '"can_use_tool"' in head:
            self.waiting = True
            self.ensure_sender()
            return
        if '"type":"user"' in head or '"type":"result"' in head or '"control_cancel_request"' in head:
            self.waiting = False

        # End of a turn: "result" (is_error tells success from failure).
        if '"type":"result"' in head:
            self.done_req = 1 if '"is_error":true' in s else 0
            self.ensure_sender()
            return

        # Usage limits: rate_limit_event carries the utilisation (0..1) and reset time of the 5-hour and 7-day windows.
        if complete and '"rate_limit_event"' in head:
            o = json.loads(s)
            w5 = get(o, "rate_limit_info", "unifiedWindows", "five_hour")
            w7 = get(o, "rate_limit_info", "unifiedWindows", "seven_day")
            if isinstance(get(w5, "utilization"), (int, float)):
                self.use5 = round(get(w5, "utilization") * 100)
            if isinstance(get(w7, "utilization"), (int, float)):
                self.use7 = round(get(w7, "utilization") * 100)
            if get(w5, "resetsAt") is not None:
                self.reset5 = num(w5, "resetsAt")
            if get(w7, "resetsAt") is not None:
                self.reset7 = num(w7, "resetsAt")
            self.ensure_sender()
            return

        if not complete or '"stream_event"' not in s:
            return
        o = json.loads(s)
        if get(o, "type") != "stream_event":
            return
        ev = get(o, "event")
        key = get(o, "parent_tool_use_id") or ""
        m = self.msgs.setdefault(key, Msg())
        kind = get(ev, "type")
        if kind == "message_start":
            self.waiting = False
            m = self.msgs[key] = Msg()
            u = get(ev, "message", "usage")
            self.add("i", num(u, "input_tokens") + num(u, "cache_creation_input_tokens"))
            self.add("r", num(u, "cache_read_input_tokens"))
        elif kind == "content_block_start":
            if get(ev, "content_block", "type") in ("thinking", "redacted_thinking"):
                m.thinking = True
        elif kind == "content_block_delta":
            d = get(ev, "delta")
            dt = get(d, "type")
            if dt == "text_delta":
                m.t += length(d, "text")
                m.et += self.emit(m, "t", m.t, m.et)
            elif dt == "thinking_delta":
                # Redacted thinking streams no text, only "estimated_tokens" (tokens added since the last frame).
                m.k += length(d, "thinking") + num(d, "estimated_tokens") * CHARS_PER_TOKEN
                m.ek += self.emit(m, "k", m.k, m.ek)
            elif dt == "input_json_delta":
                m.j += length(d, "partial_json")
                m.ej += self.emit(m, "j", m.j, m.ej)
        elif kind == "message_delta":   # the real output_tokens: tops up what the estimate missed
            fu = get(ev, "usage")
            out_tok = num(fu, "output_tokens")
            th = get(fu, "output_tokens_details", "thinking_tokens")
            if isinstance(th, (int, float)):
                m.ek += self.fix(m, "k", int(th) - m.ek)
                if m.ej > m.et:
                    m.ej += self.fix(m, "j", out_tok - int(th) - m.et - m.ej)
                else:
                    m.et += self.fix(m, "t", out_tok - int(th) - m.et - m.ej)
            else:
                self.fix(m, "k" if m.thinking else "t", out_tok - m.sent)

    def emit(self, m, kind, chars, done):
        """Sends the estimated tokens of this type that were not sent yet; returns how many."""
        tokens = chars // CHARS_PER_TOKEN - done
        if tokens <= 0:
            return 0
        m.sent += tokens
        self.add(kind, tokens)
        return tokens

    def fix(self, m, kind, missing):
        if missing <= 0:
            return 0
        m.sent += missing
        self.add(kind, missing)
        return missing

    # ---------- Sending to the LED (own thread, so nothing slows the pass-through down) ----------
    def add(self, kind, n):
        if n <= 0:
            return
        with self.gate:
            self.pending[kind] = self.pending.get(kind, 0) + n
        self.ensure_sender()

    def ensure_sender(self):
        if self.sender is None:
            self.sender = threading.Thread(target=self.send_loop, daemon=True)
            self.sender.start()

    def send_loop(self):
        host = os.environ.get("TOKENLED_HOST", "tokenled.local")
        udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        ep = None
        next_resolve = next_ping = next_wait = next_use = 0.0
        sent_waiting = False
        sent = (-2, -2, 0, 0)
        while True:
            now = time.monotonic()
            if now >= next_resolve:
                try:   # if resolving fails, the last address is kept
                    ep = (socket.getaddrinfo(host, LED_PORT, socket.AF_INET, socket.SOCK_DGRAM)[0][4][0], LED_PORT)
                except Exception:
                    pass
                next_resolve = now + (5 if ep is None else 60)
            if ep is not None:
                with self.gate:
                    lines = ["T %s %d\n" % kv for kv in self.pending.items()]
                    self.pending.clear()
                if now >= next_ping:
                    lines.append("P\n")
                    next_ping = now + 2
                w = self.waiting
                if w and now >= next_wait:
                    lines.append("A 1\n")   # repeated: the LED expires it
                    next_wait = now + 1
                if not w and sent_waiting:
                    lines.append("A 0\n")
                sent_waiting = w
                d = self.done_req
                if d >= 0:
                    self.done_req = -1
                    lines.append("D %d\n" % d)
                use = (self.use5, self.use7, self.reset5, self.reset7)
                if (use != sent or now >= next_use) and (use[0] >= 0 or use[1] >= 0):
                    lines.append("U %d %d %d %d\n" % use)   # repeated: the LED forgets old values
                    sent = use
                    next_use = now + 60
                if lines:
                    try:
                        udp.sendto("".join(lines).encode("ascii"), ep)
                    except Exception:
                        pass
            time.sleep(0.02)


# ---------- Start and pass-through ----------
def pump_stdin(p, tap):
    try:
        while True:
            b = os.read(0, 65536)
            if not b:
                break
            write_all(p.stdin.fileno(), b)
            try:
                tap.scan_input(b)
            except Exception:
                pass
    except Exception:
        pass
    try:
        p.stdin.close()
    except Exception:
        pass


def main():
    if len(sys.argv) < 2:
        sys.stderr.write("token_tap: path of claude is missing\n")
        return 2
    p = subprocess.Popen(sys.argv[1:], stdin=subprocess.PIPE, stdout=subprocess.PIPE, bufsize=0,
                         preexec_fn=die_with_parent if sys.platform.startswith("linux") else None)
    for sig in ("SIGTERM", "SIGINT", "SIGHUP"):   # claude must not outlive us
        if hasattr(signal, sig):
            signal.signal(getattr(signal, sig), lambda s, f: p.send_signal(s))
    tap = Tap()
    threading.Thread(target=pump_stdin, args=(p, tap), daemon=True).start()
    src = p.stdout.fileno()
    while True:
        b = os.read(src, 65536)
        if not b:
            break
        try:
            write_all(1, b)
        except OSError:   # the extension is gone
            p.terminate()
            break
        try:
            tap.feed(b)
        except Exception:   # a failure of the LED tap must never affect Claude
            pass
    rc = p.wait()
    tap.waiting = False
    if tap.sender is not None:
        time.sleep(0.15)   # let the last packets out
    return rc if rc >= 0 else 128 - rc


if __name__ == "__main__":
    sys.exit(main())
