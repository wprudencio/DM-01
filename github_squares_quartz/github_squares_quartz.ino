// GitHub Squares — ESP32-C3 + ST7735 160x128.
//
// Sister of btc_ticker: same WiFi/portal/fetch pipeline, rendered in the
// SIGNAL/VHS theme — near-black deck, white ink, light-grey rules, a magenta
// accent strip and a signal-green live pip, with per-element VHS glitch hits.
// The chart is the public GitHub contribution calendar drawn as squares that
// ramp dim grey -> light grey -> white -> signal green. No API key: data comes
// from the keyless JSON mirror github-contributions-api.jogruber.de (365 days
// of date/count/level) with a deterministic demo year when offline. The portal
// (NVS "dm01", shared with the other sketches) stores WiFi + the GitHub
// username.
//
// Controls: tap pad (GPIO 0) / BOOT (GPIO 9) = refresh now, hold >= 1.5 s =
// WiFi/user setup portal. WS2812 (GPIO 10) pulses while syncing and breathes
// once data is live.
//
// WiFi reliability follows the esp32-c3-ws2812 skill §10 (8.5 dBm TX, sleep
// off, hard-reset retries, begin() never called while a connect is pending).

#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <Adafruit_NeoPixel.h>
#include <SPI.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <math.h>
#include "Screenshot.h"

// ── Pins (ESP32-C3 + ST7735) ──
#define TFT_CS    5
#define TFT_RST   4
#define TFT_DC    3
#define TFT_MOSI  2
#define TFT_SCLK  1
#define TOUCH_PIN 0
#define BOOT_PIN  9
#define LED_PIN   10
#define NUMPIXELS 1

Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_RST);
Adafruit_NeoPixel pixels(NUMPIXELS, LED_PIN, NEO_GRB + NEO_KHZ800);

#define WIDTH  160
#define HEIGHT 128

// ── Framebuffer (full frame, one atomic flush) ──
static uint16_t fb[WIDTH * HEIGHT];
#define FPIX(x,y,c) if((x)>=0&&(x)<WIDTH&&(y)>=0&&(y)<HEIGHT) fb[(y)*WIDTH+(x)]=(c)

void fbClear(uint16_t c) {
  for (int i = 0; i < WIDTH * HEIGHT; i++) fb[i] = c;
}
void fbHLine(int x, int y, int w, uint16_t c) {
  if (y < 0 || y >= HEIGHT) return;
  if (x < 0) { w += x; x = 0; }
  if (x + w > WIDTH) w = WIDTH - x;
  if (w <= 0) return;
  uint16_t* p = &fb[y * WIDTH + x];
  while (w--) *p++ = c;
}
void fbFillRect(int x, int y, int w, int h, uint16_t c) {
  for (int i = 0; i < h; i++) fbHLine(x, y + i, w, c);
}
void fbRect(int x, int y, int w, int h, uint16_t c) {
  fbHLine(x, y, w, c);
  fbHLine(x, y + h - 1, w, c);
  if (h > 2) {
    for (int i = 1; i < h - 1; i++) { FPIX(x, y + i, c); FPIX(x + w - 1, y + i, c); }
  }
}
void fbFillCircle(int cx, int cy, int r, uint16_t c) {
  int x = 0, y = r, d = 3 - 2 * r;
  while (x <= y) {
    fbHLine(cx - x, cy - y, 2 * x + 1, c);
    fbHLine(cx - x, cy + y, 2 * x + 1, c);
    fbHLine(cx - y, cy - x, 2 * y + 1, c);
    fbHLine(cx - y, cy + x, 2 * y + 1, c);
    if (d < 0) d += 4 * x + 6;
    else { d += 4 * (x - y) + 10; y--; }
    x++;
  }
}
void fbDrawLine(int x0, int y0, int x1, int y1, uint16_t c) {
  int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int err = dx + dy, e2;
  for (;;) {
    FPIX(x0, y0, c);
    if (x0 == x1 && y0 == y1) break;
    e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}
void fbFlush() {
  tft.startWrite();
  tft.setAddrWindow(0, 0, WIDTH, HEIGHT);
  tft.writePixels(fb, WIDTH * HEIGHT);
  tft.endWrite();
}

uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) { return tft.color565(r, g, b); }

// ── QUARTZ palette — light LCD only, no state switching ──
uint16_t panelBG()    { return rgb(148, 158, 130); } // light LCD green-grey
uint16_t panelInk()   { return rgb( 28,  34,  24); } // dark ink
uint16_t panelGhost() { return rgb(138, 148, 120); } // unlit segments / dot grid
uint16_t panelHI()    { return rgb(178, 186, 160); } // light inner rule
uint16_t panelEdge()  { return rgb( 96, 104,  82); } // dark olive rules
uint16_t accent()     { return rgb(200,  40,  40); } // alarm red
uint16_t accentDK()   { return rgb(120,  24,  24); } // dark red

