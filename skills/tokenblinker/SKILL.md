---
name: tokenblinker
description: Set up, test, configure and troubleshoot TokenBlinker (Token LED), the ESP32-C3 desk light that flashes live with the Claude Code token stream, blinks white while Claude waits for approval and shows usage limits. Use it whenever the user mentions TokenBlinker, Token LED, tokenled.local, token_tap, claudeProcessWrapper, or wants a light or desk gadget that shows what Claude Code is doing - flashing the firmware, WiFi setup, installing the VS Code launcher, making it flash, changing colours, brightness, tokens per flash, night mode, strip LEDs, buzzer or MQTT, reading its counters or usage, or fixing "the LED does not flash" or "Claude will not start since I added the wrapper".
license: MIT
compatibility: Live flashing needs the Claude Code extension for VS Code (or Cursor and other VS Code forks); terminal sessions get per-response counting. Helper scripts need Python 3.8+, standard library only. Windows builds the launcher with the .NET Framework C# compiler that ships with Windows.
metadata:
  author: vmynick
  version: "1.0"
  homepage: https://github.com/vmynick/TokenBlinker
---

# TokenBlinker

TokenBlinker is a small WiFi LED (ESP32-C3 SuperMini, optional 7-LED strip in a 3D-printed box) that flashes with
the tokens Claude Code generates. Colours: cyan text, purple thinking, green tool input, orange new input, dark blue
cache read; white blinking = Claude waits for approval; solid green / blinking red = turn finished / failed.

```
VS Code Claude extension -> token_tap (launcher) -> claude -> api.anthropic.com   (traffic untouched)
                               | copies claude's output through byte for byte and reads the stream on the side
                               '-- UDP 4210 "T t 12", "A 1", "D 0" --> LED (tokenled.local) --> flashes
```

## The rule that shapes everything: fail open

The LED is a toy and Claude is the tool. If the LED is off, the network is down or the launcher has a bug, Claude
must keep working exactly as before. So:

- Nothing goes into Claude's API path: no proxy, no `ANTHROPIC_BASE_URL`, no background service. The only hook
  is the `claudeCode.claudeProcessWrapper` VS Code setting, and the launcher it names passes bytes through unchanged.
- That one setting can still lock the user out: if it names a file that is missing, not executable or broken,
  the VS Code extension cannot start Claude at all, and the user loses the tool they would use to fix it. So the
  launcher is installed to a stable place (`~/.tokenblinker/`, not a download folder, repo or this skill's
  folder, which can move or update) and self-tested **before** the setting is written, and every time you touch
  the setting you tell the user how to undo it (delete the line, start a new Claude session).

## The helper script

Everything goes through `scripts/tokenled.py` in this skill's directory (standard library only, spends no tokens):

| Command | What it does |
|---|---|
| `tokenled.py status` | state, live token and flash counters, 5-hour and weekly usage with reset times |
| `tokenled.py test [TYPE ...] [-n N]` | flashes and checks they were shown; TYPE: text thinking tool input cache done error approval all |
| `tokenled.py config` | every setting as JSON |
| `tokenled.py set KEY=VALUE ...` | changes settings, reads them back, prints old -> new and an undo command |
| `tokenled.py install` | builds/copies the launcher to `~/.tokenblinker/` and self-tests it |
| `tokenled.py selftest [LAUNCHER]` | pass-through byte for byte, exit code, fail-open with the LED unreachable, LED signal |
| `tokenled.py doctor` | checks the VS Code setting, the launcher, the LED and settings that make it look dead |

Run it as `python` on Windows and `python3` on macOS/Linux, quoting the full path. The LED address is `--host`,
else the `TOKENLED_HOST` environment variable, else `tokenled.local`. Exit code 1 plus a plain message means the
LED could not be reached; read the message instead of retrying blindly.

Work out which of the four jobs below the user wants: new setup, a quick test or status, a settings change, or
something is wrong. Then go to that section.

## Setup

Some steps are physical or need the user's browser or phone; tell them exactly what to do and do the rest yourself.

1. **Firmware** (user): open https://vmynick.github.io/TokenBlinker/ in Chrome or Edge on a desktop, plug the
   board in with a USB-C *data* cable, click *Install*. With the repository and PlatformIO: `pio run -t upload`.
