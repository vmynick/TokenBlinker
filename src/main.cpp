// Claude token LED — ESP32-C3 SuperMini Plus, beépített WS2812B (GPIO8); WiFi-n (elemről is) vagy USB-n
// A PC nyers tokenszámokat küld, a villanásokat az ESP számolja. Beállítások: http://tokenled.local
// WiFi nélkül (nincs mentve, vagy 30 s alatt nem csatlakozik) beállító AP indul: "TokenLED-xxxx",
// nyílt hálózat, a telefon magától feldobja a beállító oldalt (http://192.168.4.1).
// Bemenet: UDP 4210 vagy USB soros, soronként (egy UDP-csomagban több sor is lehet):
//   "T <típus> <token>"   típus: t=szöveg, k=thinking, j=tool input, i=új bemenet, r=cache olvasás
//   "B <típus> <db>"      közvetlenül ennyi villanás (teszt)
//   "P"                   életjel (a webes felület mutatja; derengés, ha be van kapcsolva)
//   "W <ssid>\t<jelszó>"  WiFi beállítása (csak USB-n), a flash-be menti; válasz: "IP <cím>"
//   "?"                   állapot (csak USB-n): "IP <cím>" vagy "NOWIFI"
// LED: bekapcsoláskor piros-zöld-kék teszt; 3 s-onként halvány piros pötty = csatlakozik,
//      sárga pötty = beállító AP fut.
// Könyvtár: Adafruit NeoPixel

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <Adafruit_NeoPixel.h>

#define LED_PIN     8
#define UDP_PORT    4210
#define HOSTNAME    "tokenled"   // http://tokenled.local
#define QUEUE_LEN   32
#define MAX_PENDING 300     // ennél több várakozó villanást eldob, hogy ne késsen percekig
#define ALIVE_MS    5000    // ennyi ideig számít élőnek az utolsó üzenet után
#define AP_AFTER_MS 30000   // ennyi sikertelen csatlakozás után indul a beállító AP
#define AP_RETRY_MS 180000  // AP-ban ennyi idő után, ha senki nem csatlakozott rá, újra a mentett WiFi

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
uint32_t lastMsg = 0;

enum { IDLE, ON, OFF } state = IDLE;
uint32_t until = 0;
char curType = 0;
uint32_t shown = 1;   // utoljára kiírt szín; 1 = még semmi

const char TYPES[] = "tkjir";
uint32_t rem[5], total[5];

// ---------- Beállítások (webes felület, flash) ----------
struct Cfg { uint16_t out, in, cache; uint8_t bright; bool glow, saver; } cfg;

void loadCfg() {
  prefs.begin("cfg", true);
  cfg.out    = prefs.getUShort("out", 10);
  cfg.in     = prefs.getUShort("in", 100);
  cfg.cache  = prefs.getUShort("cache", 0);
  cfg.bright = prefs.getUChar("bright", 120);
  cfg.glow   = prefs.getBool("glow", false);
  cfg.saver  = prefs.getBool("saver", true);
  prefs.end();
}

void saveCfg() {
  prefs.begin("cfg", false);
  prefs.putUShort("out", cfg.out);
  prefs.putUShort("in", cfg.in);
  prefs.putUShort("cache", cfg.cache);
  prefs.putUChar("bright", cfg.bright);
  prefs.putBool("glow", cfg.glow);
  prefs.putBool("saver", cfg.saver);
  prefs.end();
}

void applyCfg() {
  px.setBrightness(cfg.bright);
  shown = 1;   // új fényerő: a következő színt mindenképp kiírja
  if (!apOn) WiFi.setSleep(cfg.saver);
}

// ---------- LED ----------
void led(uint32_t c) {
  if (c == shown) return;
  shown = c;
  px.setPixelColor(0, c);
  px.show();
}

