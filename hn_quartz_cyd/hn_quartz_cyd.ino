// HACKER NEWS · CYD (ESP32-2432S028R) — 320x240.
//
// Mirror of hn_display (C3): top stories from the keyless Hacker News Firebase
// API in the SIGNAL/VHS theme (signal-vhs-ui skill) — dark-grey deck (#242424),
// white ink, magenta accent strip, signal-green live pip + scores, light-grey
// rules and a per-element VHS glitch (chroma ripple + palette-washed rip bands).
//
// Long-press BOOT for the marquee. No detail reader, no touch — the list is
// the whole UI, like the C3 sketch. No RGB LED on the CYD (GPIO4 doubles as
// the TFT reset), so state is on-screen and on serial only.

#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <math.h>
#include "Screenshot.h"

// --- Display pins ---
#define TFT_CS    15
#define TFT_DC    2
#define TFT_RST   4
#define TFT_MOSI  13
#define TFT_SCLK  14
#define TFT_MISO  12
#define TFT_BL    21

// --- Button ---
#define BTN_PIN    0

// --- Touch (XPT2046 on a separate HSPI bus) ---
#define TOUCH_CS   33
#define TOUCH_CLK  25
#define TOUCH_MOSI 32
#define TOUCH_MISO 39
#define TOUCH_IRQ  36

// --- WiFi ---
// Set your network before flashing: nothing is committed to the repo.
#define WIFI_SSID     ""
#define WIFI_PASSWORD ""

// --- Display ---
#define WIDTH  320
#define HEIGHT 240

Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);
SPIClass touchSPI(HSPI);

// --- Framebuffer (strip-based: 64 rows per strip) ---
#define FB_H   64
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
void fbFlush() {
  int h = FB_H;
  if (fbTop + h > HEIGHT) h = HEIGHT - fbTop;
  tft.startWrite();
  tft.setAddrWindow(0, fbTop, WIDTH, h);
  tft.writePixels(fb, WIDTH * h);
  tft.endWrite();
}

uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) { return tft.color565(r, g, b); }

// ── SIGNAL palette — #242424 deck, white ink, magenta + signal green ──
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

// ── Departure Mono (rasterized, 11px) at every scale. y is the cap top. ──
#include "DepartureMono.h"

static inline const uint8_t* dmGlyph(char ch) {
  if (ch < 32 || ch > 90) return dmFont[0];
  return dmFont[ch - 32];
}
void fbChar(int x, int y, char ch, uint16_t c, int scale) {
  if (ch == ' ') return;
  if (ch >= 'a' && ch <= 'z') ch -= 32; // normalize — labels are UPPER
  const uint8_t* g = dmGlyph(ch);
  int cy = y - DM_TOP * scale;
  for (int row = 0; row < DM_H; row++) {
    uint8_t bits = g[row];
    if (!bits) continue;
    for (int col = 0; col < DM_W; col++)
      if (bits & (0x40 >> col)) fbFillRect(x + col * scale, cy + row * scale, scale, scale, c);
  }
}
void fbText(int x, int y, const char* s, uint16_t c, int scale = 1) {
  while (*s) { fbChar(x, y, *s, c, scale); x += DM_ADV * scale; s++; }
}
int textW57(const char* s, int scale = 1) { int n = 0; while (*s++) n++; return n ? (n * DM_ADV * scale - scale) : 0; }



// --- Layout (2x the C3 proportions) ---
#define HDR_Y      9
#define STRIP_LN   26
#define STRIP_Y    28
#define STORY_TOP  40
#define STORY_H    18
#define MAX_VISIBLE 10
#define SCORE_X    32
#define TITLE_X    70
#define TITLE_RM   6
#define STATUS_H   16
#define STATUS_Y   (HEIGHT - STATUS_H)
#define LONG_PRESS_MS 600
#define SCROLL_SPEED  2

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
bool booting = true;
int refreshIntervalMs = 3600000;

// --- Button state ---
bool scrolling = false;
int  scrollX = 0;
bool btnDown = false;
unsigned long btnAt = 0;
int  releaseCnt = 0;

