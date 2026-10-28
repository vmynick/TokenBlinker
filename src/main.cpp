// Claude token LED — ESP32-C3 SuperMini Plus: onboard WS2812B (GPIO8) plus an optional external WS2812 strip
// on its own data pin (default GPIO4), so the two can be used together or separately.
// The PC sends raw token counts, the ESP turns them into flashes. Settings: http://tokenled.local
// Without WiFi (nothing saved, or no connection within 30 s) a setup AP "TokenLED-xxxx" starts:
// open network, phones open the setup page by themselves (http://192.168.4.1).
// Input: UDP 4210 or USB serial, one command per line (several lines per UDP packet are fine):
//   "T <type> <tokens>"   type: t=text, k=thinking, j=tool input, i=new input, r=cache read
//   "B <type> <count>"    flash <count> times directly (test)
//   "A 1" / "A 0"         Claude waits for / got the user's approval (repeat "A 1" every ~1 s)
//   "D 0" / "D 1"        Claude finished a turn / finished with an error (green / red signal)
//   "U <5h%> <7d%> [<5h reset> <7d reset>]"  usage limits in percent (-1 = unknown), shown as a bar on the strip;
//                         optional reset times (Unix time, 0 = unknown), shown on the Live page
//   "P"                   keep-alive (shown on the web page; dim glow if enabled)
//   "W <ssid>\t<pass>"    set WiFi (USB only), saved to flash; reply: "IP <address>"
//   "?"                   status (USB only): "IP <address>" or "NOWIFI"
// LED: red-green-blue test at power-on; a dim dot every 3 s = connecting (red) / setup AP running (yellow).
// Onboard LED: mirrors everything (default), shows status/approval only, or is off. The strip shows the flashes.
// It is an RGB WS2812B (default) or a plain LED (lit on LOW or HIGH) that shows every colour as a brightness.
// Optional buzzer (active or passive) on its own GPIO: beeps for an approval wait and/or clicks with every flash.
// Web pages: Live (http://tokenled.local), Settings, System (WiFi, firmware update over WiFi).
// Optional: history strip, usage meter, night dimming (NTP time), 7-day totals (kept in flash), MQTT / Home Assistant.
// Libraries: Adafruit NeoPixel, PubSubClient

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <Update.h>
#include <time.h>
#include <map>
#include <functional>
#include <PubSubClient.h>
#include <Adafruit_NeoPixel.h>

#define LED_PIN     8        // onboard LED (fixed): WS2812B or a plain LED
#define OB_PWM_CH   0        // LEDC channel that dims a plain onboard LED
#define BZ_CH       2        // LEDC channel of a passive buzzer (own timer, so its tone does not change the LED PWM)
#define STRIP_PIN   4        // default data pin of the external strip; pin and LED count are set on the web page
#define UDP_PORT    4210
#define HOSTNAME    "tokenled"   // http://tokenled.local
#define QUEUE_LEN   32
#define MAX_PENDING 50      // more queued flashes than this are dropped so the LED never lags by more than about a second
#define ALIVE_MS    5000    // how long the last message counts as "Claude is running"
#define WAIT_MS     5000    // approval wait expires this long after the last "A 1"
#define AP_AFTER_MS 30000   // failed-connection time before the setup AP starts
#define AP_RETRY_MS 180000  // in AP mode, retry the saved WiFi after this long if nobody joined the AP
#define WORK_MS     4000    // tokens within this time = "working" (MQTT state)
#define USE_MS      1800000 // usage values older than this are not shown
#define HIST_MAX    300     // history strip: LEDs used
#define HIST_MS     150     // history strip: one step
#define STATS_MS    300000  // daily totals are saved to flash this often (when changed)
#define BTN_PIN     9       // BOOT button: short = quiet mode, long (1.5 s) = reset counters
#define COL_OK      0x00FF00
#define COL_ERR     0xFF0000

Adafruit_NeoPixel ob(1, LED_PIN, NEO_GRB + NEO_KHZ800);        // onboard LED
Adafruit_NeoPixel px(0, STRIP_PIN, NEO_GRB + NEO_KHZ800);      // external strip (none until a count is set)
WiFiUDP udp;
WebServer http(80);
DNSServer dns;
Preferences prefs;
bool netUp = false, mdnsStarted = false, apOn = false;
uint32_t staSince = 0, apSince = 0, reconnectAt = 0;
String savedSsid, savedPass, apName, ssidOptions;

struct Item { char type; uint16_t n; };
Item q[QUEUE_LEN];
uint8_t qHead = 0, qCount = 0;
uint16_t pending = 0;
uint32_t lastMsg = 0, lastTok = 0, waitUntil = 0, approvals = 0;
uint32_t sigUntil = 0, doneOk = 0, doneErr = 0;
bool sigPending = false, sigErr = false;
uint8_t lastDone = 0;                    // 0 none since the last tokens, 1 finished, 2 error
int use5 = -1, use7 = -1;                // usage limits in percent
uint32_t reset5 = 0, reset7 = 0;         // when those windows reset (Unix time, 0 = unknown)
uint32_t useAt = 0;
uint32_t hist[HIST_MAX], hcur = 0;       // history strip
uint8_t appliedB = 0;
volatile bool mqOk = false, mqReconf = false;
uint32_t waitStart = 0;                  // when the current approval wait began
bool quiet = false;                      // BOOT button: night brightness until pressed again
uint32_t hours[24], hourId = 0;          // tokens (output + new input) per hour, [0] = this hour
SemaphoreHandle_t cfgLock;               // settings strings are shared with the MQTT task

enum { IDLE, ON, OFF } state = IDLE;
uint32_t until = 0;
char curType = 0;
uint32_t shown = 1;   // last colour written; 1 = nothing yet
int shownT = -9, shownFn = -9;      // last strip target written
bool shownF = false;  // last write was a token flash

const char TYPES[] = "tkjir";
const char* NAMES[] = {"Text", "Thinking", "Tool", "New input", "Cache", "Approval"};
const uint32_t DEF_COL[] = {0x00B4FF, 0x9600FF, 0x00FF3C, 0xFF5A00, 0x000078, 0xFFFFFF};   // cyan, purple, green, orange, dark blue, white
uint32_t rem[5], total[5], flashes[5];

// ---------- Settings (web page, flash) ----------
struct Cfg {
  uint16_t out, in, cache, count;
  uint8_t bright, pin, onb;   // pin: strip data pin; onb: onboard LED 0 = mirror, 1 = status only, 2 = off
  bool glow, saver;
  bool doneSig, hist, night, mq, onbSig, onbUse;   // onbSig: finished/error on the onboard LED; onbUse: idle colour shows usage
  uint8_t onbMask, onbBr;   // onboard LED: shown functions (bit 0-4 types, 5 approval), brightness in percent
  uint8_t onbT;             // onboard LED type: 0 RGB (WS2812B), 1 plain LED lit on LOW, 2 plain LED lit on HIGH
  int8_t bzPin;             // buzzer GPIO, -1 = none
  uint8_t bzT, bzAp;        // buzzer: 0 active, 1 passive; approval wait: 0 silent, 1 one beep, 2 beep until answered
  bool bzFlash;             // buzzer clicks with every flash
  uint16_t bzHz;            // tone of a passive buzzer
  uint8_t meter, nFrom, nTo, nBright;   // meter: 0 off, 1 five-hour, 2 weekly; night hours 0-23
  uint16_t mqPort, remind;   // remind: seconds of pending approval until the blink speeds up (0 = off)
  String tz, mqHost, mqUser, mqPass, mqTopic;
  uint32_t col[6];   // colour per type (+ approval), 0xRRGGBB
  String list[6];    // strip LEDs per type (+ approval), as typed: "1,3,5-8", "all", empty = auto
} cfg;

// Parsed LED list of one function. Empty list = automatic: with 5+ strip LEDs every type has its own LED
// (approval: the 6th, needs 6+), otherwise all strip LEDs.
struct Sel { bool autoSel = true, all = false; uint8_t n = 0; uint16_t lo[8], hi[8]; } sel[6];

void parseSel(const String& str, Sel& s) {
  s = Sel();
  String t = str;
  t.trim();
  t.toLowerCase();
  if (!t.length()) return;
  s.autoSel = false;
  const char* p = t.c_str();
  while (*p && s.n < 8) {
    while (*p && !isdigit((unsigned char)*p) && *p != 'a') p++;
    if (!*p) break;
    if (*p == 'a') { s.all = true; return; }          // "all"
    long a = strtol(p, (char**)&p, 10), b = a;
    while (*p == ' ') p++;
    if (*p == '-') { p++; b = strtol(p, (char**)&p, 10); }   // range "5-8"
    if (a == 0) { s.all = true; return; }
    if (b < a) b = a;
    s.lo[s.n] = min(a, 65535L);
    s.hi[s.n] = min(b, 65535L);
    s.n++;
  }
  if (!s.n) s.autoSel = true;   // nothing valid typed
}

// Allowed strip pins: not the onboard LED (8), boot (9), USB (18, 19) or UART (20, 21) pins.
bool pinOk(int p) { return (p >= 0 && p <= 7) || p == 10; }

