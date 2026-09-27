// ─── DVD BOUNCE · ESP32-C3 + ST7735 160x128 ─────────────────────────────────
// The classic DVD screensaver: the wordmark drifts across a dark deck, changes
// colour on every wall hit and — rarely — nails a corner.
//
// Clean modern look (no VHS / no LCD theme): deep navy deck, live counters in
// the HUD and a corner celebration. The mark is the real DVD logo: the
// official Wikimedia SVG path rasterised to a 62x29 antialiased coverage mask
// (DvdLogo.h), blended over the deck so the edges stay smooth at this size.
//
// Controls: tap pad (GPIO 0) / BOOT (GPIO 9) = cycle speed (1x/2x/3x),
// hold = reset the counters. WS2812 mirrors the logo colour and runs a
// rainbow when a corner is hit. Counters persist in NVS.

#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>
#include <Adafruit_NeoPixel.h>
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
Preferences prefs;

#define WIDTH  160
#define HEIGHT 128

// ─── Framebuffer (global — never on the stack) ───
static uint16_t fb[WIDTH * HEIGHT];
#define FPIX(x, y, c) if ((x) >= 0 && (x) < WIDTH && (y) >= 0 && (y) < HEIGHT) fb[(y) * WIDTH + (x)] = (c)

uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) { return tft.color565(r, g, b); }

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
void fbFlush() {
  tft.startWrite();
  tft.setAddrWindow(0, 0, WIDTH, HEIGHT);
  tft.writePixels(fb, WIDTH * HEIGHT);
  tft.endWrite();
}

#include "Dm01Intro.h"

// ── Departure Mono: the 11px raster drives the "CORNER!" banner and the
//    medium-size text; the HUD, toast and labels use the hand-drawn 5x6 set.
//    y is the cap top. ──
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
// 11px raster at 1x — the "medium" size between the 5x6 labels and scale 2
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

// ─── Palette: deep navy deck, cool greys, one accent per bounce ───
static const uint8_t DECK_RGB[3] = { 7, 11, 18 };  // #070B12 deck
uint16_t colBG()    { return rgb(DECK_RGB[0], DECK_RGB[1], DECK_RGB[2]); }
uint16_t colLabel() { return rgb( 64,  76,  94); } // HUD labels
uint16_t colFaint() { return rgb( 30,  38,  52); } // hairlines / toast border

// logo colour cycle (each wall hit advances one step)
static const uint8_t LOGO_RGB[6][3] = {
  {   0, 229, 255 }, // cyan
  { 255,  45, 120 }, // rose
  { 255, 201,  74 }, // amber
  { 139,  92, 246 }, // violet
  {  43, 217, 124 }, // green
  { 255, 122,  26 }, // orange
};

// ─── Logo — the official mark, rasterised from the Wikimedia SVG path: the
//     "DVD" wordmark over the wide disc with its centre hole, as a 62x29
//     antialiased coverage mask (DvdLogo.h). Each pixel blends the deck and
//     the current logo colour through a 17-step LUT rebuilt per colour. ───
#include "DvdLogo.h"

#define LOGO_W DVD_LOGO_W
#define LOGO_H DVD_LOGO_H
#define BOUNCE_X (WIDTH - LOGO_W)     // 98
#define BOUNCE_Y (HEIGHT - LOGO_H)    // 101
#define CORNER_TOL 0.25f              // both axes within this of their wall = corner

uint16_t logoCol;
int colorIdx = 0;
static uint16_t blendLut[17];         // coverage 0..16 blended deck→logoCol

void setLogoColor(int idx) {
  colorIdx = idx % 6;
  const uint8_t* c = LOGO_RGB[colorIdx];
  logoCol = rgb(c[0], c[1], c[2]);
  for (int i = 0; i <= 16; i++)
    blendLut[i] = tft.color565(
      (c[0] * i + DECK_RGB[0] * (16 - i) + 8) >> 4,
      (c[1] * i + DECK_RGB[1] * (16 - i) + 8) >> 4,
      (c[2] * i + DECK_RGB[2] * (16 - i) + 8) >> 4);
}
void nextLogoColor() { setLogoColor(colorIdx + 1); }

void drawLogo(int x, int y) {
  for (int row = 0; row < LOGO_H; row++) {
    const uint8_t* src = &DVD_LOGO[row * LOGO_W];
    int px = x;
    for (int col = 0; col < LOGO_W; col++, px++) {
      uint8_t cov = *src++;
      if (!cov) continue;
      FPIX(px, y + row, cov >= 248 ? logoCol : blendLut[(cov + 8) >> 4]);
    }
  }
}

// ─── State ───
float lx = 0, ly = 0, vx = 55, vy = 38;

unsigned long bounces = 0, corners = 0;
int speedMode = 0;
static const float SPEEDS[3] = { 55.0f, 90.0f, 140.0f };

#define MAX_PARTS 32
struct Part { float x, y, vx, vy; unsigned long until; uint16_t col; };
Part parts[MAX_PARTS];

