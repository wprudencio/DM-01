// Pong Clock — ESP32-C3 + ST7735 160x128.
//
// A Pong match plays itself and the score IS the time: left score = hours,
// right score = minutes. The paddles track the ball and only "step aside" when
// the clock needs a point — the left paddle misses when the minute must
// advance (right score +1), the right paddle when the hour must advance.
// Anything the game can't express with a single point (NTP resync, long
// offline gap) snaps the digits with a red flash.
//
// Time comes from NTP only (no API keys). Each zone is a POSIX TZ string, so
// DST is handled by newlib. Before the first sync the match runs on a fake
// epoch (boot ≈ 12:00 UTC) and snaps once SNTP lands. Credentials live in NVS
// ("dm01", shared with btc_ticker) and are written by the captive portal —
// never hardcoded.
//
// Optional input: tap pad (GPIO 0) / BOOT (GPIO 9) = nightlight on/off;
// hold ≥ 1.5 s = WiFi setup portal. The WS2812 (GPIO 10) is the nightlight —
// a slow warm glow — and doubles as the network/status light before sync.
//
// WiFi reliability follows the esp32-c3-ws2812 skill §10: TX power 8.5 dBm,
// sleep off, auto-reconnect off, retries hard-reset first, and begin() is
// never called while a connection attempt is still pending. Rendering uses the
// SIGNAL/VHS theme (signal-vhs-ui skill): near-black deck, magenta + signal
// green accents, Departure Mono type and a colourful per-element glitch filter.

#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <Adafruit_NeoPixel.h>
#include <SPI.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <math.h>
#include <time.h>
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

// ── SIGNAL palette — near-black deck, magenta + signal green (VHS theme) ──
uint16_t panelBG()    { return rgb( 16,  16,  16); } // #101010 deck
uint16_t panelInk()   { return rgb(255, 255, 255); } // white ink
uint16_t panelGhost() { return rgb( 74,  74,  74); } // grid dots / stale digits
uint16_t panelHI()    { return rgb(170, 170, 170); } // light grey rule
uint16_t panelEdge()  { return rgb(170, 170, 170); } // light grey rules + borders
uint16_t accent()     { return rgb(236,   0, 140); } // #EC008C magenta
uint16_t accentDK()   { return rgb(104,   0,  62); } // dimmed magenta
uint16_t accentGN()   { return rgb(  0, 230, 118); } // #00E676 signal green

// ── VHS filter: slow chroma ripple + per-element glitch hits. Each UI band
//     tears on its own 6s slots and its rip rows are washed with cycling
//     primaries (yellow/cyan/green/magenta/red/blue). Only a single row is
//     buffered (the taps read horizontal neighbours), keeping RAM tiny. ──
static uint16_t vhsRow[WIDTH];

static inline uint32_t vhsNoise(uint32_t x) {
  x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
  return x;
}

// saturated monitor-test primaries for the rip rows
static uint16_t vhsPal(uint8_t i) {
  switch (i % 6) {
    case 0: return rgb(255, 255,   0); // yellow
    case 1: return rgb(  0, 255, 255); // cyan
    case 2: return rgb(  0, 255,   0); // green
    case 3: return rgb(255,   0, 255); // magenta
    case 4: return rgb(255,   0,   0); // red
    default:return rgb(  0,   0, 255); // blue
  }
}

static bool elemHit(int i, uint32_t t, int &dx, uint8_t &pal) {
  const uint32_t SLOT = 6000;
  uint32_t phased = t + (uint32_t)(i * 1013u);
  uint32_t s = vhsNoise(phased / SLOT * 2654435761u ^ ((uint32_t)(i + 3) * 97u));
  if ((s % 5u) != 0) return false;
  uint32_t start = (s >> 8) % (SLOT - 500u);
  uint32_t dur   = 200u + ((s >> 20) % 240u);
  uint32_t local = phased % SLOT;
  if (local < start || local >= start + dur) return false;
  dx  = (int)((s >> 12) % 11u) - 5;   // ±5px tears
  pal = (uint8_t)(s >> 24);
  return true;
}

