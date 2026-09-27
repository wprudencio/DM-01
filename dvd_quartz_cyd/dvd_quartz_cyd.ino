// ─── DVD BOUNCE · CYD (ESP32-2432S028R) — 320x240 ───────────────────────────
// Port of dvd/dvd.ino (ESP32-C3 + ST7735 160x128) to the Cheap Yellow Display:
// ST7789 320x240 + XPT2046 resistive touch. Everything is the 2x C3 layout.
//
// The classic DVD screensaver: the wordmark drifts across a dark deck, changes
// colour on every wall hit and — rarely — nails a corner.
//
// Clean modern look (no VHS / no LCD theme): deep navy deck, live counters in
// the HUD and a corner celebration. The mark is the real DVD logo: the
// official Wikimedia SVG path rasterised to a 124x55 antialiased coverage mask
// (DvdLogo.h), blended over the deck so the edges stay smooth at this size.
//
// Controls: tap the touch screen (or BOOT) = cycle speed (1x/2x/3x),
// hold = reset the counters. No NeoPixel on CYD — the corner hit flashes a
// frame around the screen instead. Counters persist in NVS.

#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
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

// ── Touch (XPT2046 on separate HSPI bus) + BOOT button ──
#define TOUCH_CS   33
#define TOUCH_CLK  25
#define TOUCH_MOSI 32
#define TOUCH_MISO 39
#define TOUCH_IRQ  36
#define BOOT_PIN   0

Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);
SPIClass touchSPI(HSPI);
Preferences prefs;

#define WIDTH  320
#define HEIGHT 240

// ─── Framebuffer (64-row strips; a full 150 KB frame doesn't fit ESP32 DRAM) ───
#define FB_H 64
static uint16_t fb[WIDTH * FB_H];
static int fbTop = 0;
static bool introMode = false;
static ScreenshotStripSession shot;
#define FPIX(x, y, c) if ((x) >= 0 && (x) < WIDTH && (y) >= fbTop && (y) < fbTop + FB_H) fb[(y - fbTop) * WIDTH + (x)] = (c)

uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) { return tft.color565(r, g, b); }

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
  for (int i = 1; i < h - 1; i++) { FPIX(x, y + i, c); FPIX(x + w - 1, y + i, c); }
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
  int h = FB_H;
  if (fbTop + h > HEIGHT) h = HEIGHT - fbTop;
  tft.startWrite();
  tft.setAddrWindow(0, fbTop, WIDTH, h);
  tft.writePixels(fb, WIDTH * h);
  tft.endWrite();
}

#define DM01_SCALE 2
#include "Dm01Intro.h"

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
int textW(const char* s, int scale = 1) {
  int n = 0; while (*s++) n++;
  return n ? n * 5 * scale - scale : 0;
}


// ─── Palette: deep navy deck, cool greys, one accent per bounce ───
static const uint8_t DECK_RGB[3] = { 148, 158, 130 };  // light LCD deck
uint16_t colBG()    { return rgb(DECK_RGB[0], DECK_RGB[1], DECK_RGB[2]); }
uint16_t colLabel() { return rgb( 28,  34,  24); } // HUD labels (ink)
uint16_t colFaint() { return rgb( 96, 104,  82); } // hairlines / toast border

// logo colour cycle (each wall hit advances one step)
static const uint8_t LOGO_RGB[6][3] = {
  { 200,  40,  40 }, // alarm red
  {  28,  34,  24 }, // ink
  {  32,  74, 120 }, // deep blue
  {  34,  92,  52 }, // forest green
  { 140,  28,  90 }, // magenta ink
  { 170,  84,  20 }, // burnt orange
};

// ─── Logo — the official mark, rasterised from the Wikimedia SVG path at 2x
//     the C3 mask (124x55, DvdLogo.h). Each pixel blends the deck and the
//     current logo colour through a 17-step LUT rebuilt per colour. ───
#include "DvdLogo.h"

