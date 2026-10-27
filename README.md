# Claude token LED

Asztali LED, ami villog, amikor Claude tokent generál.

- **claude.ai** → narancs LED
- **Claude Code** → cián (szöveg), lila (thinking), zöld (tool input)

Egyik rész sem hívja a modellt, tehát nem fogyaszt tokent: csak a már futó forgalmat figyeli.

## Fájlok

| Fájl | Mi ez |
|---|---|
| `token_tap.cs` → `token_tap.exe` | **Élő villogás:** indító a VS Code Claude-bővítményhez; a Claude kimenetéből élőben küldi a tokeneket a LED-nek. |
| `token_blinker.py` | WiFi-beállítás USB-n, színteszt, és napló alapú számolás a terminálos Claude Code-hoz. |
| `src/main.cpp` + `platformio.ini` | Firmware: ESP32-C3 SuperMini Plus, beépített WS2812B (GPIO8), WiFi + USB + webes felület. |
| `token_led_test.py` | PC-s LED-emulátor ablak + Claude Code proxy + továbbítás az ESP32-re. Csak stdlib, nem módosít semmit a gépen. |
| `token_led.ino` | WiFi-s ESP32-C3 firmware, 2× WS2812 (vagy 1 LED keverve). |
| `claude-token-led.user.js` | Tampermonkey userscript a claude.ai-hoz. |

## Élő villogás (ESP32-C3 SuperMini Plus, WiFi, elemről is)

```
VS Code Claude-bővítmény ──> token_tap.exe ──> claude.exe ──HTTPS──> api.anthropic.com   (változatlan)
                                  │  a claude.exe kimenetét bájtra pontosan továbbadja,
                                  │  közben a stream_event sorokból számol
                                  └──UDP "T t 12"──> ESP32 (tokenled.local:4210) ──> villanások
```

Nincs proxy és nincs háttérben futó program, az API-forgalom érintetlen. Ha a LED ki van kapcsolva vagy nincs hálózat, a Claude ugyanúgy működik, csak nem villog. A bővítmény a `claudeCode.claudeProcessWrapper` beállítással indítja a `token_tap.exe`-t (VS Code felhasználói `settings.json`); kikapcsolás: ezt a sort törölni, majd új Claude-munkamenet.

A szöveg/thinking/tool tokeneit generálás közben becsüli (4 karakter ≈ 1 token), az üzenet végén a valódi `output_tokens` szerint pótolja a különbséget; a bemenet és a cache a kérés elején, pontosan megy.

| Szín | Mi | Egy villanás (alap) |
|---|---|---|
| cián | szöveg | 10 token |
| lila | thinking | 10 token |
| zöld | tool input | 10 token |
| narancs | új bemenet (`input` + `cache_creation`) | 100 token |
| sötétkék | cache olvasás | ki |

**Webes felület: http://tokenled.local**: token/villanás típusonként (0 = ki), fényerő, halvány derengés amíg a Claude fut (alapból ki), energiatakarékos WiFi (elemhez; kikapcsolva gyorsabb), színteszt, állapot és tokenszámok bekapcsolás óta, WiFi-hálózat módosítása.

**Rákötött LED-ek:** a webes felületen állítható a LED-ek száma (1-től 65 535-ig) és az adatláb (alap: GPIO8, a beépített LED). Külső WS2812 szalagot másik lábra (pl. GPIO4) kösd, a GND közös legyen, az 5 V a szalag saját tápjáról menjen. Egy villanás minden LED-et felgyújt a típus színével; az állapotjelző pötty és a derengés csak az elsőt. Sok LED sok áramot visz (kb. 60 mA/LED fehéren): szalagnál vedd lejjebb a fényerőt. Minden kirajzolás ~30 µs/LED ideig blokkol, ezért több ezer LED-nél a villanások lassulnak.