void vhsApply() {
  uint32_t t = millis();
  float ph = (float)t * 0.0018f;

  // per-element bands: header, strip, court above the clock, clock, court below
  static const int bandY0[5] = {   2, 13,  22,  50,  86 };
  static const int bandY1[5] = {  13, 18,  50,  86, 126 };
  int bDx[5], bBoost[5]; float bChroma[5]; uint8_t bPal[5]; bool bHit[5];
  for (int i = 0; i < 5; i++) {
    bHit[i]    = elemHit(i, t, bDx[i], bPal[i]);
    bDx[i]     = bHit[i] ? bDx[i] : 0;
    bChroma[i] = bHit[i] ? 7.0f : 0.0f;
    bBoost[i]  = bHit[i] ? 4 : 0;
  }

  int flick = 246 + (int)(vhsNoise(t / 110u) % 11);

  for (int y = 0; y < HEIGHT; y++) {
    int bi = -1;
    for (int i = 0; i < 5; i++) if (y >= bandY0[i] && y < bandY1[i]) { bi = i; break; }
    if (bi < 0) continue;
    bool hit = bHit[bi];
    float lsh = 1.4f + 1.3f * sinf(y * 0.085f + ph) + bChroma[bi];
    float rsh = 1.8f + 2.1f * sinf(y * 0.061f + 1.7f + ph * 1.3f) + bChroma[bi];
    int boost = bBoost[bi], xOff = bDx[bi];
    // rip colour: shifts every 60ms and every 8 rows inside the hit
    uint16_t pc = vhsPal((uint8_t)(bPal[bi] + t / 60u + (y >> 3)));
    int pr = (pc >> 11) & 0x1F, pg = (pc >> 5) & 0x3F, pb = pc & 0x1F;
    if (lsh < 0) lsh = 0; if (rsh < 0) rsh = 0;
    int li0 = (int)lsh, ri0 = (int)rsh;   // nearest-tap chroma (the sub-pixel
                                          // interpolation cost more than it showed)
    memcpy(vhsRow, &fb[y * WIDTH], WIDTH * sizeof(uint16_t));
    const uint16_t* s = vhsRow;
    uint16_t* d = &fb[y * WIDTH];
    for (int x = 0; x < WIDTH; x++) {
      int cx = x + xOff;
      if (cx < 0) cx = 0; else if (cx >= WIDTH) cx = WIDTH - 1;
      int lp = cx - li0; if (lp < 0) lp = 0;
      int rp = cx + ri0; if (rp >= WIDTH) rp = WIDTH - 1;
      uint16_t cC = s[cx];
      int r = (3 * (s[lp] >> 11) + (cC >> 11)) >> 2;    // 75% shift = stronger fringe
      int g = (cC >> 5) & 0x3F;
      int b = (3 * (s[rp] & 0x1F) + (cC & 0x1F)) >> 2;
      r = (r * flick) >> 8; g = (g * flick) >> 8; b = (b * flick) >> 8;
      r += boost; g += boost; b += boost;
      if (hit) {                                       // wash the band with the palette
        int w = (g > 22) ? 3 : 2;                      // 75% on ink, 50% on the deck
        r = (r * (4 - w) + pr * w) >> 2;
        g = (g * (4 - w) + pg * w) >> 2;
        b = (b * (4 - w) + pb * w) >> 2;
      }
      if (r < 0) r = 0; if (r > 31) r = 31;
      if (g < 0) g = 0; if (g > 63) g = 63;
      if (b < 0) b = 0; if (b > 31) b = 31;
      d[x] = (uint16_t)((r << 11) | (g << 5) | b);
    }
  }
}

void vhsFlush() { vhsApply(); fbFlush(); }

// ── Departure Mono: labels use the hand-drawn 5x6 pixel set (crisp at any
//    size); scale >= 2 uses the pixel-perfect 11px raster. y is the cap top. ──
#include "DepartureMono.h"
#include "DepartureMonoSmall.h"

