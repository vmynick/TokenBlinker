# TokenBlinker troubleshooting

Run `tokenled.py doctor` first and fix its FAIL lines; then find the symptom here.

## Claude does not start in VS Code (since the wrapper was set)

Remove the `claudeCode.claudeProcessWrapper` line from the user settings.json and start a new Claude session. That
restores Claude at once. Then find out why:

- The path no longer exists: the file was moved, deleted, or lived in a repository or download folder.
  `tokenled.py install` puts it in `~/.tokenblinker/`, a place nothing else touches.
- macOS / Linux: the file is not executable (`chmod +x`), or its `#!` interpreter is gone (Python upgraded or
  removed, a virtualenv deleted). `tokenled.py install` writes the absolute path of a base Python into the `#!`
  line, because VS Code started from the Dock does not have the shell's PATH.
- Windows: an antivirus quarantined `token_tap.exe`, or a downloaded copy is blocked as coming from the internet.
  The locally compiled one from `install` avoids the second.
- Run `tokenled.py selftest <path>`. Put the line back only after it passes.

## Claude works but the LED never flashes

1. **A new session?** The setting only applies to Claude sessions started after it was added.
2. **Is the wrapper active?** `doctor` shows the setting. If it is missing, see Setup in SKILL.md. Terminal
   `claude` never uses the wrapper: run `token_blinker.py` for terminal sessions.
3. **Is the LED on the network?** Watch it for 3 s:
   - a dim **red** dot every 3 s: it cannot join the WiFi (wrong password, or a 5 GHz-only network: the ESP32-C3
     only does 2.4 GHz). After 30 s it opens its setup network.
   - a dim **yellow** dot: the setup network `TokenLED-xxxx` is open. Join it and pick the WiFi. If the saved
     network is only temporarily away, it retries every 3 minutes while nobody is connected to the setup network.
   - red-green-blue at power-on only: the firmware runs.
4. **Name does not resolve** (`tokenled.local`): mDNS is blocked or flaky (guest WiFi, client isolation, some
   mesh routers, VPNs). Use the IP: find it in the router, or over USB (`token_blinker.py --wifi` prints it after
   connecting). Reserve that IP in the router's DHCP settings. Then set `TOKENLED_HOST=<ip>` where VS Code will
   see it:
   - Windows: `setx TOKENLED_HOST 192.168.1.50`, then quit VS Code completely and start it again.
   - macOS: `launchctl setenv TOKENLED_HOST 192.168.1.50` (until reboot; add it to a login item for permanence),
     or start VS Code from a shell that exports it.
   - Linux: export it in `~/.profile` and log in again.
   A first mDNS lookup can take several seconds on Windows. That is normal, and the launcher keeps the
   address once found.
5. **Web page works, flashes do not**: UDP 4210 from the computer to the LED is blocked (firewall, VLAN, guest
   network). `tokenled.py test` says so when nothing sent over UDP arrives.
6. **Settings make it look dead** (`doctor` warns): onboard LED off (`onb=2`) with no strip (`count=0`), very low
   `bright`, night mode with `nbr=0`, or tokens per flash set to 0 (`out`, `in`).

## It flashes, but…

- **Only after each response, not live**: that is the terminal mode (`token_blinker.py` reads the logs). Live
  flashing needs the VS Code extension and the wrapper.
- **Lags behind**: power-saving WiFi; `tokenled.py set saver=off` (needs USB power, not battery).
- **Far fewer flashes than tokens / nothing for a long thinking block**: the backlog is capped at about 1 s so
  the LED stays live; excess flashes are dropped on purpose. Raise `out` to make each flash mean more.
- **Thinking shows little**: thinking usually arrives redacted; the launcher counts its `estimated_tokens` and
  tops up with the real count at the end of each message.
- **Approval blink missing**: only the VS Code wrapper sees approval requests, not terminal sessions. It also
  needs the white colour (`c5`) not set to black and, on the strip, an LED for approval (`g5`, or 6+ LEDs).
- **Approval blink stuck**: it cannot get stuck for long. The LED forgets it 5 s after the last refresh, and the
  answer (`control_response`) ends it at once. With several sessions, answering one clears it for all.
- **No usage numbers or usage bar**: they come from Claude's `rate_limit_event`, which not every account or
  session sends. Values older than 30 minutes are hidden. The bar also needs `meter=1` or `2` and 2+ strip LEDs.
- **Wrong night hours**: set `tz` to the user's POSIX time zone (see settings.md); the LED gets time from NTP.

## Flashing the firmware

- The web installer needs Chrome or Edge on a desktop (Web Serial), and a USB *data* cable: many USB-C cables
  only charge.
- If the board is not offered: hold BOOT while plugging it in, then try again.
- With the repository: `pio run -t upload`. The PlatformIO serial monitor and `token_blinker.py --usb` cannot
  hold the port at the same time as the upload.

## Updating

- Launcher: run `tokenled.py install` again. It is safe while sessions run: on Windows the in-use exe is renamed
  and deleted later. New sessions get the new launcher.
- Firmware: System page → *Firmware update* with `firmware.bin` keeps WiFi and settings. The web installer works
  too; if it offers to erase the device, erasing also wipes WiFi and settings (save a backup on the System page first).