uint32_t color(char type) {
  switch (type) {
    case 'k': return px.Color(150, 0, 255);  // thinking
    case 'j': return px.Color(0, 255, 60);   // tool input
    case 'i': return px.Color(255, 90, 0);   // új bemenet
    case 'r': return px.Color(0, 0, 120);    // cache olvasás
    default:  return px.Color(0, 180, 255);  // szöveg
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

// Villanások lejátszása; minél több vár, annál gyorsabb.
void play() {
  uint32_t now = millis();
  if (state != IDLE && (int32_t)(now - until) < 0) return;

  if (state == ON) {
    led(0);
    int off = constrain(80 - pending * 2, 20, 80);
    if (!qCount || q[qHead].type != curType) off += 150;  // szünet típusváltáskor
    state = OFF; until = now + off;
    return;
  }

  if (!qCount) {
    state = IDLE;
    uint32_t c = 0;
    bool dot = now % 3000 < 40;
    if (cfg.glow && lastMsg && now - lastMsg < ALIVE_MS) c = px.Color(0, 6, 10);
    else if (apOn && dot) c = px.Color(40, 25, 0);
    else if (!netUp && dot) c = px.Color(40, 0, 0);
    led(c);
    return;
  }

  Item& it = q[qHead];
  curType = it.type;
  led(color(curType));
  if (--it.n == 0) { qHead = (qHead + 1) % QUEUE_LEN; qCount--; }
  pending--;
  state = ON; until = now + (pending > 30 ? 20 : 35);
}

// ---------- Bemenet ----------
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

// ---------- WiFi: kliens, és beállító AP, amíg nincs hálózat ----------
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
  WiFi.setAutoReconnect(false);   // amíg az AP fut, az STA ne keressen: a csatornaváltás ledobná a telefont
  WiFi.disconnect();
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);
  int n = WiFi.scanNetworks();    // egyszer, mielőtt bárki csatlakozna
  ssidOptions = "";
  for (int i = 0; i < n; i++) ssidOptions += "<option value=\"" + esc(WiFi.SSID(i)) + "\">";
  WiFi.scanDelete();
  WiFi.softAP(apName.c_str());
  dns.start(53, "*", WiFi.softAPIP());   // minden név ide mutat: a telefon feldobja a beállító oldalt
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
  reconnectAt = millis() + 1000;   // a webes válasz még kimenjen, utána vált
}