static inline const uint8_t* dmsGlyph(char ch) {
  if (ch < 32 || ch > 90) return dmsFont[0];
  return dmsFont[ch - 32];
}
static inline const uint8_t* dmGlyphBig(char ch) {
  if (ch < 32 || ch > 90) return dmFont[0];
  return dmFont[ch - 32];
}
void fbChar(int x, int y, char ch, uint16_t c, int scale) {
  if (ch == ' ') return;
  if (ch >= 'a' && ch <= 'z') ch -= 32; // normalize — labels are UPPER
  if (scale >= 2) {
    const uint8_t* g = dmGlyphBig(ch);
    int cy = y - DM_TOP * scale;                    // y = cap/digit top
    for (int row = 0; row < DM_H; row++) {
      uint8_t bits = g[row];
      if (!bits) continue;
      for (int col = 0; col < DM_W; col++)
        if (bits & (0x40 >> col)) fbFillRect(x + col * scale, cy + row * scale, scale, scale, c);
    }
  } else {
    const uint8_t* g = dmsGlyph(ch);
    int cy = y - DMS_TOP;
    for (int row = 0; row < DMS_H; row++) {
      uint8_t bits = g[row];
      if (!bits) continue;
      for (int col = 0; col < DMS_ADV; col++)
        if (bits & (0x40 >> col)) fbFillRect(x + col, cy + row, 1, 1, c);
    }
  }
}
void fbText(int x, int y, const char* s, uint16_t c, int scale = 1) {
  while (*s) {
    fbChar(x, y, *s, c, scale);
    x += (scale >= 2) ? DM_ADV * scale : DMS_ADV;
    s++;
  }
}
int textW57(const char* s, int scale = 1) {
  int n = 0; while (*s++) n++;
  if (!n) return 0;
  return (scale >= 2) ? (n * DM_ADV * scale - scale) : (n * DMS_ADV - 1);
}
// 11px raster at 1x — setup screens need a size between the 5x6 labels and 2x
void fbText11(int x, int y, const char* s, uint16_t c) {
  while (*s) {
    if (*s != ' ') {
      char ch = (*s >= 'a' && *s <= 'z') ? (char)(*s - 32) : *s;
      const uint8_t* g = dmGlyphBig(ch);
      int cy = y - DM_TOP;
      for (int row = 0; row < DM_H; row++) {
        uint8_t bits = g[row];
        if (!bits) continue;
        for (int col = 0; col < DM_W; col++)
          if (bits & (0x40 >> col)) fbFillRect(x + col, cy + row, 1, 1, c);
      }
    }
    x += DM_ADV;
    s++;
  }
}
int textW11(const char* s) { int n = 0; while (*s++) n++; return n ? n * DM_ADV - 1 : 0; }


// ── Layout (skill §8) ──
#define HDR_Y 3
#define ACC_Y 13          // dark line; red bar 14..17
#define FLD_L 8           // ball centre min x (left paddle face + BALL_R)
#define FLD_R 152         // ball centre max x
#define FLD_T 22          // ball centre min y
#define FLD_B 124         // ball centre max y
#define NET_X 80
#define CLK_Y 56          // block clock cap top
#define CLK_H 24          // clock ink height (scale 3)
#define CLK_SC 3

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
int zoneIdx = 0;

// ── NTP clock ──
#define NTP_VALID 1700000000L   // anything past Nov 2023 means SNTP landed
unsigned long ntpStartMs = 0;
bool ntpStarted = false;

