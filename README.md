# Claude token LED

A desk LED (ESP32-C3 SuperMini Plus) that flashes live with the tokens Claude Code generates, and
signals when Claude waits for your approval. WiFi, battery-capable, no proxy.

It never calls the model and spends no tokens: it only watches output that is already flowing.

## Files

| File | What it is |
|---|---|
| `token_tap.cs` → `token_tap.exe` | **Live flashing:** launcher for the VS Code Claude extension. Passes the `claude.exe` output through unchanged and sends the token stream and approval waits to the LED. |
| `src/main.cpp` + `platformio.ini` | Firmware: onboard WS2812B (GPIO8) or an external strip, WiFi + USB + web page. |
| `token_blinker.py` | WiFi setup over USB, colour test, and log-based counting for terminal Claude Code sessions. |
| `token_led.ino`, `token_led_test.py`, `token_proxy.py`, `claude-token-led.user.js` | Old proxy-based experiments (Hungarian comments), kept for reference. |

## How it works

```
VS Code Claude extension ──> token_tap.exe ──> claude.exe ──HTTPS──> api.anthropic.com   (unchanged)
                                  │  passes claude.exe's output through byte for byte,
                                  │  meanwhile counts from the stream_event lines
                                  └──UDP "T t 12" / "A 1"──> ESP32 (tokenled.local:4210) ──> flashes
```

There is no proxy and no background program, and the API traffic is untouched. If the LED is off or
there is no network, Claude works exactly the same, it just does not flash. The extension starts
`token_tap.exe` through the `claudeCode.claudeProcessWrapper` setting (VS Code user `settings.json`);
to switch it off, delete that line and start a new Claude session.

Text, thinking and tool-input tokens are estimated while generating (4 characters ≈ 1 token); at the end of
each message the real `output_tokens` tops up the difference. Input and cache tokens come exactly at the
start of the request.

| Colour | What | One flash (default) |
|---|---|---|
| cyan | text | 10 tokens |
| purple | thinking | 10 tokens |
| green | tool input | 10 tokens |
| orange | new input (`input` + `cache_creation`) | 100 tokens |
| dark blue | cache read | off |
| white, blinking | **Claude waits for your approval** | — |

Approval: when Claude asks permission for a tool (`control_request` / `can_use_tool` on its output),
the LED blinks white once per second until the request is answered. The LED forgets it after 5 s without a
refresh, so a crashed launcher cannot leave it blinking. With several Claude sessions open, answering one
clears the signal for all. The launcher also passes Claude's input through (byte for byte) and ends the signal the moment
your answer (`control_response`) goes in, so the LED stops blinking even while the approved tool is still running.

## Web page: http://tokenled.local

- **Virtual LEDs** (one column per type + approval) with live token counts and flash counts.
- Tokens per flash per type (0 = off), brightness, dim glow while Claude runs (off by default), power-saving WiFi
  (for battery; off = faster), colour test (including an approval blink), status, WiFi network change.
- **Onboard LED (GPIO8) and external strip are separate outputs.** The onboard LED can mirror everything (default),
  show only status and approval, or be off. The external strip has its own data pin (default **GPIO4**; allowed: GPIO 0-7 and 10)
  and any number of LEDs from 0 (none, default) to 65 535. Wire it with a shared GND and power it from its own 5 V supply;
  a strip draws up to ~60 mA per LED at full white, so lower the brightness. Drawing takes ~30 µs per LED, so flashes get
  slower with thousands of LEDs.
