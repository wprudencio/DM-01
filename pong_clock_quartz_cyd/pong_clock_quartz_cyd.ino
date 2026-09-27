// Pong Clock — CYD (ESP32-2432S028R) version.
// Port of pong_clock/pong_clock.ino (ESP32-C3 + ST7735 160x128) to the Cheap
// Yellow Display: ST7789 320x240 + XPT2046 resistive touch, 64-row strip
// framebuffer.
//
// World clock: three Pong matches play themselves at once, one per timezone
// (LOCAL / NEW YORK / TOKYO), and each lane's score IS the time — the left
// score is hours, the right is minutes. A paddle only steps aside when its
// clock needs a point (left miss = minute advances, right miss = hour); NTP
// resyncs or long offline gaps snap the digits with a red flash.
//
// Time is NTP only (no API keys). Each zone is a POSIX TZ string, so newlib
// applies DST. Before sync the lanes run on a fake epoch (boot ≈ 12:00 UTC)
// and snap once SNTP lands. Credentials and the local timezone index live in
// NVS ("dm01", shared with the other sketches), written by the captive portal
// — never hardcoded.
//
// Input (optional): tap = backlight on/off (screen doubles as the night
// light), hold >= 1.5 s = WiFi/timezone setup portal. Boots with the shared
// DM-01 intro.
//
// WiFi reliability follows the esp32-c3-ws2812 skill §10: TX power 8.5 dBm,
// sleep off, auto-reconnect off, retries hard-reset first and begin() is
// never called while a connection attempt is still pending.

#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <math.h>
#include <time.h>
#include "Screenshot.h"

// ── Pong state (declared early: function prototypes reference the type) ──
#define BALL_R 3
#define PAD_W  4
#define PAD_H  18
#define SERVE_SPEED 95.0f
#define MAX_SPEED   215.0f
#define PAD_SPEED   150.0f
#define MIN_ANGLE   (18.0f * PI / 180.0f)
#define MAX_ANGLE   (50.0f * PI / 180.0f)
#define SERVE_MS    750
#define OUT_MARGIN  14

struct Pong {
  int y0, y1;
  float bx, by, vx, vy;      // ball centre + velocity (px, px/s)
  float lpy, rpy;            // paddle centre y
  int hh, mm;                // score = time
  unsigned long flashHUntil, flashMUntil;
  unsigned long serveAt;
};

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

// ── QUARTZ palette — light LCD only, no state switching ──
uint16_t panelBG()    { return rgb(148, 158, 130); } // light LCD green-grey
uint16_t panelInk()   { return rgb( 28,  34,  24); } // dark ink
uint16_t panelGhost() { return rgb(138, 148, 120); } // unlit segments / dot grid
uint16_t panelHI()    { return rgb(178, 186, 160); } // light inner rule
uint16_t panelEdge()  { return rgb( 96, 104,  82); } // dark olive rules
uint16_t accent()     { return rgb(200,  40,  40); } // alarm red
uint16_t accentDK()   { return rgb(120,  24,  24); } // dark red

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

// ── Layout (320x240) ──
#define HDR_Y 6
#define ACC_Y 22          // dark line 22; red bar 23..28
#define LANE_Y0 30
#define LANE_H  70
#define FLD_DY0 16        // ball centre min y, relative to lane top
#define FLD_DY1 68        // ball centre max y
#define FLD_X0  10        // ball centre min x
#define FLD_X1  310       // ball centre max x
#define CLK_DY  28        // block clock cap top, relative to lane top
#define CLK_SC  4
#define LANE_COUNT 3

// ── Time zones (POSIX TZ strings — newlib applies DST) ──
struct Zone { const char* label; const char* tz; };
static const Zone ZONES[] = {
  {"UTC",       "UTC0"},
  {"SAO PAULO", "<-03>3"},
  {"NEW YORK",  "EST5EDT,M3.2.0,M11.1.0"},
  {"LONDON",    "GMT0BST,M3.5.0/1,M10.5.0"},
  {"BERLIN",    "CET-1CEST,M3.5.0,M10.5.0/3"},
  {"TOKYO",     "JST-9"},
  {"SYDNEY",    "AEST-10AEDT,M10.1.0,M4.1.0/3"},
  {"KOLKATA",   "IST-5:30"},
};
#define ZONE_COUNT ((int)(sizeof(ZONES) / sizeof(ZONES[0])))
int localZone = 0;
// Lane 0 follows the portal timezone; lanes 1-2 are fixed world clocks.
#define LANE_NY 2
#define LANE_TOKYO 5
int laneZone[LANE_COUNT] = {0, LANE_NY, LANE_TOKYO};

