// Claude token LED — ESP32-C3 SuperMini Plus, onboard WS2812B (GPIO8) or an external WS2812 strip.
// The PC sends raw token counts, the ESP turns them into flashes. Settings: http://tokenled.local
// Without WiFi (nothing saved, or no connection within 30 s) a setup AP "TokenLED-xxxx" starts:
// open network, phones open the setup page by themselves (http://192.168.4.1).
// Input: UDP 4210 or USB serial, one command per line (several lines per UDP packet are fine):
//   "T <type> <tokens>"   type: t=text, k=thinking, j=tool input, i=new input, r=cache read
//   "B <type> <count>"    flash <count> times directly (test)
//   "A 1" / "A 0"         Claude waits for / got the user's approval (repeat "A 1" every ~1 s)
//   "P"                   keep-alive (shown on the web page; dim glow if enabled)
//   "W <ssid>\t<pass>"    set WiFi (USB only), saved to flash; reply: "IP <address>"
//   "?"                   status (USB only): "IP <address>" or "NOWIFI"
// LED: red-green-blue test at power-on; a dim dot every 3 s = connecting (red) / setup AP running (yellow).
// Library: Adafruit NeoPixel

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <Adafruit_NeoPixel.h>

#define LED_PIN     8        // default data pin (onboard WS2812B); pin and LED count are set on the web page
#define UDP_PORT    4210
#define HOSTNAME    "tokenled"   // http://tokenled.local
#define QUEUE_LEN   32
#define MAX_PENDING 300     // more queued flashes than this are dropped so the LED never lags by minutes
#define ALIVE_MS    5000    // how long the last message counts as "Claude is running"
#define WAIT_MS     5000    // approval wait expires this long after the last "A 1"
#define AP_AFTER_MS 30000   // failed-connection time before the setup AP starts
#define AP_RETRY_MS 180000  // in AP mode, retry the saved WiFi after this long if nobody joined the AP
#define AUTO        0xFFFF  // LED assignment: automatic

Adafruit_NeoPixel px(1, LED_PIN, NEO_GRB + NEO_KHZ800);
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
uint32_t lastMsg = 0, waitUntil = 0, approvals = 0;

enum { IDLE, ON, OFF } state = IDLE;
uint32_t until = 0;
char curType = 0;
uint32_t shown = 1;   // last colour written; 1 = nothing yet
int shownT = -9;      // last target written

const char TYPES[] = "tkjir";
const char* NAMES[] = {"Text", "Thinking", "Tool", "New input", "Cache", "Approval"};
const char* COLORS[] = {"#0bf", "#90f", "#0f4", "#f60", "#33f", "#fff"};
uint32_t rem[5], total[5], flashes[5];

// ---------- Settings (web page, flash) ----------
struct Cfg {
  uint16_t out, in, cache, count;
  uint8_t bright, pin;
  bool glow, saver;
  uint16_t tgt[6];   // LED per type (+ approval): 0 = all LEDs, n = LED n (1-based), AUTO = automatic
} cfg;

// Allowed data pins: not the boot (9), USB (18, 19) or UART (20, 21) pins.
bool pinOk(int p) { return (p >= 0 && p <= 8) || p == 10; }

void loadCfg() {
  prefs.begin("cfg", true);
  cfg.count  = prefs.getUShort("count", 1);
  cfg.pin    = prefs.getUChar("pin", LED_PIN);
  cfg.out    = prefs.getUShort("out", 10);
  cfg.in     = prefs.getUShort("in", 100);
  cfg.cache  = prefs.getUShort("cache", 0);
  cfg.bright = prefs.getUChar("bright", 120);
  cfg.glow   = prefs.getBool("glow", false);
  cfg.saver  = prefs.getBool("saver", true);
  for (int i = 0; i < 6; i++) cfg.tgt[i] = prefs.getUShort((String("g") + i).c_str(), AUTO);
  prefs.end();
}