void loadCfg() {
  prefs.begin("cfg", true);
  cfg.count  = prefs.getUShort("scount", 0);
  cfg.pin    = prefs.getUChar("spin", STRIP_PIN);
  cfg.onb    = prefs.getUChar("onb", 0);
  cfg.out    = prefs.getUShort("out", 10);
  cfg.in     = prefs.getUShort("in", 100);
  cfg.cache  = prefs.getUShort("cache", 0);
  cfg.bright = prefs.getUChar("bright", 120);
  cfg.glow   = prefs.getBool("glow", false);
  cfg.saver  = prefs.getBool("saver", true);
  cfg.doneSig = prefs.getBool("done", true);
  cfg.hist   = prefs.getBool("hist", false);
  cfg.meter  = prefs.getUChar("meter", 0);
  cfg.night  = prefs.getBool("night", false);
  cfg.nFrom  = prefs.getUChar("nfrom", 22);
  cfg.nTo    = prefs.getUChar("nto", 7);
  cfg.nBright = prefs.getUChar("nbr", 5);
  cfg.tz     = prefs.getString("tz", "CET-1CEST,M3.5.0,M10.5.0/3");
  cfg.remind = prefs.getUShort("remind", 45);
  cfg.onbMask = prefs.getUChar("onbmask", 0x3F);
  cfg.onbBr  = prefs.getUChar("onbbr", 100);
  cfg.onbT   = prefs.getUChar("onbt", 0);
  cfg.bzPin  = prefs.getChar("bzpin", -1);
  cfg.bzT    = prefs.getUChar("bzt", 0);
  cfg.bzAp   = prefs.getUChar("bzap", 1);
  cfg.bzFlash = prefs.getBool("bzfl", false);
  cfg.bzHz   = prefs.getUShort("bzhz", 2700);
  cfg.onbSig = prefs.getBool("onbsig", true);
  cfg.onbUse = prefs.getBool("onbuse", false);
  cfg.mq     = prefs.getBool("mq", false);
  cfg.mqHost = prefs.getString("mqhost", "");
  cfg.mqPort = prefs.getUShort("mqport", 1883);
  cfg.mqUser = prefs.getString("mquser", "");
  cfg.mqPass = prefs.getString("mqpass", "");
  cfg.mqTopic = prefs.getString("mqtopic", "tokenled");
  for (int i = 0; i < 6; i++) {
    cfg.col[i]  = prefs.getUInt((String("c") + i).c_str(), DEF_COL[i]);
    cfg.list[i] = prefs.getString((String("l") + i).c_str(), "");
    parseSel(cfg.list[i], sel[i]);
  }
  prefs.end();
}

void saveCfg() {
  prefs.begin("cfg", false);
  prefs.putUShort("scount", cfg.count);
  prefs.putUChar("spin", cfg.pin);
  prefs.putUChar("onb", cfg.onb);
  prefs.putUShort("out", cfg.out);
  prefs.putUShort("in", cfg.in);
  prefs.putUShort("cache", cfg.cache);
  prefs.putUChar("bright", cfg.bright);
  prefs.putBool("glow", cfg.glow);
  prefs.putBool("saver", cfg.saver);
  prefs.putBool("done", cfg.doneSig);
  prefs.putBool("hist", cfg.hist);
  prefs.putUChar("meter", cfg.meter);
  prefs.putBool("night", cfg.night);
  prefs.putUChar("nfrom", cfg.nFrom);
  prefs.putUChar("nto", cfg.nTo);
  prefs.putUChar("nbr", cfg.nBright);
  prefs.putString("tz", cfg.tz);
  prefs.putUShort("remind", cfg.remind);
  prefs.putUChar("onbmask", cfg.onbMask);
  prefs.putUChar("onbbr", cfg.onbBr);
  prefs.putUChar("onbt", cfg.onbT);
  prefs.putChar("bzpin", cfg.bzPin);
  prefs.putUChar("bzt", cfg.bzT);
  prefs.putUChar("bzap", cfg.bzAp);
  prefs.putBool("bzfl", cfg.bzFlash);
  prefs.putUShort("bzhz", cfg.bzHz);
  prefs.putBool("onbsig", cfg.onbSig);
  prefs.putBool("onbuse", cfg.onbUse);
  prefs.putBool("mq", cfg.mq);
  prefs.putString("mqhost", cfg.mqHost);
  prefs.putUShort("mqport", cfg.mqPort);
  prefs.putString("mquser", cfg.mqUser);
  prefs.putString("mqpass", cfg.mqPass);
  prefs.putString("mqtopic", cfg.mqTopic);
  for (int i = 0; i < 6; i++) {
    prefs.putUInt((String("c") + i).c_str(), cfg.col[i]);
    prefs.putString((String("l") + i).c_str(), cfg.list[i]);
  }
  prefs.end();
}

// ---------- Time (NTP), night dimming, daily totals ----------
bool timeOk() { return time(nullptr) > 1700000000; }

bool nightNow() {
  if (!cfg.night || !timeOk() || cfg.nFrom == cfg.nTo) return false;
  time_t t = time(nullptr);
  struct tm tm;
  localtime_r(&t, &tm);
  return cfg.nFrom < cfg.nTo ? (tm.tm_hour >= cfg.nFrom && tm.tm_hour < cfg.nTo)
                             : (tm.tm_hour >= cfg.nFrom || tm.tm_hour < cfg.nTo);
}

uint8_t curBright() { return quiet || nightNow() ? cfg.nBright : cfg.bright; }

// ---------- Onboard LED: WS2812B, or a plain LED dimmed by PWM ----------
uint8_t obHw = 255;   // type GPIO8 is set up for (255 = not yet)
uint8_t obBr = 0;     // onboard brightness (0-255), for the plain LED

void obBegin() {   // (re)configures GPIO8 when the type changes
  if (obHw == cfg.onbT) return;
  if (obHw == 1 || obHw == 2) ledcDetachPin(LED_PIN);
  obHw = cfg.onbT;
  if (obHw == 0) ob.begin();
  else { ledcSetup(OB_PWM_CH, 5000, 8); ledcAttachPin(LED_PIN, OB_PWM_CH); }
}

void obSet(uint32_t c) {   // a plain LED shows the brightest channel of the colour
  if (cfg.onbT == 0) { ob.setPixelColor(0, c); ob.show(); return; }
  uint32_t v = max(max(c >> 16 & 255, c >> 8 & 255), c & 255) * obBr / 255;
  ledcWrite(OB_PWM_CH, cfg.onbT == 1 ? 255 - v : v);
}

void applyBright() {
  uint8_t b = curBright();
  if (b == appliedB) return;
  appliedB = b;
  px.setBrightness(b);
  obBr = (uint32_t)b * cfg.onbBr / 100;
  ob.setBrightness(obBr);
  shown = 1;   // new brightness: force the next colour to be written
}

// ---------- Buzzer ----------
int8_t bzHw = -1;    // pin the buzzer is set up on (-1 = none)
bool bzHwP = false;  // set up as passive
uint32_t bzOff = 0;  // when the current beep ends (0 = silent)

void bzSound(bool on) {
  if (bzHw < 0) return;
  if (bzHwP) ledcWrite(BZ_CH, on ? 512 : 0);   // 50 % duty of the 10-bit tone
  else digitalWrite(bzHw, on ? HIGH : LOW);
}

void bzEnd() {   // releases the buzzer pin (before a pin change)
  if (bzHw < 0) return;
  bzSound(false);
  if (bzHwP) ledcDetachPin(bzHw);
  pinMode(bzHw, INPUT);
  bzHw = -1;
  bzOff = 0;
}

void bzBegin() {
  if (bzHw >= 0 || cfg.bzPin < 0) return;
  bzHw = cfg.bzPin;
  bzHwP = cfg.bzT == 1;
  if (bzHwP) { ledcSetup(BZ_CH, cfg.bzHz, 10); ledcAttachPin(bzHw, BZ_CH); }
  else pinMode(bzHw, OUTPUT);
  bzSound(false);
}

void bzBeep(uint16_t ms) {   // silent at night and in quiet mode
  if (bzHw < 0 || quiet || nightNow()) return;
  bzSound(true);
  bzOff = (millis() + ms) | 1;   // never 0
}

void applyCfg() {
  obBegin();
  bzBegin();
  appliedB = 0;
  applyBright();
  shown = 1;
  if (!apOn) WiFi.setSleep(cfg.saver);
}

void startNtp() { configTzTime(cfg.tz.c_str(), "pool.ntp.org", "time.google.com"); }

// Tokens per day (today first) for the last 7 days: out (text+thinking+tool), new input, cache read.
struct Day { uint32_t out, in, cache; };
#define DAYS 30
Day days[DAYS];   // days[0] = today
uint32_t dayId = 0;       // calendar day of days[0] (0 = clock not set yet)
bool statsDirty = false;
uint32_t statsAt = 0;

uint32_t civilDay(const struct tm& t) {   // days since 0000-03-01, continuous across months and years
  int y = t.tm_year + 1900, m = t.tm_mon + 1;
  y -= m <= 2;
  int era = y / 400;
  unsigned yoe = y - era * 400;
  unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + t.tm_mday - 1;
  return era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy;
}

void loadStats() {
  prefs.begin("stats", true);
  dayId = prefs.getUInt("day", 0);
  size_t dl = prefs.getBytesLength("days");
  if (dl == sizeof(days) || dl == 7 * sizeof(Day)) prefs.getBytes("days", days, dl);   // older firmware kept 7 days
  hourId = prefs.getUInt("hour", 0);
  if (prefs.getBytesLength("hours") == sizeof(hours)) prefs.getBytes("hours", hours, sizeof(hours));
  prefs.end();
}

void saveStats() {
  prefs.begin("stats", false);
  prefs.putUInt("day", dayId);
  prefs.putBytes("days", days, sizeof(days));
  prefs.putUInt("hour", hourId);
  prefs.putBytes("hours", hours, sizeof(hours));
  prefs.end();
  statsDirty = false;
  statsAt = millis();
}

void rollHour() {
  if (!timeOk()) return;
  uint32_t id = time(nullptr) / 3600;
  if (!hourId) { hourId = id; return; }
  if (id == hourId) return;
  uint32_t d = id > hourId ? id - hourId : 24;
  if (d >= 24) memset(hours, 0, sizeof(hours));
  else {
    memmove(hours + d, hours, (24 - d) * sizeof(hours[0]));
    memset(hours, 0, d * sizeof(hours[0]));
  }
  hourId = id;
  statsDirty = true;
}

void rollDay() {
  if (!timeOk()) return;
  time_t t = time(nullptr);
  struct tm tm;
  localtime_r(&t, &tm);
  uint32_t id = civilDay(tm);
  if (!dayId) { dayId = id; statsDirty = true; return; }
  if (id == dayId) return;
  uint32_t d = id > dayId ? id - dayId : DAYS;
  if (d >= DAYS) memset(days, 0, sizeof(days));
  else {
    memmove(days + d, days, (DAYS - d) * sizeof(Day));
    memset(days, 0, d * sizeof(Day));
  }
  dayId = id;
  saveStats();
}