static inline bool ntpValid() { return time(nullptr) > NTP_VALID; }
// Before the first sync the match runs on a fake epoch so it always shows a
// plausible time: boot ≈ 12:00 UTC, each zone offset applied by the TZ string.
static inline time_t clockNow() {
  time_t t = time(nullptr);
  return t > NTP_VALID ? t : t + 43200;
}
// Cached UTC offsets: switching $TZ in the loop leaks in newlib (see the
// esp32-c3-ws2812 skill §10). Refreshed once a day; offsets from a localtime_r
// result converted with civil-day math (newlib's mktime round trip returns 0).
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
    setenv("TZ", ZONES[i].tz, 1);
    tzset();
    struct tm lt;
    localtime_r(&t, &lt);
    long asUtc = daysFromCivil(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday) * 86400L +
                 lt.tm_hour * 3600L + lt.tm_min * 60L + lt.tm_sec;
    zoneOff[i] = (int)(asUtc - (long)t);
  }
  zoneOffDay = gt.tm_yday;
}
void zoneHM(int zi, int& h, int& m) {
  long zt = (long)clockNow() + zoneOff[zi];
  zt = ((zt % 86400L) + 86400L) % 86400L;
  h = (int)(zt / 3600);
  m = (int)((zt / 60) % 60);
}

// ── Network state ──
enum NetMode : uint8_t { NET_BOOT, NET_CONNECTING, NET_UP, NET_OFFLINE, NET_PORTAL };
NetMode netMode = NET_BOOT;
bool bootDone = false;
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

// ── Touch / buttons ──
int idleLevel = HIGH;
bool wasPressed = false;
unsigned long pressStartMs = 0;
bool holdFired = false;
#define HOLD_MS 1500

// ── Pong ──
#define BALL_R 2
#define PAD_W  3
#define PAD_H  18
#define SERVE_SPEED 88.0f
#define MAX_SPEED   190.0f
#define PAD_SPEED   135.0f
#define MIN_ANGLE   (18.0f * PI / 180.0f)
#define MAX_ANGLE   (50.0f * PI / 180.0f)
#define SERVE_MS    700

struct Pong {
  float bx, by, vx, vy;      // ball centre + velocity (px, px/s)
  float lpy, rpy;            // paddle centre y
  int hh, mm;                // score = time
  unsigned long flashHUntil, flashMUntil;
  unsigned long serveAt;
};
Pong pg;
int tgtH = 12, tgtM = 0;

static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

float predictY(float planeX) {
  if (fabsf(pg.vx) < 1.0f) return pg.by;
  float t = (planeX - pg.bx) / pg.vx;
  if (t < 0) return pg.by;
  float y = pg.by + pg.vy * t;
  float span = FLD_B - FLD_T;
  float rel = fmodf(y - FLD_T, 2.0f * span);
  if (rel < 0) rel += 2.0f * span;
  if (rel > span) rel = 2.0f * span - rel;
  return FLD_T + rel;
}

void startServe(bool towardRight) {
  pg.bx = (FLD_L + FLD_R) / 2.0f;
  pg.by = (FLD_T + FLD_B) / 2.0f;
  float ang = (18.0f + (float)(esp_random() % 22)) * (PI / 180.0f); // 18..39°
  if (esp_random() & 1) ang = -ang;
  pg.vx = (towardRight ? 1.0f : -1.0f) * SERVE_SPEED * cosf(ang);
  pg.vy = SERVE_SPEED * sinf(ang);
  pg.serveAt = millis() + SERVE_MS;
}

// Reflect off a paddle: speed climbs per hit, exit angle comes from where the
// ball struck (never flat, so rallies can't degenerate into straight lines).
void paddleBounce(bool offLeft) {
  float sp = sqrtf(pg.vx * pg.vx + pg.vy * pg.vy) * 1.06f;
  if (sp > MAX_SPEED) sp = MAX_SPEED;
  float off = pg.by - (offLeft ? pg.lpy : pg.rpy);
  off = clampf(off / (PAD_H / 2.0f), -1.0f, 1.0f);
  float ang = off * MAX_ANGLE;
  if (fabsf(ang) < MIN_ANGLE) ang = (ang < 0 ? -1.0f : 1.0f) * MIN_ANGLE;
  pg.vx = (offLeft ? 1.0f : -1.0f) * sp * cosf(ang);
  pg.vy = sp * sinf(ang);
  pg.bx = offLeft ? FLD_L : FLD_R;
}

