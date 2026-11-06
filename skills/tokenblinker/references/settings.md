# TokenBlinker settings

Every key that `tokenled.py config` shows and `tokenled.py set KEY=VALUE` accepts. They are the field names of the
LED's Settings page (http://tokenled.local/settings) and of its backup file. The device clamps out-of-range numbers;
`tokenled.py set` refuses them instead, so the user gets the value they asked for or a clear error.

Types by index: **0 text, 1 thinking, 2 tool input, 3 new input, 4 cache read, 5 approval**. Instead of the index
keys you can write `color.<type>`, `leds.<type>` and `mirror.<type>` with the type names
`text thinking tool input cache approval`. Booleans take on/off, true/false, yes/no or 1/0.

## Flashes

| Key | Default | Meaning |
|---|---|---|
| `out` | 10 | Text, thinking and tool-input tokens per flash (0-60000, 0 = these types never flash) |
| `in` | 100 | New input (`input` + `cache_creation`) tokens per flash (0 = off) |
| `cache` | 0 | Cache-read tokens per flash (0 = off; cache reads are huge, keep this high or off) |
| `bright` | 120 | Brightness of flashes, 1-255 (the strip draws up to ~60 mA per LED at full white) |
| `glow` | off | Dim glow while Claude is running |
| `saver` | on | Power-saving WiFi for battery use: a few hundred ms of lag. Off = fastest, needs USB power |
| `done` | on | End of a turn: solid green (finished) or blinking red (error) |
| `remind` | 45 | After this many seconds of pending approval the white blink doubles its speed (0-3600, 0 = off) |

## Colours and strip LEDs

| Key | Default | Meaning |
|---|---|---|
| `c0`..`c5` / `color.<type>` | `#00b4ff` `#9600ff` `#00ff3c` `#ff5a00` `#000078` `#ffffff` | Colour per type, `#rrggbb`. The script also takes `#rgb` and the names red green blue white cyan purple magenta pink orange yellow amber teal warmwhite darkblue black |
| `g0`..`g5` / `leds.<type>` | empty | Strip LEDs for that type: a list like `1,3,5-8` (1 = first LED), `all` or `0` = every LED. Types may share LEDs. Empty = automatic: with 5+ LEDs each type gets its own LED (approval the 6th when there are 6+), otherwise all strip LEDs flash together in the type's colour |
| `count` | 0 | Number of strip LEDs (0-65535, 0 = no strip). **Wiring** |
| `pin` | 4 | Strip data GPIO (0-7 or 10). **Wiring** |
| `hist` | off | History: every flash enters at LED 1 and runs along the strip |
| `meter` | 0 | Usage bar on the strip: 0 off, 1 five-hour limit, 2 weekly limit (dim green/yellow/red, needs 2+ LEDs) |

Status dots and the glow go to the onboard LED, or to the strip's first LED when the onboard LED is off.

## Onboard LED (GPIO8)

| Key | Default | Meaning |
|---|---|---|
| `onb` | 0 | Mode: 0 mirror the selected functions, 1 status + approval only, 2 off |
| `onbt` | 0 | Type: 0 RGB WS2812B (SuperMini Plus), 1 plain LED lit on LOW (blue LED of the plain SuperMini), 2 plain LED lit on HIGH. A plain LED shows colours as brightness. **Wiring** |
| `onbbr` | 100 | Onboard brightness in percent (1-100) |
| `onbm0`..`onbm5` / `mirror.<type>` | all on | Which functions the onboard LED mirrors in mode 0 |
| `onbsig` | on | Onboard LED shows the finished / error signal |
| `onbuse` | off | Idle colour follows the 5-hour usage (green < 60 %, yellow < 85 %, red above) |

## Buzzer (off by default)

| Key | Default | Meaning |
|---|---|---|
| `bzpin` | -1 | Buzzer GPIO (0-7 or 10, not the strip's pin; -1 = none). **Wiring** |
| `bzt` | 0 | 0 active buzzer (beeps by itself), 1 passive (needs a tone). **Wiring** |
| `bzhz` | 2700 | Tone of a passive buzzer, 100-10000 Hz |
| `bzap` | 1 | Approval wait: 0 silent, 1 one beep (one more when the reminder starts), 2 beep every second until answered |
| `bzfl` | off | Short click with every flash |

The buzzer is silent at night and in quiet mode (short press on BOOT). Use a module with a transistor, or keep a
bare buzzer under ~20 mA.

## Night and time

| Key | Default | Meaning |
|---|---|---|
| `night` | off | Dim between two hours |
| `nfrom`, `nto` | 22, 7 | Start and end hour (0-23); the span may cross midnight |
| `nbr` | 5 | Night brightness (0 = dark) |
| `tz` | `CET-1CEST,M3.5.0,M10.5.0/3` | POSIX time zone string, used with NTP |

Common `tz` values: UTC `UTC0`; UK `GMT0BST,M3.5.0/1,M10.5.0`; Central Europe `CET-1CEST,M3.5.0,M10.5.0/3`;
Eastern Europe `EET-2EEST,M3.5.0/3,M10.5.0/4`; US Eastern `EST5EDT,M3.2.0,M11.1.0`; US Central
`CST6CDT,M3.2.0,M11.1.0`; US Mountain `MST7MDT,M3.2.0,M11.1.0`; US Pacific `PST8PDT,M3.2.0,M11.1.0`; India
`IST-5:30`; Japan `JST-9`; Sydney `AEST-10AEDT,M10.1.0,M4.1.0/3`. Quote the value in the shell because of the commas.

## MQTT / Home Assistant (off by default)

| Key | Default | Meaning |
|---|---|---|
| `mq` | off | Publish the state over MQTT (runs in its own task; a missing broker never delays the LED) |
| `mqhost`, `mqport` | empty, 1883 | Broker |
| `mquser`, `mqpass` | empty | Login. `mqpass` is write-only: never shown or exported, an empty value keeps the old one |
| `mqtopic` | `tokenled` | Topic prefix: `<prefix>/state` (idle, working, approval, done, error), `<prefix>/usage5`, `<prefix>/usage7`, plus Home Assistant discovery |

## Not settings

WiFi is changed on the System page (http://tokenled.local/system), in AP mode, or over USB with
`token_blinker.py --wifi`. Firmware updates: System page → upload `firmware.bin`, or the web installer again.
The System page also downloads and restores a settings backup (`/config.json`, without passwords).
