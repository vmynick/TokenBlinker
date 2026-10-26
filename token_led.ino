// Claude token LED — ESP32-C3 + 2x WS2812
// LED0 = claude.ai (narancs), LED1 = Claude Code (cián=szöveg, lila=thinking, zöld=tool)
// NUM_LEDS 1 esetén a két forrás egy LED-en keveredik.
// Bemenet: UDP 4210 "C t 12" / "W t 12"  |  HTTP GET /p?s=w&t=t&n=12
// Könyvtár: Adafruit NeoPixel

#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Adafruit_NeoPixel.h>

const char* WIFI_SSID = "SSID";
const char* WIFI_PASS = "JELSZO";

#define LED_PIN    8      // WS2812 adat (C3-DevKitM beépített RGB: GPIO8)
#define NUM_LEDS   2
#define UDP_PORT   4210
#define MAX_BRIGHT 120    // 0-255, asztalra ennyi bőven elég
#define DECAY      0.90f  // 10 ms-onként; kisebb = rövidebb villanás

Adafruit_NeoPixel px(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);
WiFiUDP udp;
WebServer http(80);

struct Ch { float e; uint8_t r, g, b; };
Ch chWeb  {0, 255, 90, 0};
Ch chCode {0, 0, 180, 255};

void pulse(char src, char type, int n) {
  if (n <= 0) return;
  Ch& c = (src == 'C' || src == 'c') ? chCode : chWeb;
  if (&c == &chCode) {
    switch (type) {
      case 'k': c.r = 150; c.g = 0;   c.b = 255; break;  // thinking
      case 'j': c.r = 0;   c.g = 255; c.b = 60;  break;  // tool input
      default:  c.r = 0;   c.g = 180; c.b = 255; break;  // szöveg
    }
  }
  c.e = min(c.e + 40.0f + n * 8.0f, 255.0f);
}

uint32_t col(const Ch& c, float s) {
  return px.Color(c.r * s, c.g * s, c.b * s);
}

void render() {
  float sw = chWeb.e / 255.0f, sc = chCode.e / 255.0f;
  if (NUM_LEDS >= 2) {
    px.setPixelColor(0, col(chWeb, sw));
    px.setPixelColor(1, col(chCode, sc));
  } else {
    px.setPixelColor(0, px.Color(
      min(255.0f, chWeb.r * sw + chCode.r * sc),
      min(255.0f, chWeb.g * sw + chCode.g * sc),
      min(255.0f, chWeb.b * sw + chCode.b * sc)));
  }
  px.show();
  chWeb.e  = chWeb.e  < 1 ? 0 : chWeb.e  * DECAY;
  chCode.e = chCode.e < 1 ? 0 : chCode.e * DECAY;
}

void handleHttp() {
  String s = http.arg("s"), t = http.arg("t");
  pulse(s.length() ? s[0] : 'W', t.length() ? t[0] : 't', http.arg("n").toInt());
  http.sendHeader("Access-Control-Allow-Origin", "*");
  http.send(204);
}

void setup() {
  px.begin();
  px.setBrightness(MAX_BRIGHT);
  px.clear(); px.show();

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    px.setPixelColor(0, px.Color(40, 0, 0)); px.show(); delay(150);
    px.clear(); px.show(); delay(150);
  }
  MDNS.begin("tokenled");               // http://tokenled.local
  MDNS.addService("http", "tcp", 80);
  udp.begin(UDP_PORT);
  http.on("/p", handleHttp);
  http.begin();
}

void loop() {
  int len;
  while ((len = udp.parsePacket()) > 0) {
    char buf[32];
    int r = udp.read(buf, sizeof(buf) - 1);
    buf[r > 0 ? r : 0] = 0;
    char src = 0, type = 't'; int n = 0;
    if (sscanf(buf, "%c %c %d", &src, &type, &n) == 3) pulse(src, type, n);
  }
  http.handleClient();

  static uint32_t last = 0;
  if (millis() - last >= 10) { last = millis(); render(); }
}
