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
uint16_t cBlack()   { return rgb(148, 158, 130); } // light LCD deck
uint16_t cWhite()   { return rgb( 28,  34,  24); } // dark ink
uint16_t cMagenta() { return rgb(200,  40,  40); } // alarm red accent strip
uint16_t cGreen()   { return rgb( 28,  34,  24); } // live pip + scores (ink)
uint16_t cLine()    { return rgb( 96, 104,  82); } // dark olive rules
uint16_t cGhost()   { return rgb(138, 148, 120); } // stale digits / pip off
uint16_t cMagentaD(){ return rgb(120,  24,  24); } // dark red edging

uint16_t scale565(uint16_t c, int pct) {   // pct/256 brightness (>256 brightens)
  int r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
  r = r * pct >> 8; g = g * pct >> 8; b = b * pct >> 8;
  if (r > 31) r = 31; if (g > 63) g = 63; if (b > 31) b = 31;
  return (uint16_t)((r << 11) | (g << 5) | b);
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

void present() {
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
        if (cx + 5 > TITLE_X + avail) break;
        fbChar(cx, y + 1, *p, cWhite(), 1);
        cx += 5;
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