unsigned long cornerUntil = 0;
char toast[16] = { 0 };
unsigned long toastUntil = 0;
unsigned long celebrateUntil = 0;
unsigned long lastSave = 0;

int idleLevel = HIGH;
bool wasPressed = false;
unsigned long pressStartMs = 0;
bool holdFired = false;
#define HOLD_MS 1200

void spawnPart(float x, float y, float sp, uint16_t col) {
  for (int i = 0; i < MAX_PARTS; i++) {
    if (millis() < parts[i].until) continue;
    float a = (esp_random() % 628) * 0.01f;
    parts[i].x = x; parts[i].y = y;
    parts[i].vx = cosf(a) * sp;
    parts[i].vy = sinf(a) * sp;
    parts[i].until = millis() + 320 + esp_random() % 260;
    parts[i].col = col;
    return;
  }
}

void burst(float x, float y, int n, float sp, uint16_t col) {
  for (int i = 0; i < n; i++) spawnPart(x, y, sp * (0.6f + (esp_random() % 80) * 0.01f), col);
}

void saveStats() {
  prefs.putULong("b", bounces);
  prefs.putULong("c", corners);
}

void resetStats() {
  bounces = 0; corners = 0;
  saveStats();
  snprintf(toast, sizeof(toast), "RESET");
  toastUntil = millis() + 900;
}

void cornerHit() {
  corners++;
  cornerUntil = millis() + 1800;
  celebrateUntil = millis() + 1800;
  // burst out of the corner the logo just hit
  float cx = (lx < BOUNCE_X * 0.5f) ? 0 : WIDTH;
  float cy = (ly < BOUNCE_Y * 0.5f) ? 0 : HEIGHT;
  burst(cx, cy, 26, 130, logoCol);
  burst(cx, cy, 10, 90, rgb(255, 255, 255));
  saveStats();
  Serial.printf("[dvd] CORNER! bounces=%lu corners=%lu\n", bounces, corners);
}

void cycleSpeed() {
  speedMode = (speedMode + 1) % 3;
  float sp = SPEEDS[speedMode];
  float mag = sqrtf(vx * vx + vy * vy);
  if (mag > 0.01f) { vx = vx / mag * sp; vy = vy / mag * sp; }
  snprintf(toast, sizeof(toast), "SPEED %dX", speedMode + 1);
  toastUntil = millis() + 900;
  Serial.printf("[dvd] speed %dx\n", speedMode + 1);
}

void update(float dt) {
  unsigned long now = millis();

  lx += vx * dt;
  ly += vy * dt;

  bool hitX = false, hitY = false;
  if (lx <= 0) { lx = 0; vx = -vx; hitX = true; }
  else if (lx >= BOUNCE_X) { lx = BOUNCE_X; vx = -vx; hitX = true; }
  if (ly <= 0) { ly = 0; vy = -vy; hitY = true; }
  else if (ly >= BOUNCE_Y) { ly = BOUNCE_Y; vy = -vy; hitY = true; }

  if (hitX || hitY) {
    bounces++;
    nextLogoColor();
    float ix = hitX ? (lx <= 0 ? 0 : WIDTH) : lx + LOGO_W * 0.5f;
    float iy = hitY ? (ly <= 0 ? 0 : HEIGHT) : ly + LOGO_H * 0.5f;
    burst(ix, iy, 4, 60, logoCol);
    // corner: the other axis also sits at its wall right now
    bool nearX = (lx <= CORNER_TOL) || (lx >= BOUNCE_X - CORNER_TOL);
    bool nearY = (ly <= CORNER_TOL) || (ly >= BOUNCE_Y - CORNER_TOL);
    if (nearX && nearY) cornerHit();
  }

  for (int i = 0; i < MAX_PARTS; i++) {
    if (now >= parts[i].until) continue;
    parts[i].x += parts[i].vx * dt;
    parts[i].y += parts[i].vy * dt;
  }

  if (now - lastSave >= 30000UL) { lastSave = now; saveStats(); }
}

// ─── Input: tap = speed, hold = reset ───
void calibrateTouch() {
  pinMode(BOOT_PIN, INPUT_PULLUP);
  delay(300);
  int highCount = 0;
  for (int i = 0; i < 20; i++) { if (digitalRead(TOUCH_PIN) == HIGH) highCount++; delay(10); }
  idleLevel = (highCount > 10) ? HIGH : LOW;
}
static inline bool inputPressed() {
  if (digitalRead(BOOT_PIN) == LOW) return true;
  return digitalRead(TOUCH_PIN) != idleLevel;
}
void handleTouch() {
  bool pressed = inputPressed();
  unsigned long now = millis();
  if (pressed && !wasPressed) {
    pressStartMs = now;
    wasPressed = true;
    holdFired = false;
  } else if (!pressed && wasPressed) {
    unsigned long dur = now - pressStartMs;
    if (!holdFired && dur >= 60 && dur < HOLD_MS) cycleSpeed();
    wasPressed = false;
    holdFired = false;
    delay(20);
  } else if (pressed && wasPressed && !holdFired && (now - pressStartMs >= HOLD_MS)) {
    holdFired = true;
    resetStats();
  }
}