void movePaddle(float& py, float target, float dt) {
  float step = PAD_SPEED * dt;
  float d = target - py;
  if (d > step) d = step; else if (d < -step) d = -step;
  py = clampf(py + d, FLD_T + PAD_H / 2.0f, FLD_B - PAD_H / 2.0f);
}

void scorePoint(bool leftMissed) {
  // Ball out on the left -> right player (minutes) scores, and vice versa.
  if (leftMissed) {
    pg.mm = (pg.mm + 1) % 60;
    if (pg.mm != tgtM) pg.mm = tgtM;   // wrapped past the target: snap
    pg.flashMUntil = millis() + 900;
  } else {
    pg.hh = (pg.hh + 1) % 24;
    if (pg.hh != tgtH) pg.hh = tgtH;
    pg.flashHUntil = millis() + 900;
  }
  startServe(leftMissed);   // serve toward the player who just lost the point
}

void syncScores() {
  if (pg.mm != tgtM && (tgtM - pg.mm + 60) % 60 > 1) { pg.mm = tgtM; pg.flashMUntil = millis() + 900; }
  if (pg.hh != tgtH && (tgtH - pg.hh + 24) % 24 > 1) { pg.hh = tgtH; pg.flashHUntil = millis() + 900; }
}

void refreshTargets(bool force) {
  static unsigned long last = 0;
  if (!force && millis() - last < 500) return;
  last = millis();
  refreshZoneOffsets(false);
  zoneHM(zoneIdx, tgtH, tgtM);
  syncScores();
}

void pongStep(float dt) {
  float midY = (FLD_T + FLD_B) / 2.0f;
  if (pg.serveAt != 0 && millis() >= pg.serveAt) pg.serveAt = 0;
  bool inPlay = (pg.serveAt == 0);

  // Paddles aim at the intercept (plus a small wobble so rallies vary); when
  // the clock needs a point they run away from the ball instead.
  float rPred = predictY(FLD_R) + sinf(millis() * 0.0017f) * 3.5f;
  float lPred = predictY(FLD_L) + sinf(millis() * 0.0023f + 2.0f) * 3.5f;
  float rTarget = (pg.hh != tgtH) ? (rPred < midY ? FLD_B : FLD_T)
                 : (pg.vx > 0 ? rPred : midY);
  float lTarget = (pg.mm != tgtM) ? (lPred < midY ? FLD_B : FLD_T)
                 : (pg.vx < 0 ? lPred : midY);
  movePaddle(pg.rpy, rTarget, dt);
  movePaddle(pg.lpy, lTarget, dt);

  if (!inPlay) return;

  pg.bx += pg.vx * dt;
  pg.by += pg.vy * dt;

  if (pg.by < FLD_T) { pg.by = FLD_T; pg.vy = fabsf(pg.vy); }
  if (pg.by > FLD_B) { pg.by = FLD_B; pg.vy = -fabsf(pg.vy); }

  // Paddle planes: only a paddle that isn't stepping aside can return the ball
  if (pg.vx < 0 && pg.bx <= FLD_L && pg.mm == tgtM &&
      fabsf(pg.by - pg.lpy) <= PAD_H / 2.0f + BALL_R) paddleBounce(true);
  if (pg.vx > 0 && pg.bx >= FLD_R && pg.hh == tgtH &&
      fabsf(pg.by - pg.rpy) <= PAD_H / 2.0f + BALL_R) paddleBounce(false);

  // Out of bounds: the point always resolves to the clock's target
  if (pg.bx < FLD_L - 12) scorePoint(true);
  else if (pg.bx > FLD_R + 12) scorePoint(false);
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
  Serial.printf("WiFi begin -> %d\n", (int)WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str()));

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