// ── NTP clock ──
#define NTP_VALID 1700000000L
unsigned long ntpStartMs = 0;
bool ntpStarted = false;

static inline bool ntpValid() { return time(nullptr) > NTP_VALID; }
// Before sync time() counts from boot: +12 h gives a plausible fake epoch so
// every lane keeps a sensible time and snaps to real time once SNTP lands.
static inline time_t clockNow() {
  time_t t = time(nullptr);
  return t > NTP_VALID ? t : t + 43200;
}
// Zone times are computed from cached UTC offsets instead of switching $TZ:
// newlib's setenv/tzset leak ~24 bytes on every TZ *change*, and the lane
// rotation changed TZ 6x/s — the heap died after ~20 minutes and the sketch
// wedged. Offsets come from a localtime_r result converted with exact
// civil-day math (newlib's mktime round trip returned 0 here).
int zoneOff[ZONE_COUNT];
int zoneOffDay = -1;
static long daysFromCivil(long y, int m, int d) {
  y -= m <= 2;
  const long era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (long)doe - 719468;
}
void refreshZoneOffsets(bool force) {
  time_t t = clockNow();
  struct tm gt;
  gmtime_r(&t, &gt);
  if (!force && gt.tm_yday == zoneOffDay) return;
  for (int i = 0; i < ZONE_COUNT; i++) {
    setenv("TZ", ZONES[i].tz, 1);   // ZONE_COUNT switches per day, not per frame
    tzset();
    struct tm lt;
    localtime_r(&t, &lt);
    long asUtc = daysFromCivil(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday) * 86400L +
                 lt.tm_hour * 3600L + lt.tm_min * 60L + lt.tm_sec;
    zoneOff[i] = (int)(asUtc - (long)t);
  }
  zoneOffDay = gt.tm_yday;
  Serial.printf("[clock] zone offsets refreshed (day %d)\n", zoneOffDay);
}
void zoneHM(int zi, int& h, int& m) {
  long zt = (long)clockNow() + zoneOff[zi];
  zt = ((zt % 86400L) + 86400L) % 86400L;
  h = (int)(zt / 3600);
  m = (int)((zt / 60) % 60);
}
void zoneDate(char* buf, size_t n) {
  static const char* MON[] = {"JAN","FEB","MAR","APR","MAY","JUN",
                              "JUL","AUG","SEP","OCT","NOV","DEC"};
  time_t t = clockNow() + zoneOff[laneZone[0]];
  struct tm lt;
  gmtime_r(&t, &lt);
  snprintf(buf, n, "%02d %s", lt.tm_mday, MON[lt.tm_mon]);
}

// ── Network state ──
enum NetMode : uint8_t { NET_BOOT, NET_CONNECTING, NET_UP, NET_OFFLINE, NET_PORTAL };
NetMode netMode = NET_BOOT;
bool bootDone = false;
bool bootScreen = false;
unsigned long lastReconnectMs = 0;
#define RECONNECT_MS 15000UL
#define NTP_RETRY_MS 60000UL

const char* AP_SSID = "PONG-Setup";
WebServer server(80);
DNSServer dnsServer;
#define DNS_PORT 53
Preferences preferences;
String wifiSSID = "";
String wifiPassword = "";

// ── Touch ──
bool wasPressed = false;
unsigned long pressStartMs = 0;
bool holdFired = false;
bool screenOn = true;
#define HOLD_MS 1500

// ── Pong (one match per lane) ──
Pong lanes[LANE_COUNT];
int tgtH[LANE_COUNT], tgtM[LANE_COUNT];

static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