// ---------- LED ----------
uint32_t scaleCol(uint32_t c, uint32_t k) {   // k/255 of the colour
  return (((c >> 16 & 255) * k / 255) << 16) | (((c >> 8 & 255) * k / 255) << 8) | ((c & 255) * k / 255);
}

bool histMode() { return cfg.hist && px.numPixels() >= 2; }

int meterValue() {   // usage in percent for the meter, -1 = off / unknown / stale
  if (!cfg.meter || !useAt || millis() - useAt > USE_MS) return -1;
  return constrain(cfg.meter == 1 ? use5 : use7, -1, 100);
}

// Background of the strip: the history (running light) or the usage bar, dim.
void drawBase() {
  px.clear();
  uint16_t n = min<uint16_t>(px.numPixels(), HIST_MAX);
  if (n < 2) return;
  if (cfg.hist) {
    for (uint16_t i = 0; i < n; i++)
      if (hist[i]) px.setPixelColor(i, scaleCol(hist[i], 255 - (uint32_t)i * 200 / n));
    return;
  }
  int u = meterValue();
  if (u < 0) return;
  uint32_t lit = (u * n + 50) / 100;
  if (u > 0 && !lit) lit = 1;
  uint32_t c = u < 60 ? px.Color(0, 30, 0) : u < 85 ? px.Color(35, 25, 0) : px.Color(40, 0, 0);
  for (uint32_t i = 0; i < lit; i++) px.setPixelColor(i, c);
}

void histTick() {
  static uint32_t at = 0;
  uint32_t now = millis();
  if (now - at < HIST_MS) return;
  at = now;
  uint16_t n = min<uint16_t>(px.numPixels(), HIST_MAX);
  if (!histMode()) return;
  bool any = hcur;
  for (uint16_t i = 0; i < n && !any; i++) any = hist[i];
  if (!any) return;
  memmove(hist + 1, hist, (n - 1) * sizeof(hist[0]));
  hist[0] = hcur;
  hcur = 0;
  shown = 1;
}

// Lights the strip LEDs that belong to function f in colour c on top of the background.
// f = -1: every LED, -3: background only (history mode: the flash goes into the history instead).
void drawStrip(uint32_t c, int f) {
  drawBase();
  if (c == 0 || f == -3) return;
  if (f < 0) { px.fill(c); return; }
  Sel& s = sel[f];
  if (s.autoSel) {
    if (cfg.count >= (f < 5 ? 5 : 6)) px.setPixelColor(f, c); else px.fill(c);
  } else if (s.all) {
    px.fill(c);
  } else {
    for (int r = 0; r < s.n; r++)
      for (uint32_t i = s.lo[r] - 1; i < s.hi[r] && i < cfg.count; i++) px.setPixelColor(i, c);
  }
}

// target: function 0-5 (type or approval: its LEDs on the strip), -1 = all strip LEDs,
//         -2 = status (first strip LED, only when the onboard LED is off), -3 = onboard LED only (history mode).
// flash: a token flash (the onboard LED may be set to show status only).
// fn: what is shown, for the onboard LED's filter: 0-4 type, 5 approval, 6 finished/error signal, -1 other.
void led(uint32_t c, int target = -1, bool flash = false, int fn = -1) {
  if (c == 0) target = -1;
  if (c == shown && target == shownT && flash == shownF && fn == shownFn) return;
  shownFn = fn;
  shown = c;
  shownT = target;
  shownF = flash;
  bool obOn = cfg.onb == 0 || (cfg.onb == 1 && !flash);
  if (obOn && cfg.onb == 0 && fn >= 0 && fn <= 5 && !(cfg.onbMask >> fn & 1)) obOn = false;   // this function is not shown on the onboard LED
  if (obOn && fn == 6 && !cfg.onbSig) obOn = false;
  obSet(obOn ? c : 0);
  if (!px.numPixels()) return;
  if (target == -2 && cfg.onb != 2) c = 0;
  if (target == -2) { drawBase(); if (c) px.setPixelColor(0, c); } else drawStrip(c, target);
  px.show();
}

// New strip length or data pin: switch the old strip off and resize.
void applyHw() {
  px.clear();
  px.show();
  px.setPin(cfg.pin);
  px.updateLength(cfg.count);
  if (cfg.count && !px.numPixels()) { cfg.count = 0; saveCfg(); }   // did not fit in memory
  px.setBrightness(appliedB);
  shown = 1;
}

uint32_t rgb(int i) {
  uint32_t v = cfg.col[i];
  return px.Color((v >> 16) & 255, (v >> 8) & 255, v & 255);
}

String hex(int i) {
  char b[8];
  snprintf(b, sizeof(b), "#%06x", (unsigned)(cfg.col[i] & 0xFFFFFF));
  return b;
}

uint32_t color(char type) { return rgb(type ? (strchr(TYPES, type) ? strchr(TYPES, type) - TYPES : 0) : 0); }

void enqueue(char type, long n) {
  n = min(n, (long)MAX_PENDING - pending);
  if (n <= 0) return;
  if (qCount) {
    Item& tail = q[(qHead + qCount - 1) % QUEUE_LEN];
    if (tail.type == type) { tail.n += n; pending += n; return; }
  }
  if (qCount == QUEUE_LEN) return;
  q[(qHead + qCount) % QUEUE_LEN] = {type, (uint16_t)n};
  qCount++;
  pending += n;
}

// ---------- Event log (RAM, newest first on the Live page) ----------
enum { EV_BOOT, EV_START, EV_APPROVAL, EV_ANSWERED, EV_DONE, EV_ERROR, EV_U5, EV_U7 };
struct Ev { uint8_t kind; uint16_t detail; uint32_t up; time_t wall; };
#define EV_N 30
Ev evs[EV_N];
uint8_t evHead = 0, evCount = 0;

void logEv(uint8_t kind, uint16_t detail = 0) {
  evs[evHead] = {kind, detail, millis() / 1000, timeOk() ? time(nullptr) : 0};
  evHead = (evHead + 1) % EV_N;
  if (evCount < EV_N) evCount++;
}

int useLevel(int v) { return v >= 100 ? 3 : v >= 85 ? 2 : v >= 60 ? 1 : 0; }

int typeIdx(char t) {
  const char* p = t ? strchr(TYPES, t) : nullptr;
  return p ? p - TYPES : 0;
}

uint16_t unitFor(char t) {
  return t == 'i' ? cfg.in : t == 'r' ? cfg.cache : cfg.out;
}

void addTokens(char type, long n) {
  if (n <= 0) return;
  int i = typeIdx(type);
  type = TYPES[i];
  total[i] += n;
  if (!lastTok || millis() - lastTok > 15000) logEv(EV_START);
  lastTok = millis();
  lastDone = 0;
  if (i != 4) hours[0] += n;
  (i == 3 ? days[0].in : i == 4 ? days[0].cache : days[0].out) += n;
  statsDirty = true;
  uint16_t u = unitFor(type);
  if (!u) return;
  rem[i] += n;
  enqueue(type, rem[i] / u);
  rem[i] %= u;
}

void setWaiting(bool on) {
  if (!on) {
    if (waitUntil && (int32_t)(millis() - waitUntil) < 0) logEv(EV_ANSWERED);
    waitUntil = 0;
    return;
  }
  if (!waitUntil || (int32_t)(millis() - waitUntil) >= 0) { approvals++; waitStart = millis(); logEv(EV_APPROVAL); }
  waitUntil = millis() + WAIT_MS;
}

// Plays the queued flashes; the more are waiting, the faster it goes.
void play() {
  uint32_t now = millis();
  if (waitUntil && (int32_t)(now - waitUntil) < 0) {      // Claude waits for approval: drop every other flash, white blink only
    qCount = 0;
    pending = 0;
    hcur = 0;
    state = IDLE;
    uint32_t per = cfg.remind && now - waitStart > cfg.remind * 1000UL ? 125 : 250;   // faster once it has waited a while
    if (now / per % 2 == 0) led(rgb(5), 5, false, 5);
    else led(0);
    return;
  }
  if (state != IDLE && (int32_t)(now - until) < 0) return;

  if (state == ON) {
    led(0);
    int off = constrain(30 - pending, 6, 30);
    if (!qCount || q[qHead].type != curType) off += 50;   // short pause when the type changes
    state = OFF; until = now + off;
    return;
  }

  if (!qCount) {
    state = IDLE;
    if (sigPending) {
      sigPending = false;
      sigUntil = now + (sigErr ? 2000 : 1500);
    }
    if (sigUntil && (int32_t)(now - sigUntil) < 0) {      // finished (solid green) / error (red blinking)
      if (!sigErr || now / 250 % 2 == 0) led(sigErr ? COL_ERR : COL_OK, -1, false, 6);
      else led(0);
      return;
    }
    sigUntil = 0;
    if (waitUntil && (int32_t)(now - waitUntil) < 0) {     // Claude waits for approval: white blink
      if (now / 500 % 2 == 0) led(rgb(5), 5, false, 5);
      else led(0);
      return;
    }
    uint32_t c = 0;
    bool dot = now % 3000 < 40;
    if (cfg.glow && lastMsg && now - lastMsg < ALIVE_MS) c = px.Color(0, 6, 10);
    else if (apOn && dot) c = px.Color(40, 25, 0);
    else if (!netUp && dot) c = px.Color(40, 0, 0);
    else if (cfg.onbUse && useAt && now - useAt < USE_MS && use5 >= 0)       // idle colour = 5-hour usage
      c = use5 < 60 ? px.Color(0, 8, 0) : use5 < 85 ? px.Color(12, 8, 0) : px.Color(16, 0, 0);
    led(c, -2);
    return;
  }

  Item& it = q[qHead];
  curType = it.type;
  bool hm = histMode();
  if (hm) hcur = color(curType);
  led(color(curType), hm ? -3 : typeIdx(curType), true, typeIdx(curType));
  if (cfg.bzFlash) bzBeep(5);   // a click
  flashes[typeIdx(curType)]++;
  if (--it.n == 0) { qHead = (qHead + 1) % QUEUE_LEN; qCount--; }
  pending--;
  state = ON; until = now + (pending > 15 ? 10 : 22);
}