**WiFi beállítása (AP mód):** ha nincs mentett hálózat, vagy 30 s alatt nem tud csatlakozni, a LED saját, nyílt hálózatot indít: `TokenLED-xxxx` (a MAC utolsó 4 jegye). Telefonnal rácsatlakozva feljön a beállító oldal (ha nem: http://192.168.4.1), ott kiválasztható a hálózat és megadható a jelszó. Sikeres csatlakozás után az AP eltűnik. Ha a mentett hálózat csak átmenetileg nem elérhető, 3 percenként újrapróbálja, amíg senki nincs az AP-n. USB-n ugyanez: `python token_blinker.py --wifi`.

A maradék nem vész el, átvivődik. Ha sok villanás vár, a LED gyorsít, 300 fölött eldobja a többit. 3 s-onként halvány pötty: piros = WiFi-re csatlakozik, sárga = beállító AP fut; bekapcsoláskor piros-zöld-kék teszt.

```powershell
pio run -t upload                 # firmware (COM9, platformio.ini)
python token_blinker.py --wifi    # USB-n, az AP mód helyett: bekéri a WiFi-nevet és a jelszót, az ESP flash-be menti
python token_blinker.py --test 3  # színteszt
C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe /nologo /optimize /r:System.Web.Extensions.dll token_tap.cs
```

A `token_tap.exe` újrafordítása csak akkor megy, ha egy Claude-munkamenet sem használja (fájlzár). Ha a projektmappa elköltözik, a VS Code-beállítás útvonalát is át kell írni, különben a Claude nem indul.

Terminálos Claude Code-hoz: `python token_blinker.py` (naplóból, válaszonként; a VS Code-os munkameneteket kihagyja, hogy ne számoljon duplán).

Elemről: modem sleep és 80 MHz CPU a kisebb fogyasztásért (néhány száz ms késés). A panelen nincs töltő, a 3,3–6 V-os VCC/5V lábra mehet a táp; a WS2812B kikapcsolva is fogyaszt kb. 1 mA-t.

Protokoll (UDP 4210 és USB CDC, soronként, egy UDP-csomagban több sor is lehet): `T <t|k|j|i|r> <token>` = nyers tokenek, `B <típus> <db>` = ennyi villanás, `P` = életjel; csak USB-n: `W <ssid>\t<jelszó>`, `?` → `IP <cím>` / `NOWIFI`.

## Régi, proxys változat

### Architektúra

```
Claude Code ──HTTP──> token_led_test.py (127.0.0.1:8787) ──HTTPS──> api.anthropic.com
                           │  SSE content_block_delta figyelés
                           ├──> emulátor ablak
                           └──UDP "C t 12"──> ESP32 (tokenled.local:4210)

claude.ai (böngésző) ── Tampermonkey userscript ──HTTP GET /p?s=w&t=t&n=12──> ESP32 :80
```

## Protokoll (ESP32 és emulátor egyaránt)

- **UDP 4210:** `"<forrás> <típus> <n>"`
  - forrás: `W` = claude.ai, `C` = Claude Code
  - típus: `t` = szöveg, `k` = thinking, `j` = tool input
  - n: becsült tokenszám az adott ablakban
- **HTTP:** `GET /p?s=w&t=t&n=12` (CORS: `*`)

## LED-logika

- Proxy 50 ms-onként, userscript 100 ms-onként küld egy csomagot (az ablakban gyűlt tokenekkel).
- Minden csomag: `energia += 40 + n*8` (max 255), 10 ms-onként `×0.90` csillapítás.
- Tokenbecslés: 4 karakter ≈ 1 token. Nem streamelt JSON válasznál a `usage.output_tokens` pontos értéke.

## Használat

### Teszt a PC-n (nem állít át semmit)

```bash
python token_led_test.py --demo     # szimulált forgalom
python token_led_test.py            # proxy mód
```

Proxy módban, egy másik terminálban (csak arra a munkamenetre érvényes):

```powershell
$env:ANTHROPIC_BASE_URL="http://127.0.0.1:8787"; claude
```

Kézi teszt: `http://127.0.0.1:8080/p?s=w&n=20`

### ESP32

1. Arduino IDE / PlatformIO, könyvtár: **Adafruit NeoPixel**.
2. `token_led.ino`: `WIFI_SSID`, `WIFI_PASS`, `LED_PIN`, `NUM_LEDS` beállítása.
3. Feltöltés után: `http://tokenled.local/p?s=w&n=20`
4. `token_led_test.py`-ban `ESP_HOST` = `tokenled.local` vagy az ESP IP-je.

Bekapcsoláskor csatlakozás közben az első LED pirosan villog.

### claude.ai

1. Tampermonkey bővítmény telepítése.
2. Új szkript → `claude-token-led.user.js` tartalma.
3. `LED` konstans: `http://tokenled.local` (vagy IP, vagy `http://127.0.0.1:8080` az emulátorhoz).

A szkript a `[data-is-streaming="true"]` elemet figyeli; ha a claude.ai UI változik, ezt kell igazítani.

### Állandó Claude Code beállítás (opcionális)

`~/.claude/settings.json`:

```json
{ "env": { "ANTHROPIC_BASE_URL": "http://127.0.0.1:8787" } }
```

Figyelem: ha a proxy nem fut, a Claude Code nem működik.

## Ötletek / TODO

- Proxy futtatása szolgáltatásként (home lab), `HOST=0.0.0.0` több géphez.
- Pontos, N tokenenkénti éles villanás mód.
- Fallback: ha a proxy nem érhető el, közvetlen továbbítás (Claude Code ne álljon le).
- 3D nyomtatott ház, diffúzor.
