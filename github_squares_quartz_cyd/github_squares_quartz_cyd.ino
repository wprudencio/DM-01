// GitHub Squares — CYD (ESP32-2432S028R) version.
// Port of github_squares/github_squares.ino (ESP32-C3 + ST7735 160x128) to the
// Cheap Yellow Display: ST7789 320x240 + XPT2046 resistive touch, 64-row strip
// framebuffer.
//
// Rendered in the SIGNAL/VHS theme: near-black deck, white ink, light-grey
// rules, magenta accent strip and per-element VHS glitch hits, with the
// contribution calendar ramped dim grey -> light grey -> white -> signal
// green. The bigger panel shows the full year: week columns with weekday and
// month labels, the yearly total in big ghost 7-seg, and current/today/best
// stats. Deterministic demo year when offline.
//
// Data comes from GitHub's official GraphQL API
// (user.contributionsCollection — needs a personal access token entered in
// the setup portal) with the keyless public mirror
// (github-contributions-api.jogruber.de) as fallback when no token is set.
// Both yield date/count/level days; the token path is real-time, the mirror
// lags the profile by a while.
//
// Controls: touch tap = refresh now (marching ants + SYNC... while live),
// BOOT tap = theme toggle (both states render the SIGNAL deck), either held
// >= 1.5 s = WiFi/user/token portal.
// No NeoPixel on the CYD — state feedback is on-screen + serial.
//
// WiFi reliability follows the esp32-c3-ws2812 skill §10 (8.5 dBm TX, sleep
// off, hard-reset retries, begin() never called while a connect is pending).

#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <math.h>
#include "Screenshot.h"

// ── Pins: CYD (ESP32-2432S028R) ──
#define TFT_CS    15
#define TFT_DC    2
#define TFT_RST   4
#define TFT_MOSI  13
#define TFT_SCLK  14
#define TFT_MISO  12
#define TFT_BL    21

// ── Touch (XPT2046 on separate HSPI bus) + BOOT button (GPIO 0) ──
#define TOUCH_CS   33
#define TOUCH_CLK  25
#define TOUCH_MOSI 32
#define TOUCH_MISO 39
#define TOUCH_IRQ  36
#define BOOT_PIN   0

Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);
SPIClass touchSPI(HSPI);

#define WIDTH  320
#define HEIGHT 240

// ── Framebuffer (strip-based: 64 rows; the 150 KB full frame does not fit) ──
#define FB_H 64
static uint16_t fb[WIDTH * FB_H];
static int fbTop = 0;
static ScreenshotStripSession shot;

#define FPIX(x,y,c) if((x)>=0&&(x)<WIDTH&&(y)>=fbTop&&(y)<fbTop+FB_H) fb[(y-fbTop)*WIDTH+(x)]=(c)

void fbClear(uint16_t c) {
  for (int i = 0; i < WIDTH * FB_H; i++) fb[i] = c;
}
void fbHLine(int x, int y, int w, uint16_t c) {
  if (y < fbTop || y >= fbTop + FB_H) return;
  if (x < 0) { w += x; x = 0; }
  if (x + w > WIDTH) w = WIDTH - x;
  if (w <= 0) return;
  uint16_t* p = &fb[(y - fbTop) * WIDTH + x];
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
  int h = FB_H;
  if (fbTop + h > HEIGHT) h = HEIGHT - fbTop;
  tft.startWrite();
  tft.setAddrWindow(0, fbTop, WIDTH, h);
  tft.writePixels(fb, WIDTH * h);
  tft.endWrite();
}

uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) { return tft.color565(r, g, b); }

// ── SIGNAL palette — near-black deck, magenta + signal green (VHS theme).
//    darkTheme is kept as a BOOT-toggle behaviour (persisted in NVS), but both
//    states render the same SIGNAL deck. ──
bool darkTheme = true;
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

// 1x 4x5 glyphs for tight labels (month / weekday keys) — the VHS size
static inline void fbChar1x(int x, int y, char ch, uint16_t c) {
  if (ch == ' ') return;
  if (ch >= 'a' && ch <= 'z') ch -= 32;
  const uint8_t* g = glyph45(ch);
  for (int row = 0; row < 5; row++) {
    uint8_t bits = g[row];
    for (int col = 0; col < 4; col++)
      if (bits & (0b1000 >> col)) fbFillRect(x + col, y + row, 1, 1, c);
  }
}
void fbTextSmall(int x, int y, const char* s, uint16_t c) {
  while (*s) { fbChar1x(x, y, *s, c); x += 5; s++; }
}

