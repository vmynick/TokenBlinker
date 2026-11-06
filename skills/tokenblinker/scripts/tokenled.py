#!/usr/bin/env python3
"""TokenBlinker (Token LED) helper for the tokenblinker skill. Standard library only, Python 3.8+.
It never calls Claude and spends no tokens.

  tokenled.py status                  state, live token and flash counters, usage limits
  tokenled.py test [TYPE ...] [-n N]  flash the LED; TYPE: text thinking tool input cache done error approval all
  tokenled.py config                  all settings as JSON
  tokenled.py set KEY=VALUE ...       change settings; prints old -> new so it can be undone
  tokenled.py install                 put the VS Code launcher in ~/.tokenblinker and self-test it
  tokenled.py selftest [LAUNCHER]     check a launcher: byte-for-byte pass-through, exit code, fail-open, LED signal
  tokenled.py doctor                  check the LED, its settings, the launcher and the VS Code setting

LED address: --host, else the TOKENLED_HOST environment variable, else tokenled.local.
"""
import argparse
import datetime
import json
import os
import re
import socket
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
UDP_PORT = 4210
IS_WIN = os.name == "nt"
INSTALL_DIR = Path(os.environ.get("TOKENBLINKER_HOME") or Path.home() / ".tokenblinker")
LAUNCHER = INSTALL_DIR / ("token_tap.exe" if IS_WIN else "token_tap.py")
WRAPPER_KEY = "claudeCode.claudeProcessWrapper"
UNREACHABLE = "tokenblinker-selftest.invalid"

TYPES = ["text", "thinking", "tool", "input", "cache", "approval"]   # index = settings suffix (c0..c5, g0..g5, onbm0..5)
CODE = {"text": "t", "thinking": "k", "tool": "j", "input": "i", "cache": "r"}
LABEL = ["text", "thinking", "tool input", "new input", "cache read"]
TEST_TYPES = TYPES[:5] + ["done", "error", "approval", "all"]

RANGES = {"out": (0, 60000), "in": (0, 60000), "cache": (0, 60000), "bright": (1, 255), "remind": (0, 3600),
          "onb": (0, 2), "onbt": (0, 2), "onbbr": (1, 100), "bzt": (0, 1), "bzhz": (100, 10000), "bzap": (0, 2),
          "count": (0, 65535), "meter": (0, 2), "nfrom": (0, 23), "nto": (0, 23), "nbr": (0, 255),
          "mqport": (1, 65535)}
PINS = {0, 1, 2, 3, 4, 5, 6, 7, 10}          # GPIOs the firmware accepts for the strip and the buzzer
WIRING = {"count", "pin", "onbt", "bzpin", "bzt"}   # depend on how the board is wired
COLOURS = {"red": "ff0000", "green": "00ff00", "blue": "0000ff", "white": "ffffff", "cyan": "00b4ff",
           "purple": "9600ff", "magenta": "ff00ff", "pink": "ff4080", "orange": "ff5a00", "yellow": "ffc800",
           "amber": "ffb000", "teal": "00ffb4", "warmwhite": "ffb46e", "darkblue": "000078", "black": "000000"}

try:
    sys.stdout.reconfigure(errors="replace")
except Exception:
    pass


def die(msg, code=1):
    print(msg)
    sys.exit(code)


# ---------- talking to the LED ----------
_http = urllib.request.build_opener(urllib.request.ProxyHandler({}))   # a system proxy must not get LAN requests


def host_of(a):
    return getattr(a, "host", None) or os.environ.get("TOKENLED_HOST") or "tokenled.local"


def resolve(host, tries=3):
    for i in range(tries):
        try:
            return socket.getaddrinfo(host, UDP_PORT, socket.AF_INET, socket.SOCK_DGRAM)[0][4][0]
        except OSError:
            if i + 1 < tries:
                time.sleep(0.7)
    return None