// ─── LED ───
void updateLed() {
  static unsigned long last = 0;
  if (millis() - last < 40) return;
  last = millis();
  if (millis() < celebrateUntil) {
    float p = (millis() % 1400) / 1400.0f;
    pixels.setBrightness(80);
    pixels.setPixelColor(0, pixels.ColorHSV((uint16_t)(p * 65535.0f)));
  } else {
    pixels.setBrightness(28);
    pixels.setPixelColor(0, logoCol);
  }
  pixels.show();
}

// ─── Render ───
void drawHUD() {
  char buf[16];
  uint16_t val = logoCol;
  unsigned long b = bounces > 99999 ? 99999 : bounces;
  snprintf(buf, sizeof(buf), "%lu", b);
  fbText(4, 3, "BOUNCES", colLabel());
  fbText(4 + textW57("BOUNCES") + 4, 3, buf, val);

  unsigned long c = corners > 99999 ? 99999 : corners;
  snprintf(buf, sizeof(buf), "%lu", c);
  int vw = textW57(buf);
  int lw = textW57("CORNERS");
  fbText(WIDTH - 4 - vw - 4 - lw, 3, "CORNERS", colLabel());
  fbText(WIDTH - 4 - vw, 3, buf, val);
}

void drawToast() {
  int tw = textW11(toast);
  int bw = tw + 20, bh = 24;
  int bx = (WIDTH - bw) / 2, by = HEIGHT / 2 - bh / 2;
  fbFillRect(bx, by, bw, bh, colBG());
  fbRect(bx, by, bw, bh, colFaint());
  fbText11(bx + 10, by + 7, toast, logoCol);
}

void render() {
  unsigned long now = millis();
  fbClear(colBG());

  drawLogo((int)lx, (int)ly);

  for (int i = 0; i < MAX_PARTS; i++) {
    if (now >= parts[i].until) continue;
    FPIX((int)parts[i].x, (int)parts[i].y, parts[i].col);
    FPIX((int)parts[i].x + 1, (int)parts[i].y, parts[i].col);
  }

  drawHUD();

  if (now < cornerUntil) {
    const char* s = "CORNER!";
    int w = textW57(s, 2);
    int x = (WIDTH - w) / 2, y = HEIGHT / 2 - 12;
    fbFillRect(x - 6, y - 6, w + 12, 34, colBG());
    fbRect(x - 6, y - 6, w + 12, 34, logoCol);
    fbText(x, y, s, logoCol, 2);
  } else if (toast[0] && now < toastUntil) {
    drawToast();
  }
}

void setup() {
  Serial.begin(115200);
#if defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT
  // never let a debug log stall the animation: with the CDC connected but the
  // host not draining (no monitor open), a plain write() blocks up to 20x100ms
  Serial.setTxTimeoutMs(0);
#endif
  delay(300);
  Serial.println("\n=== DVD BOUNCE ===");

  pinMode(TOUCH_PIN, INPUT);
  pixels.begin();
  pixels.setBrightness(0);
  pixels.clear();
  pixels.show();

  SPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);
  tft.initR(INITR_BLACKTAB);
  tft.setSPISpeed(40000000);
  tft.setRotation(1);
  tft.fillScreen(ST7735_BLACK);

  prefs.begin("dvd", false);
  bounces = prefs.getULong("b", 0);
  corners = prefs.getULong("c", 0);

  setLogoColor(esp_random() % 6);
  lx = 20 + esp_random() % (BOUNCE_X - 40);
  ly = 20 + esp_random() % (BOUNCE_Y - 40);
  float a = 0.45f + (esp_random() % 100) * 0.01f;   // never too shallow
  vx = cosf(a) * SPEEDS[0];
  vy = sinf(a) * SPEEDS[0];

  calibrateTouch();
  dm01Start();
}

void loop() {
  bool introSkip = inputPressed();
  if (dm01Frame(introSkip)) {
    pixels.setBrightness(70);
    pixels.setPixelColor(0, dm01Pal((int)(millis() / 150)));
    pixels.show();
    fbFlush();
    screenshotHandle(fb, WIDTH, HEIGHT);
    delay(16);
    return;
  }

  static unsigned long lastFrame = 0;
  unsigned long now = millis();
  float dt = (now - lastFrame) / 1000.0f;
  lastFrame = now;
  if (dt > 0.06f) dt = 0.06f;

  handleTouch();
  update(dt);
  updateLed();
  render();
  fbFlush();
  screenshotHandle(fb, WIDTH, HEIGHT);

  // pace to 60 FPS
  static uint32_t nextFrame = 0;
  uint32_t ms = millis();
  if ((int32_t)(nextFrame - ms) > 0) delay(nextFrame - ms);
  nextFrame += 16;
  if ((int32_t)(nextFrame - ms) < -100) nextFrame = ms + 16;
}