// GitHub contribution levels mapped onto the SIGNAL ramp:
// empty -> dim grey -> light grey -> white -> signal green (strongest).
// contribution ramp: a proper green scale (the grey->white ramp read dull)
uint16_t ghLvl(uint8_t lvl) {
  switch (lvl) {
    case 1:  return rgb(155, 233, 168); // GitHub #9BE9A8
    case 2:  return rgb( 64, 196,  99); // GitHub #40C463
    case 3:  return rgb( 48, 161,  78); // GitHub #30A14E
    case 4:  return rgb( 33, 110,  57); // GitHub #216E39
    default: return rgb(168, 176, 150); // empty cell, quiet on the LCD
  }
}

// ── 4x5 bitmap font (labels) — the QUARTZ label face. MSB = left column,
//    5px advance at scale 1. ──
static const uint8_t gDig45[10][5] = {
  {0b0110,0b1001,0b1001,0b1001,0b0110},{0b0010,0b0110,0b0010,0b0010,0b0111},
  {0b0110,0b1001,0b0010,0b0100,0b1111},{0b0110,0b1001,0b0010,0b1001,0b0110},
  {0b1001,0b1001,0b1111,0b0001,0b0001},{0b1111,0b1000,0b1110,0b0001,0b1110},
  {0b0110,0b1000,0b1110,0b1001,0b0110},{0b1111,0b0001,0b0010,0b0100,0b0100},
  {0b0110,0b1001,0b0110,0b1001,0b0110},{0b0110,0b1001,0b0111,0b0001,0b0110},
};
static const uint8_t gLet45[26][5] = {
  {0b0110,0b1001,0b1111,0b1001,0b1001},{0b1110,0b1001,0b1110,0b1001,0b1110},
  {0b0110,0b1001,0b1000,0b1001,0b0110},{0b1100,0b1010,0b1001,0b1010,0b1100},
  {0b1111,0b1000,0b1110,0b1000,0b1111},{0b1111,0b1000,0b1110,0b1000,0b1000},
  {0b0110,0b1000,0b1011,0b1001,0b0110},{0b1001,0b1001,0b1111,0b1001,0b1001},
  {0b0111,0b0010,0b0010,0b0010,0b0111},{0b0011,0b0001,0b0001,0b1001,0b0110},
  {0b1001,0b1010,0b1100,0b1010,0b1001},{0b1000,0b1000,0b1000,0b1000,0b1111},
  {0b1001,0b1111,0b1111,0b1001,0b1001},{0b1001,0b1101,0b1011,0b1001,0b1001},
  {0b0110,0b1001,0b1001,0b1001,0b0110},{0b1110,0b1001,0b1110,0b1000,0b1000},
  {0b0110,0b1001,0b1001,0b1010,0b0101},{0b1110,0b1001,0b1110,0b1010,0b1001},
  {0b0111,0b1000,0b0110,0b0001,0b1110},{0b1111,0b0010,0b0010,0b0010,0b0010},
  {0b1001,0b1001,0b1001,0b1001,0b0110},{0b1001,0b1001,0b1001,0b0110,0b0110},
  {0b1001,0b1001,0b1111,0b1111,0b1001},{0b1001,0b1001,0b0110,0b1001,0b1001},
  {0b1001,0b1001,0b0110,0b0010,0b0010},{0b1111,0b0001,0b0110,0b1000,0b1111},
};
static const uint8_t gExcl45[5]  = {0b0100,0b0100,0b0100,0b0000,0b0100};
static const uint8_t gColon45[5] = {0b0000,0b0100,0b0000,0b0100,0b0000};
static const uint8_t gDot45[5]   = {0b0000,0b0000,0b0000,0b0000,0b0100};
static const uint8_t gQuest45[5] = {0b0110,0b1001,0b0010,0b0000,0b0010};
static const uint8_t gDash45[5]  = {0b0000,0b0000,0b0111,0b0000,0b0000};
static const uint8_t gPlus45[5]  = {0b0000,0b0100,0b1110,0b0100,0b0000};