// --- JSON helpers ---
bool jsonExtractString(const String& json, const char* key, char* out, int outMax) {
  String search = "\""; search += key; search += "\":\"";
  int start = json.indexOf(search); if (start < 0) return false;
  start += search.length(); int end = start;
  while (end < json.length()) { char c = json[end]; if (c == '\\') { end += 2; continue; } if (c == '"') break; end++; }
  int len = end - start; if (len >= outMax) len = outMax - 1;
  memcpy(out, json.c_str() + start, len); out[len] = '\0'; return true;
}
bool jsonExtractInt(const String& json, const char* key, int* out) {
  String search = "\""; search += key; search += "\":";
  int start = json.indexOf(search); if (start < 0) return false;
  start += search.length();
  while (start < json.length() && (json[start] == ' ' || json[start] == '\t')) start++;
  int val = 0; bool neg = false;
  if (json[start] == '-') { neg = true; start++; }
  while (start < json.length() && json[start] >= '0' && json[start] <= '9') { val = val * 10 + (json[start] - '0'); start++; }
  *out = neg ? -val : val; return true;
}

// --- HTTP ---
bool httpGet(const char* url, String& outPayload, int* outCode) {
  HTTPClient http; http.setTimeout(15000); http.setConnectTimeout(10000);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS); http.begin(url);
  int code = http.GET(); if (outCode) *outCode = code;
  bool ok = false;
  if (code == 200) { outPayload = http.getString(); ok = true; }
  else if (code > 0) { http.getString(); }
  http.end(); return ok;
}

// --- Fetch ---
void startFetch() {
  if (fetching) return;
  fetching = true; fetchPhase = 0; fetchStoryIdx = 0; topIDCount = 0; storyCount = 0;
  fetchFailed = false;
  snprintf(statusMsg, sizeof(statusMsg), "FETCHING HN...");
}

bool continueFetch() {
  if (fetchPhase == 0) {
    snprintf(statusMsg, sizeof(statusMsg), "FETCHING LIST...");
    String payload; int code = 0;
    bool ok = httpGet("https://hacker-news.firebaseio.com/v0/topstories.json", payload, &code);
    lastHttpCode = code;
    if (!ok) { snprintf(statusMsg, sizeof(statusMsg), "HTTP ERR %d", code); fetching = false; fetchFailed = true; return true; }
    topIDCount = 0; int pos = 1;
    while (pos < payload.length() && topIDCount < 35) {
      while (pos < payload.length() && (payload[pos] < '0' || payload[pos] > '9')) pos++;
      if (pos >= payload.length()) break;
      int val = 0;
      while (pos < payload.length() && payload[pos] >= '0' && payload[pos] <= '9') { val = val * 10 + (payload[pos] - '0'); pos++; }
      topIDs[topIDCount++] = val;
    }
    if (topIDCount == 0) { snprintf(statusMsg, sizeof(statusMsg), "NO STORIES"); fetching = false; fetchFailed = true; return true; }
    fetchPhase = 1; fetchStoryIdx = 0; return false;
  }
  if (fetchStoryIdx < topIDCount && storyCount < MAX_STORIES) {
    int id = topIDs[fetchStoryIdx];
    char url[80]; snprintf(url, sizeof(url), "https://hacker-news.firebaseio.com/v0/item/%d.json", id);
    snprintf(statusMsg, sizeof(statusMsg), "FETCH %d/%d", storyCount + 1, MAX_STORIES);
    String payload; int code = 0;
    bool ok = httpGet(url, payload, &code); lastHttpCode = code;
    if (ok) {
      int typeStart = payload.indexOf("\"type\":\""); bool isStory = false;
      if (typeStart > 0) { typeStart += 8; if (payload.substring(typeStart, typeStart + 5) == "story") isStory = true; }
      if (isStory) {
        Story* s = &stories[storyCount]; s->valid = false; s->title[0] = '\0'; s->score = 0; s->descendants = 0;
        if (jsonExtractString(payload, "title", s->title, TITLE_MAX)) {
          jsonExtractInt(payload, "score", &s->score);
          jsonExtractInt(payload, "descendants", &s->descendants);
          s->valid = true; storyCount++;
        }
      }
    }
    if (storyCount >= MAX_STORIES || fetchStoryIdx >= topIDCount) {
      for (int i = 0; i < storyCount - 1; i++)
        for (int j = i + 1; j < storyCount; j++)
          if (stories[j].score > stories[i].score) { Story tmp = stories[i]; stories[i] = stories[j]; stories[j] = tmp; }
      fetching = false;
      if (storyCount > 0) { hasData = true; fetchFailed = false; lastFetchMs = millis(); snprintf(statusMsg, sizeof(statusMsg), "%d STORIES", storyCount); }
      else { hasData = false; fetchFailed = true; snprintf(statusMsg, sizeof(statusMsg), "RETRY 30s"); }
      return true;
    }
    fetchStoryIdx++; return false;
  }
  fetching = false;
  if (storyCount > 0) { hasData = true; fetchFailed = false; lastFetchMs = millis(); snprintf(statusMsg, sizeof(statusMsg), "%d STORIES", storyCount); }
  else { hasData = false; fetchFailed = true; snprintf(statusMsg, sizeof(statusMsg), "RETRY 30s"); }
  return true;
}