def http(ip, path, body=None, timeout=6):
    req = urllib.request.Request("http://%s%s" % (ip, path), data=body,
                                 headers={"Content-Type": "text/plain"} if body is not None else {})
    err = None
    for _ in range(3):   # the ESP sometimes misses a request while it is busy flashing
        try:
            with _http.open(req, timeout=timeout) as r:
                return r.read().decode("utf-8", "replace")
        except urllib.error.HTTPError:
            raise
        except (urllib.error.URLError, OSError) as e:
            err = e
            time.sleep(0.5)
    raise err


def udp(ip, lines):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.sendto(("".join(l + "\n" for l in lines)).encode("ascii"), (ip, UDP_PORT))
    s.close()


def connect(a):
    host = host_of(a)
    ip = resolve(host)
    if not ip:
        die("Cannot find the LED: '%s' does not resolve. Is it powered and on the same network? "
            "Run `tokenled.py doctor`, or pass --host <IP>." % host)
    try:
        json.loads(http(ip, "/s"))
    except Exception as e:
        die("%s (%s) resolves but its web server does not answer (%s). Run `tokenled.py doctor`." % (host, ip, e))
    return host, ip


def state(ip):
    return json.loads(http(ip, "/s"))


def config(ip):
    return json.loads(http(ip, "/config.json"))


# ---------- status ----------
def when(ts):
    if not ts:
        return ""
    t = datetime.datetime.fromtimestamp(ts)
    return ", resets %s" % t.strftime("%a %H:%M")


def cmd_status(a):
    host, ip = connect(a)
    s = state(ip)
    if a.json:
        print(json.dumps(s))
        return
    print("Token LED at %s (%s): %s%s" % (host, ip, s["s"], "  << waiting for approval" if s["w"] else ""))
    print("Since power-on or the last counter reset:")
    for name, t, f in zip(LABEL, s["t"], s["f"]):
        print("  %-11s %13s tokens %9s flashes" % (name, "{:,}".format(t), "{:,}".format(f)))
    print("  approvals asked %d, turns finished %d, turns with error %d" % (s["a"], s["d"], s["e"]))
    for k, r, name in (("u5", "r5", "5-hour"), ("u7", "r7", "weekly")):
        print("  %s limit: %s" % (name, "%d %%%s" % (s[k], when(s[r])) if s[k] >= 0
                                  else "unknown (Claude has not reported it in the last 30 min)"))
    print("Charts, 7-day and 30-day totals, recent events: http://%s" % host)


# ---------- test ----------
def cmd_test(a):
    host, ip = connect(a)
    want = a.types or ["text", "thinking", "tool", "input", "cache"]
    if "all" in want:
        want = ["text", "thinking", "tool", "input", "cache", "done", "approval"]
    n = max(1, min(a.n, 50))
    before = state(ip)
    flash = [t for t in want if t in CODE]
    if flash:
        udp(ip, ["B %s %d" % (CODE[t], n) for t in flash])
        # Wait until they have been shown: the LED may still be playing a backlog (Claude's own stream), and an
        # approval blink sent too early would drop them from its queue.
        end = time.monotonic() + 4 + 0.15 * n * len(flash)
        while time.monotonic() < end:
            time.sleep(0.3)
            cur = state(ip)
            if all(cur["f"][TYPES.index(t)] >= before["f"][TYPES.index(t)] + n for t in flash):
                break
    for sig, line in (("done", "D 0"), ("error", "D 1")):
        if sig in want:
            udp(ip, [line])
            time.sleep(2.5)
    if "approval" in want:
        udp(ip, ["A 1"])   # the LED ends it by itself after 5 s
        time.sleep(0.4)
    after = state(ip)

    print("Sent to %s (%s): %s" % (host, ip, ", ".join(want)))
    seen = []
    for t in flash:
        i = TYPES.index(t)
        got = after["f"][i] - before["f"][i]
        seen.append(got > 0)
        print("  %-9s %s" % (t, "%d flashes shown" % got if got > 0 else "not seen (counter unchanged)"))
    for sig, key, what in (("done", "d", "green finished signal"), ("error", "e", "red error signal")):
        if sig in want:
            seen.append(after[key] > before[key])
            print("  %-9s %s" % (sig, what if seen[-1] else "not seen"))
    if "approval" in want:
        seen.append(bool(after["w"]))
        print("  approval  %s" % ("blinking now (stops by itself in 5 s)" if seen[-1] else "not seen"))
    if seen and not any(seen):
        print("The web page answers but nothing sent over UDP arrived: port %d may be blocked between this "
              "computer and the LED (firewall, guest network, VLAN)." % UDP_PORT)
    elif not all(seen):
        print("Some of it was not seen in time; the LED may have been busy. Check http://%s" % host)
    c = config(ip)
    if c.get("onb") == 2 and not c.get("count"):
        print("Note: the onboard LED is off (onb=2) and no strip is set (count=0), so nothing lights up.")