// Non-blocking retry: prepare + begin and let loop() poll the status, so the
// clock keeps playing while the radio tries to come back.
void wifiRetryStart() {
  wifiPrepare(true, false);
  Serial.printf("WiFi retry ssid=\"%s\" -> %d\n", wifiSSID.c_str(), (int)WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str()));
}

void loadCredentials() {
  preferences.begin("dm01", true);
  wifiSSID = preferences.getString("ssid", "");
  wifiPassword = preferences.getString("password", "");
  zoneIdx = preferences.getInt("tz", 0);
  preferences.end();
  if (zoneIdx < 0 || zoneIdx >= ZONE_COUNT) zoneIdx = 0;

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

// ── Captive portal (NVS "dm01", shared with btc_ticker) ──
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
<p>WiFi + timezone setup &mdash; saved to NVS, device reboots.</p>
<form action="/save" method="POST">
  <label for="ssid">WiFi name</label>
  <input type="text" id="ssid" name="ssid" placeholder="SSID" required>
  <label for="password">Password</label>
  <input type="password" id="password" name="password" placeholder="password">
  <label for="tz">Timezone</label>
  <select id="tz" name="tz">)HTML";
  for (int i = 0; i < ZONE_COUNT; i++) {
    html += "<option value='" + String(i) + "'";
    if (i == zoneIdx) html += " selected";
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

// ── Touch: tap = nightlight, hold = setup portal ──
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

bool nightOn = true;

void onShortTap() {
  nightOn = !nightOn;
  Serial.printf("[btn] nightlight %s\n", nightOn ? "ON" : "OFF");
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

// ── LED: nightlight + status ──
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
  } else if (!ntpValid()) {
    float p = (sinf(millis() * 0.004f) + 1.0f) * 0.5f;
    pixels.setBrightness(60);
    pixels.setPixelColor(0, pixels.Color(0, 20, 60 + p * 120));
  } else if (nightOn) {
    float p = (sinf(millis() * 0.0015f) + 1.0f) * 0.5f;
    pixels.setBrightness(22);
    pixels.setPixelColor(0, pixels.Color(255, 105 + p * 35, 18));
  } else {
    pixels.setBrightness(0);
    pixels.clear();
  }
  pixels.show();
}

// ── Rendering: Casio F-91W light LCD ──
static inline bool blinkOn() { return (millis() / 450) % 2 == 0; }

const char* statusText(uint16_t& color) {
  if (ntpValid()) { color = panelEdge(); return "NTP"; }
  if (netMode == NET_UP) { color = blinkOn() ? panelInk() : panelGhost(); return "SYNC"; }
  color = blinkOn() ? accent() : panelGhost();
  return "OFF";
}

void drawField() {
  // net dashes only above/below the centred clock (light grey rules)
  for (int y = FLD_T; y < CLK_Y - 6; y += 8) fbFillRect(NET_X, y, 2, 4, panelEdge());
  for (int y = CLK_Y + CLK_H + 6; y <= FLD_B; y += 8) fbFillRect(NET_X, y, 2, 4, panelEdge());

  // block clock HH:MM centred — the score is still what the match advances
  char hbuf[4], mbuf[4];
  snprintf(hbuf, sizeof(hbuf), "%02d", pg.hh);
  snprintf(mbuf, sizeof(mbuf), "%02d", pg.mm);
  uint16_t litH = (millis() < pg.flashHUntil) ? accent() : panelInk();
  uint16_t litM = (millis() < pg.flashMUntil) ? accent() : panelInk();
  int wH = textW57(hbuf, CLK_SC), wC = textW57(":", CLK_SC), wM = textW57(mbuf, CLK_SC);
  int x = (WIDTH - (wH + wC + wM)) / 2;
  fbText(x, CLK_Y, hbuf, litH, CLK_SC);
  fbText(x + wH, CLK_Y, ":", panelInk(), CLK_SC);
  fbText(x + wH + wC, CLK_Y, mbuf, litM, CLK_SC);

  // paddles at the field edges: magenta left, signal green right
  int lpx = FLD_L - BALL_R - PAD_W + 1;
  fbFillRect(lpx, (int)roundf(pg.lpy) - PAD_H / 2, PAD_W, PAD_H, accent());
  fbFillRect(FLD_R + BALL_R, (int)roundf(pg.rpy) - PAD_H / 2, PAD_W, PAD_H, accentGN());
  // white ball
  fbFillCircle((int)roundf(pg.bx), (int)roundf(pg.by), BALL_R, panelInk());
}