static inline const uint8_t* glyph45(char ch) {
  if (ch >= '0' && ch <= '9') return gDig45[ch - '0'];
  if (ch >= 'A' && ch <= 'Z') return gLet45[ch - 'A'];
  if (ch == '!') return gExcl45;
  if (ch == ':') return gColon45;
  if (ch == '.') return gDot45;
  if (ch == '?') return gQuest45;
  if (ch == '-') return gDash45;
  if (ch == '+') return gPlus45;
  return gQuest45;
}
void fbChar(int x, int y, char ch, uint16_t c, int scale) {
  if (ch == ' ') return;
  if (ch >= 'a' && ch <= 'z') ch -= 32;
  const uint8_t* g = glyph45(ch);
  for (int row = 0; row < 5; row++) {
    uint8_t bits = g[row];
    for (int col = 0; col < 4; col++)
      if (bits & (0b1000 >> col))
        fbFillRect(x + col * scale, y + row * scale, scale, scale, c);
  }
}
void fbText(int x, int y, const char* s, uint16_t c, int scale = 1) {
  while (*s) { fbChar(x, y, *s, c, scale); x += 5 * scale; s++; }
}
int textW57(const char* s, int scale = 1) {
  int n = 0; while (*s++) n++;
  return n ? n * 5 * scale - scale : 0;
}
void fbText11(int x, int y, const char* s, uint16_t c) { fbText(x, y, s, c, 2); }
int textW11(const char* s) { return textW57(s, 2); }


// ── Layout (skill §8) — C3 mirror of the CYD proportions ──
#define HDR_Y 3
#define ACC_Y 10          // dark line; red bar 11..14
#define CELL 4
#define CELL_GAP 1
#define WEEKS 30
#define GRID_W (WEEKS * (CELL + CELL_GAP) - CELL_GAP)   // 149
#define GRID_H (7 * (CELL + CELL_GAP) - CELL_GAP)       // 34
#define GRID_X ((WIDTH - GRID_W) / 2)                   // 5
#define GRID_Y 38
#define TOT_LBL_Y 77   // cap top, clear of the block digits below
#define TOT_Y 85       // scale-3 block digits (24px ink)
#define STAT_LBL_Y 18  // small labels, baseline-aligned with the values
#define STAT_NUM_Y 16  // 11px raster at 1x (8px ink)

// ── Config + network state (NVS "dm01", shared with the other sketches) ──
String ghUser = "wprudencio";
enum NetMode : uint8_t { NET_BOOT, NET_CONNECTING, NET_UP, NET_OFFLINE, NET_PORTAL };
NetMode netMode = NET_BOOT;
bool bootDone = false;
unsigned long lastReconnectMs = 0;
#define RECONNECT_MS 15000UL

const char* AP_SSID = "GH-SQUARES-Setup";
WebServer server(80);
DNSServer dnsServer;
#define DNS_PORT 53
Preferences preferences;
String wifiSSID = "";
String wifiPassword = "";

// ── Contribution data ──
#define DAYS_MAX 380
struct GhDay { int32_t days; uint16_t count; uint8_t level; };
GhDay ghDays[DAYS_MAX];
int ghCount = 0;
int ghTotal = 0;
int ghStreak = 0;      // current streak (days)
int ghBest = 0;        // longest streak
bool dataIsLive = false;
bool gettingData = true;
unsigned long lastDataMs = 0;
unsigned long lastAttemptMs = 0;
bool fetching = false;
bool fetchQueued = false;

static inline bool blinkOn(unsigned long t) { return (t / 380) % 2 == 0; }

// Howard Hinnant days-from-civil: 1970-01-01 = 0, (days+4)%7 = 0 => Sunday
static int32_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int32_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int32_t)doe - 719468;
}

void computeStats() {
  ghTotal = 0;
  for (int i = 0; i < ghCount; i++) ghTotal += ghDays[i].count;
  ghStreak = 0;
  int i = ghCount - 1;
  if (i >= 0 && ghDays[i].count == 0) i--;   // today may still be in progress
  while (i >= 0 && ghDays[i].count > 0) { ghStreak++; i--; }
  ghBest = 0;
  int run = 0;
  for (int k = 0; k < ghCount; k++) {
    if (ghDays[k].count > 0) { run++; if (run > ghBest) ghBest = run; }
    else run = 0;
  }
}

// ── Demo data (deterministic year used until the first live fetch) ──
void loadDemoData() {
  static const uint16_t cntByLvl[5] = {0, 1, 3, 7, 15};
  const int32_t endDays = 20001;             // a Saturday → full last column
  int32_t start = endDays - 364;
  ghCount = 0;
  int run = 0;
  for (int i = 0; i < 365; i++) {
    int32_t d = start + i;
    int wd = (int)((d + 4) % 7);             // 0 = Sunday
    uint32_t h = (uint32_t)d * 2654435761u;
    h ^= h >> 16; h *= 0x7feb352dU; h ^= h >> 15; h *= 0x846ca68bU; h ^= h >> 16;
    int r = h % 100;
    int quiet = (wd == 0 || wd == 6) ? 62 : 38;   // weekends lighter
    if (run > 0) quiet += 8;                      // momentum while hot
    uint8_t lvl = 0;
    if (r >= quiet) {
      int q = (r - quiet) * 100 / (100 - quiet);
      lvl = q < 45 ? 1 : (q < 70 ? 2 : (q < 88 ? 3 : 4));
      run++;
    } else run = 0;
    ghDays[ghCount].days = d;
    ghDays[ghCount].level = lvl;
    ghDays[ghCount].count = cntByLvl[lvl];
    ghCount++;
  }
  dataIsLive = false;
  computeStats();
}