# ---------- settings ----------
def cmd_config(a):
    host, ip = connect(a)
    print(json.dumps(config(ip), indent=1))


def norm_colour(v):
    v = v.strip().lower().replace(" ", "")
    v = COLOURS.get(v, v).lstrip("#")
    if re.fullmatch(r"[0-9a-f]{3}", v):
        v = "".join(ch * 2 for ch in v)
    if not re.fullmatch(r"[0-9a-f]{6}", v):
        raise ValueError("colour must be #rrggbb, #rgb or one of: " + ", ".join(sorted(COLOURS)))
    return "#" + v


def real_key(k):
    k = k.strip()
    m = re.fullmatch(r"(color|colour|leds|mirror)\.(\w+)", k.lower())
    if m:
        kind, t = m.groups()
        t = {"output": "text", "think": "thinking", "tools": "tool", "new_input": "input", "newinput": "input",
             "cache_read": "cache", "wait": "approval"}.get(t, t)
        if t not in TYPES:
            raise ValueError("unknown type '%s' (use %s)" % (t, ", ".join(TYPES)))
        return {"color": "c", "colour": "c", "leds": "g", "mirror": "onbm"}[kind] + str(TYPES.index(t))
    return {"brightness": "bright"}.get(k.lower(), k)


def coerce(key, raw, cur):
    if re.fullmatch(r"c[0-5]", key):
        return norm_colour(raw)
    if isinstance(cur, bool):
        v = raw.strip().lower()
        if v in ("true", "on", "yes", "1"):
            return True
        if v in ("false", "off", "no", "0"):
            return False
        raise ValueError("%s is on/off" % key)
    if isinstance(cur, int):
        v = int(raw.strip())
        if key in ("pin", "bzpin"):
            if v not in PINS and not (key == "bzpin" and v == -1):
                raise ValueError("%s must be a GPIO in 0-7 or 10%s" % (key, " (-1 = no buzzer)" if key == "bzpin" else ""))
        elif key in RANGES:
            lo, hi = RANGES[key]
            if not lo <= v <= hi:
                raise ValueError("%s must be %d..%d" % (key, lo, hi))
        return v
    v = raw.replace('"', "").replace("\\", "")   # the firmware drops these characters anyway
    return v.strip()


