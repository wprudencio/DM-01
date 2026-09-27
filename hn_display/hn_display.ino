// HACKER NEWS — ESP32-C3 + ST7735 160x128.
//
// Top stories from the keyless Hacker News Firebase API, in the SIGNAL/VHS
// theme (signal-vhs-ui skill): near-black deck, white ink, a magenta accent
// strip, signal-green live pip + scores, light-grey rules and a per-element
// VHS glitch (chroma ripple + palette-washed rip bands).
//
// Long-press the pad (GPIO 0) to marquee-scroll long titles; 's' over serial
// toggles it too. Screenshots: send 'S' (serial screenshot hook).

#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>
#include <Adafruit_NeoPixel.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <math.h>
#include "Screenshot.h"

// --- Pinout ---
#define TFT_CS    5
#define TFT_RST   4
#define TFT_DC    3
#define TFT_MOSI  2
#define TFT_SCLK  1

#define TOUCH_PIN     0
#define LED_PIN       10
#define NUMPIXELS     1
#define LONG_PRESS_MS 600
#define MARQUEE_MS_PER_PX  25
#define MARQUEE_PAUSE_MS   2000

// --- WiFi ---
// Set your network before flashing: nothing is committed to the repo.
#define WIFI_SSID     ""
#define WIFI_PASSWORD ""

// --- Display ---
#define WIDTH  160
#define HEIGHT 128

Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_RST);
Adafruit_NeoPixel pixels(NUMPIXELS, LED_PIN, NEO_GRB + NEO_KHZ800);

// --- Framebuffer ---
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
void fbFlush() {
  tft.startWrite();
  tft.setAddrWindow(0, 0, WIDTH, HEIGHT);
  tft.writePixels(fb, WIDTH * HEIGHT);
  tft.endWrite();
}

uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) { return tft.color565(r, g, b); }

// ── SIGNAL palette — near-black deck, white ink, magenta + signal green ──
uint16_t cBlack()   { return rgb( 16,  16,  16); } // #101010 deck
uint16_t cWhite()   { return rgb(255, 255, 255); } // white ink
uint16_t cMagenta() { return rgb(236,   0, 140); } // #EC008C accent strip
uint16_t cGreen()   { return rgb(  0, 230, 118); } // #00E676 live + scores
uint16_t cLine()    { return rgb(170, 170, 170); } // light grey rules
uint16_t cGhost()   { return rgb( 74,  74,  74); } // stale digits / pip off
uint16_t cMagentaD(){ return rgb(104,   0,  62); } // dimmed magenta edging

uint16_t scale565(uint16_t c, int pct) {   // pct/256 brightness (>256 brightens)
  int r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
  r = r * pct >> 8; g = g * pct >> 8; b = b * pct >> 8;
  if (r > 31) r = 31; if (g > 63) g = 63; if (b > 31) b = 31;
  return (uint16_t)((r << 11) | (g << 5) | b);
}

// ── Departure Mono: labels use the hand-drawn 5x6 pixel set (crisp at 1x);
//    scale >= 2 uses the pixel-perfect 11px raster. y is the cap top. ──
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
    int cy = y - DM_TOP * scale;
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