// Ends a beep; approval wait: one beep at the start (and one more at the reminder), or a beep every second
// in step with the white blink (every half second after the reminder).
void buzzerTick() {
  static uint32_t started = 0, slot = 0;
  static bool reminded = false;
  uint32_t now = millis();
  if (bzOff && (int32_t)(now - bzOff) >= 0) { bzSound(false); bzOff = 0; }
  if (!cfg.bzAp || !waitUntil || (int32_t)(now - waitUntil) >= 0) return;
  bool late = cfg.remind && now - waitStart > cfg.remind * 1000UL;
  uint32_t s = now / (late ? 500 : 1000);
  if (waitStart != started) { started = waitStart; reminded = false; slot = s; bzBeep(120); return; }
  if (cfg.bzAp == 1) { if (late && !reminded) { reminded = true; bzBeep(120); } return; }
  if (s != slot) { slot = s; bzBeep(80); }
}

// ---------- BOOT button ----------
void resetCounters() {
  memset(total, 0, sizeof(total));
  memset(flashes, 0, sizeof(flashes));
  memset(rem, 0, sizeof(rem));
  approvals = doneOk = doneErr = 0;
  lastDone = 0;
}

void buttonTick() {
  static bool down = false, longDone = false;
  static uint32_t at = 0;
  bool p = digitalRead(BTN_PIN) == LOW;
  uint32_t now = millis();
  if (p && !down) { down = true; longDone = false; at = now; }
  else if (p && down && !longDone && now - at > 1500) {
    longDone = true;
    resetCounters();
    obSet(ob.Color(0, 0, 255)); delay(150); shown = 1;
  } else if (!p && down) {
    down = false;
    if (!longDone && now - at > 30) {
      quiet = !quiet;
      applyBright();
      obSet(quiet ? ob.Color(255, 160, 0) : ob.Color(0, 255, 0)); delay(150); shown = 1;
    }
  }
}

// ---------- Input ----------
void printStatus() {
  if (WiFi.status() == WL_CONNECTED) Serial.printf("IP %s\n", WiFi.localIP().toString().c_str());
  else Serial.println("NOWIFI");
}

void storeWifi(const String& ssid, const String& pass);

void handleLine(char* s, bool usb) {
  lastMsg = millis();
  char type; long n, m, r5 = 0, r7 = 0;
  if (sscanf(s, "T %c %ld", &type, &n) == 2) addTokens(type, n);
  else if (sscanf(s, "B %c %ld", &type, &n) == 2) enqueue(type, n);
  else if (sscanf(s, "A %ld", &n) == 1) setWaiting(n != 0);
  else if (sscanf(s, "D %ld", &n) == 1) {
    setWaiting(false);
    logEv(n ? EV_ERROR : EV_DONE);
    lastDone = n ? 2 : 1;
    (n ? doneErr : doneOk)++;
    if (cfg.doneSig) { sigErr = n != 0; sigPending = true; sigUntil = 0; }
  }
  else if (sscanf(s, "U %ld %ld %ld %ld", &n, &m, &r5, &r7) >= 2) {   // reset times are optional (older launchers)
    if (useLevel(n) > useLevel(use5)) logEv(EV_U5, n);
    if (useLevel(m) > useLevel(use7)) logEv(EV_U7, m);
    use5 = n; use7 = m; useAt = millis();
    reset5 = r5; reset7 = r7;
    shown = 1;
  }
  else if (usb && s[0] == 'W' && s[1] == ' ') {
    char* tab = strchr(s + 2, '\t');
    if (tab) { *tab = 0; storeWifi(s + 2, tab + 1); }
  }
  else if (usb && s[0] == '?') printStatus();
}

void readSerial() {
  static char buf[128];
  static uint8_t len = 0;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      buf[len] = 0;
      if (len) handleLine(buf, true);
      len = 0;
    } else if (len < sizeof(buf) - 1) {
      buf[len++] = c;
    }
  }
}

// ---------- WiFi: client, and a setup AP while there is no network ----------
String esc(String s) {
  s.replace("&", "&amp;");
  s.replace("<", "&lt;");
  s.replace(">", "&gt;");
  s.replace("\"", "&quot;");
  return s;
}

void staStart() {
  staSince = millis();
  WiFi.setAutoReconnect(true);
  if (savedSsid.length()) WiFi.begin(savedSsid.c_str(), savedPass.c_str());
}

void apStart() {
  WiFi.setAutoReconnect(false);   // while the AP runs the STA must not scan: channel hopping would drop the phone
  WiFi.disconnect();
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);
  int n = WiFi.scanNetworks();    // once, before anybody joins
  ssidOptions = "";
  for (int i = 0; i < n; i++) ssidOptions += "<option value=\"" + esc(WiFi.SSID(i)) + "\">";
  WiFi.scanDelete();
  WiFi.softAP(apName.c_str());
  dns.start(53, "*", WiFi.softAPIP());   // every name points here: the phone opens the setup page
  apOn = true;
  apSince = millis();
  Serial.printf("AP %s\n", apName.c_str());
}

void apStop() {
  dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(cfg.saver);
  apOn = false;
}