#define LOGO_W DVD_LOGO_W
#define LOGO_H DVD_LOGO_H
#define BOUNCE_X (WIDTH - LOGO_W)     // 196
#define BOUNCE_Y (HEIGHT - LOGO_H)    // 185
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
float lx = 0, ly = 0, vx = 110, vy = 76;

unsigned long bounces = 0, corners = 0;
int speedMode = 0;
static const float SPEEDS[3] = { 110.0f, 180.0f, 280.0f };

#define MAX_PARTS 32
struct Part { float x, y, vx, vy; unsigned long until; uint16_t col; };
Part parts[MAX_PARTS];

unsigned long cornerUntil = 0;
char toast[16] = { 0 };
unsigned long toastUntil = 0;
unsigned long lastSave = 0;

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
  // burst out of the corner the logo just hit
  float cx = (lx < BOUNCE_X * 0.5f) ? 0 : WIDTH;
  float cy = (ly < BOUNCE_Y * 0.5f) ? 0 : HEIGHT;
  burst(cx, cy, 26, 260, logoCol);
  burst(cx, cy, 10, 180, rgb(255, 255, 255));
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
    burst(ix, iy, 4, 120, logoCol);
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

// ── Touch: tap = speed, hold = reset (BOOT works the same) ──
void touchInit() {
  touchSPI.begin(TOUCH_CLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
  pinMode(TOUCH_CS, OUTPUT);
  digitalWrite(TOUCH_CS, HIGH);
  pinMode(TOUCH_IRQ, INPUT);  // GPIO36 input-only: no internal pullup (external PU on CYD)
  pinMode(BOOT_PIN, INPUT_PULLUP);
}
bool touchPressed() {
  // Debounce: a glitch must persist across 3 loop passes, so line noise can
  // neither change the speed nor reset the counters.
  static uint8_t lowN = 0;
  if (digitalRead(TOUCH_IRQ) != LOW) { lowN = 0; return false; }
  if (lowN < 3) { lowN++; return false; }
  return true;
}
static inline bool inputPressed() {
  if (digitalRead(BOOT_PIN) == LOW) return true;
  return touchPressed();
}
bool touchRead(int &tx, int &ty) {          // false = idle-looking reading
  touchSPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  digitalWrite(TOUCH_CS, LOW);
  touchSPI.transfer(0xD0);
  uint16_t xRaw = ((touchSPI.transfer(0) << 8) | touchSPI.transfer(0)) >> 3;
  touchSPI.transfer(0x90);
  uint16_t yRaw = ((touchSPI.transfer(0) << 8) | touchSPI.transfer(0)) >> 3;
  digitalWrite(TOUCH_CS, HIGH);
  touchSPI.endTransaction();
  bool valid = xRaw > 150 && xRaw < 3950 && yRaw > 150 && yRaw < 3950;
  // CYD landscape: raw Y -> screen X, raw X -> screen Y
  tx = map(yRaw, 200, 3900, 0, 320);
  ty = map(xRaw, 3900, 200, 0, 240);
  if (tx < 0) tx = 0; if (tx > 319) tx = 319;
  if (ty < 0) ty = 0; if (ty > 239) ty = 239;
  return valid;
}
void handleTouch() {
  bool pressed = inputPressed();
  unsigned long now = millis();
  if (pressed && !wasPressed) {
    bool ok = true;
    if (digitalRead(BOOT_PIN) != LOW) { int tx, ty; ok = touchRead(tx, ty); }
    if (ok) { pressStartMs = now; wasPressed = true; holdFired = false; }
    else pressed = false;                    // line noise, not a real press
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

// ─── Render ───
void drawHUD() {
  char buf[16];
  uint16_t val = logoCol;
  unsigned long b = bounces > 999999 ? 999999 : bounces;
  snprintf(buf, sizeof(buf), "%lu", b);
  fbText(8, 6, "BOUNCES", colLabel());
  fbText(8 + textW("BOUNCES") + 8, 6, buf, val);

  unsigned long c = corners > 999999 ? 999999 : corners;
  snprintf(buf, sizeof(buf), "%lu", c);
  int vw = textW(buf);
  int lw = textW("CORNERS");
  fbText(WIDTH - 8 - vw - 8 - lw, 6, "CORNERS", colLabel());
  fbText(WIDTH - 8 - vw, 6, buf, val);
}

void drawToast() {
  int tw = textW(toast, 2);
  int bw = tw + 40, bh = 48;
  int bx = (WIDTH - bw) / 2, by = HEIGHT / 2 - bh / 2;
  fbFillRect(bx, by, bw, bh, colBG());
  fbRect(bx, by, bw, bh, colFaint());
  fbText(bx + 20, by + 14, toast, logoCol, 2);
}

void drawFrame() {
  unsigned long now = millis();
  fbClear(colBG());

  drawLogo((int)lx, (int)ly);

  for (int i = 0; i < MAX_PARTS; i++) {
    if (now >= parts[i].until) continue;
    FPIX((int)parts[i].x, (int)parts[i].y, parts[i].col);
    FPIX((int)parts[i].x + 1, (int)parts[i].y, parts[i].col);
    FPIX((int)parts[i].x, (int)parts[i].y + 1, parts[i].col);
    FPIX((int)parts[i].x + 1, (int)parts[i].y + 1, parts[i].col);
  }

  drawHUD();

  if (now < cornerUntil) {
    // CYD has no NeoPixel: flash a frame around the screen in the logo colour
    fbFillRect(0, 0, WIDTH, 3, logoCol);
    fbFillRect(0, HEIGHT - 3, WIDTH, 3, logoCol);
    fbFillRect(0, 0, 3, HEIGHT, logoCol);
    fbFillRect(WIDTH - 3, 0, 3, HEIGHT, logoCol);
    const char* s = "CORNER!";
    int w = textW(s, 4);
    int x = (WIDTH - w) / 2, y = HEIGHT / 2 - 22;
    fbFillRect(x - 12, y - 10, w + 24, 68, colBG());
    fbRect(x - 12, y - 10, w + 24, 68, logoCol);
    fbText(x, y, s, logoCol, 4);
  } else if (toast[0] && now < toastUntil) {
    drawToast();
  }
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
  randomSeed(micros() + analogRead(0));

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  SPI.begin(TFT_SCLK, TFT_MISO, TFT_MOSI, TFT_CS);
  tft.init(240, 320);
  tft.setSPISpeed(80000000);   // ST7789 takes 80 MHz — halves the strip flush
  tft.setRotation(1);
  tft.invertDisplay(false);
  tft.fillScreen(0x0000);

  touchInit();

  prefs.begin("dvd", false);
  bounces = prefs.getULong("b", 0);
  corners = prefs.getULong("c", 0);

  setLogoColor(esp_random() % 6);
  lx = 40 + esp_random() % (BOUNCE_X - 80);
  ly = 40 + esp_random() % (BOUNCE_Y - 80);
  float a = 0.45f + (esp_random() % 100) * 0.01f;   // never too shallow
  vx = cosf(a) * SPEEDS[0];
  vy = sinf(a) * SPEEDS[0];

  Serial.println("[dvd] CYD edition boot");
  dm01Start();
}

void loop() {
  unsigned long t = millis();
  screenshotStripPoll(shot, WIDTH, HEIGHT);
  if (dm01Tick(inputPressed())) {
    introMode = true;
    render();
    delay(16);
    return;
  }
  introMode = false;

  static unsigned long lastFrame = 0;
  unsigned long now = millis();
  float dt = (now - lastFrame) / 1000.0f;
  lastFrame = now;
  if (dt > 0.06f) dt = 0.06f;

  handleTouch();
  update(dt);
  render();

  // pace to ~60 FPS: the frame itself costs ~30ms, so this rarely waits
  static uint32_t nextFrame = 0;
  uint32_t ms = millis();
  if ((int32_t)(nextFrame - ms) > 0) delay(nextFrame - ms);
  nextFrame += 16;
  if ((int32_t)(nextFrame - ms) < -100) nextFrame = ms + 16;
}