float predictY(Pong& p, float planeX) {
  if (fabsf(p.vx) < 1.0f) return p.by;
  float t = (planeX - p.bx) / p.vx;
  if (t < 0) return p.by;
  float y = p.by + p.vy * t;
  float span = p.y1 - p.y0;
  float rel = fmodf(y - p.y0, 2.0f * span);
  if (rel < 0) rel += 2.0f * span;
  if (rel > span) rel = 2.0f * span - rel;
  return p.y0 + rel;
}

void startServe(Pong& p, bool towardRight) {
  p.bx = (FLD_X0 + FLD_X1) / 2.0f;
  p.by = (p.y0 + p.y1) / 2.0f;
  float ang = (18.0f + (float)(esp_random() % 22)) * (PI / 180.0f); // 18..39°
  if (esp_random() & 1) ang = -ang;
  p.vx = (towardRight ? 1.0f : -1.0f) * SERVE_SPEED * cosf(ang);
  p.vy = SERVE_SPEED * sinf(ang);
  p.serveAt = millis() + SERVE_MS;
}

void movePaddle(Pong& p, float& py, float target, float dt) {
  float step = PAD_SPEED * dt;
  float d = target - py;
  if (d > step) d = step; else if (d < -step) d = -step;
  py = clampf(py + d, p.y0 + PAD_H / 2.0f, p.y1 - PAD_H / 2.0f);
}

// Reflect off a paddle: speed climbs per hit, exit angle comes from where the
// ball struck (never flat, so rallies can't degenerate into straight lines).
void paddleBounce(Pong& p, bool offLeft) {
  float sp = sqrtf(p.vx * p.vx + p.vy * p.vy) * 1.06f;
  if (sp > MAX_SPEED) sp = MAX_SPEED;
  float off = p.by - (offLeft ? p.lpy : p.rpy);
  off = clampf(off / (PAD_H / 2.0f), -1.0f, 1.0f);
  float ang = off * MAX_ANGLE;
  if (fabsf(ang) < MIN_ANGLE) ang = (ang < 0 ? -1.0f : 1.0f) * MIN_ANGLE;
  p.vx = (offLeft ? 1.0f : -1.0f) * sp * cosf(ang);
  p.vy = sp * sinf(ang);
  p.bx = offLeft ? FLD_X0 : FLD_X1;
}

void scorePoint(int i, bool leftMissed) {
  // Ball out on the left -> right player (minutes) scores, and vice versa.
  Pong& p = lanes[i];
  if (leftMissed) {
    p.mm = (p.mm + 1) % 60;
    if (p.mm != tgtM[i]) p.mm = tgtM[i];   // wrapped past the target: snap
    p.flashMUntil = millis() + 900;
  } else {
    p.hh = (p.hh + 1) % 24;
    if (p.hh != tgtH[i]) p.hh = tgtH[i];
    p.flashHUntil = millis() + 900;
  }
  startServe(p, leftMissed);   // serve toward the player who just lost the point
}

void syncScores(Pong& p, int tH, int tM) {
  if (p.mm != tM && (tM - p.mm + 60) % 60 > 1) { p.mm = tM; p.flashMUntil = millis() + 900; }
  if (p.hh != tH && (tH - p.hh + 24) % 24 > 1) { p.hh = tH; p.flashHUntil = millis() + 900; }
}

void refreshTargets(bool force) {
  static unsigned long last = 0;
  if (!force && millis() - last < 500) return;
  last = millis();
  refreshZoneOffsets(false);   // recomputes on the day rollover only
  for (int i = 0; i < LANE_COUNT; i++) {
    zoneHM(laneZone[i], tgtH[i], tgtM[i]);
    syncScores(lanes[i], tgtH[i], tgtM[i]);
  }
}