void storeWifi(const String& ssid, const String& pass) {
  prefs.begin("wifi", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();
  savedSsid = ssid;
  savedPass = pass;
  reconnectAt = millis() + 1000;   // let the web reply go out first, then switch
}

void network() {
  uint32_t now = millis();
  if (reconnectAt && (int32_t)(now - reconnectAt) >= 0) {   // new WiFi credentials
    reconnectAt = 0;
    if (apOn) apStop();
    WiFi.disconnect();
    staStart();
  }

  bool up = WiFi.status() == WL_CONNECTED;
  if (up && !netUp) {
    if (!mdnsStarted) {
      MDNS.begin(HOSTNAME);
      MDNS.addService("http", "tcp", 80);
      mdnsStarted = true;
    }
    if (apOn) apStop();
    startNtp();
    printStatus();
  }
  if (!up && netUp) staSince = now;   // connection lost: count from here
  netUp = up;

  if (!up && !reconnectAt) {
    if (!apOn && (!savedSsid.length() || now - staSince > AP_AFTER_MS)) {
      apStart();
    } else if (apOn && savedSsid.length() && now - apSince > AP_RETRY_MS && !WiFi.softAPgetStationNum()) {
      apStop();
      staStart();
    }
  }

  if (apOn) dns.processNextRequest();
  if (up) {
    char buf[160];
    while (udp.parsePacket() > 0) {
      int r = udp.read(buf, sizeof(buf) - 1);
      buf[r > 0 ? r : 0] = 0;
      char* save;
      for (char* line = strtok_r(buf, "\r\n", &save); line; line = strtok_r(nullptr, "\r\n", &save))
        handleLine(line, false);
    }
  }
  http.handleClient();
}

// ---------- Web pages: Live, Settings, System ----------
const char PAGE_HEAD[] PROGMEM = R"(<!doctype html><html lang=en><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1"><title>Token LED</title>
<style>body{font:15px system-ui,sans-serif;max-width:480px;margin:16px auto;padding:0 16px;background:#141414;color:#ddd}
h2{margin:8px 0 8px}h3{margin:24px 0 8px;font-size:15px;color:#999}a{color:#6bf}
nav{display:flex;gap:20px;margin:0 0 16px;border-bottom:1px solid #333}nav a{padding:6px 0;text-decoration:none;color:#999}
nav a.a{color:#fff;border-bottom:2px solid #6bf}
label{display:flex;justify-content:space-between;align-items:center;gap:12px;margin:10px 0}
input:not([type=checkbox]),select{width:90px;padding:4px;background:#222;color:#eee;border:1px solid #444;border-radius:4px}
input[name=ssid],input[name=pass],input.w{width:180px}
input.lst{width:110px}input[type=color]{width:44px;height:28px;padding:0}
button{padding:8px 16px;margin:12px 8px 0 0;background:#333;color:#eee;border:1px solid #555;border-radius:6px}
.d{display:inline-block;width:10px;height:10px;border-radius:50%;margin-right:3px}
.box{border:1px solid #a80;border-radius:8px;padding:4px 12px 12px;margin-bottom:16px}
table{width:100%;border-collapse:collapse}td,th{padding:3px 0}td+td,th+th{text-align:right;font-variant-numeric:tabular-nums}
th{color:#888;font-weight:normal}
small{color:#888}.ok{color:#6c6}
details.sec{border:1px solid #333;border-radius:8px;padding:0 12px;margin:10px 0;background:#181818}
details.sec>summary{cursor:pointer;padding:11px 0;font-weight:600;color:#ddd}
details.sec[open]>summary{border-bottom:1px solid #333;margin-bottom:6px}
details.sec>small:last-child{display:block;margin-bottom:10px}
.g{display:grid;grid-template-columns:repeat(6,1fr);gap:4px;text-align:center;margin:12px 0}
.l{width:30px;height:30px;border-radius:50%;margin:0 auto 6px;border:1px solid #333}
.g small{display:block}.g b{font-size:12px}
.bar{height:8px;background:#222;border-radius:4px;margin:4px 0 12px}.bar i{display:block;height:8px;border-radius:4px;background:#4a4;width:0}
</style><h2>Token LED</h2>)";

String nav(int cur) {
  const char* href[] = {"/", "/settings", "/system"};
  const char* name[] = {"Live", "Settings", "System"};
  String s = "<nav>";
  for (int i = 0; i < 3; i++) s += String("<a href=") + href[i] + (i == cur ? " class=a" : "") + ">" + name[i] + "</a>";
  return s + "</nav>";
}

String page(int cur) { return String(FPSTR(PAGE_HEAD)) + nav(cur); }

String dot(const String& c) { return "<i class=d style=background:" + c + "></i>"; }

String numField(const char* name, const String& label, long v, long lo, long hi) {
  return "<label><span>" + label + "</span><input type=number name=" + name + " min=" + lo + " max=" + hi +
         " value=" + v + "></label>";
}

String checkField(const char* name, const char* label, bool v) {
  return String("<label><span>") + label + "</span><input type=checkbox name=" + name + (v ? " checked" : "") + "></label>";
}

String textField(const char* name, const char* label, const String& v, const char* type = "text", const char* ph = "") {
  return String("<label><span>") + label + "</span><input class=w type=" + type + " name=" + name + " placeholder=\"" + ph +
         "\" value=\"" + esc(v) + "\"></label>";
}

String selField(const char* name, const char* label, int cur, std::initializer_list<const char*> opts) {
  String s = String("<label><span>") + label + "</span><select name=" + name + ">";
  int i = 0;
  for (const char* o : opts) { s += String("<option value=") + i + (i == cur ? " selected" : "") + ">" + o + "</option>"; i++; }
  return s + "</select></label>";
}

// Time zones for the drop-down: label and POSIX TZ string (the value that is stored and used).
struct Zone { const char* name; const char* tz; };
const Zone ZONES[] = {
  {"Honolulu", "HST10"}, {"Anchorage", "AKST9AKDT,M3.2.0,M11.1.0"}, {"Los Angeles / Vancouver", "PST8PDT,M3.2.0,M11.1.0"},
  {"Denver", "MST7MDT,M3.2.0,M11.1.0"}, {"Phoenix", "MST7"}, {"Chicago / Mexico City", "CST6CDT,M3.2.0,M11.1.0"},
  {"New York / Toronto", "EST5EDT,M3.2.0,M11.1.0"}, {"São Paulo / Buenos Aires", "<-03>3"},
  {"UTC", "UTC0"}, {"London / Dublin", "GMT0BST,M3.5.0/1,M10.5.0"}, {"Lisbon", "WET0WEST,M3.5.0/1,M10.5.0"},
  {"Budapest / Berlin / Paris / Rome / Vienna / Prague / Warsaw", "CET-1CEST,M3.5.0,M10.5.0/3"},
  {"Athens / Helsinki / Kyiv / Bucharest", "EET-2EEST,M3.5.0/3,M10.5.0/4"}, {"Istanbul / Moscow", "<+03>-3"},
  {"Dubai", "<+04>-4"}, {"Mumbai / Delhi", "IST-5:30"}, {"Bangkok / Jakarta", "<+07>-7"},
  {"Beijing / Singapore / Perth", "CST-8"}, {"Tokyo / Seoul", "JST-9"},
  {"Sydney / Melbourne", "AEST-10AEDT,M10.1.0,M4.1.0/3"}, {"Auckland", "NZST-12NZDT,M9.5.0,M4.1.0/3"},
};

String tzField(const String& cur) {
  String s = "<label><span>Time zone</span><select name=tz class=w style=width:190px>";
  bool known = false;
  for (const Zone& z : ZONES) {
    bool sel = cur == z.tz;
    known |= sel;
    s += String("<option value=\"") + esc(z.tz) + "\"" + (sel ? " selected" : "") + ">" + z.name + "</option>";
  }
  if (!known) s += "<option value=\"" + esc(cur) + "\" selected>Custom: " + esc(cur) + "</option>";
  return s + "</select></label>";
}

String wifiForm() {
  return "<form method=post action=/wifi><label><span>Network (2.4 GHz)</span><input name=ssid list=ssids required value=\"" +
         esc(savedSsid) + "\"></label><datalist id=ssids>" + ssidOptions + "</datalist>"
         "<label><span>Password</span><input type=password name=pass></label><button>Connect</button></form>";
}

String kfmt(uint32_t n) {
  if (n >= 1000000) return String(n / 1e6, 1) + "M";
  if (n >= 1000) return String(n / 1e3, 1) + "k";
  return String(n);
}

const char* stateName() {
  uint32_t now = millis();
  if (waitUntil && (int32_t)(now - waitUntil) < 0) return "approval";
  if (lastTok && now - lastTok < WORK_MS) return "working";
  if (lastDone) return lastDone == 2 ? "error" : "done";
  return "idle";
}

String evRows() {
  const char* text[] = {"Device started", "Claude started working", "Waiting for your approval", "Approval answered",
                        "Finished", "Finished with an error", "5-hour usage reached ", "Weekly usage reached "};
  String s;
  for (int i = 0; i < evCount; i++) {
    Ev& e = evs[(evHead + EV_N - 1 - i) % EV_N];
    String when;
    if (e.wall) {
      struct tm tm;
      localtime_r(&e.wall, &tm);
      char b[12];
      strftime(b, sizeof(b), "%H:%M:%S", &tm);
      when = b;
    } else {
      when = String("up ") + e.up / 60 + " min";
    }
    String what = text[e.kind];
    if (e.kind >= EV_U5) what += String(e.detail) + " %";
    s += "<tr><td>" + what + "<td>" + when + "</tr>";
  }
  return s;
}

void handleLog() {
  http.sendHeader("Cache-Control", "no-store");
  http.send(200, "text/html; charset=utf-8", evRows());
}

void handleRoot() {
  uint32_t ago = (millis() - lastMsg) / 1000;
  String s = page(0);
  if (apOn) s += "<div class=box><h3>WiFi setup</h3>" + wifiForm() + "</div>";

  // Virtual LEDs: one column per type, plus the approval signal
  s += "<div class=g>";
  for (int i = 0; i < 6; i++) {
    s += String("<div><div class=l onclick=\"fetch('/b?t=") + (i < 5 ? String(TYPES[i]) : String("a")) + "')\" id=l" + i + " style=background:" + hex(i) + ";opacity:.15></div><b>" + NAMES[i] +
         "</b><small id=n" + i + ">" + (i < 5 ? total[i] : approvals) + "</small><small id=f" + i + ">" +
         (i < 5 ? String(flashes[i]) + " flashes" : String("idle")) + "</small></div>";
  }
  s += "</div><small>Click a LED to test it.</small>";

  s += "<table><tr><td>State</td><td id=st>" + String(stateName()) + "</td></tr>"
       "<tr><td>Usage, 5 hours</td><td id=u5>-</td></tr></table><div class=bar><i id=b5></i></div><small id=r5></small>"
       "<table><tr><td>Usage, 7 days</td><td id=u7>-</td></tr></table><div class=bar><i id=b7></i></div><small id=r7></small>";

  s += "<h3>Last 24 hours</h3><div style='display:flex;align-items:flex-end;gap:2px;height:60px'>";
  uint32_t mx = 1;
  int peak = 0;
  for (int i = 0; i < 24; i++) if (hours[i] > mx) { mx = hours[i]; peak = i; }
  for (int i = 23; i >= 0; i--) {
    String lab = String("-") + i + " h";
    if (timeOk()) {
      time_t t = time(nullptr) - i * 3600L;
      struct tm tm;
      localtime_r(&t, &tm);
      char b[8];
      strftime(b, sizeof(b), "%H:00", &tm);
      lab = b;
    }
    s += "<div title='" + lab + "  " + kfmt(hours[i]) + "' style='flex:1;background:#6bf;min-height:1px;height:" +
         String(hours[i] * 100UL / mx) + "%'></div>";
  }
  s += "</div><small>Output + new input per hour, newest on the right.";
  if (hours[peak]) {
    String lab = String("-") + peak + " h";
    if (timeOk()) {
      time_t t = time(nullptr) - peak * 3600L;
      struct tm tm;
      localtime_r(&t, &tm);
      char b[8];
      strftime(b, sizeof(b), "%H:00", &tm);
      lab = b;
    }
    s += " Busiest hour: " + lab + " (" + kfmt(hours[peak]) + ").";
  }
  s += "</small>";

  s += "<h3>Last 7 days</h3><table><tr><th>Day<th>Output<th>New input<th>Cache</tr>";
  Day sum = {0, 0, 0};
  time_t now = time(nullptr);
  for (int d = 0; d < 7; d++) {
    String label = d == 0 ? "today" : String("-") + d + " d";
    if (timeOk()) {
      time_t t = now - d * 86400L;
      struct tm tm;
      localtime_r(&t, &tm);
      char b[12];
      strftime(b, sizeof(b), "%m-%d", &tm);
      label = String(b) + (d == 0 ? " (today)" : "");
    }
    s += "<tr><td>" + label + "<td>" + kfmt(days[d].out) + "<td>" + kfmt(days[d].in) + "<td>" + kfmt(days[d].cache) + "</tr>";
    sum.out += days[d].out; sum.in += days[d].in; sum.cache += days[d].cache;
  }
  s += "<tr><td><b>7 days</b><td>" + kfmt(sum.out) + "<td>" + kfmt(sum.in) + "<td>" + kfmt(sum.cache) + "</tr></table>";
  s += "<small>Kept in flash (saved every 5 min). Output = text + thinking + tool input.</small>";

  s += "<h3>Last 30 days</h3><div style='display:flex;align-items:flex-end;gap:2px;height:60px'>";
  uint32_t dmx = 1, dsum = 0;
  for (int d = 0; d < DAYS; d++) { uint32_t v = days[d].out + days[d].in; dsum += v; if (v > dmx) dmx = v; }
  for (int d = DAYS - 1; d >= 0; d--) {
    String lab = String("-") + d + " d";
    if (timeOk()) {
      time_t t = now - d * 86400L;
      struct tm tm;
      localtime_r(&t, &tm);
      char b[12];
      strftime(b, sizeof(b), "%m-%d", &tm);
      lab = b;
    }
    uint32_t v = days[d].out + days[d].in;
    s += "<div title='" + lab + "  " + kfmt(v) + "' style='flex:1;background:" + (d == 0 ? "#9bf" : "#46a") +
         ";min-height:1px;height:" + String(v * 100UL / dmx) + "%'></div>";
  }
  s += "</div><small>Output + new input per day, today on the right. 30 days: " + kfmt(dsum) + ".</small>";

  s += "<h3>Recent events</h3><table id=ev>" + evRows() + "</table><small>Kept in RAM (cleared on restart).</small>";

  s += "<h3>Status</h3><table>";
  if (apOn) s += "<tr><td>Setup network</td><td>" + apName + "</td></tr>";
  if (netUp) {
    s += "<tr><td>WiFi</td><td>" + esc(WiFi.SSID()) + "</td></tr>";
    s += "<tr><td>IP</td><td>" + WiFi.localIP().toString() + "</td></tr>";
    s += "<tr><td>WiFi signal</td><td>" + String(WiFi.RSSI()) + " dBm</td></tr>";
  }
  s += "<tr><td>Uptime</td><td>" + String(millis() / 60000) + " min</td></tr>";
  s += "<tr><td>Last message from the PC</td><td>" + (lastMsg ? String(ago) + " s ago" : String("none yet")) + "</td></tr>";
  if (quiet) s += "<tr><td>Quiet mode (BOOT button)</td><td>on</td></tr>";
  s += "<tr><td>Queued flashes</td><td>" + String(pending) + "</td></tr>";
  s += "<tr><td>Turns finished / with error</td><td id=dn>" + String(doneOk) + " / " + String(doneErr) + "</td></tr>";
  s += "</table><small>Token and flash counts since power-on; the last column counts approval requests.</small>";

  s += F("<script>const n=[],f=[],l=[],lit=[],$=i=>document.getElementById(i);let wait=0,last=null;"
         "for(let i=0;i<6;i++){n[i]=$('n'+i);f[i]=$('f'+i);l[i]=$('l'+i);lit[i]=0;}"
         "function use(k,v,r){$('u'+k).textContent=v<0?'-':v+' %';const b=$('b'+k).style;b.width=Math.max(v,0)+'%';"
         "b.background=v>=85?'#c44':v>=60?'#cb4':'#4a4';$('r'+k).textContent=r?when(r):'';}"
         // reset time in the browser's time zone: "Resets 13:30 (in 2 h 10 min)", another day: "Resets Fri 09:00 (in 2 d 3 h)"
         "function when(r){const d=new Date(r*1000),s=r-Date.now()/1000,"
         "t=d.toLocaleTimeString('en-GB',{hour:'2-digit',minute:'2-digit'}),"
         "day=d.toDateString()==new Date().toDateString()?'':d.toLocaleDateString('en-GB',{weekday:'short'})+' ';"
         "if(s<=0)return 'Reset at '+day+t+' (passed)';const h=Math.floor(s/3600),m=Math.floor(s%3600/60);"
         "return 'Resets '+day+t+' (in '+(h>=24?Math.floor(h/24)+' d '+h%24+' h':h?h+' h '+m+' min':m+' min')+')';}"
         "function poll(){if(document.hidden)return;fetch('/s').then(r=>r.json()).then(s=>{"
         "for(let i=0;i<5;i++){n[i].textContent=s.t[i];f[i].textContent=s.f[i]+' flashes';"
         "if(last&&s.f[i]!=last[i])lit[i]=Date.now();}"
         "wait=s.w;n[5].textContent=s.a;f[5].textContent=s.w?'waiting':'idle';last=s.f;"
         "$('st').textContent=s.s;use(5,s.u5,s.r5);use(7,s.u7,s.r7);$('dn').textContent=s.d+' / '+s.e;}).catch(()=>{});}"
         "function fade(){const now=Date.now();for(let i=0;i<5;i++)"
         "l[i].style.opacity=.15+.85*Math.max(0,1-(now-lit[i])/400);"
         "l[5].style.opacity=wait?(Math.floor(now/500)%2?1:.35):.15;requestAnimationFrame(fade);}"
         "setInterval(poll,100);fade();setInterval(()=>{if(!document.hidden)fetch('/log').then(r=>r.text()).then(t=>{$('ev').innerHTML=t;}).catch(()=>{});},1500);</script>");
  http.send(200, "text/html; charset=utf-8", s);
}

// Remembers which collapsible sections are open (per browser).
String secJs(const char* key) {
  return String("<script>(()=>{const d=document.querySelectorAll('details.sec');let o=[];"
                "try{o=JSON.parse(localStorage.getItem('") + key + "')||'null')||[];}catch(e){}"
                "d.forEach((e,i)=>{if(o.length)e.open=!!o[i];e.addEventListener('toggle',()=>{"
                "try{localStorage.setItem('" + key + "',JSON.stringify([...d].map(x=>x.open)));}catch(e){}});});})();</script>";
}

void handleSettings() {
  String s = page(1);
  if (http.hasArg("saved")) s += "<p class=ok>Saved.</p>";
  s += "<form method=post action=/save><details class=sec open><summary>Flashes and signals</summary>";
  s += numField("out", dot(hex(0)) + dot(hex(1)) + dot(hex(2)) + "Text, thinking, tool: tokens per flash", cfg.out, 0, 60000);
  s += numField("in", dot(hex(3)) + "New input: tokens per flash", cfg.in, 0, 60000);
  s += numField("cache", dot(hex(4)) + "Cache read: tokens per flash", cfg.cache, 0, 60000);
  s += "<small>0 = no flash for this type.</small>";
  s += numField("bright", "Brightness (1–255)", cfg.bright, 1, 255);
  s += checkField("glow", "Dim glow while Claude is running", cfg.glow);
  s += checkField("saver", "Power-saving WiFi (for battery; off = faster)", cfg.saver);
  s += checkField("done", "Signal when Claude finishes (green) or fails (red)", cfg.doneSig);
  s += numField("remind", "Approval blink gets faster after (s, 0 = off)", cfg.remind, 0, 3600);

  s += "</details><details class=sec><summary>Onboard LED (GPIO8)</summary>";
  s += selField("onbt", "Type", cfg.onbT, {"RGB (WS2812B)", "plain LED, lit on LOW (e.g. blue SuperMini LED)", "plain LED, lit on HIGH"});
  s += "<small>A plain LED cannot show colours: it shows every colour as a brightness.</small>";
  s += selField("onb", "Mode", cfg.onb, {"mirror (selected functions)", "status + approval only", "off"});
  s += "<label><span>Brightness (%)</span><input type=number name=onbbr min=1 max=100 value=" + String(cfg.onbBr) + "></label>";
  s += "<small>Mirror shows these functions:</small>";
  for (int i = 0; i < 6; i++)
    s += String("<label><span>") + dot(hex(i)) + NAMES[i] + "</span><input type=checkbox name=onbm" + i + (cfg.onbMask >> i & 1 ? " checked" : "") + "></label>";
  s += checkField("onbsig", "Show the finished (green) / error (red) signal", cfg.onbSig);
  s += checkField("onbuse", "Idle colour shows the 5-hour usage (green / yellow / red)", cfg.onbUse);
  s += "<small>Status dots (connecting, setup network) are always shown unless the mode is off.</small>";
  s += "</details><details class=sec><summary>Buzzer</summary>";
  s += numField("bzpin", "GPIO (0–7, 10; -1 = none)", cfg.bzPin, -1, 10);
  s += selField("bzt", "Type", cfg.bzT, {"active (beeps by itself)", "passive (needs a tone)"});
  s += numField("bzhz", "Tone of a passive buzzer (Hz)", cfg.bzHz, 100, 10000);
  s += selField("bzap", "Approval wait", cfg.bzAp, {"silent", "one beep (one more at the reminder)", "beep until answered"});
  s += checkField("bzfl", "Click with every flash", cfg.bzFlash);
  s += "<small>Silent at night and in quiet mode. It cannot share the strip's pin. Test: click the approval LED on the Live page.</small>";
  s += "</details><details class=sec><summary>LEDs and colours</summary>";
  s += "<label><span>External strip: LEDs</span><input type=number name=count min=0 max=65535 list=cnt value=" +
       String(cfg.count) + "></label><datalist id=cnt><option>0<option>1<option>2<option>5<option>6<option>10<option>50<option>100</datalist>";
  s += numField("pin", "External strip data pin (GPIO 0–7, 10)", cfg.pin, 0, 10);
  for (int i = 0; i < 6; i++) {
    s += String("<label><span>") + dot(hex(i)) + NAMES[i] + "</span><span><input type=color name=c" + i + " value=" + hex(i) +
         "> <input class=lst name=g" + i + " placeholder=auto value=\"" + esc(cfg.list[i]) + "\"></span></label>";
  }
  s += "<small>Colour per type (onboard LED and strip). LEDs (strip): a list like <b>1,3,5-8</b> (1 = first LED), "
       "<b>all</b> or 0 = every LED, empty = auto: with 5+ LEDs every type has its own LED (approval: the 6th), "
       "otherwise all LEDs flash together. Several types may share LEDs.</small>";

  s += "</details><details class=sec><summary>Strip display</summary>";
  s += checkField("hist", "History: flashes run along the strip like a running light", cfg.hist);
  s += selField("meter", "Usage bar (dim, needs 2+ LEDs)", cfg.meter, {"off", "5-hour limit", "weekly limit"});
  s += "<small>The usage bar is not shown while the history is on.</small>";

  s += "</details><details class=sec><summary>Night and time</summary>";
  s += checkField("night", "Dim at night", cfg.night);
  s += numField("nfrom", "From (hour)", cfg.nFrom, 0, 23);
  s += numField("nto", "To (hour)", cfg.nTo, 0, 23);
  s += numField("nbr", "Night brightness (0 = off)", cfg.nBright, 0, 255);
  s += tzField(cfg.tz);
  s += String("<small>Time from the internet (NTP)") + (timeOk() ? ", clock is set." : ", clock not set yet.") + "</small>";

  s += "</details><details class=sec><summary>MQTT / Home Assistant</summary>";
  s += checkField("mq", "Publish the state over MQTT", cfg.mq);
  s += textField("mqhost", "Broker", cfg.mqHost, "text", "192.168.0.10");
  s += numField("mqport", "Port", cfg.mqPort, 1, 65535);
  s += textField("mquser", "User", cfg.mqUser);
  s += textField("mqpass", "Password", "", "password", cfg.mqPass.length() ? "(unchanged)" : "");
  s += textField("mqtopic", "Topic prefix", cfg.mqTopic);
  s += "<small>Topics: <b>prefix/state</b> (idle, working, approval, done, error), <b>prefix/usage5</b>, <b>prefix/usage7</b>; "
       "Home Assistant finds the sensors by itself (MQTT discovery). A missing broker never affects the LED.</small>";

  s += "</details><button>Save</button></form><form method=post action=/test><button>Colour test</button></form>";
  s += secJs("open");
  http.send(200, "text/html; charset=utf-8", s);
}

void handleSystem() {
  String s = page(2);
  s += "<details class=sec open><summary>WiFi</summary>";
  if (apOn) s += "<p>Setup network running: " + apName + "</p>";
  s += wifiForm();
  s += "<small>If it cannot connect within 30 s, the setup network starts: " + apName + "</small>";
  s += "</details><details class=sec><summary>Firmware update</summary><form method=post action=/update enctype=multipart/form-data>"
       "<input type=file name=fw accept=.bin required style=width:100%><br><button>Upload and restart</button></form>"
       "<small>Pick <b>.pio/build/supermini/firmware.bin</b>. The page has no password: anyone on your network can open it.</small>";
  s += "</details><details class=sec><summary>Backup</summary><a href=/config.json>Download settings</a> <small>(WiFi and MQTT password are not included)</small>"
       "<br><input type=file id=imp accept=.json style=width:100%><small>Pick a settings file to restore it.</small>"
       "<script>$=i=>document.getElementById(i);$('imp').onchange=e=>{const f=e.target.files[0];if(!f)return;"
       "f.text().then(t=>fetch('/import',{method:'POST',body:t,headers:{'Content-Type':'text/plain'}}))"
       ".then(r=>r.text()).then(m=>{alert(m);location.href='/settings';});};</script>";
  s += "</details><details class=sec><summary>Device</summary><table><tr><td>Free memory</td><td>" + String(ESP.getFreeHeap() / 1024) + " kB</td></tr>"
       "<tr><td>Clock</td><td>" + (timeOk() ? "set" : "not set") + "</td></tr>"
       "<tr><td>MQTT</td><td>" + (!cfg.mq ? "off" : mqOk ? "connected" : "not connected") + "</td></tr></table>";
  s += "</details>" + secJs("open2");
  http.send(200, "text/html; charset=utf-8", s);
}

// Live state for the page: tokens and flashes per type (t,k,j,i,r order), approval wait, usage, state.
void handleState() {
  String s = "{\"t\":[";
  for (int i = 0; i < 5; i++) s += String(i ? "," : "") + total[i];
  s += "],\"f\":[";
  for (int i = 0; i < 5; i++) s += String(i ? "," : "") + flashes[i];
  bool waiting = waitUntil && (int32_t)(millis() - waitUntil) < 0;
  bool fresh = useAt && millis() - useAt < USE_MS;
  s += "],\"w\":" + String(waiting ? 1 : 0) + ",\"a\":" + String(approvals) + ",\"s\":\"" + stateName() + "\"" +
       ",\"u5\":" + (fresh ? use5 : -1) + ",\"u7\":" + (fresh ? use7 : -1) +
       ",\"r5\":" + (fresh ? reset5 : 0) + ",\"r7\":" + (fresh ? reset7 : 0) + ",\"d\":" + doneOk + ",\"e\":" + doneErr + "}";
  http.sendHeader("Cache-Control", "no-store");
  http.send(200, "application/json", s);
}

void redirectTo(const char* where) {
  http.sendHeader("Location", where);
  http.send(303);
}

typedef std::function<String(const String&)> ArgFn;
typedef std::function<bool(const String&)> HasFn;

// Applies a full set of settings (from the form or from an imported file) and saves them.
void applySettings(const ArgFn& arg, const HasFn& has) {
  xSemaphoreTake(cfgLock, portMAX_DELAY);
  cfg.out    = constrain(arg("out").toInt(), 0, 60000);
  cfg.in     = constrain(arg("in").toInt(), 0, 60000);
  cfg.cache  = constrain(arg("cache").toInt(), 0, 60000);
  cfg.bright = constrain(arg("bright").toInt(), 1, 255);
  cfg.glow   = has("glow");
  cfg.saver  = has("saver");
  cfg.doneSig = has("done");
  for (int i = 0; i < 6; i++) {
    String a = arg(String("g") + i);
    a.trim();
    cfg.list[i] = a.substring(0, 60);
    parseSel(cfg.list[i], sel[i]);
    String c = arg(String("c") + i);   // "#rrggbb"
    if (c.length() == 7 && c[0] == '#') cfg.col[i] = strtoul(c.c_str() + 1, nullptr, 16) & 0xFFFFFF;
  }
  uint16_t count = constrain(arg("count").toInt(), 0, 65535);
  cfg.onb = constrain(arg("onb").toInt(), 0, 2);
  int pin = arg("pin").toInt();
  bool hw = count != cfg.count || (pinOk(pin) && pin != cfg.pin);
  cfg.count = count;
  if (pinOk(pin)) cfg.pin = pin;

  cfg.onbBr = constrain(arg("onbbr").toInt(), 1, 100);
  cfg.onbT  = constrain(arg("onbt").toInt(), 0, 2);
  String bp = arg("bzpin");
  bp.trim();
  int b = bp.length() ? bp.toInt() : -1;   // an empty field means none, not GPIO0
  cfg.bzPin = pinOk(b) && !(b == cfg.pin && cfg.count) ? b : -1;
  cfg.bzT   = constrain(arg("bzt").toInt(), 0, 1);
  cfg.bzHz  = constrain(arg("bzhz").toInt(), 100, 10000);
  cfg.bzAp  = constrain(arg("bzap").toInt(), 0, 2);
  cfg.bzFlash = has("bzfl");
  cfg.onbMask = 0;
  for (int i = 0; i < 6; i++) if (has(String("onbm") + i)) cfg.onbMask |= 1 << i;
  cfg.onbSig = has("onbsig");
  cfg.onbUse = has("onbuse");
  cfg.hist  = has("hist");
  cfg.meter = constrain(arg("meter").toInt(), 0, 2);
  memset(hist, 0, sizeof(hist));
  hcur = 0;
  cfg.night = has("night");
  cfg.nFrom = constrain(arg("nfrom").toInt(), 0, 23);
  cfg.nTo   = constrain(arg("nto").toInt(), 0, 23);
  cfg.nBright = constrain(arg("nbr").toInt(), 0, 255);
  String tz = arg("tz");
  tz.trim();
  if (tz.length() && tz != cfg.tz) { cfg.tz = tz.substring(0, 60); startNtp(); }

  cfg.remind = constrain(arg("remind").toInt(), 0, 3600);
  cfg.mq = has("mq");
  cfg.mqHost = arg("mqhost").substring(0, 60);
  cfg.mqHost.trim();
  cfg.mqPort = constrain(arg("mqport").toInt(), 1, 65535);
  cfg.mqUser = arg("mquser").substring(0, 40);
  if (arg("mqpass").length()) cfg.mqPass = arg("mqpass").substring(0, 60);
  String topic = arg("mqtopic");
  topic.trim();
  cfg.mqTopic = topic.length() ? topic.substring(0, 40) : "tokenled";
  mqReconf = true;
  saveCfg();
  xSemaphoreGive(cfgLock);
  bzEnd();   // release the old buzzer pin first: the strip may move onto it
  if (hw) applyHw();
  applyCfg();
}

void handleSave() {
  applySettings([](const String& k) { return http.arg(k); }, [](const String& k) { return http.hasArg(k); });
  redirectTo("/settings?saved");
}

// ---------- Settings backup (flat JSON, same keys as the form) ----------
String jq(String v) {
  v.replace("\"", "");
  v.replace("\\", "");
  return "\"" + v + "\"";
}

String exportJson() {
  String s = "{\n";
  auto num = [&](const char* k, long v) { s += String("\"") + k + "\":" + v + ",\n"; };
  auto flag = [&](const char* k, bool v) { s += String("\"") + k + "\":" + (v ? "true" : "false") + ",\n"; };
  auto txt = [&](const String& k, const String& v) { s += "\"" + k + "\":" + jq(v) + ",\n"; };
  num("out", cfg.out); num("in", cfg.in); num("cache", cfg.cache); num("bright", cfg.bright);
  flag("glow", cfg.glow); flag("saver", cfg.saver); flag("done", cfg.doneSig); num("remind", cfg.remind);
  num("onb", cfg.onb); num("onbt", cfg.onbT); num("onbbr", cfg.onbBr); flag("onbsig", cfg.onbSig); flag("onbuse", cfg.onbUse);
  num("bzpin", cfg.bzPin); num("bzt", cfg.bzT); num("bzhz", cfg.bzHz); num("bzap", cfg.bzAp); flag("bzfl", cfg.bzFlash);
  for (int i = 0; i < 6; i++) flag((String("onbm") + i).c_str(), cfg.onbMask >> i & 1); num("count", cfg.count); num("pin", cfg.pin);
  flag("hist", cfg.hist); num("meter", cfg.meter);
  flag("night", cfg.night); num("nfrom", cfg.nFrom); num("nto", cfg.nTo); num("nbr", cfg.nBright); txt("tz", cfg.tz);
  flag("mq", cfg.mq); txt("mqhost", cfg.mqHost); num("mqport", cfg.mqPort); txt("mquser", cfg.mqUser); txt("mqtopic", cfg.mqTopic);
  for (int i = 0; i < 6; i++) { txt(String("c") + i, hex(i)); txt(String("g") + i, cfg.list[i]); }
  s.remove(s.length() - 2, 1);   // last comma
  return s + "}\n";
}

std::map<String, String> parseJson(const String& j) {
  std::map<String, String> m;
  int i = 0, n = j.length();
  while (true) {
    int k = j.indexOf('"', i);
    if (k < 0) break;
    int ke = j.indexOf('"', k + 1);
    int c = ke < 0 ? -1 : j.indexOf(':', ke);
    if (c < 0) break;
    String key = j.substring(k + 1, ke), v;
    i = c + 1;
    while (i < n && j[i] == ' ') i++;
    if (i < n && j[i] == '"') {
      int e = j.indexOf('"', i + 1);
      if (e < 0) break;
      v = j.substring(i + 1, e);
      i = e + 1;
    } else {
      int e = i;
      while (e < n && j[e] != ',' && j[e] != '}' && j[e] != '\n') e++;
      v = j.substring(i, e);
      v.trim();
      i = e;
    }
    m[key] = v;
  }
  return m;
}

void handleExport() {
  http.sendHeader("Content-Disposition", "attachment; filename=tokenled-settings.json");
  http.send(200, "application/json", exportJson());
}

void handleImport() {
  auto in = parseJson(http.arg("plain"));
  if (!in.count("out") || !in.count("bright")) { http.send(400, "text/plain", "This is not a Token LED settings file."); return; }
  auto m = parseJson(exportJson());   // missing keys keep their current value
  for (auto& kv : in) m[kv.first] = kv.second;
  applySettings([&](const String& k) { auto it = m.find(k); return it == m.end() ? String() : it->second; },
                [&](const String& k) { auto it = m.find(k); return it != m.end() && it->second == "true"; });
  http.send(200, "text/plain", "Settings restored.");
}

void handleBlink() {   // click on a virtual LED: 'a' = approval blink for 3 s, otherwise 3 flashes of that type
  String t = http.arg("t");
  if (t == "a") { waitUntil = millis() + 3000; waitStart = millis(); approvals++; }
  else if (t.length() == 1 && strchr(TYPES, t[0])) enqueue(t[0], 3);
  http.send(204);
}

void handleTest() {
  for (const char* t = TYPES; *t; t++) enqueue(*t, 3);
  sigErr = false;
  sigPending = cfg.doneSig;
  redirectTo("/settings");
}

void handleWifi() {
  String ssid = http.arg("ssid");
  ssid.trim();
  if (!ssid.length()) { redirectTo("/"); return; }
  storeWifi(ssid, http.arg("pass"));
  String s = FPSTR(PAGE_HEAD);
  s += "<p>Saved, connecting to <b>" + esc(ssid) + "</b></p>"
       "<p>If it works, the setup network disappears and the LED is available at "
       "<a href=http://tokenled.local>http://tokenled.local</a></p>"
       "<p><small>If it fails within 30 s, the setup network (" + apName + ") comes back.</small></p>";
  http.send(200, "text/html; charset=utf-8", s);
}

// Firmware update over WiFi: the browser posts firmware.bin, the ESP writes it to the other app slot and restarts.
void handleUpdateDone() {
  bool ok = !Update.hasError();
  http.sendHeader("Connection", "close");
  http.send(200, "text/plain", ok ? "Update OK, restarting..." : String("Update failed: ") + Update.errorString());
  if (ok) {
    if (statsDirty) saveStats();
    delay(500);
    ESP.restart();
  }
}

void handleUpload() {
  HTTPUpload& u = http.upload();
  if (u.status == UPLOAD_FILE_START) {
    led(px.Color(0, 0, 255));
    Update.begin(UPDATE_SIZE_UNKNOWN);
  } else if (u.status == UPLOAD_FILE_WRITE) {
    Update.write(u.buf, u.currentSize);
  } else if (u.status == UPLOAD_FILE_END) {
    Update.end(true);
  } else if (u.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
  }
}

// In setup mode every unknown address (including the phones' connectivity checks) leads to the setup page.
void handleNotFound() {
  if (!apOn) { http.send(404, "text/plain", "Not found"); return; }
  http.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/");
  http.send(302);
}

// ---------- MQTT (own task: a missing broker must never delay the LED) ----------
WiFiClient mqNet;
PubSubClient mqtt(mqNet);

void mqPub(const String& t, const String& v) { mqtt.publish(t.c_str(), v.c_str(), true); }

void mqDiscovery(const String& pre) {
  String id = apName;
  id.toLowerCase();
  id.replace("-", "_");
  String dev = ",\"avty_t\":\"" + pre + "/availability\",\"dev\":{\"ids\":[\"" + id + "\"],\"name\":\"Token LED\"}";
  const char* key[] = {"state", "usage5", "usage7"};
  const char* name[] = {"Claude state", "Claude usage 5 h", "Claude usage 7 d"};
  for (int i = 0; i < 3; i++) {
    String c = String("{\"name\":\"") + name[i] + "\",\"uniq_id\":\"" + id + "_" + key[i] + "\",\"stat_t\":\"" + pre + "/" + key[i] + "\"" +
               (i ? ",\"unit_of_measurement\":\"%\"" : "") + dev + "}";
    mqPub("homeassistant/sensor/" + id + "/" + key[i] + "/config", c);
  }
}

void mqttTask(void*) {
  uint32_t nextTry = 0, backoff = 5000;
  String lastState, host, user, pass, pre;
  int l5 = -2, l7 = -2;
  uint16_t port = 1883;
  for (;;) {
    vTaskDelay(200 / portTICK_PERIOD_MS);
    bool on;
    xSemaphoreTake(cfgLock, portMAX_DELAY);
    on = cfg.mq && cfg.mqHost.length() && netUp;
    if (on && (mqReconf || !mqtt.connected())) {
      host = cfg.mqHost; port = cfg.mqPort; user = cfg.mqUser; pass = cfg.mqPass; pre = cfg.mqTopic;
    }
    xSemaphoreGive(cfgLock);
    if (mqReconf) { mqReconf = false; if (mqtt.connected()) mqtt.disconnect(); nextTry = 0; }
    mqOk = mqtt.connected();
    if (!on) { if (mqtt.connected()) mqtt.disconnect(); continue; }

    if (!mqtt.connected()) {
      if ((int32_t)(millis() - nextTry) < 0) continue;
      mqNet.setTimeout(2);
      mqtt.setBufferSize(512);
      mqtt.setServer(host.c_str(), port);
      bool ok = mqtt.connect(apName.c_str(), user.length() ? user.c_str() : nullptr, user.length() ? pass.c_str() : nullptr,
                             (pre + "/availability").c_str(), 0, true, "offline");
      if (!ok) { nextTry = millis() + backoff; backoff = min<uint32_t>(backoff * 2, 60000); continue; }
      backoff = 5000;
      mqPub(pre + "/availability", "online");
      mqDiscovery(pre);
      lastState = "";
      l5 = l7 = -2;
    }
    mqtt.loop();
    String st = stateName();
    if (st != lastState) { mqPub(pre + "/state", st); lastState = st; }
    bool fresh = useAt && millis() - useAt < USE_MS;
    int a = fresh ? use5 : -1, b = fresh ? use7 : -1;
    if (a != l5) { mqPub(pre + "/usage5", a < 0 ? "unknown" : String(a)); l5 = a; }
    if (b != l7) { mqPub(pre + "/usage7", b < 0 ? "unknown" : String(b)); l7 = b; }
  }
}

void setup() {
  setCpuFrequencyMhz(80);     // on battery: less power, enough for WiFi
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);   // never block on output when no USB host is attached
  pinMode(BTN_PIN, INPUT_PULLUP);
  cfgLock = xSemaphoreCreateMutex();
  loadCfg();
  loadStats();
  if (!pinOk(cfg.pin)) cfg.pin = LED_PIN;
  px.setPin(cfg.pin);
  px.updateLength(cfg.count);
  if (cfg.count && !px.numPixels()) cfg.count = 0;
  obBegin();
  px.begin();
  if (cfg.bzPin >= 0 && (!pinOk(cfg.bzPin) || (cfg.bzPin == cfg.pin && cfg.count))) cfg.bzPin = -1;
  bzBegin();
  applyBright();   // appliedB is still 0, so both brightnesses are set (onboard with its percentage)
  const uint32_t test[] = {px.Color(255, 0, 0), px.Color(0, 255, 0), px.Color(0, 0, 255), 0};
  for (uint32_t c : test) { led(c); delay(250); }

  prefs.begin("wifi", true);
  savedSsid = prefs.getString("ssid");
  savedPass = prefs.getString("pass");
  prefs.end();

  WiFi.persistent(false);     // we store the WiFi credentials ourselves (Preferences)
  WiFi.setHostname(HOSTNAME); // must come before mode(), otherwise it has no effect
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(cfg.saver);   // modem sleep: much less power, costs a few hundred ms of latency
  String mac = WiFi.macAddress();
  apName = "TokenLED-" + mac.substring(12, 14) + mac.substring(15);

  http.on("/", handleRoot);
  http.on("/settings", handleSettings);
  http.on("/system", handleSystem);
  http.on("/s", handleState);
  http.on("/log", handleLog);
  http.on("/save", HTTP_POST, handleSave);
  http.on("/test", HTTP_POST, handleTest);
  http.on("/b", handleBlink);
  http.on("/config.json", handleExport);
  http.on("/import", HTTP_POST, handleImport);
  http.on("/wifi", HTTP_POST, handleWifi);
  http.on("/update", HTTP_POST, handleUpdateDone, handleUpload);
  http.onNotFound(handleNotFound);
  http.begin();
  udp.begin(UDP_PORT);
  staStart();
  logEv(EV_BOOT);
  xTaskCreate(mqttTask, "mqtt", 8192, nullptr, 1, nullptr);
}

void loop() {
  readSerial();
  network();
  buttonTick();
  histTick();
  play();
  buzzerTick();
  static uint32_t slow = 0;
  if (millis() - slow > 1000) {
    slow = millis();
    rollDay();
    rollHour();
    applyBright();
    if (statsDirty && millis() - statsAt > STATS_MS) saveStats();
  }
  delay(1);
}