void saveCfg() {
  prefs.begin("cfg", false);
  prefs.putUShort("count", cfg.count);
  prefs.putUChar("pin", cfg.pin);
  prefs.putUShort("out", cfg.out);
  prefs.putUShort("in", cfg.in);
  prefs.putUShort("cache", cfg.cache);
  prefs.putUChar("bright", cfg.bright);
  prefs.putBool("glow", cfg.glow);
  prefs.putBool("saver", cfg.saver);
  for (int i = 0; i < 6; i++) prefs.putUShort((String("g") + i).c_str(), cfg.tgt[i]);
  prefs.end();
}

void applyCfg() {
  px.setBrightness(cfg.bright);
  shown = 1;   // new brightness: force the next colour to be written
  if (!apOn) WiFi.setSleep(cfg.saver);
}

// ---------- LED ----------
// target: -1 = all LEDs (flash), -2 = first LED only (status dot, glow), n >= 0 = that LED.
void led(uint32_t c, int target = -1) {
  if (c == 0) target = -1;
  if (c == shown && target == shownT) return;
  shown = c;
  shownT = target;
  px.clear();
  if (target == -1) px.fill(c);
  else px.setPixelColor(target == -2 ? 0 : target, c);
  px.show();
}

// New LED count or data pin: switch the old strip off and resize.
void applyHw() {
  px.clear();
  px.show();
  px.setPin(cfg.pin);
  px.updateLength(cfg.count);
  if (!px.numPixels()) { px.updateLength(1); cfg.count = 1; saveCfg(); }   // did not fit in memory
  px.setBrightness(cfg.bright);
  shown = 1;
}

// Which LED a type (0-4) or the approval signal (5) uses; -1 = all LEDs.
// Automatic: with 5+ LEDs every type has its own LED, the approval signal gets the 6th; otherwise all LEDs.
int targetFor(int idx) {
  int v = cfg.tgt[idx];
  if (v == AUTO) return cfg.count >= (idx < 5 ? 5 : 6) ? idx : -1;
  return v >= 1 && v <= cfg.count ? v - 1 : -1;
}

uint32_t color(char type) {
  switch (type) {
    case 'k': return px.Color(150, 0, 255);  // thinking
    case 'j': return px.Color(0, 255, 60);   // tool input
    case 'i': return px.Color(255, 90, 0);   // new input
    case 'r': return px.Color(0, 0, 120);    // cache read
    default:  return px.Color(0, 180, 255);  // text
  }
}

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
  uint16_t u = unitFor(type);
  if (!u) return;
  rem[i] += n;
  enqueue(type, rem[i] / u);
  rem[i] %= u;
}

void setWaiting(bool on) {
  if (!on) { waitUntil = 0; return; }
  if (!waitUntil || (int32_t)(millis() - waitUntil) >= 0) approvals++;
  waitUntil = millis() + WAIT_MS;
}

// Plays the queued flashes; the more are waiting, the faster it goes.
void play() {
  uint32_t now = millis();
  if (state != IDLE && (int32_t)(now - until) < 0) return;

  if (state == ON) {
    led(0);
    int off = constrain(80 - pending * 2, 20, 80);
    if (!qCount || q[qHead].type != curType) off += 150;  // pause when the type changes
    state = OFF; until = now + off;
    return;
  }

  if (!qCount) {
    state = IDLE;
    if (waitUntil && (int32_t)(now - waitUntil) < 0) {     // Claude waits for approval: white blink
      if (now / 500 % 2 == 0) led(px.Color(255, 255, 255), targetFor(5));
      else led(0);
      return;
    }
    uint32_t c = 0;
    bool dot = now % 3000 < 40;
    if (cfg.glow && lastMsg && now - lastMsg < ALIVE_MS) c = px.Color(0, 6, 10);
    else if (apOn && dot) c = px.Color(40, 25, 0);
    else if (!netUp && dot) c = px.Color(40, 0, 0);
    led(c, -2);
    return;
  }

  Item& it = q[qHead];
  curType = it.type;
  led(color(curType), targetFor(typeIdx(curType)));
  flashes[typeIdx(curType)]++;
  if (--it.n == 0) { qHead = (qHead + 1) % QUEUE_LEN; qCount--; }
  pending--;
  state = ON; until = now + (pending > 30 ? 20 : 35);
}