void pongStep(int i, float dt) {
  Pong& p = lanes[i];
  float midY = (p.y0 + p.y1) / 2.0f;
  if (p.serveAt != 0 && millis() >= p.serveAt) p.serveAt = 0;
  bool inPlay = (p.serveAt == 0);

  // Paddles aim at the intercept (plus a small wobble so rallies vary); when
  // the lane's clock needs a point they run away from the ball instead.
  float rPred = predictY(p, FLD_X1) + sinf(millis() * 0.0017f) * 4.0f;
  float lPred = predictY(p, FLD_X0) + sinf(millis() * 0.0023f + 2.0f) * 4.0f;
  float rTarget = (p.hh != tgtH[i]) ? (rPred < midY ? p.y1 : p.y0)
                 : (p.vx > 0 ? rPred : midY);
  float lTarget = (p.mm != tgtM[i]) ? (lPred < midY ? p.y1 : p.y0)
                 : (p.vx < 0 ? lPred : midY);
  movePaddle(p, p.rpy, rTarget, dt);
  movePaddle(p, p.lpy, lTarget, dt);

  if (!inPlay) return;

  p.bx += p.vx * dt;
  p.by += p.vy * dt;

  if (p.by < p.y0) { p.by = p.y0; p.vy = fabsf(p.vy); }
  if (p.by > p.y1) { p.by = p.y1; p.vy = -fabsf(p.vy); }

  // Paddle planes: only a paddle that isn't stepping aside can return the ball
  if (p.vx < 0 && p.bx <= FLD_X0 && p.mm == tgtM[i] &&
      fabsf(p.by - p.lpy) <= PAD_H / 2.0f + BALL_R) paddleBounce(p, true);
  if (p.vx > 0 && p.bx >= FLD_X1 && p.hh == tgtH[i] &&
      fabsf(p.by - p.rpy) <= PAD_H / 2.0f + BALL_R) paddleBounce(p, false);

  // Out of bounds: the point always resolves to the lane's clock target
  if (p.bx < FLD_X0 - OUT_MARGIN) scorePoint(i, true);
  else if (p.bx > FLD_X1 + OUT_MARGIN) scorePoint(i, false);
}

void initGame() {
  for (int i = 0; i < LANE_COUNT; i++) {
    Pong& p = lanes[i];
    int top = LANE_Y0 + i * LANE_H;
    p.y0 = top + FLD_DY0;
    p.y1 = top + FLD_DY1;
    p.lpy = p.rpy = (p.y0 + p.y1) / 2.0f;
  }
  refreshTargets(true);
  for (int i = 0; i < LANE_COUNT; i++) {
    lanes[i].hh = tgtH[i];
    lanes[i].mm = tgtM[i];
    startServe(lanes[i], (esp_random() & 1) != 0);
  }
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

// Non-blocking retry: prepare + begin, loop() polls the status so all three
// pong lanes keep playing while the radio tries to come back.
void wifiRetryStart() {
  wifiPrepare(true, false);
  Serial.printf("WiFi retry ssid=\"%s\" -> %d\n", wifiSSID.c_str(), (int)WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str()));
}