// ── HTTP ──
bool httpGet(const String& url, String& out, int* code) {
  HTTPClient http;
  http.setTimeout(10000);
  http.setConnectTimeout(6000);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  if (!http.begin(url)) { if (code) *code = 0; return false; }
  int c = http.GET();
  if (code) *code = c;
  bool ok = false;
  if (c == 200) { out = http.getString(); ok = true; }
  else if (c > 0) { http.getString(); }
  http.end();
  return ok;
}

bool parseContribs(const String& json) {
  const char* s = json.c_str();
  int n = 0;
  int pos = 0;
  while (n < DAYS_MAX) {
    int d = json.indexOf("\"date\":\"", pos);
    if (d < 0) break;
    int ds = d + 8;
    if (ds + 10 > (int)json.length()) break;
    int lv = json.indexOf("\"level\":", ds);
    if (lv < 0) break;
    int cn = json.indexOf("\"count\":", ds);
    if (cn < 0) break;
    int objEnd = json.indexOf('}', ds);
    if (objEnd < 0) break;
    int y = atoi(s + ds);
    int m = atoi(s + ds + 5);
    int dd = atoi(s + ds + 8);
    int lvl = atoi(s + lv + 8);
    int cnt = atoi(s + cn + 8);
    if (y >= 2020 && y <= 2100 && m >= 1 && m <= 12 && dd >= 1 && dd <= 31) {
      ghDays[n].days = daysFromCivil(y, (unsigned)m, (unsigned)dd);
      ghDays[n].level = (uint8_t)(lvl < 0 ? 0 : (lvl > 4 ? 4 : lvl));
      ghDays[n].count = (uint16_t)(cnt < 0 ? 0 : (cnt > 65535 ? 65535 : cnt));
      n++;
    }
    pos = objEnd + 1;
  }
  if (n < 14) return false;
  ghCount = n;
  dataIsLive = true;
  gettingData = false;
  lastDataMs = millis();
  computeStats();
  Serial.printf("[gh] days=%d total=%d streak=%d best=%d\n", ghCount, ghTotal, ghStreak, ghBest);
  return true;
}

// ── Fetch scheduling (contributions change slowly) ──
#define REFRESH_MS 900000UL   // 15 min
#define RETRY_MS   60000UL

void startFetch() {
  if (fetching) { fetchQueued = true; return; }
  fetching = true;
  lastAttemptMs = millis();
  Serial.printf("[gh] fetch %s\n", ghUser.c_str());
}

bool continueFetch() {
  String payload;
  int code = 0;
  String url = String("https://github-contributions-api.jogruber.de/v4/") + ghUser + "?y=last";
  bool got = httpGet(url, payload, &code);
  if (got && parseContribs(payload)) {
    fetching = false;
    if (fetchQueued) { fetchQueued = false; startFetch(); }
    return true;
  }
  Serial.printf("[gh] fetch failed http=%d\n", code);
  fetching = false;
  if (gettingData) { loadDemoData(); gettingData = false; }
  return true;
}

void updateFetch() {
  if (netMode == NET_PORTAL) return;
  if (fetching) return;
  if (netMode != NET_UP) return;   // offline: the demo year keeps rendering
  unsigned long base = lastAttemptMs > lastDataMs ? lastAttemptMs : lastDataMs;
  unsigned long wait = lastDataMs ? REFRESH_MS : RETRY_MS;
  if (millis() - base >= wait) startFetch();
}

// ── WiFi (esp32-c3-ws2812 skill §10) ──
void wifiPrepare(bool hardReset, bool doScan) {
  static bool eventsRegistered = false;
  if (!eventsRegistered) {
    eventsRegistered = true;
    WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
      if (event == ARDUINO_EVENT_WIFI_STA_CONNECTED) {
        Serial.println("WiFi associated");
      } else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
        Serial.printf("WiFi disconnected reason=%d\n", (int)info.wifi_sta_disconnected.reason);
      } else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
        Serial.println("WiFi got IP");
      }
    });
  }

  if (hardReset) {
    WiFi.persistent(false);
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);
    delay(200);
  }

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  WiFi.setAutoReconnect(false);

  if (hardReset && doScan) {
    int16_t best = -1;
    int32_t bestRssi = -127;
    int16_t n = WiFi.scanNetworks(false, false, false, 120);
    for (int16_t i = 0; i < n; i++) {
      if (WiFi.SSID(i) == wifiSSID && (int32_t)WiFi.RSSI(i) > bestRssi) { best = i; bestRssi = WiFi.RSSI(i); }
    }
    if (best >= 0) Serial.printf("WiFi visible ch=%d rssi=%d\n", (int)WiFi.channel(best), (int)bestRssi);
    else Serial.println("WiFi SSID not visible");
    WiFi.scanDelete();
  }
}