// ---------- Input ----------
void printStatus() {
  if (WiFi.status() == WL_CONNECTED) Serial.printf("IP %s\n", WiFi.localIP().toString().c_str());
  else Serial.println("NOWIFI");
}

void storeWifi(const String& ssid, const String& pass);

void handleLine(char* s, bool usb) {
  lastMsg = millis();
  char type; long n;
  if (sscanf(s, "T %c %ld", &type, &n) == 2) addTokens(type, n);
  else if (sscanf(s, "B %c %ld", &type, &n) == 2) enqueue(type, n);
  else if (sscanf(s, "A %ld", &n) == 1) setWaiting(n != 0);
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

// ---------- Web page ----------
const char PAGE_HEAD[] PROGMEM = R"(<!doctype html><html lang=en><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1"><title>Token LED</title>
<style>body{font:15px system-ui,sans-serif;max-width:480px;margin:16px auto;padding:0 16px;background:#141414;color:#ddd}
h2{margin:8px 0 16px}h3{margin:24px 0 8px;font-size:15px;color:#999}a{color:#6bf}
label{display:flex;justify-content:space-between;align-items:center;gap:12px;margin:10px 0}
input:not([type=checkbox]){width:90px;padding:4px;background:#222;color:#eee;border:1px solid #444;border-radius:4px}
input[name=ssid],input[name=pass]{width:180px}
button{padding:8px 16px;margin:12px 8px 0 0;background:#333;color:#eee;border:1px solid #555;border-radius:6px}
.d{display:inline-block;width:10px;height:10px;border-radius:50%;margin-right:3px}
.box{border:1px solid #a80;border-radius:8px;padding:4px 12px 12px;margin-bottom:16px}
summary{cursor:pointer;color:#999;margin-top:24px}
table{width:100%;border-collapse:collapse}td{padding:3px 0}td+td{text-align:right;font-variant-numeric:tabular-nums}
small{color:#888}
.g{display:grid;grid-template-columns:repeat(6,1fr);gap:4px;text-align:center;margin:12px 0}
.l{width:30px;height:30px;border-radius:50%;margin:0 auto 6px;border:1px solid #333}
.g small{display:block}.g b{font-size:12px}</style><h2>Token LED</h2>)";

String dot(const char* c) { return String("<i class=d style=background:") + c + "></i>"; }

String numField(const char* name, const String& label, long v, long lo, long hi) {
  return "<label><span>" + label + "</span><input type=number name=" + name + " min=" + lo + " max=" + hi +
         " value=" + v + "></label>";
}

String checkField(const char* name, const char* label, bool v) {
  return String("<label><span>") + label + "</span><input type=checkbox name=" + name + (v ? " checked" : "") + "></label>";
}

String wifiForm() {
  return "<form method=post action=/wifi><label><span>Network (2.4 GHz)</span><input name=ssid list=ssids required value=\"" +
         esc(savedSsid) + "\"></label><datalist id=ssids>" + ssidOptions + "</datalist>"
         "<label><span>Password</span><input type=password name=pass></label><button>Connect</button></form>";
}

void handleRoot() {
  uint32_t ago = (millis() - lastMsg) / 1000;
  String s = FPSTR(PAGE_HEAD);
  if (apOn) s += "<div class=box><h3>WiFi setup</h3>" + wifiForm() + "</div>";

  // Virtual LEDs: one column per type, plus the approval signal
  s += "<div class=g>";
  for (int i = 0; i < 6; i++) {
    s += String("<div><div class=l id=l") + i + " style=background:" + COLORS[i] + ";opacity:.15></div><b>" + NAMES[i] +
         "</b><small id=n" + i + ">" + (i < 5 ? total[i] : approvals) + "</small><small id=f" + i + ">" +
         (i < 5 ? String(flashes[i]) + " flashes" : String("idle")) + "</small></div>";
  }
  s += "</div>";

  s += "<form method=post action=/save>";
  s += numField("out", dot("#0bf") + dot("#90f") + dot("#0f4") + "Text, thinking, tool: tokens per flash", cfg.out, 0, 60000);
  s += numField("in", dot("#f60") + "New input: tokens per flash", cfg.in, 0, 60000);
  s += numField("cache", dot("#33f") + "Cache read: tokens per flash", cfg.cache, 0, 60000);
  s += "<small>0 = no flash for this type.</small>";
  s += numField("bright", "Brightness (1–255)", cfg.bright, 1, 255);
  s += checkField("glow", "Dim glow while Claude is running", cfg.glow);
  s += checkField("saver", "Power-saving WiFi (for battery; off = faster)", cfg.saver);

  s += "<h3>LEDs</h3>";
  s += "<label><span>Attached LEDs</span><input type=number name=count min=1 max=65535 list=cnt value=" +
       String(cfg.count) + "></label><datalist id=cnt><option>1<option>2<option>5<option>6<option>10<option>50<option>100</datalist>";
  s += numField("pin", "Data pin (GPIO 0–8, 10; onboard LED: 8)", cfg.pin, 0, 10);
  for (int i = 0; i < 6; i++) {
    s += String("<label><span>") + dot(COLORS[i]) + NAMES[i] + " LED</span><input type=number name=g" + i +
         " min=0 max=65534 placeholder=auto value=" + (cfg.tgt[i] == AUTO ? String("") : String(cfg.tgt[i])) + "></label>";
  }
  s += "<small>LED number (1 = first), 0 = all LEDs, empty = auto: with 5+ LEDs every type has its own LED "
       "(approval: the 6th), otherwise all LEDs flash together.</small>";
  s += "<br><button>Save</button></form><form method=post action=/test><button>Colour test</button></form>";

  s += "<h3>Status</h3><table>";
  if (apOn) s += "<tr><td>Setup network</td><td>" + apName + "</td></tr>";
  if (netUp) {
    s += "<tr><td>WiFi</td><td>" + esc(WiFi.SSID()) + "</td></tr>";
    s += "<tr><td>IP</td><td>" + WiFi.localIP().toString() + "</td></tr>";
    s += "<tr><td>WiFi signal</td><td>" + String(WiFi.RSSI()) + " dBm</td></tr>";
  }
  s += "<tr><td>Uptime</td><td>" + String(millis() / 60000) + " min</td></tr>";
  s += "<tr><td>Last message from the PC</td><td>" + (lastMsg ? String(ago) + " s ago" : String("none yet")) + "</td></tr>";
  s += "<tr><td>Queued flashes</td><td>" + String(pending) + "</td></tr>";
  s += "</table><small>Token and flash counts since power-on; the last column counts approval requests.</small>";
  if (!apOn) s += "<details><summary>Change WiFi network</summary>" + wifiForm() +
                  "<small>If it cannot connect within 30 s, the setup network starts: " + apName + "</small></details>";

  s += F("<script>const n=[],f=[],l=[],lit=[];let wait=0,last=null;"
         "for(let i=0;i<6;i++){n[i]=document.getElementById('n'+i);f[i]=document.getElementById('f'+i);"
         "l[i]=document.getElementById('l'+i);lit[i]=0;}"
         "function poll(){if(document.hidden)return;fetch('/s').then(r=>r.json()).then(s=>{"
         "for(let i=0;i<5;i++){n[i].textContent=s.t[i];f[i].textContent=s.f[i]+' flashes';"
         "if(last&&s.f[i]!=last[i])lit[i]=Date.now();}"
         "wait=s.w;n[5].textContent=s.a;f[5].textContent=s.w?'waiting':'idle';last=s.f;}).catch(()=>{});}"
         "function fade(){const now=Date.now();for(let i=0;i<5;i++)"
         "l[i].style.opacity=.15+.85*Math.max(0,1-(now-lit[i])/400);"
         "l[5].style.opacity=wait?(Math.floor(now/500)%2?1:.35):.15;requestAnimationFrame(fade);}"
         "setInterval(poll,200);fade();</script>");
  http.send(200, "text/html; charset=utf-8", s);
}

// Live state for the virtual LEDs: tokens and flashes per type (t,k,j,i,r order), approval wait.
void handleState() {
  String s = "{\"t\":[";
  for (int i = 0; i < 5; i++) s += String(i ? "," : "") + total[i];
  s += "],\"f\":[";
  for (int i = 0; i < 5; i++) s += String(i ? "," : "") + flashes[i];
  bool waiting = waitUntil && (int32_t)(millis() - waitUntil) < 0;
  s += "],\"w\":" + String(waiting ? 1 : 0) + ",\"a\":" + String(approvals) + "}";
  http.sendHeader("Cache-Control", "no-store");
  http.send(200, "application/json", s);
}

void redirectHome() {
  http.sendHeader("Location", "/");
  http.send(303);
}

void handleSave() {
  cfg.out    = constrain(http.arg("out").toInt(), 0, 60000);
  cfg.in     = constrain(http.arg("in").toInt(), 0, 60000);
  cfg.cache  = constrain(http.arg("cache").toInt(), 0, 60000);
  cfg.bright = constrain(http.arg("bright").toInt(), 1, 255);
  cfg.glow   = http.hasArg("glow");
  cfg.saver  = http.hasArg("saver");
  for (int i = 0; i < 6; i++) {
    String a = http.arg(String("g") + i);
    a.trim();
    cfg.tgt[i] = a.length() ? constrain(a.toInt(), 0, 65534) : AUTO;
  }
  uint16_t count = constrain(http.arg("count").toInt(), 1, 65535);
  int pin = http.arg("pin").toInt();
  bool hw = count != cfg.count || (pinOk(pin) && pin != cfg.pin);
  cfg.count = count;
  if (pinOk(pin)) cfg.pin = pin;
  saveCfg();
  if (hw) applyHw();
  applyCfg();
  redirectHome();
}

void handleTest() {
  for (const char* t = TYPES; *t; t++) enqueue(*t, 3);
  setWaiting(true);
  redirectHome();
}

void handleWifi() {
  String ssid = http.arg("ssid");
  ssid.trim();
  if (!ssid.length()) { redirectHome(); return; }
  storeWifi(ssid, http.arg("pass"));
  String s = FPSTR(PAGE_HEAD);
  s += "<p>Saved, connecting to <b>" + esc(ssid) + "</b></p>"
       "<p>If it works, the setup network disappears and the LED is available at "
       "<a href=http://tokenled.local>http://tokenled.local</a></p>"
       "<p><small>If it fails within 30 s, the setup network (" + apName + ") comes back.</small></p>";
  http.send(200, "text/html; charset=utf-8", s);
}

// In setup mode every unknown address (including the phones' connectivity checks) leads to the setup page.
void handleNotFound() {
  if (!apOn) { http.send(404, "text/plain", "Not found"); return; }
  http.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/");
  http.send(302);
}

void setup() {
  setCpuFrequencyMhz(80);     // on battery: less power, enough for WiFi
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);   // never block on output when no USB host is attached
  loadCfg();
  if (!pinOk(cfg.pin)) cfg.pin = LED_PIN;
  px.updateLength(cfg.count);
  if (!px.numPixels()) { px.updateLength(1); cfg.count = 1; }
  px.setPin(cfg.pin);
  px.begin();
  px.setBrightness(cfg.bright);
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
  http.on("/s", handleState);
  http.on("/save", HTTP_POST, handleSave);
  http.on("/test", HTTP_POST, handleTest);
  http.on("/wifi", HTTP_POST, handleWifi);
  http.onNotFound(handleNotFound);
  http.begin();
  udp.begin(UDP_PORT);
  staStart();
}

void loop() {
  readSerial();
  network();
  play();
  delay(5);
}