void loadCredentials() {
  preferences.begin("dm01", true);
  wifiSSID = preferences.getString("ssid", "");
  wifiPassword = preferences.getString("password", "");
  localZone = preferences.getInt("tz", 0);
  preferences.end();
  if (localZone < 0 || localZone >= ZONE_COUNT) localZone = 0;
  laneZone[0] = localZone;

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

void startNtp() {
  configTzTime("UTC0", "pool.ntp.org", "time.cloudflare.com", "time.google.com");
  ntpStartMs = millis();
  ntpStarted = true;
  Serial.println("SNTP started");
}

void onWifiUp() {
  netMode = NET_UP;
  lastReconnectMs = millis();
  Serial.printf("WiFi OK IP=%s RSSI=%d\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
  startNtp();
}

void netWatchdog() {
  if (netMode == NET_UP) {
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi lost -> offline clock");
      netMode = NET_OFFLINE;
      lastReconnectMs = millis();
    } else if (!ntpValid() && ntpStarted && millis() - ntpStartMs > NTP_RETRY_MS) {
      startNtp();   // safe: configTzTime stops and restarts SNTP
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

// ── Captive portal (NVS "dm01", shared with the other sketches) ──
String portalPage() {
  String html = R"HTML(<!DOCTYPE html><html><head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Pong Clock Setup</title>
<style>
  body{background:#07050f;color:#f4eeff;font-family:ui-monospace,SFMono-Regular,Menlo,monospace;margin:0;padding:24px}
  main{max-width:360px;margin:0 auto}
  h1{color:#ff8c1e;letter-spacing:.15em;margin:0 0 4px}
  p{color:#746a9b;margin:0 0 24px;font-size:14px}
  label{display:block;font-size:12px;color:#b3a8cf;margin:14px 0 6px;text-transform:uppercase;letter-spacing:.1em}
  input,select{width:100%;box-sizing:border-box;padding:12px;background:#150e28;border:1px solid #3a2a5e;color:#f4eeff;border-radius:4px}
  button{width:100%;margin-top:22px;padding:13px;border:0;border-radius:4px;font-weight:bold;letter-spacing:.1em;color:#07050f;background:linear-gradient(90deg,#ff8c1e,#dc3cc8);cursor:pointer}
</style></head><body><main>
<h1>PONG CLOCK</h1>
<p>WiFi + local timezone &mdash; saved to NVS, device reboots. Lanes 2-3 are
fixed world clocks (New York / Tokyo).</p>
<form action="/save" method="POST">
  <label for="ssid">WiFi name</label>
  <input type="text" id="ssid" name="ssid" placeholder="SSID" required>
  <label for="password">Password</label>
  <input type="password" id="password" name="password" placeholder="password">
  <label for="tz">Local timezone</label>
  <select id="tz" name="tz">)HTML";
  for (int i = 0; i < ZONE_COUNT; i++) {
    html += "<option value='" + String(i) + "'";
    if (i == localZone) html += " selected";
    html += ">" + String(ZONES[i].label) + "</option>";
  }
  html += R"HTML(</select>
  <button type="submit">Connect</button>
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
  int tz = server.arg("tz").toInt();
  if (tz < 0 || tz >= ZONE_COUNT) tz = 0;
  if (ssid.length() == 0) {
    server.send(200, "text/html", "SSID required. <a href='/'>Back</a>");
    return;
  }
  preferences.begin("dm01", false);
  preferences.putString("ssid", ssid);
  preferences.putString("password", password);
  preferences.putInt("tz", tz);
  preferences.end();
  server.send(200, "text/html",
    "<html><head><meta name='viewport' content='width=device-width,initial-scale=1'></head>"
    "<body style='background:#07050f;color:#f4eeff;font-family:monospace;text-align:center;padding:60px'>"
    "<h2 style='color:#ff8c1e'>SAVED</h2><p>Rebooting...</p></body></html>");
  delay(900);
  ESP.restart();
}

void startPortal() {
  netMode = NET_PORTAL;
  digitalWrite(TFT_BL, HIGH);   // never configure with the screen dark
  screenOn = true;
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

// ── Touch (XPT2046): tap = backlight, hold = setup portal ──
void touchInit() {
  touchSPI.begin(TOUCH_CLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
  pinMode(TOUCH_CS, OUTPUT);
  digitalWrite(TOUCH_CS, HIGH);
  pinMode(TOUCH_IRQ, INPUT);  // GPIO36 input-only: external pull-up on the CYD
  pinMode(BOOT_PIN, INPUT_PULLUP);
}
bool touchPressed() {
  // Debounce the raw IRQ: a glitch must persist across 3 loop passes, so line
  // noise can neither tap the backlight nor fire the setup portal.
  static uint8_t lowN = 0;
  if (digitalRead(TOUCH_IRQ) != LOW) { lowN = 0; return false; }
  if (lowN < 3) { lowN++; return false; }
  return true;
}
static inline bool inputPressed() {
  if (digitalRead(BOOT_PIN) == LOW) return true;
  return touchPressed();
}
// Returns false when the panel reads like an untouched panel (rails), which is
// what the XPT2046 reports when PENIRQ drops out on its own.
bool touchRead(int &tx, int &ty) {
  touchSPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  digitalWrite(TOUCH_CS, LOW);
  touchSPI.transfer(0xD0);
  uint16_t xRaw = ((touchSPI.transfer(0) << 8) | touchSPI.transfer(0)) >> 3;
  touchSPI.transfer(0x90);
  uint16_t yRaw = ((touchSPI.transfer(0) << 8) | touchSPI.transfer(0)) >> 3;
  digitalWrite(TOUCH_CS, HIGH);
  touchSPI.endTransaction();
  bool valid = xRaw > 150 && xRaw < 3950 && yRaw > 150 && yRaw < 3950;
  // CYD landscape swaps the axes: raw Y -> screen X, raw X -> screen Y
  tx = map(yRaw, 200, 3900, 0, 320);
  ty = map(xRaw, 3900, 200, 0, 240);
  if (tx < 0) tx = 0; if (tx > 319) tx = 319;
  if (ty < 0) ty = 0; if (ty > 239) ty = 239;
  return valid;
}

void onShortTap() {
  screenOn = !screenOn;
  digitalWrite(TFT_BL, screenOn ? HIGH : LOW);
  Serial.printf("[touch] screen %s\n", screenOn ? "ON" : "OFF");
}

void onLongHold() {
  Serial.println("[touch] setup portal");
  startPortal();
}

void handleTouch() {
  if (netMode == NET_PORTAL) return;
  bool pressed = inputPressed();
  unsigned long now = millis();

  if (pressed && !wasPressed) {
    int tx, ty;
    if (!touchRead(tx, ty)) return;   // idle-looking read: line noise, not a tap
    pressStartMs = now;
    wasPressed = true;
    holdFired = false;
    Serial.printf("[touch] press @ %d,%d\n", tx, ty);
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

bool introMode = false;

// ── DM-01 boot intro (2x for 320x240) ──
#define DM01_SCALE 2
#include "Dm01Intro.h"

// ── Rendering: QUARTZ light LCD, scaled 2x ──
static inline bool blinkOn() { return (millis() / 450) % 2 == 0; }

const char* statusText(uint16_t& color) {
  if (ntpValid()) { color = panelEdge(); return "NTP"; }
  if (netMode == NET_UP) { color = blinkOn() ? panelInk() : panelGhost(); return "SYNC"; }
  color = blinkOn() ? accent() : panelGhost();
  return "OFF";
}

void drawLane(int i) {
  Pong& p = lanes[i];
  int top = LANE_Y0 + i * LANE_H;
  const char* lab = ZONES[laneZone[i]].label;
  fbText((WIDTH - textW57(lab, 2)) / 2, top + 3, lab, panelInk(), 2);

  // ghost 7-seg clock HH:MM centred — the score is still what the match advances
  uint16_t litH = (millis() < p.flashHUntil) ? accent() : panelInk();
  uint16_t litM = (millis() < p.flashMUntil) ? accent() : panelInk();
  const int Wc = 20, Hc = 30, T = 5, gap = 6, colonW = 9, colonT = 5;
  int x = (WIDTH - (Wc * 4 + 4 * gap + colonW)) / 2;
  int y = top + CLK_DY;
  sevenSegDigit(x, y, Wc, Hc, T, p.hh / 10, litH, panelGhost()); x += Wc + gap;
  sevenSegDigit(x, y, Wc, Hc, T, p.hh % 10, litH, panelGhost()); x += Wc + gap;
  fbFillRect(x + 2, y + Hc / 2 - colonT - 3, colonT, colonT, panelInk());
  fbFillRect(x + 2, y + Hc / 2 + 3, colonT, colonT, panelInk());
  x += colonW + gap;
  sevenSegDigit(x, y, Wc, Hc, T, p.mm / 10, litM, panelGhost()); x += Wc + gap;
  sevenSegDigit(x, y, Wc, Hc, T, p.mm % 10, litM, panelGhost());

  // paddles at the lane edges: red left, ink right
  int lpx = FLD_X0 - BALL_R - PAD_W + 1;   // 4
  fbFillRect(lpx, (int)roundf(p.lpy) - PAD_H / 2, PAD_W, PAD_H, accent());
  fbFillRect(FLD_X1 + BALL_R, (int)roundf(p.rpy) - PAD_H / 2, PAD_W, PAD_H, panelInk());
  // ink ball
  fbFillCircle((int)roundf(p.bx), (int)roundf(p.by), BALL_R, panelInk());

  if (i < LANE_COUNT - 1) fbHLine(0, top + LANE_H - 1, WIDTH, panelEdge());
}

void drawHeader() {
  fbText(12, HDR_Y, "PONG", panelInk(), 2);
  char dbuf[12];
  zoneDate(dbuf, sizeof(dbuf));
  fbText((WIDTH - textW57(dbuf, 2)) / 2, HDR_Y, dbuf, panelInk(), 2);
  uint16_t sc;
  const char* st = statusText(sc);
  fbText(WIDTH - 12 - textW57(st, 2), HDR_Y, st, sc, 2);
}

void drawBootScreen() {
  fbText(12, HDR_Y, "PONG", panelInk(), 2);
  fbText(WIDTH - 12 - textW57("SYNC", 2), HDR_Y, "SYNC", panelEdge(), 2);
  fbHLine(8, ACC_Y, WIDTH - 16, accentDK());
  fbFillRect(8, ACC_Y + 1, WIDTH - 16, 6, accent());
  for (int y = 44; y <= 200; y += 4)
    for (int x = ((y / 4) & 1) * 2; x < WIDTH; x += 4) FPIX(x, y, panelGhost());
  fbText((WIDTH - textW57("CONNECTING", 2)) / 2, 84, "CONNECTING", panelInk(), 2);
  if (wifiSSID.length()) {
    const char* ss = wifiSSID.c_str();
    fbText((WIDTH - textW57(ss, 2)) / 2, 122, ss, panelInk(), 2);
  }
  fbText(12, HEIGHT - 24, "TAP LIGHT", panelInk(), 2);
  fbText(WIDTH - 12 - textW57("HOLD SETUP", 2), HEIGHT - 24, "HOLD SETUP", accentDK(), 2);
}

void drawPortal() {
  fbText(12, HDR_Y, "PONG", panelInk(), 2);
  fbText(WIDTH - 12 - textW57("SETUP", 2), HDR_Y, "SETUP", accent(), 2);
  fbHLine(8, ACC_Y, WIDTH - 16, accentDK());
  fbHLine(8, ACC_Y + 1, WIDTH - 16, accentDK());
  fbFillRect(8, ACC_Y + 2, WIDTH - 16, 8, accent());
  fbText((WIDTH - textW57("WIFI SETUP", 2)) / 2, 56, "WIFI SETUP", panelInk(), 2);
  fbText(24, 96, "1 JOIN WIFI", panelInk(), 2);
  fbText(24, 116, AP_SSID, panelInk(), 2);
  fbText(24, 148, "2 OPEN BROWSER", panelInk(), 2);
  fbText(24, 168, "192.168.4.1", accent(), 2);
  fbText((WIDTH - textW57("SAVES + REBOOTS", 2)) / 2, 208, "SAVES + REBOOTS", accentDK(), 2);
}

void drawFrame() {
  fbClear(panelBG());   // always the near-black deck — no state switching
  if (netMode == NET_PORTAL) { drawPortal(); return; }
  if (bootScreen) { drawBootScreen(); return; }
  drawHeader();
  fbHLine(8, ACC_Y, WIDTH - 16, accentDK());
  fbFillRect(8, ACC_Y + 1, WIDTH - 16, 6, accent());
  for (int i = 0; i < LANE_COUNT; i++) drawLane(i);
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
  Serial.println("\n=== PONG CLOCK CYD ===");

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  SPI.begin(TFT_SCLK, TFT_MISO, TFT_MOSI, TFT_CS);
  tft.init(240, 320);
  tft.setSPISpeed(80000000);
  tft.setRotation(1);
  // The library ST7789 init sends INVON; the CYD panel needs it off.
  tft.invertDisplay(false);
  tft.fillScreen(0x0000);

  touchInit();
  loadCredentials();
  initGame();
  refreshZoneOffsets(true);
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
    Serial.println("offline clock");
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

  static unsigned long lastFrameMs = 0;
  unsigned long nowMs = millis();
  float dt = (nowMs - lastFrameMs) / 1000.0f;
  lastFrameMs = nowMs;
  if (dt > 0.06f) dt = 0.06f;

  handleTouch();
  netWatchdog();
  refreshTargets(false);
  for (int i = 0; i < LANE_COUNT; i++) pongStep(i, dt);
  render();
  delay(8);
}