// ── VHS filter: slow chroma ripple + per-element glitch hits. Each UI band
//    tears on its own 6s slots and its rip rows are washed with cycling
//    primaries (yellow/cyan/green/magenta/red/blue). Only a single row is
//    buffered (the taps read horizontal neighbours), keeping RAM tiny. ──
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

  // per-element bands: header, accent strip, status bar. The story rows stay
  // crisp — the chroma ripple never runs over the news titles.
  static const int bandY0[3] = {   1, 13, 113 };
  static const int bandY1[3] = {  13, 19, 128 };
  int bDx[3], bBoost[3]; float bChroma[3]; uint8_t bPal[3]; bool bHit[3];
  for (int i = 0; i < 3; i++) {
    bHit[i]    = elemHit(i, t, bDx[i], bPal[i]);
    bDx[i]     = bHit[i] ? bDx[i] : 0;
    bChroma[i] = bHit[i] ? 7.0f : 0.0f;
    bBoost[i]  = bHit[i] ? 4 : 0;
  }

  int flick = 246 + (int)(vhsNoise(t / 110u) % 11);

  for (int y = 0; y < HEIGHT; y++) {
    int bi = -1;
    for (int i = 0; i < 3; i++) if (y >= bandY0[i] && y < bandY1[i]) { bi = i; break; }
    if (bi < 0) continue;
    bool hit = bHit[bi];
    float lsh = 1.4f + 1.3f * sinf(y * 0.085f + ph) + bChroma[bi];
    float rsh = 1.8f + 2.1f * sinf(y * 0.061f + 1.7f + ph * 1.3f) + bChroma[bi];
    int boost = bBoost[bi], xOff = bDx[bi];
    // rip colour: shifts every 60ms and every 8 rows inside the hit
    uint16_t pc = vhsPal((uint8_t)(bPal[bi] + t / 60u + (y >> 3)));
    int pr = (pc >> 11) & 0x1F, pg = (pc >> 5) & 0x3F, pb = pc & 0x1F;
    if (lsh < 0) lsh = 0; if (rsh < 0) rsh = 0;
    int li0 = (int)lsh, lfr = (int)((lsh - li0) * 256.0f);
    int ri0 = (int)rsh, rfr = (int)((rsh - ri0) * 256.0f);
    memcpy(vhsRow, &fb[y * WIDTH], WIDTH * sizeof(uint16_t));
    const uint16_t* s = vhsRow;
    uint16_t* d = &fb[y * WIDTH];
    for (int x = 0; x < WIDTH; x++) {
      int cx = x + xOff;
      if (cx < 0) cx = 0; if (cx > WIDTH - 1) cx = WIDTH - 1;
      uint16_t cC = s[cx];
      int lp = cx - li0, lq = lp - 1; if (lp < 0) lp = 0; if (lq < 0) lq = 0;
      int rp = cx + ri0, rq = rp + 1; if (rp > WIDTH - 1) rp = WIDTH - 1; if (rq > WIDTH - 1) rq = WIDTH - 1;
      uint16_t la = s[lp], lb = s[lq], ra = s[rp], rb = s[rq];
      int rSrc = (int)(((((la >> 11) & 0x1F) * (256 - lfr)) + (((lb >> 11) & 0x1F) * lfr)) >> 8);
      int bSrc = (int)((((ra & 0x1F) * (256 - rfr)) + ((rb & 0x1F) * rfr)) >> 8);
      int r = (3 * rSrc + ((cC >> 11) & 0x1F)) >> 2;   // 75% shift = stronger fringe
      int g = (cC >> 5) & 0x3F;
      int b = (3 * bSrc + (cC & 0x1F)) >> 2;
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

void present() {
  vhsApply();
  fbFlush();
  screenshotHandle(fb, WIDTH, HEIGHT);
}

// --- Layout ---
#define HEADER_H   14
#define STRIP_LINE 13
#define STATUS_H   9
#define STATUS_Y   (HEIGHT - STATUS_H)
#define STORY_H    11
#define STORY_TOP  19
#define MAX_VISIBLE 9
#define SCORE_X    20
#define TITLE_X    46
#define TITLE_RM   4

// --- Story Storage ---
#define MAX_STORIES 30
#define TITLE_MAX   80

struct Story {
  char title[TITLE_MAX];
  int score;
  int descendants;
  bool valid;
};

Story stories[MAX_STORIES];
int storyCount = 0;

// --- State ---
unsigned long lastFetchMs = 0;
bool fetching = false;
bool hasData = false;
int fetchPhase = 0;
int fetchStoryIdx = 0;
int topIDs[35];
int topIDCount = 0;
int lastHttpCode = 0;
char statusMsg[40] = "BOOTING...";
bool fetchFailed = false;
int refreshIntervalMs = 3600000;

// --- Button state ---
bool scrolling = false;       // long-press marquee mode
unsigned long scrollStartMs = 0;
bool btnDown = false;
unsigned long btnAt = 0;
int  releaseCnt = 0;          // debounce release

// --- LED ---
void setLed(uint8_t r, uint8_t g, uint8_t b) {
  pixels.setPixelColor(0, pixels.Color(r, g, b));
  pixels.show();
}

// --- JSON helpers ---
bool jsonExtractString(const String& json, const char* key, char* out, int outMax) {
  String search = "\"";
  search += key;
  search += "\":\"";
  int start = json.indexOf(search);
  if (start < 0) return false;
  start += search.length();
  int end = start;
  while (end < json.length()) {
    char c = json[end];
    if (c == '\\') { end += 2; continue; }
    if (c == '"') break;
    end++;
  }
  int len = end - start;
  if (len >= outMax) len = outMax - 1;
  memcpy(out, json.c_str() + start, len);
  out[len] = '\0';
  return true;
}

bool jsonExtractInt(const String& json, const char* key, int* out) {
  String search = "\"";
  search += key;
  search += "\":";
  int start = json.indexOf(search);
  if (start < 0) return false;
  start += search.length();
  while (start < json.length() && (json[start] == ' ' || json[start] == '\t')) start++;
  int val = 0;
  bool neg = false;
  if (json[start] == '-') { neg = true; start++; }
  while (start < json.length() && json[start] >= '0' && json[start] <= '9') {
    val = val * 10 + (json[start] - '0');
    start++;
  }
  *out = neg ? -val : val;
  return true;
}

// --- HTTP ---
bool httpGet(const char* url, String& outPayload, int* outCode) {
  HTTPClient http;
  http.setTimeout(15000);
  http.setConnectTimeout(10000);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.begin(url);

  int code = http.GET();
  if (outCode) *outCode = code;

  bool ok = false;
  if (code == 200) {
    outPayload = http.getString();
    ok = true;
  } else if (code > 0) {
    http.getString();
  }
  http.end();
  return ok;
}

// --- Fetch ---
void startFetch() {
  if (fetching) return;
  fetching = true;
  fetchPhase = 0;
  fetchStoryIdx = 0;
  topIDCount = 0;
  storyCount = 0;
  fetchFailed = false;
  snprintf(statusMsg, sizeof(statusMsg), "FETCHING HN...");
  setLed(0, 60, 120);
}

bool continueFetch() {
  if (fetchPhase == 0) {
    snprintf(statusMsg, sizeof(statusMsg), "FETCHING LIST...");
    String payload;
    int code = 0;
    bool ok = httpGet("https://hacker-news.firebaseio.com/v0/topstories.json", payload, &code);
    lastHttpCode = code;

    if (!ok) {
      snprintf(statusMsg, sizeof(statusMsg), "HTTP ERR %d", code);
      fetching = false;
      fetchFailed = true;
      setLed(120, 0, 60);
      return true;
    }

    topIDCount = 0;
    int pos = 1;
    while (pos < payload.length() && topIDCount < 35) {
      while (pos < payload.length() && (payload[pos] < '0' || payload[pos] > '9')) pos++;
      if (pos >= payload.length()) break;
      int val = 0;
      while (pos < payload.length() && payload[pos] >= '0' && payload[pos] <= '9') {
        val = val * 10 + (payload[pos] - '0');
        pos++;
      }
      topIDs[topIDCount++] = val;
    }

    if (topIDCount == 0) {
      snprintf(statusMsg, sizeof(statusMsg), "NO STORIES");
      fetching = false;
      fetchFailed = true;
      setLed(120, 80, 0);
      return true;
    }

    fetchPhase = 1;
    fetchStoryIdx = 0;
    return false;
  }

  if (fetchStoryIdx < topIDCount && storyCount < MAX_STORIES) {
    int id = topIDs[fetchStoryIdx];
    char url[80];
    snprintf(url, sizeof(url), "https://hacker-news.firebaseio.com/v0/item/%d.json", id);
    snprintf(statusMsg, sizeof(statusMsg), "FETCH %d/%d", storyCount + 1, MAX_STORIES);

    String payload;
    int code = 0;
    bool ok = httpGet(url, payload, &code);
    lastHttpCode = code;

    if (ok) {
      int typeStart = payload.indexOf("\"type\":\"");
      bool isStory = false;
      if (typeStart > 0) {
        typeStart += 8;
        if (payload.substring(typeStart, typeStart + 5) == "story") {
          isStory = true;
        }
      }

      if (isStory) {
        Story* s = &stories[storyCount];
        s->valid = false;
        s->title[0] = '\0';
        s->score = 0;
        s->descendants = 0;

        if (jsonExtractString(payload, "title", s->title, TITLE_MAX)) {
          jsonExtractInt(payload, "score", &s->score);
          jsonExtractInt(payload, "descendants", &s->descendants);
          s->valid = true;
          storyCount++;
        }
      }
    }

    // Sort by score descending before continuing
    if (storyCount >= MAX_STORIES || fetchStoryIdx >= topIDCount) {
      // Simple bubble sort by score
      for (int i = 0; i < storyCount - 1; i++) {
        for (int j = i + 1; j < storyCount; j++) {
          if (stories[j].score > stories[i].score) {
            Story tmp = stories[i];
            stories[i] = stories[j];
            stories[j] = tmp;
          }
        }
      }
      fetching = false;
      if (storyCount > 0) {
        hasData = true;
        fetchFailed = false;
        lastFetchMs = millis();
        snprintf(statusMsg, sizeof(statusMsg), "%d STORIES", storyCount);
        setLed(0, 120, 60);
      } else {
        // No stories fetched — retry soon, don't update lastFetchMs
        hasData = false;
        fetchFailed = true;
        snprintf(statusMsg, sizeof(statusMsg), "RETRY 30s");
        setLed(120, 80, 0);
      }
      return true;
    }

    fetchStoryIdx++;
    return false;
  }

  // Ran out of IDs without filling MAX_STORIES
  fetching = false;
  if (storyCount > 0) {
    hasData = true;
    fetchFailed = false;
    lastFetchMs = millis();
    snprintf(statusMsg, sizeof(statusMsg), "%d STORIES", storyCount);
    setLed(0, 120, 60);
  } else {
    hasData = false;
    fetchFailed = true;
    snprintf(statusMsg, sizeof(statusMsg), "RETRY 30s");
    setLed(120, 80, 0);
  }
  return true;
}

// --- Rendering ---
void drawStrip() {
  fbHLine(0, STRIP_LINE, WIDTH, cMagentaD());
  int p = 230;
  if (fetching) p = 205 + (int)(50.0f * (0.5f + 0.5f * sinf(millis() * 0.004f)));
  fbFillRect(0, STRIP_LINE + 1, WIDTH, 4, scale565(cMagenta(), p));
}

void drawHeader() {
  fbText(4, 3, "HACKER NEWS", cWhite());
  // live pip: green blink while fetching, solid when loaded, magenta on error
  uint16_t pip;
  if (fetching) pip = ((millis() / 500) % 2 == 0) ? cGreen() : cGhost();
  else if (fetchFailed) pip = cMagenta();
  else if (hasData) pip = cGreen();
  else pip = cGhost();
  fbFillRect(4 + textW57("HACKER NEWS") + 5, 5, 4, 4, pip);
  fbText(WIDTH - 4 - textW57("TOP"), 3, "TOP", cLine());
  drawStrip();
}

void drawStatusBar() {
  fbFillRect(0, STATUS_Y, WIDTH, STATUS_H, cBlack());
  fbText(2, STATUS_Y + 2, statusMsg, cLine());

  if (WiFi.status() == WL_CONNECTED) {
    int rssi = WiFi.RSSI();
    int bars;
    if (rssi > -50) bars = 4;
    else if (rssi > -65) bars = 3;
    else if (rssi > -75) bars = 2;
    else if (rssi > -85) bars = 1;
    else bars = 0;
    int bx = WIDTH - 22;
    for (int i = 0; i < 4; i++) {
      uint16_t bc = (i < bars) ? cGreen() : cGhost();
      fbFillRect(bx + i * 5, STATUS_Y + STATUS_H - 2 - i * 2, 3, 1 + i * 2, bc);
    }
  } else {
    fbText(WIDTH - 4 - textW57("NO WIFI"), STATUS_Y + 2, "NO WIFI", cMagenta());
  }
}

void drawStoryList() {
  if (!hasData || storyCount == 0) return;
  int avail = WIDTH - TITLE_X - TITLE_RM;

  // Marquee: each title stops once its own tail reaches the right edge and
  // waits there; once the longest title finishes, the whole list holds for
  // MARQUEE_PAUSE_MS (all endings visible), then jumps back to the start.
  int globalOff = 0;
  if (scrolling) {
    int maxPass = 0;
    for (int i = 0; i < storyCount; i++) {
      if (!stories[i].valid) continue;
      int pass = textW57(stories[i].title) - avail;
      if (pass > maxPass) maxPass = pass;
    }
    if (maxPass > 0) {
      unsigned long scrollMs = (unsigned long)maxPass * MARQUEE_MS_PER_PX;
      unsigned long phase = (millis() - scrollStartMs) % (scrollMs + MARQUEE_PAUSE_MS);
      globalOff = (phase < scrollMs) ? (int)(phase / MARQUEE_MS_PER_PX) : maxPass;
    }
  }

  for (int i = 0; i < storyCount; i++) {
    int y = STORY_TOP + i * STORY_H;
    if (i >= MAX_VISIBLE || y + STORY_H > STATUS_Y) break;

    Story* s = &stories[i];
    if (!s->valid) continue;

    // Title first — when scrolling it may run under the rank/score area,
    // which is repainted on top afterwards.
    if (scrolling) {
      int tw = textW57(s->title);
      if (tw > avail) {
        int pass = tw - avail;
        int off = (globalOff < pass) ? globalOff : pass;
        fbText(TITLE_X - off, y + 1, s->title, cWhite());
        fbFillRect(0, y, TITLE_X, STORY_H - 1, cBlack());
        fbFillRect(WIDTH - TITLE_RM, y, TITLE_RM, STORY_H - 1, cBlack());
      } else {
        fbText(TITLE_X, y + 1, s->title, cWhite());
      }
    } else {
      // clip: draw char by char until the title would pass the right edge
      int cx = TITLE_X;
      const char* p = s->title;
      while (*p) {
        if (cx + DMS_ADV > TITLE_X + avail) break;
        fbChar(cx, y + 1, *p, cWhite(), 1);
        cx += DMS_ADV;
        p++;
      }
    }

    // Rank
    char rbuf[4];
    snprintf(rbuf, sizeof(rbuf), "%d.", i + 1);
    fbText(2, y + 1, rbuf, cLine());

    // Score (green, live) — tight after the rank so the titles get the width
    char sbuf[8];
    snprintf(sbuf, sizeof(sbuf), "%d", s->score);
    fbText(SCORE_X, y + 1, sbuf, cGreen());
  }
}

void drawLoading() {
  fbClear(cBlack());
  drawHeader();

  const char* msg = "LOADING";
  fbText((WIDTH - textW57(msg, 2)) / 2, 36, msg, cWhite(), 2);
  int dots = (millis() / 400) % 4;
  for (int i = 0; i < dots; i++) fbText((WIDTH + textW57(msg, 2)) / 2 + 3 + i * 8, 36, ".", cGreen(), 2);

  if (statusMsg[0]) fbText(4, 62, statusMsg, cLine());
  fbText(4, 74, WiFi.status() == WL_CONNECTED ? "WIFI OK" : "NO WIFI",
         WiFi.status() == WL_CONNECTED ? cGreen() : cMagenta());

  if (storyCount > 0 && fetchPhase > 0) {
    char buf[24];
    snprintf(buf, sizeof(buf), "GOT %d STORIES", storyCount);
    fbText(4, 88, buf, cWhite());
  }

  drawStatusBar();
  present();
}

void renderAll() {
  fbClear(cBlack());
  drawHeader();
  drawStoryList();
  drawStatusBar();
  present();
}

// --- WiFi connect ---
bool wifiConnect(uint32_t timeoutMs, bool hardReset) {
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

  if (hardReset) {
    int16_t best = -1;
    int32_t bestRssi = -127;
    int16_t n = WiFi.scanNetworks(false, false, false, 120);
    for (int16_t i = 0; i < n; i++) {
      if (WiFi.SSID(i) == WIFI_SSID && (int32_t)WiFi.RSSI(i) > bestRssi) {
        best = i;
        bestRssi = WiFi.RSSI(i);
      }
    }
    if (best >= 0) {
      Serial.printf("WiFi visible ch=%d rssi=%d\n", (int)WiFi.channel(best), (int)bestRssi);
    } else {
      Serial.println("WiFi SSID not visible");
    }
    WiFi.scanDelete();
  }

  Serial.printf("WiFi begin -> %d\n", (int)WiFi.begin(WIFI_SSID, WIFI_PASSWORD));

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

// --- Setup ---
void setup() {
  Serial.begin(115200);
#if defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT
  // never let a debug log stall the frame loop (host not draining the CDC)
  Serial.setTxTimeoutMs(0);
#endif
  delay(300);
  Serial.println("\n=== HN DISPLAY BOOT ===");

  pixels.begin();
  pixels.setBrightness(30);
  setLed(120, 80, 0);

  SPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);
  tft.initR(INITR_BLACKTAB);
  tft.setSPISpeed(40000000);
  tft.setRotation(1);
  tft.fillScreen(ST7735_BLACK);

  // Button
  pinMode(TOUCH_PIN, INPUT_PULLDOWN);

  // Boot screen
  fbClear(cBlack());
  drawHeader();
  fbText(20, 52, "CONNECTING", cLine());
  fbText(20, 64, "TO WIFI...", cLine());
  drawStatusBar();
  present();

  // WiFi
  if (wifiConnect(15000, false)) {
    Serial.printf("WiFi OK IP=%s RSSI=%d\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
    setLed(0, 120, 60);
    snprintf(statusMsg, sizeof(statusMsg), "WIFI OK");
    startFetch();
  } else {
    Serial.println("WiFi FAILED");
    setLed(120, 0, 60);
    fetchFailed = true;
    snprintf(statusMsg, sizeof(statusMsg), "WIFI FAIL");
  }
}

// --- Loop ---
void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    setLed(120, 0, 60);
    snprintf(statusMsg, sizeof(statusMsg), "WIFI LOST");
    static unsigned long lastReconnect = 0;
    if (millis() - lastReconnect > 15000) {
      lastReconnect = millis();
      Serial.println("WiFi reconnect...");
      snprintf(statusMsg, sizeof(statusMsg), "WIFI RETRY");
      renderAll();
      if (wifiConnect(8000, true) && !hasData && !fetching) {
        startFetch();
      }
    }
    renderAll();
    delay(200);
    return;
  }

  if (fetching) {
    bool done = continueFetch();
    if (done && hasData) {
      Serial.printf("Fetch done: %d stories\n", storyCount);
    }
    drawLoading();
    delay(10);
    return;
  }

  unsigned long now = millis();
  if (hasData && now - lastFetchMs >= (unsigned long)refreshIntervalMs) {
    Serial.println("Periodic refresh...");
    startFetch();
    drawLoading();
    return;
  }

  // Retry fetch if no data (initial or after failed fetch)
  if (!hasData && !fetching) {
    static unsigned long lastRetry = 0;
    if (millis() - lastRetry > 30000 || lastRetry == 0) {
      lastRetry = millis();
      startFetch();
    }
  }

  // --- Button handling ---
  bool pinDown = (digitalRead(TOUCH_PIN) == HIGH);
  unsigned long nowMs = millis();

  if (Serial.available() && Serial.peek() == 's') {  // toggle marquee (tests/screenshots)
    Serial.read();
    scrolling = !scrolling;
    btnDown = false;
    if (scrolling) scrollStartMs = millis();
    Serial.printf("marquee %s\n", scrolling ? "on" : "off");
  }

  if (pinDown && !btnDown) {
    btnDown = true;
    btnAt = nowMs;
  }

  if (pinDown && btnDown && (nowMs - btnAt >= LONG_PRESS_MS)) {
    if (!scrolling) {
      scrolling = true;
      scrollStartMs = nowMs;
    }
  }

  if (!pinDown && btnDown) {
    releaseCnt++;
    if (releaseCnt >= 4) {     // ~120ms debounce
      btnDown = false;
      scrolling = false;
      releaseCnt = 0;
    }
  } else {
    releaseCnt = 0;
  }

  renderAll();
  delay(30);
}