def cmd_set(a):
    host, ip = connect(a)
    cur = config(ip)
    new = dict(cur)
    changes = []
    for item in a.pairs:
        if "=" not in item:
            die("Expected KEY=VALUE, got '%s'" % item)
        k, v = item.split("=", 1)
        try:
            key = real_key(k)
            if key == "mqpass":   # write-only: never exported, kept when empty
                new[key] = v
                changes.append((key, "(hidden)", "(set)"))
                continue
            if key not in cur:
                raise ValueError("unknown setting '%s'. Known: %s" % (k, ", ".join(sorted(cur))))
            new[key] = coerce(key, v, cur[key])
        except ValueError as e:
            die("Not changed: %s" % e)
        changes.append((key, cur[key], new[key]))
    if new.get("bzpin", -1) != -1 and new.get("count") and new.get("bzpin") == new.get("pin"):
        die("Not changed: the buzzer cannot use the strip's data pin (GPIO %d)." % new["pin"])

    body = json.dumps(new, ensure_ascii=False, indent=0).encode("utf-8")
    try:
        reply = http(ip, "/import", body)
    except urllib.error.HTTPError as e:
        die("The LED refused the settings: HTTP %d %s" % (e.code, e.read().decode("utf-8", "replace")))
    time.sleep(0.3)
    back = config(ip)
    print("%s -> %s" % (host, reply.strip()))
    undo = []
    for key, old, want in changes:
        if key == "mqpass":
            print("  mqpass: set")
            continue
        got = back.get(key)
        ok = str(got).lower() == str(want).lower()
        print("  %s: %s -> %s%s" % (key, json.dumps(old), json.dumps(got), "" if ok else "  (asked for %s)" % json.dumps(want)))
        undo.append("%s=%s" % (key, str(old).lower() if isinstance(old, bool) else old))
    if any(k in WIRING for k, _, _ in changes):
        print("  wiring settings changed: check that they match how the strip / buzzer is connected")
    if undo:
        print("Undo: tokenled.py set " + " ".join('"%s"' % u if " " in u or "," in u else u for u in undo))


# ---------- launcher install and self-test ----------
def find_csc():
    windir = os.environ.get("WINDIR", r"C:\Windows")
    for fw in ("Framework64", "Framework"):
        p = Path(windir) / "Microsoft.NET" / fw / "v4.0.30319" / "csc.exe"
        if p.exists():
            return p
    return None


def base_python():
    """A Python that will still exist later: the base interpreter, not a virtualenv's."""
    exe = Path(sys.executable)
    if sys.prefix != getattr(sys, "base_prefix", sys.prefix):
        for name in ("python3", "python"):
            p = Path(sys.base_prefix) / "bin" / name
            if p.exists():
                return p
    return exe


def cmd_install(a):
    INSTALL_DIR.mkdir(parents=True, exist_ok=True)
    if IS_WIN:
        csc = find_csc()
        if not csc:
            die("The C# compiler of the .NET Framework (csc.exe) was not found. Download token_tap.exe from "
                "https://github.com/vmynick/TokenBlinker/releases/latest into %s and run `tokenled.py selftest`."
                % INSTALL_DIR)
        fresh = INSTALL_DIR / "token_tap.new.exe"
        r = subprocess.run([str(csc), "/nologo", "/optimize", "/r:System.Web.Extensions.dll", "/out:" + str(fresh),
                            str(HERE / "token_tap.cs")], capture_output=True, text=True)
        if r.returncode != 0 or not fresh.exists():
            die("Compiling token_tap.cs failed:\n" + r.stdout + r.stderr)
        if LAUNCHER.exists():
            try:
                LAUNCHER.unlink()
            except OSError:   # a running Claude session holds it; a renamed exe keeps running
                LAUNCHER.rename(INSTALL_DIR / ("token_tap_old_%d.exe" % time.time()))
        fresh.rename(LAUNCHER)
        for old in INSTALL_DIR.glob("token_tap_old_*.exe"):
            try:
                old.unlink()   # only succeeds once no session uses it
            except OSError:
                pass
    else:
        src = (HERE / "token_tap.py").read_text(encoding="utf-8").splitlines(True)
        py = base_python()
        if src and src[0].startswith("#!"):
            src[0] = "#!%s\n" % py   # absolute: an app started from the Dock / desktop has a minimal PATH
        tmp = LAUNCHER.with_suffix(".new")
        tmp.write_text("".join(src), encoding="utf-8")
        tmp.chmod(0o755)
        os.replace(str(tmp), str(LAUNCHER))
        print("Interpreter: %s (run install again if this Python is removed or moved)" % py)
    print("Installed: %s" % LAUNCHER)
    ok = selftest(LAUNCHER, a)
    print()
    if ok:
        print("Add this line to the VS Code user settings.json, then start a new Claude session:")
        print('  "%s": %s' % (WRAPPER_KEY, json.dumps(str(LAUNCHER))))
        files = settings_files()
        if files:
            print("User settings file(s) found:")
            for f in files:
                print("  %s" % f)
        print("To switch it off again: delete that line and start a new Claude session.")
    else:
        die("Do NOT point VS Code at this launcher until the self-test passes.")