2. **WiFi** (user): the LED opens an open network `TokenLED-xxxx`; join it with a phone, the setup page opens (or
   http://192.168.4.1), pick a **2.4 GHz** network. Alternative over USB: the user runs
   `python scripts/token_blinker.py --wifi` in their own terminal (needs `pip install pyserial`). That script asks
   for the password interactively. Do not ask the user to paste their WiFi password into the chat.
3. **Check the LED**: `tokenled.py status`. If it fails, `tokenled.py doctor` explains why.
4. **Launcher**: `tokenled.py install`. Continue only if the self-test says OK. On Windows it compiles
   `token_tap.cs` locally with the built-in .NET Framework compiler. That avoids an unsigned download, and the
   exe is replaced safely even while a Claude session is using the old one.
5. **VS Code setting**: the install output prints the exact line and the user settings file(s) it found.
   - Back up the file first (copy it next to itself as `settings.json.bak-tokenblinker`).
   - Add the line inside the top-level `{ }` with the Edit tool. The file is JSON *with comments*, so do not
     load and re-dump it with a JSON library: that would strip the user's comments. On Windows the backslashes
     must be doubled, as printed.
   - If the key already exists, replace its value instead of adding a second one.
   - Tell the user what you changed, and that it takes effect in a **new** Claude session (or after
     *Developer: Reload Window*). To switch it off: delete the line and start a new session.
6. **Confirm**: ask the user to start a new session and say something to Claude; the LED should flash. Live
   flashing applies to new sessions only.
7. **Terminal `claude` users**: the wrapper only exists in the VS Code extension. For terminal sessions,
   `python scripts/token_blinker.py` counts from the session logs per response (not live; no approval signal).
   It skips VS Code sessions, so nothing is counted twice.
8. Optional hardware (strip on GPIO4, buzzer, the printed case): see the project README at
   https://github.com/vmynick/TokenBlinker; afterwards set `count` (number of strip LEDs) with `tokenled.py set`.

## Test and status

- "Is it working?" / "flash it": `tokenled.py test` (3 flashes of each token type) or `tokenled.py test all`
  (adds the green finished signal and a 5 s approval blink). It costs no Claude tokens. `done`/`error` add an
  entry to the LED's event log.
- Counters, current state and usage limits: `tokenled.py status`. These counters run since the LED was powered
  on (a long press on BOOT resets them). Point to http://tokenled.local for the 24-hour, 7-day and 30-day charts
  and the event log.
- End-to-end check of the launcher without spending tokens: `tokenled.py selftest`.

While you work, your own output streams to the LED too, so it may still be playing a short backlog when a test
starts. `test` waits for its flashes to be confirmed, so trust its output.

## Changing settings

`tokenled.py config` shows the current values; `tokenled.py set` changes several at once and verifies them.
Translate the user's words into keys with [references/settings.md](references/settings.md), which lists every key,
its range and its meaning. Common ones:

| User says | Command |
|---|---|
| brighter / dimmer | `set bright=60` (1-255) |
| fewer / more flashes | `set out=20` (text, thinking, tool tokens per flash; `in`, `cache` likewise; 0 = off) |
| make thinking red | `set color.thinking=#ff0000` (types: text thinking tool input cache approval) |
| which strip LED shows what | `set leds.text=1 "leds.approval=6,7"` (`all`, ranges like `2-4`, empty = automatic) |
| dark at night | `set night=on nfrom=23 nto=7 nbr=0` (needs the right `tz`, a POSIX TZ string) |
| louder approval | `set bzap=2` (beep every second until answered; needs a buzzer on `bzpin`) |
| lower latency | `set saver=off` (needs USB power) |

- After a change, give the user the `Undo:` line it prints.
- `count`, `pin`, `onbt`, `bzpin` and `bzt` describe the wiring. Ask what is connected before changing them:
  a wrong value does not damage anything, but the LED goes dark.
- `mqpass` is write-only: it is never shown or exported.

## Troubleshooting

Start with `tokenled.py doctor`. It prints OK / INFO / WARN / FAIL lines, and FAIL lines say what to do. Then use
[references/troubleshooting.md](references/troubleshooting.md) for the symptom.

One case comes before any debugging: **Claude does not start in VS Code since the wrapper was set**. Remove the
`claudeCode.claudeProcessWrapper` line first, so the user has a working Claude again. Then investigate: doctor,
`install`, selftest, and put the line back only after the self-test passes.

## Protocol (for scripts and integrations)

UDP port 4210 (and USB serial at 115200), one command per line, several lines per packet allowed:
`T <t|k|j|i|r> <tokens>` raw tokens (text, thinking, tool, new input, cache; the LED divides by tokens per
flash), `B <type> <n>` n flashes directly, `A 1` / `A 0` approval wait (expires after 5 s, so repeat it each
second), `D 0` / `D 1` turn finished / error, `U <5h%> <7d%> [<5h reset> <7d reset>]` usage (Unix times),
`P` keep-alive. USB only: `W <ssid>\t<password>` stores WiFi, `?` answers `IP <address>` or `NOWIFI`.
HTTP: `GET /s` live state (JSON), `GET /config.json` settings, `POST /import` settings JSON (keys left out keep
their value; `out` and `bright` are required).