bool wifiConnect(uint32_t timeoutMs, bool hardReset) {
  wifiPrepare(hardReset, hardReset);
  Serial.printf("WiFi begin ssid=\"%s\" -> %d\n", wifiSSID.c_str(), (int)WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str()));

  unsigned long t0 = millis();
  wl_status_t last = WL_IDLE_STATUS;
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) {
    wl_status_t st = WiFi.status();
    if (st != last) {
      Serial.printf("WiFi status %d -> %d at %lums\n", (int)last, (int)st, millis() - t0);
      last = st;
    }
    delay(250);
  }
  Serial.printf("WiFi result=%d after %lums\n", (int)WiFi.status(), millis() - t0);
  return WiFi.status() == WL_CONNECTED;
}

// Non-blocking retry: prepare + begin, loop() polls the status so the demo
// year keeps rendering while the radio tries to come back.
void wifiRetryStart() {
  wifiPrepare(true, true);   // scan logs whether the stored SSID is visible
  Serial.printf("WiFi retry ssid=\"%s\" -> %d\n", wifiSSID.c_str(), (int)WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str()));
}

void loadCredentials() {
  preferences.begin("dm01", true);
  wifiSSID = preferences.getString("ssid", "");
  wifiPassword = preferences.getString("password", "");
  String u = preferences.getString("ghuser", "");
  preferences.end();
  if (u.length()) ghUser = u;

  if (wifiSSID.length() == 0) { // migrate the pre-DM-01 namespace
    preferences.begin("mydock", true);
    String s = preferences.getString("ssid", "");
    String p = preferences.getString("password", "");
    preferences.end();
    if (s.length() > 0) {
      preferences.begin("dm01", false);
      preferences.putString("ssid", s);
      preferences.putString("password", p);
      preferences.end();
      wifiSSID = s;
      wifiPassword = p;
      Serial.println("migrated WiFi credentials from mydock");
    }
  }
}