int textW11(const char* s) { return textW57(s, 2); }


// ── Layout (320x240): tiles are the hero — chrome stays quiet ──
#define HDR_Y 8
#define ACC_Y 26          // dark line 26; red bar 27..33
#define MON_Y 42
#define GRID_Y 52
#define CELL 5
#define CELL_GAP 1
#define WEEKS 48
#define PITCH (CELL + CELL_GAP)
#define GRID_X (((WIDTH - (WEEKS * PITCH - CELL_GAP)) / 2))   // 16
#define GRID_W (WEEKS * PITCH - CELL_GAP)                     // 287
#define GRID_H (7 * PITCH - CELL_GAP)                         // 41
#define WD_X (GRID_X - 10)
#define TOT_LBL_Y 102
#define TOT_Y 112
#define TOT_W 20
#define TOT_H 36
#define TOT_T 5
#define TOT_GAP 5
#define STAT_Y 158        // single text row: CUR / TODAY / BEST (scale 2)
#define LEG_TXT_Y 178     // LESS..MORE key (scale 1 text, 8px tiles)
#define LEG_TILE_Y 180
#define DIV_Y 200
#define FTR_Y 210         // hints (scale 2) + tiny centered GITHUB brand

// ── Config + network state (NVS "dm01", shared with the other sketches) ──
String ghUser = "wprudencio";
// No default token: leave empty to use the public mirror fallback, or set one
// in the setup portal (saved as "ghtoken" in NVS; an empty field clears it).
String ghToken = "";
bool tokenError = false;   // last token fetch got 401/403
enum NetMode : uint8_t { NET_BOOT, NET_CONNECTING, NET_UP, NET_OFFLINE, NET_PORTAL };
NetMode netMode = NET_BOOT;
bool bootDone = false;
bool bootScreen = false;
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
int ghToday = 0;       // count on the most recent day
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
// inverse, for the month labels above the grid
static void civilFromDays(int32_t z, int& y, unsigned& m, unsigned& d) {
  z += 719468;
  const int32_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = (unsigned)(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  y = (int)yoe + (int)(era * 400);
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp + (mp < 10 ? 3 : -9);
  y += (m <= 2);
}

void computeStats() {
  ghTotal = 0;
  for (int i = 0; i < ghCount; i++) ghTotal += ghDays[i].count;
  ghToday = ghCount ? ghDays[ghCount - 1].count : 0;
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

bool httpPost(const String& url, const String& body, String& out, int* code) {
  HTTPClient http;
  http.setTimeout(15000);
  http.setConnectTimeout(6000);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  if (!http.begin(url)) { if (code) *code = 0; return false; }
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Accept", "application/vnd.github+json");
  http.addHeader("User-Agent", "GH-Squares-CYD");
  if (ghToken.length()) http.addHeader("Authorization", "Bearer " + ghToken);
  int c = http.POST(body);
  if (code) *code = c;
  bool ok = false;
  if (c == 200) { out = http.getString(); ok = true; }
  else if (c > 0) { http.getString(); }
  http.end();
  return ok;
}

// map the official API day colour to a 0-4 level (light + dark palettes)
static uint8_t levelFromColor(const char* hex, int cnt) {
  char c[8];
  for (int i = 0; i < 7; i++) {
    char ch = hex[i];
    if (ch >= 'A' && ch <= 'F') ch += 32;
    c[i] = ch;
  }
  c[7] = 0;
  if (!strcmp(c, "#ebedf0") || !strcmp(c, "#161b22")) return 0;
  if (!strcmp(c, "#9be9a8") || !strcmp(c, "#0e4429")) return 1;
  if (!strcmp(c, "#40c463") || !strcmp(c, "#006d32")) return 2;
  if (!strcmp(c, "#30a14e") || !strcmp(c, "#26a641")) return 3;
  if (!strcmp(c, "#216e39") || !strcmp(c, "#39d353")) return 4;
  if (cnt <= 0) return 0;   // unknown palette: fall back to count
  if (cnt == 1) return 1;
  if (cnt <= 3) return 2;
  if (cnt <= 6) return 3;
  return 4;
}

// official GraphQL shape: data.user.contributionsCollection.
//contributionCalendar{totalContributions,weeks{contributionDays{
// date,contributionCount,color}}} — the parser only scans for the keys,
// so nesting changes don't matter.
bool parseGraphQL(const String& json) {
  // GitHub may pad with whitespace — strip it (no string value in this
  // payload legitimately contains spaces) so the key scans below match.
  String compact = json;
  compact.replace(" ", "");
  compact.replace("\n", "");
  compact.replace("\r", "");
  compact.replace("\t", "");
  const char* s = compact.c_str();
  int total = -1;
  int ti = compact.indexOf("\"totalContributions\":");
  if (ti >= 0) {
    int p = ti + 21;   // strlen("\"totalContributions\":")
    while (p < (int)compact.length() && (s[p] < '0' || s[p] > '9')) p++;
    total = atoi(s + p);
  }
  int n = 0;
  int pos = 0;
  while (n < DAYS_MAX) {
    int d = compact.indexOf("\"date\":\"", pos);
    if (d < 0) break;
    int ds = d + 8;
    if (ds + 10 > (int)compact.length()) break;
    int objEnd = compact.indexOf('}', ds);
    if (objEnd < 0) break;
    int cc = compact.indexOf("\"contributionCount\":", ds);
    int co = compact.indexOf("\"color\":\"", ds);
    if (cc < 0 || cc > objEnd || co < 0 || co > objEnd ||
        co + 16 > (int)compact.length()) { pos = objEnd + 1; continue; }
    int y = atoi(s + ds);
    int m = atoi(s + ds + 5);
    int dd = atoi(s + ds + 8);
    int cnt = atoi(s + cc + 20);
    if (y >= 2020 && y <= 2100 && m >= 1 && m <= 12 && dd >= 1 && dd <= 31) {
      ghDays[n].days = daysFromCivil(y, (unsigned)m, (unsigned)dd);
      ghDays[n].count = (uint16_t)(cnt < 0 ? 0 : (cnt > 65535 ? 65535 : cnt));
      ghDays[n].level = levelFromColor(s + co + 9, cnt);
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
  if (total >= 0) ghTotal = total;
  Serial.printf("[gh] days=%d total=%d streak=%d best=%d\n", ghCount, ghTotal, ghStreak, ghBest);
  return true;
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
  Serial.printf("[gh] fetch %s via %s\n", ghUser.c_str(), ghToken.length() ? "graphql" : "mirror");
}

bool continueFetch() {
  String payload;
  int code = 0;
  bool ok = false;
  if (ghToken.length()) {
    // NOTE: single-char var names (e.g. $l) are rejected by the API with
    // "Variable $l of type String! was provided invalid value" — use $login.
    String body = String("{\"query\":\"query($login:String!){user(login:$login){contributionsCollection{contributionCalendar{totalContributions weeks{contributionDays{date contributionCount color}}}}}}\",\"variables\":{\"login\":\"") + ghUser + "\"}}";
    if (httpPost("https://api.github.com/graphql", body, payload, &code) && parseGraphQL(payload)) {
      tokenError = false;
      ok = true;
    } else if (code == 401 || code == 403) {
      tokenError = true;
    }
  } else {
    String url = String("https://github-contributions-api.jogruber.de/v4/") + ghUser + "?y=last";
    ok = httpGet(url, payload, &code) && parseContribs(payload);
  }
  if (ok) {
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
  if (netMode != NET_UP) return;   // offline: the demo year keeps rendering
  if (fetching) return;
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
  wifiPrepare(true, false);
  Serial.printf("WiFi retry ssid=\"%s\" -> %d\n", wifiSSID.c_str(), (int)WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str()));
}

void loadCredentials() {
  preferences.begin("dm01", true);
  wifiSSID = preferences.getString("ssid", "");
  wifiPassword = preferences.getString("password", "");
  String u = preferences.getString("ghuser", "");
  bool hasToken = preferences.isKey("ghtoken");
  String t = preferences.getString("ghtoken", "");
  darkTheme = preferences.getBool("dark", true);
  preferences.end();
  if (u.length()) ghUser = u;
  // Saved value wins (empty = mirror fallback); a fresh flash with no NVS
  // entry keeps the hardcoded default above.
  if (hasToken) ghToken = t;

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
<p>WiFi + GitHub user &mdash; saved to NVS, device reboots. A token enables the
live official API; without one the delayed public mirror is used.</p>
<form action="/save" method="POST">
  <label for="ssid">WiFi name</label>
  <input type="text" id="ssid" name="ssid" placeholder="SSID" required>
  <label for="password">Password</label>
  <input type="password" id="password" name="password" placeholder="password">
  <label for="user">GitHub user</label>
  <input type="text" id="user" name="user" maxlength="39" placeholder="octocat">
  <label for="token">GitHub token (optional)</label>
  <input type="password" id="token" name="token" maxlength="100" placeholder="ghp_... for live data">
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
  String token = server.arg("token");
  token.trim();
  if (ssid.length() == 0) {
    server.send(200, "text/html", "SSID required. <a href='/'>Back</a>");
    return;
  }
  preferences.begin("dm01", false);
  preferences.putString("ssid", ssid);
  preferences.putString("password", password);
  preferences.putString("ghuser", user);
  preferences.putString("ghtoken", token);
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

// ── Touch (XPT2046) + BOOT button: touch tap = refresh, BOOT tap = theme,
// either held >= 1.5 s = setup portal ──
bool wasPressed = false;
bool pressWasBoot = false;
unsigned long pressStartMs = 0;
bool holdFired = false;
#define HOLD_MS 1500

void touchInit() {
  touchSPI.begin(TOUCH_CLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
  pinMode(TOUCH_CS, OUTPUT);
  digitalWrite(TOUCH_CS, HIGH);
  pinMode(TOUCH_IRQ, INPUT);  // GPIO36 input-only: external pull-up on the CYD
  pinMode(BOOT_PIN, INPUT_PULLUP);
}
bool touchPressed() { return digitalRead(TOUCH_IRQ) == LOW; }
static inline bool inputPressed() {
  if (digitalRead(BOOT_PIN) == LOW) return true;
  return touchPressed();
}
void touchRead(int &tx, int &ty) {
  touchSPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  digitalWrite(TOUCH_CS, LOW);
  touchSPI.transfer(0xD0);
  uint16_t xRaw = ((touchSPI.transfer(0) << 8) | touchSPI.transfer(0)) >> 3;
  touchSPI.transfer(0x90);
  uint16_t yRaw = ((touchSPI.transfer(0) << 8) | touchSPI.transfer(0)) >> 3;
  digitalWrite(TOUCH_CS, HIGH);
  touchSPI.endTransaction();
  // CYD landscape swaps the axes: raw Y -> screen X, raw X -> screen Y
  tx = map(yRaw, 200, 3900, 0, 320);
  ty = map(xRaw, 3900, 200, 0, 240);
  if (tx < 0) tx = 0; if (tx > 319) tx = 319;
  if (ty < 0) ty = 0; if (ty > 239) ty = 239;
}

void onShortTap() {
  Serial.println("[touch] refresh");
  if (fetching) fetchQueued = true;
  else startFetch();
}

void onLongHold() {
  Serial.println("[touch] setup portal");
  startPortal();
}

void toggleTheme() {
  darkTheme = !darkTheme;
  preferences.begin("dm01", false);
  preferences.putBool("dark", darkTheme);
  preferences.end();
  Serial.printf("[theme] %s\n", darkTheme ? "dark" : "light");
}

void handleTouch() {
  if (netMode == NET_PORTAL) return;
  bool bootPressed = (digitalRead(BOOT_PIN) == LOW);
  bool pressed = bootPressed || touchPressed();
  unsigned long now = millis();

  if (pressed && !wasPressed) {
    pressStartMs = now;
    wasPressed = true;
    holdFired = false;
    pressWasBoot = bootPressed;
    if (bootPressed) {
      Serial.println("[boot] press");
    } else {
      int tx, ty; touchRead(tx, ty);
      Serial.printf("[touch] press @ %d,%d\n", tx, ty);
    }
  } else if (!pressed && wasPressed) {
    unsigned long dur = now - pressStartMs;
    if (!holdFired && dur >= 60 && dur < HOLD_MS) {
      if (pressWasBoot) toggleTheme();
      else onShortTap();
    }
    wasPressed = false;
    holdFired = false;
    delay(20);
  } else if (pressed && wasPressed && !holdFired && (now - pressStartMs >= HOLD_MS)) {
    holdFired = true;
    onLongHold();
  }
}

bool introMode = false;

// ── DM-01 boot intro (2x for 320x240) ──
#define DM01_SCALE 2
#include "Dm01Intro.h"

// ── Rendering: SIGNAL/VHS deck + GitHub squares ──
const char* statusText(uint16_t& color) {
  if (fetching || gettingData) {
    color = blinkOn(millis()) ? panelInk() : panelGhost();
    if (fetching && !gettingData) {   // refresh in flight: SYNC + marching dots
      static char syncBuf[8];
      int dots = (millis() / 300) % 4;
      snprintf(syncBuf, sizeof(syncBuf), "SYNC%.*s", dots, "...");
      return syncBuf;
    }
    return "SYNC";
  }
  if (tokenError) { color = accent(); return "TOKEN"; }
  if (!dataIsLive) { color = blinkOn(millis()) ? accent() : panelGhost(); return "DEMO"; }
  color = panelInk();
  return "LIVE";
}

// tile: flat SIGNAL-ramp fill, no rims
void drawTile(int x, int y, uint8_t lvl) {
  fbFillRect(x, y, CELL, CELL, ghLvl(lvl));
}

// marching "working" frame around the grid while a refresh is in flight —
// pure millis() function, so it stays correct in the strip renderer
void drawFetchMarch() {
  int x0 = GRID_X - 3, y0 = GRID_Y - 3;
  int x1 = GRID_X + GRID_W + 2, y1 = GRID_Y + GRID_H + 2;
  int off = (millis() / 150) % 8;
  uint16_t c = accent();
  for (int x = x0; x <= x1; x++)
    if (((x + off) & 7) < 4) { FPIX(x, y0, c); FPIX(x, y1, c); }
  for (int y = y0; y <= y1; y++)
    if (((y + off) & 7) < 4) { FPIX(x0, y, c); FPIX(x1, y, c); }
}

void drawGrid(uint16_t empty) {
  if (ghCount <= 0) return;
  int32_t lastWeek = (ghDays[ghCount - 1].days + 4) / 7;
  int32_t firstWeek = lastWeek - WEEKS + 1;
  for (int i = 0; i < ghCount; i++) {
    int32_t d = ghDays[i].days;
    int32_t wk = (d + 4) / 7;
    if (wk < firstWeek) continue;
    int col = (int)(wk - firstWeek);
    int row = (int)((d + 4) % 7);          // 0 = Sunday
    if (col >= WEEKS) continue;
    int x = GRID_X + col * PITCH, y = GRID_Y + row * PITCH;
    if (empty) fbFillRect(x, y, CELL, CELL, empty);
    else drawTile(x, y, ghDays[i].level);
  }
  // today marker: outline the most recent cell
  int32_t d = ghDays[ghCount - 1].days;
  int col = (int)((d + 4) / 7 - firstWeek);
  int row = (int)((d + 4) % 7);
  if (col >= 0 && col < WEEKS)
    fbRect(GRID_X + col * PITCH - 1, GRID_Y + row * PITCH - 1, CELL + 2, CELL + 2, accentDK());
}

// GitHub-style LESS -> MORE key, 8 px tiles, quiet scale-1 caps
void drawLegendTile(int x, int y, uint8_t lvl) {
  fbFillRect(x, y, 8, 8, ghLvl(lvl));
}
void drawLegend() {
  int tw = textW57("LESS", 1) + 8 + (4 * 12 + 8) + 8 + textW57("MORE", 1);
  int x = (WIDTH - tw) / 2;
  fbText(x, LEG_TXT_Y, "LESS", panelEdge(), 1); x += textW57("LESS", 1) + 8;
  for (int i = 0; i < 5; i++) { drawLegendTile(x, LEG_TILE_Y, (uint8_t)i); x += (i < 4) ? 12 : 8; }
  x += 8;
  fbText(x, LEG_TXT_Y, "MORE", panelEdge(), 1);
}

void drawWeekdays() {
  static const char* wd[7] = {"", "M", "", "W", "", "F", ""};
  for (int r = 0; r < 7; r++) {
    if (!wd[r][0]) continue;
    fbTextSmall(WD_X, GRID_Y + r * PITCH + 1, wd[r], panelEdge());
  }
}

void drawMonths() {
  static const char* MN[12] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                               "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
  if (ghCount <= 0) return;
  int32_t lastWeek = (ghDays[ghCount - 1].days + 4) / 7;
  int32_t firstWeek = lastWeek - WEEKS + 1;
  int prevMonth = -1;
  int prevX = -100;
  for (int c = 0; c < WEEKS; c++) {
    int32_t sunday = (firstWeek + c) * 7 - 4;
    if (sunday < ghDays[0].days) sunday = ghDays[0].days;
    int y; unsigned m, dd;
    civilFromDays(sunday, y, m, dd);
    if ((int)m != prevMonth) {
      prevMonth = (int)m;
      int x = GRID_X + c * PITCH;
      if (x - prevX >= 16 && x + 20 <= GRID_X + GRID_W) {
        fbTextSmall(x, MON_Y, MN[m - 1], panelEdge());
        prevX = x;
      }
    }
  }
}

void drawHeaderStrip(uint16_t statusColor, const char* status) {
  char ubuf[14];
  snprintf(ubuf, sizeof(ubuf), "%.11s", ghUser.c_str());
  fbText(12, HDR_Y, ubuf, panelInk(), 1);
  fbText(WIDTH - 12 - textW57(status, 1), HDR_Y, status, statusColor, 1);
  fbHLine(8, ACC_Y, WIDTH - 16, accentDK());
  fbFillRect(8, ACC_Y + 1, WIDTH - 16, 7, accent());
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
  const int Wc = 24, Hc = 40, T = 5, gap = 5;   // ghost 7-seg total
  int x = (WIDTH - (4 * Wc + 3 * gap)) / 2;
  int div = 1000;
  for (int i = 0; i < 4; i++) {
    int d = (ghTotal / div) % 10;
    bool lead = (ghTotal < div) && (i < 3);
    sevenSegDigit(x + i * (Wc + gap), TOT_Y, Wc, Hc, T, lead ? 10 : d, lit, panelGhost());
    div /= 10;
  }
}

// one quiet text row instead of the old ghost-digit triptych:
// "CUR 0" / "TODAY 0" / "BEST 10" centered at thirds
void drawStats() {
  char b[16];
  snprintf(b, sizeof(b), "CUR %d", ghStreak);
  fbText(60 - textW57(b, 1) / 2, STAT_Y, b, panelInk(), 1);
  snprintf(b, sizeof(b), "TODAY %d", ghToday);
  fbText(160 - textW57(b, 1) / 2, STAT_Y, b, panelInk(), 1);
  snprintf(b, sizeof(b), "BEST %d", ghBest);
  fbText(260 - textW57(b, 1) / 2, STAT_Y, b, panelInk(), 1);
}

void drawFetchScreen() {
  uint16_t sc = accent();
  drawHeaderStrip(sc, "SYNC");
  for (int c = 0; c < WEEKS; c++)
    for (int r = 0; r < 7; r++)
      fbFillRect(GRID_X + c * PITCH, GRID_Y + r * PITCH, CELL, CELL, panelGhost());
  const char* ld = "LOADING";
  int w = textW57(ld, 2);
  int dots = (millis() / 300) % 4;
  int x = (WIDTH - (w + dots * 14)) / 2;
  fbText(x, 34, ld, panelInk(), 2);
  for (int i = 0; i < dots; i++) fbFillRect(x + w + 6 + i * 14, 42, 6, 6, accent());
  fbText(12, FTR_Y, "TAP REFRESH", panelInk(), 1);
  fbText((WIDTH - textW57("GITHUB", 1)) / 2, FTR_Y + 2, "GITHUB", panelEdge(), 1);
  fbText(WIDTH - 12 - textW57("HOLD SETUP", 1), FTR_Y, "HOLD SETUP", accentDK(), 1);
}

void drawBootScreen() {
  fbText(12, HDR_Y, "GITHUB", panelInk(), 1);
  fbText(WIDTH - 12 - textW57("SYNC", 1), HDR_Y, "SYNC", panelEdge(), 1);
  fbHLine(8, ACC_Y, WIDTH - 16, accentDK());
  fbFillRect(8, ACC_Y + 1, WIDTH - 16, 6, accent());
  for (int y = 44; y <= 200; y += 4)
    for (int x = ((y / 4) & 1) * 2; x < WIDTH; x += 4) FPIX(x, y, panelGhost());
  fbText((WIDTH - textW57("CONNECTING", 2)) / 2, 84, "CONNECTING", panelInk(), 2);
  if (wifiSSID.length()) {
    const char* ss = wifiSSID.c_str();
    fbText((WIDTH - textW57(ss, 1)) / 2, 128, ss, panelInk(), 1);
  }
  fbText(12, HEIGHT - 24, "TAP REFRESH", panelInk(), 1);
  fbText(WIDTH - 12 - textW57("HOLD SETUP", 1), HEIGHT - 24, "HOLD SETUP", accentDK(), 1);
}

void drawPortal() {
  fbText(12, HDR_Y, "GITHUB", panelInk(), 1);
  fbText(WIDTH - 12 - textW57("SETUP", 1), HDR_Y, "SETUP", accent(), 1);
  fbHLine(8, ACC_Y, WIDTH - 16, accentDK());
  fbHLine(8, ACC_Y + 1, WIDTH - 16, accentDK());
  fbFillRect(8, ACC_Y + 2, WIDTH - 16, 8, accent());
  fbText((WIDTH - textW57("WIFI SETUP", 2)) / 2, 52, "WIFI SETUP", panelInk(), 2);

  fbText(24, 92, "1", accent(), 2);
  fbText(52, 92, "JOIN WIFI", panelInk(), 2);
  if (textW57(AP_SSID, 2) <= WIDTH - 56) fbText(52, 118, AP_SSID, panelInk(), 2);
  else fbText(52, 122, AP_SSID, panelInk(), 1);   // long SSID: fall back to 1x

  fbText(24, 158, "2", accent(), 2);
  fbText(52, 158, "OPEN BROWSER", panelInk(), 2);
  fbText(52, 184, "192.168.4.1", panelInk(), 2);

  fbText((WIDTH - textW57("SAVES + REBOOTS", 1)) / 2, HEIGHT - 24, "SAVES + REBOOTS", panelGhost(), 1);
}

void drawFrame() {
  fbClear(panelBG());   // always the dark SIGNAL deck — no state switching
  if (netMode == NET_PORTAL) { drawPortal(); return; }
  if (bootScreen) { drawBootScreen(); return; }
  if (gettingData) { drawFetchScreen(); return; }
  uint16_t sc;
  const char* st = statusText(sc);
  drawHeaderStrip(sc, st);
  drawWeekdays();
  drawMonths();
  drawGrid(0);
  if (fetching) drawFetchMarch();
  drawTotal();
  drawStats();
  drawLegend();
  fbHLine(8, DIV_Y, WIDTH - 16, panelEdge());
  fbText(12, FTR_Y, "TAP REFRESH", panelInk(), 1);
  fbText((WIDTH - textW57("GITHUB", 1)) / 2, FTR_Y + 2, "GITHUB", panelEdge(), 1);
  fbText(WIDTH - 12 - textW57("HOLD SETUP", 1), FTR_Y, "HOLD SETUP", accentDK(), 1);
}

void render() {
  for (fbTop = 0; fbTop < HEIGHT; fbTop += FB_H) {
    if (introMode) dm01Draw(dm01Elapsed()); else drawFrame();
    int h = FB_H;
    if (fbTop + h > HEIGHT) h = HEIGHT - fbTop;
    if (!screenshotStripSend(shot, fb, fbTop, h)) fbFlush();
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== GITHUB SQUARES CYD ===");

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  SPI.begin(TFT_SCLK, TFT_MISO, TFT_MOSI, TFT_CS);
  tft.init(240, 320);
  tft.setSPISpeed(40000000);
  tft.setRotation(1);
  // The library ST7789 init sends INVON; the CYD panel needs it off.
  tft.invertDisplay(false);
  tft.fillScreen(0x0000);

  touchInit();
  loadCredentials();
  loadDemoData();          // grid is never empty, even if the first fetch fails
  dm01Start();
}

void bootNetwork() {
  if (wifiSSID.length() == 0) { startPortal(); return; }
  netMode = NET_CONNECTING;
  bootScreen = true;
  render();
  if (wifiConnect(15000, false)) { bootScreen = false; onWifiUp(); }
  else {
    bootScreen = false;
    netMode = NET_OFFLINE;
    lastReconnectMs = millis();
    loadDemoData();
    gettingData = false;   // never leave the LOADING screen hanging when offline
    Serial.println("offline demo");
  }
}

void loop() {
  screenshotStripPoll(shot, WIDTH, HEIGHT);

  if (dm01Tick(inputPressed())) {
    introMode = true;
    render();
    delay(16);
    return;
  }
  if (introMode) introMode = false;

  if (!bootDone) { bootDone = true; bootNetwork(); }

  if (netMode == NET_PORTAL) {
    dnsServer.processNextRequest();
    server.handleClient();
    render();
    delay(10);
    return;
  }

  handleTouch();
  netWatchdog();
  updateFetch();
  render();
  if (fetching) continueFetch();   // blocks, but the LOADING frame is already on screen
  delay(30);
}