def run_launcher(launcher, host, until=None, limit=20):
    """Runs the launcher around fake_claude.py. Like a real session, the input stays open (until `until()` is
    true or `limit` s pass), which gives the launcher time to find the LED: a fresh process can need several
    seconds for an mDNS name. Returns (exit code, stdout, stderr, expected stdout, expected exit code,
    seconds from closing the input to the exit)."""
    sys.path.insert(0, str(HERE))
    sys.dont_write_bytecode = True   # keep the skill folder clean
    import fake_claude
    stdin = b"ping \x00\xff\r\n" + b'{"type":"control_response"}\n'
    p = subprocess.Popen([str(launcher), sys.executable, str(HERE / "fake_claude.py")], stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=dict(os.environ, TOKENLED_HOST=host))
    got = {}
    readers = [threading.Thread(target=lambda n=n, s=s: got.__setitem__(n, s.read()), daemon=True)
               for n, s in (("out", p.stdout), ("err", p.stderr))]
    for r in readers:
        r.start()
    try:
        p.stdin.write(stdin)
        p.stdin.flush()
        t = time.monotonic()
        while until and time.monotonic() - t < limit and not until():
            time.sleep(0.5)
        p.stdin.close()
    except OSError:
        pass
    t = time.monotonic()
    try:
        rc = p.wait(timeout=30)
    except subprocess.TimeoutExpired:
        p.kill()
        raise
    for r in readers:
        r.join(5)
    return rc, got.get("out", b""), got.get("err", b""), fake_claude.payload(stdin), fake_claude.EXIT_CODE, time.monotonic() - t


def selftest(launcher, a):
    launcher = Path(launcher)
    print("Self-test of %s" % launcher)
    if not launcher.exists():
        print("  FAIL  the file does not exist")
        return False
    if not IS_WIN and not os.access(str(launcher), os.X_OK):
        print("  FAIL  not executable (chmod +x)")
        return False

    # 1) LED unreachable: Claude's output must still pass through unchanged, and nothing may hang.
    try:
        rc, out, err, want, code, secs = run_launcher(launcher, UNREACHABLE)
    except subprocess.TimeoutExpired:
        print("  FAIL  the launcher hung with the LED unreachable")
        return False
    except OSError as e:
        print("  FAIL  cannot start it: %s" % e)
        return False
    ok = True
    if out == want:
        print("  OK    output passed through byte for byte (%d bytes: binary, CRLF, a 100 kB line)" % len(want))
    else:
        print("  FAIL  output changed: %d bytes expected, %d bytes came out" % (len(want), len(out)))
        ok = False
    if rc == code:
        print("  OK    input passed through, exit code passed through (%d)" % code)
    else:
        print("  FAIL  exit code %s, expected %d" % (rc, code))
        ok = False
    print("  %s  with the LED unreachable it exits %.1f s after its input closes" % ("OK  " if secs < 5 else "WARN", secs))
    if not ok:
        if err:
            print("  stderr: " + err.decode("utf-8", "replace")[:500])
        return False

    # 2) LED reachable: the `result` line must arrive as "D 0" (turn finished).
    host = host_of(a)
    ip = resolve(host)
    if not ip:
        print("  SKIP  LED path: %s does not resolve now (the launcher itself is fine)" % host)
        return True
    try:
        before = state(ip)["d"]
        t = time.monotonic()
        run_launcher(launcher, host, until=lambda: state(ip)["d"] > before)
        took = time.monotonic() - t
        after = state(ip)["d"]
    except Exception as e:
        print("  SKIP  LED path: %s" % e)
        return True
    if after > before:
        print("  OK    the LED got the finished signal from the launcher after %.0f s (green flash)" % took)
    else:
        print("  WARN  the LED did not register the finished signal within 20 s. The launcher is safe to use, but "
              "check that UDP %d reaches %s (%s) from this computer." % (UDP_PORT, host, ip))
    return True