// ── Captive portal (NVS "dm01") ──
String portalPage() {
  String html = R"HTML(<!DOCTYPE html><html><head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>GitHub Squares Setup</title>
<style>
  body{background:#07050f;color:#f4eeff;font-family:ui-monospace,SFMono-Regular,Menlo,monospace;margin:0;padding:24px}
  main{max-width:360px;margin:0 auto}
  h1{color:#40c463;letter-spacing:.15em;margin:0 0 4px}
  p{color:#746a9b;margin:0 0 24px;font-size:14px}
  label{display:block;font-size:12px;color:#b3a8cf;margin:14px 0 6px;text-transform:uppercase;letter-spacing:.1em}
  input{width:100%;box-sizing:border-box;padding:12px;background:#150e28;border:1px solid #3a2a5e;color:#f4eeff;border-radius:4px}
  button{width:100%;margin-top:22px;padding:13px;border:0;border-radius:4px;font-weight:bold;letter-spacing:.1em;color:#07050f;background:linear-gradient(90deg,#40c463,#9be9a8);cursor:pointer}
</style></head><body><main>
<h1>GITHUB SQUARES</h1>
<p>WiFi + GitHub user &mdash; saved to NVS, device reboots.</p>
<form action="/save" method="POST">
  <label for="ssid">WiFi name</label>
  <input type="text" id="ssid" name="ssid" placeholder="SSID" required>
  <label for="password">Password</label>
  <input type="password" id="password" name="password" placeholder="password">
  <label for="user">GitHub user</label>
  <input type="text" id="user" name="user" maxlength="39" placeholder="octocat">
  <button type="submit">Save</button>
</form>
</main></body></html>)HTML";
  return html;
}

void handleRoot() {
  server.send(200, "text/html", portalPage());
}

void handleSave() {
  String ssid = server.arg("ssid");
  String password = server.arg("password");
  String user = server.arg("user");
  user.trim();
  if (user.length() == 0) user = "wprudencio";
  if (ssid.length() == 0) {
    server.send(200, "text/html", "SSID required. <a href='/'>Back</a>");
    return;
  }
  preferences.begin("dm01", false);
  preferences.putString("ssid", ssid);
  preferences.putString("password", password);
  preferences.putString("ghuser", user);
  preferences.end();
  server.send(200, "text/html",
    "<html><head><meta name='viewport' content='width=device-width,initial-scale=1'></head>"
    "<body style='background:#07050f;color:#f4eeff;font-family:monospace;text-align:center;padding:60px'>"
    "<h2 style='color:#40c463'>SAVED</h2><p>Rebooting...</p></body></html>");
  delay(900);
  ESP.restart();
}

void startPortal() {
  netMode = NET_PORTAL;
  WiFi.persistent(false);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID);
  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.onNotFound(handleRoot);
  server.begin();
  Serial.print("portal AP=");
  Serial.print(AP_SSID);
  Serial.print(" ip=");
  Serial.println(WiFi.softAPIP());
}

void onWifiUp() {
  netMode = NET_UP;
  lastReconnectMs = millis();
  Serial.printf("WiFi OK IP=%s RSSI=%d\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
  startFetch();
}

void netWatchdog() {
  if (netMode == NET_UP) {
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi lost -> demo");
      netMode = NET_OFFLINE;
      dataIsLive = false;
      if (gettingData) { loadDemoData(); gettingData = false; }
      lastReconnectMs = millis();
    }
  } else if (netMode == NET_OFFLINE) {
    if (millis() - lastReconnectMs > RECONNECT_MS) {
      netMode = NET_CONNECTING;
      lastReconnectMs = millis();
      wifiRetryStart();
    }
  } else if (netMode == NET_CONNECTING) {
    if (WiFi.status() == WL_CONNECTED) onWifiUp();
    else if (millis() - lastReconnectMs > 15000UL) {
      Serial.println("WiFi retry failed -> offline");
      netMode = NET_OFFLINE;
      lastReconnectMs = millis();
    }
  }
}

// ── Touch: tap = refresh now, hold = setup portal ──
int idleLevel = HIGH;
bool wasPressed = false;
unsigned long pressStartMs = 0;
bool holdFired = false;
#define HOLD_MS 1500

void calibrateTouch() {
  pinMode(BOOT_PIN, INPUT_PULLUP);
  delay(300);
  int highCount = 0;
  for (int i = 0; i < 20; i++) { if (digitalRead(TOUCH_PIN) == HIGH) highCount++; delay(10); }
  idleLevel = (highCount > 10) ? HIGH : LOW;
  Serial.printf("[touch] calibrate highCount=%d idle=%s\n", highCount, idleLevel == HIGH ? "HIGH" : "LOW");
}

static inline bool inputPressed() {
  if (digitalRead(BOOT_PIN) == LOW) return true;
  return digitalRead(TOUCH_PIN) != idleLevel;
}

void onShortTap() {
  Serial.println("[btn] refresh");
  if (fetching) fetchQueued = true;
  else startFetch();
}

void onLongHold() {
  Serial.println("[btn] setup portal");
  startPortal();
}

void handleTouch() {
  if (netMode == NET_PORTAL) return;
  bool pressed = inputPressed();
  unsigned long now = millis();

  if (pressed && !wasPressed) {
    pressStartMs = now;
    wasPressed = true;
    holdFired = false;
  } else if (!pressed && wasPressed) {
    unsigned long dur = now - pressStartMs;
    if (!holdFired && dur >= 60 && dur < HOLD_MS) onShortTap();
    wasPressed = false;
    holdFired = false;
    delay(20);
  } else if (pressed && wasPressed && !holdFired && (now - pressStartMs >= HOLD_MS)) {
    holdFired = true;
    onLongHold();
  }
}

// ── LED: sync pulse / live breathe / demo blink ──
void updateLed() {
  static unsigned long last = 0;
  if (millis() - last < 50) return;
  last = millis();

  if (netMode == NET_PORTAL) {
    float p = (sinf(millis() * 0.004f) + 1.0f) * 0.5f;
    pixels.setBrightness(70);
    pixels.setPixelColor(0, pixels.Color(120 + p * 100, 0, 80 + p * 80));
  } else if (netMode == NET_CONNECTING || netMode == NET_OFFLINE) {
    bool on = (millis() / 400) % 2 == 0;
    pixels.setBrightness(60);
    if (on) pixels.setPixelColor(0, pixels.Color(180, 120, 0));
    else pixels.clear();
  } else if (fetching) {
    float p = (sinf(millis() * 0.006f) + 1.0f) * 0.5f;
    pixels.setBrightness(60);
    pixels.setPixelColor(0, pixels.Color(0, 30, 70 + p * 110));
  } else if (dataIsLive) {
    float p = (sinf(millis() * 0.0018f) + 1.0f) * 0.5f;
    pixels.setBrightness(35);
    pixels.setPixelColor(0, pixels.Color(20, 90 + p * 90, 35));
  } else {
    bool on = (millis() / 600) % 2 == 0;
    pixels.setBrightness(60);
    if (on) pixels.setPixelColor(0, pixels.Color(180, 120, 0));
    else pixels.clear();
  }
  pixels.show();
}

// ── Rendering: SIGNAL/VHS deck + GitHub squares ──
const char* statusText(uint16_t& color) {
  if (fetching || gettingData) { color = blinkOn(millis()) ? panelInk() : panelGhost(); return "SYNC"; }
  if (!dataIsLive) { color = blinkOn(millis()) ? accent() : panelGhost(); return "DEMO"; }
  color = panelInk();
  return "LIVE";
}

// tile: flat SIGNAL-ramp fill; the strongest level gets a white rim
void drawTile(int x, int y, uint8_t lvl) {
  fbFillRect(x, y, CELL, CELL, ghLvl(lvl));
}

void drawGrid(uint16_t empty) {
  if (ghCount <= 0) return;
  int32_t lastWeek = (ghDays[ghCount - 1].days + 4) / 7;
  int32_t firstWeek = lastWeek - WEEKS + 1;
  const int pitch = CELL + CELL_GAP;
  for (int i = 0; i < ghCount; i++) {
    int32_t d = ghDays[i].days;
    int32_t wk = (d + 4) / 7;
    if (wk < firstWeek) continue;
    int col = (int)(wk - firstWeek);
    int row = (int)((d + 4) % 7);          // 0 = Sunday
    if (col >= WEEKS) continue;
    int x = GRID_X + col * pitch, y = GRID_Y + row * pitch;
    if (empty) fbFillRect(x, y, CELL, CELL, empty);
    else drawTile(x, y, ghDays[i].level);
  }
}

// ── 7-seg digits with ghost segments (QUARTZ LCD) ──
static const uint8_t segBits[10] = {
  0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F
};
void sevenSegSeg(int x, int y, int Wc, int Hc, int T, int seg, uint16_t c) {
  int vH = (Hc - 3 * T + 1) / 2, midT = (Hc - T) / 2;
  switch (seg) {
    case 0: fbFillRect(x, y, Wc, T, c); return;
    case 1: fbFillRect(x + Wc - T, y + T, T, vH, c); return;
    case 2: fbFillRect(x + Wc - T, y + midT + T, T, vH, c); return;
    case 3: fbFillRect(x, y + Hc - T, Wc, T, c); return;
    case 4: fbFillRect(x, y + midT + T, T, vH, c); return;
    case 5: fbFillRect(x, y + T, T, vH, c); return;
    case 6: fbFillRect(x, y + midT, Wc, T, c); return;
  }
}
void sevenSegDigit(int x, int y, int Wc, int Hc, int T, uint8_t d, uint16_t lit, uint16_t ghost) {
  for (int s = 0; s < 7; s++) sevenSegSeg(x, y, Wc, Hc, T, s, ghost);
  if (d > 9) return;
  for (int s = 0; s < 7; s++)
    if (segBits[d] & (1 << s)) sevenSegSeg(x, y, Wc, Hc, T, s, lit);
}

void drawTotal() {
  fbText((WIDTH - textW57("TOTAL")) / 2, TOT_LBL_Y, "TOTAL", panelInk());
  uint16_t lit = fetching ? panelGhost() : panelInk();
  const int Wc = 14, Hc = 26, T = 3, gap = 4;   // ghost 7-seg total
  int x = (WIDTH - (4 * Wc + 3 * gap)) / 2;
  int div = 1000;
  for (int i = 0; i < 4; i++) {
    int d = (ghTotal / div) % 10;
    bool lead = (ghTotal < div) && (i < 3);
    sevenSegDigit(x + i * (Wc + gap), TOT_Y, Wc, Hc, T, lead ? 10 : d, lit, panelGhost());
    div /= 10;
  }
}

void drawStats() {
  char b[16];
  fbText(6, STAT_LBL_Y, "CUR", panelInk());
  snprintf(b, sizeof(b), "%d", ghStreak);
  fbText(6 + textW57("CUR") + 4, STAT_LBL_Y, b, panelInk());
  snprintf(b, sizeof(b), "BEST %d", ghBest);
  fbText(WIDTH - 6 - textW57(b), STAT_LBL_Y, b, panelInk());
}

void drawHeaderStrip(uint16_t statusColor, const char* status) {
  // "@user" left, status right — the CYD header style; "GITHUB" would collide
  // with a 10-char username on a 160px screen.
  char ubuf[14];
  snprintf(ubuf, sizeof(ubuf), "%.11s", ghUser.c_str());
  fbText(6, HDR_Y, ubuf, panelInk());
  fbText(WIDTH - 6 - textW57(status), HDR_Y, status, statusColor);
  fbHLine(4, ACC_Y, WIDTH - 8, accentDK());
  fbFillRect(4, ACC_Y + 1, WIDTH - 8, 4, accent());
}

void render() {
  if (gettingData) { renderFetchScreen(); return; }
  fbClear(panelBG());   // always the dark SIGNAL deck — no state switching
  uint16_t sc;
  const char* st = statusText(sc);
  drawHeaderStrip(sc, st);
  drawGrid(0);
  drawTotal();
  drawStats();
  fbFlush();
}

void renderFetchScreen() {
  fbClear(panelBG());
  drawHeaderStrip(accent(), "SYNC");
  for (int c = 0; c < WEEKS; c++)
    for (int r = 0; r < 7; r++)
      fbFillRect(GRID_X + c * (CELL + CELL_GAP), GRID_Y + r * (CELL + CELL_GAP), CELL, CELL, panelGhost());
  const char* ld = "LOADING";
  int w = textW57(ld, 2);
  int dots = (millis() / 300) % 4;
  int x = (WIDTH - (w + dots * 7)) / 2;
  fbText(x, 22, ld, panelInk(), 2);
  for (int i = 0; i < dots; i++) fbFillRect(x + w + 3 + i * 7, 26, 3, 3, accent());
  fbFlush();
}

void renderConnectScreen() {
  fbClear(panelBG());
  drawHeaderStrip(panelGhost(), "SYNC");
  fbText11((WIDTH - textW11("CONNECTING")) / 2, 40, "CONNECTING", panelInk());
  if (wifiSSID.length()) {
    const char* ss = wifiSSID.c_str();
    if (textW11(ss) <= WIDTH - 24) fbText11((WIDTH - textW11(ss)) / 2, 60, ss, panelInk());
    else fbText((WIDTH - textW57(ss)) / 2, 62, ss, panelInk());
  }
  fbText((WIDTH - textW57("HOLD SETUP")) / 2, 100, "HOLD SETUP", accentDK());
  fbFlush();   // crisp setup text — no VHS on the connect screen
}

void renderPortal() {
  fbClear(panelBG());
  drawHeaderStrip(accent(), "SETUP");
  fbText11((WIDTH - textW11("WIFI SETUP")) / 2, 24, "WIFI SETUP", panelInk());

  fbText11(8, 44, "1", accent());
  fbText11(20, 44, "JOIN WIFI", panelInk());
  if (textW11(AP_SSID) <= WIDTH - 28) fbText11(20, 58, AP_SSID, panelInk());
  else fbText(20, 60, AP_SSID, panelInk());     // long SSID: fall back to 5x6

  fbText11(8, 84, "2", accent());
  fbText11(20, 84, "OPEN BROWSER", panelInk());
  fbText11(20, 98, "192.168.4.1", panelInk());

  fbText11((WIDTH - textW11("SAVES + REBOOTS")) / 2, 114, "SAVES + REBOOTS", panelGhost());
  fbFlush();   // crisp setup text — no VHS on the portal
}

#include "Dm01Intro.h"

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== GITHUB SQUARES ===");

  pinMode(TOUCH_PIN, INPUT);
  pixels.begin();
  pixels.setBrightness(40);
  pixels.clear();
  pixels.show();

  SPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);
  tft.initR(INITR_BLACKTAB);
  tft.setRotation(1);
  tft.setSPISpeed(40000000);
  tft.fillScreen(ST7735_BLACK);

  loadCredentials();
  calibrateTouch();
  loadDemoData();          // grid is never empty, even if the first fetch fails

  pixels.setBrightness(70);
  dm01Start();
}

void bootNetwork() {
  if (wifiSSID.length() == 0) { startPortal(); return; }
  netMode = NET_CONNECTING;
  renderConnectScreen();
  pixels.setBrightness(60);
  pixels.setPixelColor(0, pixels.Color(0, 20, 90));
  pixels.show();
  if (wifiConnect(15000, false)) onWifiUp();
  else {
    netMode = NET_OFFLINE;
    lastReconnectMs = millis();
    loadDemoData();
    gettingData = false;   // never leave the LOADING screen hanging when offline
    Serial.println("offline demo");
  }
}

void loop() {
  bool introSkip = inputPressed();
  if (dm01Frame(introSkip)) {
    pixels.setPixelColor(0, dm01Pal((int)(millis() / 150)));
    pixels.show();
    fbFlush();
    screenshotHandle(fb, WIDTH, HEIGHT);
    delay(16);
    return;
  }

  static bool introEnded = false;
  if (!introEnded && !dm01IsActive()) {
    introEnded = true;
    Serial.printf("[intro] done t=%lums\n", millis() - dm01StartMs);
  }

  if (!bootDone) { bootDone = true; bootNetwork(); }

  if (netMode == NET_PORTAL) {
    dnsServer.processNextRequest();
    server.handleClient();
    updateLed();
    renderPortal();
    screenshotHandle(fb, WIDTH, HEIGHT);
    delay(10);
    return;
  }

  handleTouch();
  netWatchdog();
  updateFetch();
  updateLed();
  render();
  screenshotHandle(fb, WIDTH, HEIGHT);
  if (fetching) continueFetch();   // blocks, but the LOADING frame is already on screen
  delay(30);
}