- **Colour per type.** Every type and the approval signal has its own colour picker (applies to the onboard LED and the strip).
- **LEDs per type (strip).** Each type can use any set of strip LEDs: a list like `1,3,5-8` (1 = first LED), `all` or `0` = every LED.
  Several types may share LEDs. Empty = automatic: with 5 or more LEDs every type gets its own LED (approval: the 6th LED when
  there are 6+), otherwise all strip LEDs flash together in the type's colour. Status dots and the glow go to the onboard LED
  (or to the strip's first LED when the onboard LED is off).

## WiFi setup (AP mode)

If no network is saved, or it cannot connect within 30 s, the LED starts its own open network
`TokenLED-xxxx` (last 4 digits of its MAC). Join it with a phone and the setup page opens by itself
(otherwise go to http://192.168.4.1): pick the network and enter the password. After a successful connection the AP
disappears. If the saved network is only temporarily unavailable, it retries every 3 minutes while nobody is on the AP.
The same over USB: `python token_blinker.py --wifi`.

A dim dot every 3 s: red = connecting to WiFi, yellow = setup AP running. At power-on: red-green-blue test.

## Usage

```powershell
pio run -t upload                 # firmware (COM9, platformio.ini)
python token_blinker.py --wifi    # WiFi over USB instead of AP mode: asks for name and password, saved in the ESP's flash
python token_blinker.py --test 3  # colour test (WiFi); --usb for the USB serial port
C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe /nologo /optimize /r:System.Web.Extensions.dll token_tap.cs
```

`--wifi` and `--usb` need `pip install pyserial`; the WiFi mode uses the standard library only. The PlatformIO serial
monitor and the script cannot hold the port at the same time.

Rebuilding `token_tap.exe` fails while a Claude session uses it (file lock). Rename the running exe first
(`ren token_tap.exe token_tap_old.exe`; running processes keep working), then build. If the project folder moves, also change
the path in the VS Code setting, otherwise Claude will not start.

Terminal Claude Code: `python token_blinker.py` (from the logs, per response; it skips VS Code sessions so nothing is
counted twice). Approval waits are only detected in VS Code.

On battery: modem sleep and 80 MHz CPU for lower power (a few hundred ms latency). The board has no charger; power
goes to the 3.3–6 V VCC/5V pin; the WS2812B draws about 1 mA even when off.

Protocol (UDP 4210 and USB CDC, one command per line, several lines per UDP packet are fine):
`T <t|k|j|i|r> <tokens>` raw tokens, `B <type> <count>` flash directly, `A 1` / `A 0` approval wait, `P` keep-alive;
USB only: `W <ssid>\t<password>`, `?` → `IP <address>` / `NOWIFI`.

## More features

- **Web pages:** *Live* (virtual LEDs, click one to test it, usage bars, 7-day totals, status), *Settings* (all options) and
  *System* (WiFi, firmware update over WiFi: upload `.pio/build/supermini/firmware.bin`; the page has no password).
- **Finished / error signal:** at the end of a turn the LED shows solid green (finished) or blinking red (error).
- **Approval has priority:** while Claude waits for approval, queued flashes are dropped and only the white blink runs.
- **Usage bar:** the strip shows the 5-hour or weekly limit (from Claude's `rate_limit_event`) as a dim green/yellow/red bar.
- **History:** optional running light, every flash enters at LED 1 and slides along the strip.
- **Night:** dim (or switch off) between two hours; time via NTP, time zone as a POSIX string.
- **7-day totals** are kept in flash (output / new input / cache per day).
- **MQTT / Home Assistant** (optional, runs in its own task, a missing broker never delays the LED): `prefix/state`
  (idle, working, approval, done, error), `prefix/usage5`, `prefix/usage7`, plus MQTT discovery.
- Flashes are fast (~50 ms per flash, faster with a backlog) and the backlog is capped at ~1 s, so the LED stays live.
  Switch off *Power-saving WiFi* on the Settings page for the lowest latency (needs mains/USB power).

- **Approval reminder:** after N seconds of pending approval (default 45, 0 = off) the white blink gets twice as fast.
- **Last 24 hours:** bar chart on the Live page (output + new input per hour) with the busiest hour; kept in flash.
- **BOOT button (GPIO9):** short press = quiet mode (night brightness) until pressed again, long press (1.5 s) = reset the live counters.
- **Backup:** System page → download the settings as JSON (`/config.json`, no WiFi or MQTT password) and restore them from a file.

New protocol lines: `D 0` / `D 1` (turn finished / error), `U <5h%> <7d%>` (usage, -1 = unknown).