// --- Rendering ---
void drawStrip() {
  fbHLine(0, STRIP_LN, WIDTH, cMagentaD());
  int p = 230;
  if (fetching) p = 205 + (int)(50.0f * (0.5f + 0.5f * sinf(millis() * 0.004f)));
  fbFillRect(0, STRIP_Y, WIDTH, 8, scale565(cMagenta(), p));
}

void drawHeader() {
  fbText(8, HDR_Y, "HACKER NEWS", cWhite(), 2);
  uint16_t pip;
  if (fetching) pip = ((millis() / 500) % 2 == 0) ? cGreen() : cGhost();
  else if (fetchFailed) pip = cMagenta();
  else if (hasData) pip = cGreen();
  else pip = cGhost();
  fbFillRect(8 + textW57("HACKER NEWS", 2) + 8, HDR_Y + 1, 6, 6, pip);
  fbText(WIDTH - 8 - textW57("TOP", 2), HDR_Y, "TOP", cLine(), 2);
  drawStrip();
}

void drawStatusBar() {
  fbFillRect(0, STATUS_Y, WIDTH, STATUS_H, cBlack());
  fbText(8, STATUS_Y + 3, statusMsg, cLine());
  if (WiFi.status() == WL_CONNECTED) {
    int rssi = WiFi.RSSI();
    int bars = (rssi > -50) ? 4 : (rssi > -65) ? 3 : (rssi > -75) ? 2 : (rssi > -85) ? 1 : 0;
    int bx = WIDTH - 8 - 32;
    for (int i = 0; i < 4; i++) {
      fbFillRect(bx + i * 8, STATUS_Y + STATUS_H - 6 - i * 3, 6, 3 + i * 3,
                 (i < bars) ? cGreen() : cGhost());
    }
  } else {
    fbText(WIDTH - 8 - textW57("NO WIFI"), STATUS_Y + 3, "NO WIFI", cMagenta());
  }
}

void drawStoryList() {
  if (!hasData || storyCount == 0) return;
  int avail = WIDTH - TITLE_X - TITLE_RM;
  for (int i = 0; i < storyCount; i++) {
    int y = STORY_TOP + i * STORY_H;
    if (i >= MAX_VISIBLE || y + STORY_H > STATUS_Y) break;
    Story* s = &stories[i]; if (!s->valid) continue;

    // Title first — while scrolling it runs under the rank/score column,
    // which is repainted on top afterwards.
    if (scrolling) {
      int tw = textW57(s->title);
      if (tw > avail) {
        int skipPx = scrollX; const char* p = s->title;
        while (skipPx > 0 && *p) { if (skipPx >= DM_ADV) { skipPx -= DM_ADV; p++; } else break; }
        fbText(TITLE_X - skipPx, y + 3, p, cWhite());
        fbFillRect(0, y, TITLE_X, STORY_H - 1, cBlack());
        fbFillRect(WIDTH - TITLE_RM, y, TITLE_RM, STORY_H - 1, cBlack());
      } else fbText(TITLE_X, y + 3, s->title, cWhite());
    } else {
      int cx = TITLE_X;
      const char* p = s->title;
      while (*p) {
        if (cx + DM_ADV > TITLE_X + avail) break;
        fbChar(cx, y + 3, *p, cWhite(), 1);
        cx += DM_ADV;
        p++;
      }
    }

    char rbuf[4]; snprintf(rbuf, sizeof(rbuf), "%d.", i + 1);
    fbText(8, y + 3, rbuf, cLine());

    // Score (green, live) — tight after the rank so the titles get the width
    char sbuf[8]; snprintf(sbuf, sizeof(sbuf), "%d", s->score);
    fbText(SCORE_X, y + 3, sbuf, cGreen());
  }
}

void drawFrame() {
  drawHeader();
  drawStoryList();
  drawStatusBar();
  if (!hasData) {
    const char* msg = booting ? "CONNECTING" : (fetching ? "LOADING" : "NO DATA");
    fbText((WIDTH - textW57(msg, 3)) / 2, 92, msg, cWhite(), 3);
    const char* hint = booting ? "TO WIFI..." : (fetching ? "" : "RETRYING...");
    if (*hint) fbText((WIDTH - textW57(hint, 2)) / 2, 140, hint, cLine(), 2);
  }
}