def cmd_selftest(a):
    sys.exit(0 if selftest(a.launcher or LAUNCHER, a) else 1)


# ---------- doctor ----------
def settings_files():
    if IS_WIN:
        base = Path(os.environ.get("APPDATA", Path.home() / "AppData" / "Roaming"))
    elif sys.platform == "darwin":
        base = Path.home() / "Library" / "Application Support"
    else:
        base = Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config"))
    found = []
    for app in ("Code", "Code - Insiders", "VSCodium", "Cursor", "Windsurf"):
        user = base / app / "User"
        for f in [user / "settings.json"] + sorted(user.glob("profiles/*/settings.json")):
            if f.exists():
                found.append(f)
    return found


def wrapper_in(path):
    """The wrapper path set in a VS Code settings file (JSON with comments), or None."""
    try:
        text = path.read_text(encoding="utf-8-sig")
    except OSError:
        return None
    for line in text.splitlines():
        if line.strip().startswith("//"):
            continue
        m = re.search(r'"%s"\s*:\s*"((?:[^"\\]|\\.)*)"' % re.escape(WRAPPER_KEY), line)
        if m:
            return json.loads('"%s"' % m.group(1))
    return None


def check_launcher(p, say):
    p = Path(p)
    if not p.exists():
        say("FAIL", "%s does not exist: Claude in VS Code cannot start. Remove the %s line now "
                    "(or run `tokenled.py install` and point it at %s)." % (p, WRAPPER_KEY, LAUNCHER))
        return
    if not IS_WIN:
        if not os.access(str(p), os.X_OK):
            say("FAIL", "%s is not executable: Claude in VS Code cannot start (chmod +x it)." % p)
            return
        first = p.read_text(encoding="utf-8", errors="replace").split("\n", 1)[0]
        interp = first[2:].strip().split()[0] if first.startswith("#!") else ""
        if interp and not interp.endswith("/env") and not Path(interp).exists():
            say("FAIL", "its interpreter %s is gone: Claude in VS Code cannot start. Run `tokenled.py install`." % interp)
            return
    say("OK", "launcher %s exists" % p)