void network() {
  uint32_t now = millis();
  if (reconnectAt && (int32_t)(now - reconnectAt) >= 0) {   // új WiFi-adatok
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
  if (!up && netUp) staSince = now;   // kapcsolat elveszett: innen számol
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

// ---------- Webes felület ----------
const char PAGE_HEAD[] PROGMEM = R"(<!doctype html><html lang=hu><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1"><title>Token LED</title>
<style>body{font:15px system-ui,sans-serif;max-width:440px;margin:16px auto;padding:0 16px;background:#141414;color:#ddd}
h2{margin:8px 0 16px}h3{margin:24px 0 8px;font-size:15px;color:#999}a{color:#6bf}
label{display:flex;justify-content:space-between;align-items:center;gap:12px;margin:10px 0}
input:not([type=checkbox]){width:90px;padding:4px;background:#222;color:#eee;border:1px solid #444;border-radius:4px}
input[name=ssid],input[name=pass]{width:180px}
button{padding:8px 16px;margin:12px 8px 0 0;background:#333;color:#eee;border:1px solid #555;border-radius:6px}
.d{display:inline-block;width:10px;height:10px;border-radius:50%;margin-right:3px}
.box{border:1px solid #a80;border-radius:8px;padding:4px 12px 12px;margin-bottom:16px}
summary{cursor:pointer;color:#999;margin-top:24px}
table{width:100%;border-collapse:collapse}td{padding:3px 0}td+td{text-align:right;font-variant-numeric:tabular-nums}
small{color:#888}</style><h2>Token LED</h2>)";

String dot(const char* c) { return String("<i class=d style=background:") + c + "></i>"; }

String numField(const char* name, const String& label, long v, long lo, long hi) {
  return "<label><span>" + label + "</span><input type=number name=" + name + " min=" + lo + " max=" + hi +
         " value=" + v + "></label>";
}

String checkField(const char* name, const char* label, bool v) {
  return String("<label><span>") + label + "</span><input type=checkbox name=" + name + (v ? " checked" : "") + "></label>";
}

String wifiForm() {
  return "<form method=post action=/wifi><label><span>Hálózat (2,4 GHz)</span><input name=ssid list=ssids required value=\"" +
         esc(savedSsid) + "\"></label><datalist id=ssids>" + ssidOptions + "</datalist>"
         "<label><span>Jelszó</span><input type=password name=pass></label><button>Csatlakozás</button></form>";
}

void handleRoot() {
  uint32_t ago = (millis() - lastMsg) / 1000;
  String s = FPSTR(PAGE_HEAD);
  if (apOn) s += "<div class=box><h3>WiFi beállítása</h3>" + wifiForm() + "</div>";
  s += "<form method=post action=/save>";
  s += numField("out", dot("#0bf") + dot("#90f") + dot("#0f4") + "Szöveg, thinking, tool: token / villanás", cfg.out, 0, 60000);
  s += numField("in", dot("#f60") + "Új bemenet: token / villanás", cfg.in, 0, 60000);
  s += numField("cache", dot("#22a") + "Cache olvasás: token / villanás", cfg.cache, 0, 60000);
  s += "<small>0 = nem villog erre a típusra.</small>";
  s += numField("bright", "Fényerő (1–255)", cfg.bright, 1, 255);
  s += checkField("glow", "Halvány derengés, amíg a Claude fut", cfg.glow);
  s += checkField("saver", "Energiatakarékos WiFi (elemhez; kikapcsolva gyorsabb)", cfg.saver);
  s += "<button>Mentés</button></form><form method=post action=/test><button>Színteszt</button></form>";
  s += "<h3>Állapot</h3><table>";
  if (apOn) s += "<tr><td>Beállító hálózat</td><td>" + apName + "</td></tr>";
  if (netUp) {
    s += "<tr><td>WiFi</td><td>" + esc(WiFi.SSID()) + "</td></tr>";
    s += "<tr><td>IP</td><td>" + WiFi.localIP().toString() + "</td></tr>";
    s += "<tr><td>WiFi jel</td><td>" + String(WiFi.RSSI()) + " dBm</td></tr>";
  }
  s += "<tr><td>Üzemidő</td><td>" + String(millis() / 60000) + " perc</td></tr>";
  s += "<tr><td>Utolsó üzenet a PC-ről</td><td>" + (lastMsg ? String(ago) + " s" : String("még nem jött")) + "</td></tr>";
  s += "<tr><td>Villanás a sorban</td><td>" + String(pending) + "</td></tr>";
  const char* names[] = {"Szöveg", "Thinking", "Tool input", "Új bemenet", "Cache olvasás"};
  for (int i = 0; i < 5; i++) s += String("<tr><td>") + names[i] + " (bekapcsolás óta)</td><td>" + total[i] + "</td></tr>";
  s += "</table>";
  if (!apOn) s += "<details><summary>WiFi-hálózat módosítása</summary>" + wifiForm() +
                  "<small>Ha az új hálózatra 30 s alatt nem csatlakozik, beállító hálózatot indít: " + apName + "</small></details>";
  http.send(200, "text/html; charset=utf-8", s);
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
  saveCfg();
  applyCfg();
  redirectHome();
}

void handleTest() {
  for (const char* t = TYPES; *t; t++) enqueue(*t, 3);
  redirectHome();
}

void handleWifi() {
  String ssid = http.arg("ssid");
  ssid.trim();
  if (!ssid.length()) { redirectHome(); return; }
  storeWifi(ssid, http.arg("pass"));
  String s = FPSTR(PAGE_HEAD);
  s += "<p>Mentve, csatlakozás ide: <b>" + esc(ssid) + "</b></p>"
       "<p>Ha sikerül, a beállító hálózat eltűnik, és a LED itt érhető el: "
       "<a href=http://tokenled.local>http://tokenled.local</a></p>"
       "<p><small>Ha 30 s alatt nem sikerül, a beállító hálózat (" + apName + ") visszajön.</small></p>";
  http.send(200, "text/html; charset=utf-8", s);
}

// Beállító módban minden ismeretlen cím (a telefonok hálózatellenőrzése is) a beállító oldalra visz.
void handleNotFound() {
  if (!apOn) { http.send(404, "text/plain", "Nincs ilyen oldal"); return; }
  http.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/");
  http.send(302);
}

void setup() {
  setCpuFrequencyMhz(80);     // elemről: kevesebb fogyasztás, a WiFi-nek ennyi elég
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);   // USB nélkül se akadjon meg a kiírás
  loadCfg();
  px.begin();
  px.setBrightness(cfg.bright);
  const uint32_t test[] = {px.Color(255, 0, 0), px.Color(0, 255, 0), px.Color(0, 0, 255), 0};
  for (uint32_t c : test) { led(c); delay(250); }

  prefs.begin("wifi", true);
  savedSsid = prefs.getString("ssid");
  savedPass = prefs.getString("pass");
  prefs.end();

  WiFi.persistent(false);     // a WiFi-adatokat mi tároljuk (Preferences)
  WiFi.setHostname(HOSTNAME); // a mode() előtt kell, különben nem érvényesül
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(cfg.saver);   // modem sleep: jóval kisebb fogyasztás, cserébe pár száz ms késés
  String mac = WiFi.macAddress();
  apName = "TokenLED-" + mac.substring(12, 14) + mac.substring(15);

  http.on("/", handleRoot);
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