void render() {
  fbClear(panelBG());
  fbText(6, HDR_Y, "PONG", panelInk());
  const char* zl = ZONES[zoneIdx].label;
  fbText((WIDTH - textW57(zl)) / 2, HDR_Y, zl, panelInk());
  uint16_t sc;
  const char* st = statusText(sc);
  fbText(WIDTH - 6 - textW57(st), HDR_Y, st, sc);

  fbHLine(4, ACC_Y, WIDTH - 8, accentDK());
  fbFillRect(4, ACC_Y + 1, WIDTH - 8, 4, accent());

  drawField();
  vhsFlush();
}

void renderConnectScreen() {
  fbClear(panelBG());
  fbText(6, HDR_Y, "PONG", panelInk());
  fbText(WIDTH - 6 - textW57("SYNC"), HDR_Y, "SYNC", panelEdge());
  fbHLine(4, ACC_Y, WIDTH - 8, accentDK());
  fbFillRect(4, ACC_Y + 1, WIDTH - 8, 4, accent());
  fbText((WIDTH - textW57("CONNECTING", 2)) / 2, 44, "CONNECTING", panelInk(), 2);
  if (wifiSSID.length()) {
    const char* ss = wifiSSID.c_str();
    fbText((WIDTH - textW57(ss)) / 2, 68, ss, panelEdge());
  }
  fbText((WIDTH - textW57("NTP CLOCK")) / 2, 100, "NTP CLOCK", accentDK());
  vhsFlush();
}

void renderPortal() {
  fbClear(panelBG());
  fbText(6, HDR_Y, "PONG", panelInk());
  fbText(WIDTH - 6 - textW57("SETUP"), HDR_Y, "SETUP", accent());
  fbHLine(4, ACC_Y, WIDTH - 8, accentDK());
  fbFillRect(4, ACC_Y + 1, WIDTH - 8, 4, accent());
  fbText11((WIDTH - textW11("WIFI SETUP")) / 2, 28, "WIFI SETUP", panelInk());
  fbText11(8, 48, "1 JOIN WIFI", panelInk());
  if (textW11(AP_SSID) <= WIDTH - 16) fbText11(8, 62, AP_SSID, panelInk());
  else fbText(8, 62, AP_SSID, panelInk());         // long SSID: fall back to 5x6
  fbText11(8, 82, "2 OPEN BROWSER", panelInk());
  fbText11(8, 96, "192.168.4.1", accent());
  fbText11((WIDTH - textW11("SAVES + REBOOTS")) / 2, 114, "SAVES + REBOOTS", accentDK());
  vhsFlush();
}

#include "Dm01Intro.h"

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== PONG CLOCK ===");

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

  refreshZoneOffsets(true);
  refreshTargets(true);
  pg.hh = tgtH;
  pg.mm = tgtM;
  pg.lpy = pg.rpy = (FLD_T + FLD_B) / 2.0f;
  startServe((esp_random() & 1) != 0);

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
  else { netMode = NET_OFFLINE; lastReconnectMs = millis(); Serial.println("offline clock"); }
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

  static unsigned long lastFrameMs = 0;
  unsigned long nowMs = millis();
  float dt = (nowMs - lastFrameMs) / 1000.0f;
  lastFrameMs = nowMs;
  if (dt > 0.06f) dt = 0.06f;

  handleTouch();
  netWatchdog();
  refreshTargets(false);
  pongStep(dt);
  updateLed();
  render();
  screenshotHandle(fb, WIDTH, HEIGHT);
  delay(8);
}