def cmd_doctor(a):
    problems = []

    def say(level, msg):
        print("  %-4s  %s" % (level, msg))
        if level in ("FAIL", "WARN"):
            problems.append(level)

    print("Python %s on %s" % (sys.version.split()[0], sys.platform))

    print("\nVS Code setting (%s)" % WRAPPER_KEY)
    files = settings_files()
    wrappers = []
    for f in files:
        w = wrapper_in(f)
        if w:
            say("OK", "%s -> %s" % (f, w))
            wrappers.append(w)
    if not files:
        say("INFO", "no VS Code user settings.json found")
    elif not wrappers:
        say("INFO", "not set: Claude in VS Code runs without the launcher, so the LED does not flash live")
    for w in dict.fromkeys(wrappers):
        check_launcher(w, say)
        if Path(w).resolve() != LAUNCHER.resolve() and Path(w).exists():
            say("INFO", "it is not the launcher this skill installs (%s); fine if it is a copy you keep up to date" % LAUNCHER)

    print("\nLauncher")
    if LAUNCHER.exists():
        check_launcher(LAUNCHER, say)
    else:
        say("INFO", "%s not installed (`tokenled.py install`)" % LAUNCHER)
    if "TOKENLED_HOST" in os.environ:
        say("INFO", "TOKENLED_HOST=%s in this environment (VS Code must have it too)" % os.environ["TOKENLED_HOST"])

    print("\nLED")
    host = host_of(a)
    ip = resolve(host)
    if not ip:
        say("FAIL", "%s does not resolve. Check the LED: a dim red dot every 3 s = cannot join WiFi, yellow = setup "
                    "network TokenLED-xxxx is open (join it and pick the WiFi). If it is on the WiFi, mDNS may be "
                    "blocked: use its IP (--host, TOKENLED_HOST) and reserve that IP in the router." % host)
    else:
        say("OK", "%s = %s" % (host, ip))
        try:
            s = state(ip)
            c = config(ip)
        except Exception as e:
            say("FAIL", "no answer from http://%s (%s)" % (ip, e))
        else:
            say("OK", "web server answers, state: %s" % s["s"])
            if c.get("onb") == 2 and not c.get("count"):
                say("WARN", "nothing can light up: onboard LED off (onb=2) and no strip (count=0)")
            if c.get("bright", 255) < 10:
                say("WARN", "brightness is very low (bright=%d)" % c["bright"])
            for k, name in (("out", "text/thinking/tool"), ("in", "new input")):
                if c.get(k) == 0:
                    say("INFO", "%s flashes are off (%s=0)" % (name, k))
            if c.get("night"):
                h, f, t = datetime.datetime.now().hour, c.get("nfrom", 0), c.get("nto", 0)
                if (f <= h < t) if f < t else (h >= f or h < t):
                    say("INFO", "night mode is on now (%d-%d h), brightness %d%s" % (f, t, c.get("nbr", 0),
                                                                                      " = dark" if not c.get("nbr") else ""))
            if c.get("saver"):
                say("INFO", "power-saving WiFi is on: a few hundred ms of lag (saver=false on USB power)")

    print()
    if problems:
        print("%d problem(s) found." % len(problems))
        sys.exit(1)
    print("No problems found.")


# ---------- main ----------
def main():
    common = argparse.ArgumentParser(add_help=False)   # --host works before or after the command
    common.add_argument("--host", default=argparse.SUPPRESS, help="LED address (default: TOKENLED_HOST or tokenled.local)")
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0], parents=[common])
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("status", parents=[common], help="state, counters, usage")
    p.add_argument("--json", action="store_true")
    p.set_defaults(fn=cmd_status)
    p = sub.add_parser("test", parents=[common], help="flash the LED")
    p.add_argument("types", nargs="*", metavar="TYPE", help=" ".join(TEST_TYPES))
    p.add_argument("-n", type=int, default=3, help="flashes per type (default 3)")
    p.set_defaults(fn=cmd_test)
    sub.add_parser("config", parents=[common], help="all settings").set_defaults(fn=cmd_config)
    p = sub.add_parser("set", parents=[common], help="change settings: KEY=VALUE ...")
    p.add_argument("pairs", nargs="+", metavar="KEY=VALUE")
    p.set_defaults(fn=cmd_set)
    sub.add_parser("install", parents=[common], help="install and self-test the VS Code launcher").set_defaults(fn=cmd_install)
    p = sub.add_parser("selftest", parents=[common], help="check a launcher")
    p.add_argument("launcher", nargs="?")
    p.set_defaults(fn=cmd_selftest)
    sub.add_parser("doctor", parents=[common], help="find what is wrong").set_defaults(fn=cmd_doctor)
    a = ap.parse_args()
    bad = [t for t in getattr(a, "types", []) if t not in TEST_TYPES]
    if bad:
        ap.error("unknown test type %s (use: %s)" % (", ".join(bad), " ".join(TEST_TYPES)))
    a.fn(a)


if __name__ == "__main__":
    main()