// --- Render (strip sweep + VHS + screenshot) ---
void render() {
  for (fbTop = 0; fbTop < HEIGHT; fbTop += FB_H) {
    fbClear(cBlack());
    drawFrame();
    int h = FB_H;
    if (fbTop + h > HEIGHT) h = HEIGHT - fbTop;
    if (!screenshotStripSend(shot, fb, fbTop, h)) fbFlush();
  }
}

// --- WiFi connect (ported from the C3 sketch: TX power + hard reset retries) ---
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

  Serial.printf("WiFi begin -> %d\n", (int)WiFi.begin(WIFI_SSID, WIFI_PASSWORD));
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) {
    delay(250);
  }
  Serial.printf("WiFi result=%d after %lums\n", (int)WiFi.status(), millis() - t0);
  return WiFi.status() == WL_CONNECTED;
}

// --- Setup ---
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== HN DISPLAY CYD ===");

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  SPI.begin(TFT_SCLK, TFT_MISO, TFT_MOSI, TFT_CS);
  tft.init(240, 320);
  tft.setSPISpeed(80000000);   // ST7789 takes 80 MHz — halves the strip flush
  tft.setRotation(1);
  tft.invertDisplay(false);
  tft.fillScreen(0x0000);

  pinMode(BTN_PIN, INPUT_PULLUP);
  touchInit();

  render();   // boot screen (CONNECTING / TO WIFI...)

  if (wifiConnect(15000, false)) {
    Serial.printf("WiFi OK IP=%s RSSI=%d\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
    snprintf(statusMsg, sizeof(statusMsg), "WIFI OK");
    booting = false;
    startFetch();
  } else {
    Serial.println("WiFi FAILED");
    booting = false;
    fetchFailed = true;
    snprintf(statusMsg, sizeof(statusMsg), "WIFI FAIL");
  }
}

// --- Touch (XPT2046): hold the screen to marquee-scroll long titles ---
void touchInit() {
  touchSPI.begin(TOUCH_CLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
  pinMode(TOUCH_CS, OUTPUT);
  digitalWrite(TOUCH_CS, HIGH);
  pinMode(TOUCH_IRQ, INPUT);   // GPIO36 input-only: external pull-up on the CYD
}
bool touchPressed() {
  // a glitch must persist across 3 loop passes so line noise can't fire a hold
  static uint8_t lowN = 0;
  if (digitalRead(TOUCH_IRQ) != LOW) { lowN = 0; return false; }
  if (lowN < 3) { lowN++; return false; }
  return true;
}

// --- Loop ---
void loop() {
  screenshotStripPoll(shot, WIDTH, HEIGHT);

  if (WiFi.status() != WL_CONNECTED) {
    snprintf(statusMsg, sizeof(statusMsg), "WIFI LOST");
    static unsigned long lastReconnect = 0;
    if (millis() - lastReconnect > 15000) {
      lastReconnect = millis();
      Serial.println("WiFi reconnect...");
      snprintf(statusMsg, sizeof(statusMsg), "WIFI RETRY");
      render();
      if (wifiConnect(8000, true) && !hasData && !fetching) {
        startFetch();
      }
    }
  }

  if (fetching) {
    bool done = continueFetch();
    if (done && hasData) Serial.printf("Fetch done: %d stories\n", storyCount);
    render();
    delay(10);
    return;
  }

  unsigned long now = millis();
  if (hasData && now - lastFetchMs >= (unsigned long)refreshIntervalMs) {
    startFetch(); render(); return;
  }
  if (!hasData && !fetching) {
    static unsigned long lastRetry = 0;
    if (millis() - lastRetry > 30000 || lastRetry == 0) { lastRetry = millis(); startFetch(); }
  }

  // Button (BOOT, active LOW) — marquee scroll
  bool pinDown = (digitalRead(BTN_PIN) == LOW) || touchPressed();
  unsigned long nowMs = millis();
  if (pinDown && !btnDown) { btnDown = true; btnAt = nowMs; }
  if (pinDown && btnDown && (nowMs - btnAt >= LONG_PRESS_MS)) scrolling = true;
  if (!pinDown && btnDown) { releaseCnt++; if (releaseCnt >= 4) { btnDown = false; scrolling = false; scrollX = 0; releaseCnt = 0; } }
  else releaseCnt = 0;
  if (scrolling) {
    int maxW = 0;
    for (int i = 0; i < storyCount; i++) { int w = textW57(stories[i].title); if (w > maxW) maxW = w; }
    int limit = maxW - (WIDTH - TITLE_X - TITLE_RM); if (limit < 0) limit = 0;
    if (scrollX < limit) { scrollX += SCROLL_SPEED; if (scrollX > limit) scrollX = limit; }
  }

  render();
  delay(30);
}
