// Flappy Bird — ESP32-C3 + ST7735 160x128, SIGNAL/VHS theme.
//
// Near-black deck, white ink, a magenta accent strip, signal-green best score
// and a VHS filter that tears one UI band at a time with the monitor
// primaries. Pipes are light grey with white highlights, the bird is white
// with a magenta rim, the ground is a grey band with light-grey rules. Labels
// use the hand-drawn 5x6 Departure Mono set, scores the 11px raster.
//
// GPIO 0 button = flap / start / retry. WS2812 on GPIO 10 = state feedback.
// High score persisted in NVS. Serial 'F' simulates a flap (screenshots/tests).
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>
#include <Adafruit_NeoPixel.h>
#include <Preferences.h>
#include <string.h>
#include "Screenshot.h"

// ── Pins (ESP32-C3 + ST7735 160x128) ──
#define TFT_CS    5
#define TFT_RST   4
#define TFT_DC    3
#define TFT_MOSI  2
#define TFT_SCLK  1
#define TOUCH_PIN 0
#define LED_PIN   10
#define NUMPIXELS 1

Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_RST);
Adafruit_NeoPixel pixels(NUMPIXELS, LED_PIN, NEO_GRB + NEO_KHZ800);
Preferences prefs;

#define WIDTH 160
#define HEIGHT 128
#define HORIZON 112

// ── Framebuffer ──
static uint16_t fb[WIDTH * HEIGHT];
#define FPIX(x,y,c) if((x)>=0&&(x)<WIDTH&&(y)>=0&&(y)<HEIGHT) fb[(y)*WIDTH+(x)]=(c)

uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) { return tft.color565(r, g, b); }

void fbClear(uint16_t c = 0x0000) {
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

// ── SIGNAL palette — near-black deck, magenta + signal green (VHS theme) ──
uint16_t panelBG()    { return rgb( 16,  16,  16); } // #101010 deck
uint16_t panelInk()   { return rgb(255, 255, 255); } // white ink
uint16_t panelGhost() { return rgb( 74,  74,  74); } // parallax ghosts / grid
uint16_t panelHI()    { return rgb(170, 170, 170); } // light grey
uint16_t panelEdge()  { return rgb(170, 170, 170); } // light grey rules + pipes
uint16_t accent()     { return rgb(236,   0, 140); } // #EC008C magenta
uint16_t accentDK()   { return rgb(104,   0,  62); } // dimmed magenta
uint16_t accentGN()   { return rgb(  0, 230, 118); } // #00E676 signal green

enum State : uint8_t { S_READY, S_PLAY, S_DYING, S_OVER };
State state = S_READY;

// ── VHS filter: a soft chroma ripple over the playfield plus the occasional
//     short glitch hit whose rip rows are washed with cycling primaries
//     (yellow/cyan/green/magenta/red/blue). Only a single row is buffered
//     (the taps read horizontal neighbours), keeping RAM tiny. ──
#define VHS_PAD 16
static uint16_t vhsRow[WIDTH + 2 * VHS_PAD];

// 256-step Q8 sine: sinf() on this FPU-less C3 takes a *very* slow path once
// its argument grows past ~140 rad (~80s of uptime), which used to drag the
// filter from 11ms to 60ms — never feed it a growing angle.
static int16_t sinQ[256];
void sinInit(){ for(int i=0;i<256;i++) sinQ[i] = (int16_t)(sinf(6.2831853f * i / 256.0f) * 256.0f); }

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
  if ((s % 12u) != 0) return false;
  uint32_t start = (s >> 8) % (SLOT - 500u);
  uint32_t dur   = 140u + ((s >> 20) % 160u);
  uint32_t local = phased % SLOT;
  if (local < start || local >= start + dur) return false;
  dx  = (int)((s >> 12) % 7u) - 3;    // ±3px tears
  pal = (uint8_t)(s >> 24);
  return true;
}

// The VHS feed is a gameplay effect: menus render clean and only the
// playfield rows get the ripple, so the HUD, accent strip and ground stay
// crisp. Hits are rarer, shorter and wash at half strength.
void vhsApply() {
  if (state == S_READY || state == S_OVER) return;

  uint32_t t = millis();
  float ph = (float)(t % 3490u) * 0.0018f;   // wrap to one turn: 3490ms*0.0018 = 2pi
  int phL = (int)(ph * 40.743665f), phR = (int)((1.7f + ph * 1.3f) * 40.743665f);

  // bands: 2 = upper playfield, 3 = lower playfield (world + bird)
  static const int bandY0[5] = {   1, 13,  18,  64, 112 };
  static const int bandY1[5] = {  13, 18,  64, 112, 128 };
  int bDx[5], bBoost[5], bChroma[5]; uint8_t bPal[5]; bool bHit[5];
  for (int i = 0; i < 5; i++) {
    bHit[i]    = (i == 2 || i == 3) && elemHit(i, t, bDx[i], bPal[i]);
    bDx[i]     = bHit[i] ? bDx[i] : 0;
    bChroma[i] = bHit[i] ? 3 : 0;
    bBoost[i]  = bHit[i] ? 2 : 0;
  }

  int flick = 250 + (int)(vhsNoise(t / 110u) % 7);

  for (int bi = 2; bi < 4; bi++) {
    int xOff = bDx[bi], chroma = bChroma[bi], boost = bBoost[bi];
    bool hit = bHit[bi];
    for (int y = bandY0[bi]; y < bandY1[bi]; y++) {
      int sL = sinQ[(uint8_t)(phL + (int)(y * 3.4632115f))];
      int sR = sinQ[(uint8_t)(phR + (int)(y * 2.4853636f))];
      int lsh = 1 + chroma + (sL * 282) / 65536;    // 1 + 1.1*sin
      int rsh = 1 + chroma + (sR * 435) / 65536;    // 1 + 1.7*sin
      if (lsh < 0) lsh = 0; if (rsh < 0) rsh = 0;
      uint16_t* d = &fb[y * WIDTH];
      uint16_t* row = vhsRow + VHS_PAD;
      memcpy(row, d, WIDTH * sizeof(uint16_t));
      for (int i = 0; i < VHS_PAD; i++) { row[-1 - i] = row[0]; row[WIDTH + i] = row[WIDTH - 1]; }
      const uint16_t* base = row + VHS_PAD + xOff;
      if (!hit) {
        for (int x = 0; x < WIDTH; x++) {
          uint16_t cC = base[x];
          int r = (3 * ((base[x - lsh] >> 11) & 0x1F) + ((cC >> 11) & 0x1F)) >> 2;
          int g = (cC >> 5) & 0x3F;
          int b = (3 * (base[x + rsh] & 0x1F) + (cC & 0x1F)) >> 2;
          r = (r * flick) >> 8; g = (g * flick) >> 8; b = (b * flick) >> 8;
          d[x] = (uint16_t)((r << 11) | (g << 5) | b);
        }
      } else {
        // rip colour: shifts every 60ms and every 8 rows inside the hit
        uint16_t pc = vhsPal((uint8_t)(bPal[bi] + t / 60u + (y >> 3)));
        int pr = (pc >> 11) & 0x1F, pg = (pc >> 5) & 0x3F, pb = pc & 0x1F;
        for (int x = 0; x < WIDTH; x++) {
          uint16_t cC = base[x];
          int r = (3 * ((base[x - lsh] >> 11) & 0x1F) + ((cC >> 11) & 0x1F)) >> 2;
          int g = (cC >> 5) & 0x3F;
          int b = (3 * (base[x + rsh] & 0x1F) + (cC & 0x1F)) >> 2;
          r = (r * flick) >> 8; g = (g * flick) >> 8; b = (b * flick) >> 8;
          int w = (g > 22) ? 2 : 1;            // 50% wash on ink, 25% on the deck
          r = (r * (4 - w) + pr * w) >> 2; r += boost;
          g = (g * (4 - w) + pg * w) >> 2; g += boost;
          b = (b * (4 - w) + pb * w) >> 2; b += boost;
          if (r > 31) r = 31; if (g > 63) g = 63; if (b > 31) b = 31;
          d[x] = (uint16_t)((r << 11) | (g << 5) | b);
        }
      }
    }
  }
}

void vhsFlush() { vhsApply(); fbFlush(); }

// ── Layout ──
#define HDR_Y   3
#define ACC_Y   13   // dim line; magenta bar 14..17
#define PLAY_T  18

// ── Game ──
#define MAX_PIPES 3
#define PIPE_W    16
#define CAP_W     22
#define CAP_H     6
#define LINE      1
#define GAP_START 48     // opening, shrinks 1px every third pipe
#define GAP_MIN   40
#define SPACING   84
#define BIRD_X    40
#define BIRD_R    5
#define HIT_R     3
#define READY_Y   80
#define MAX_GAP_DY 24    // max vertical jump between consecutive gaps
#define PIPE_R    (PIPE_W + (CAP_W - PIPE_W) / 2)
#define RUNWAY    36     // extra px before the first pipe arrives

#define PHYS_H    (1.0f/60.0f)
#define TIME_SCALE 1.0f      // 1.0 = real time; keeps physics consistent at any FPS
#define GRAV      820.0f
#define FLAP_V    -208.0f
#define MAX_FALL  230.0f
#define SCROLL_START 58.0f   // px/s at score 0, ramps with score
#define SCROLL_MAX   78.0f
#define SCROLL_STEP  1.2f

struct Pipe { float x; int gapY; int gap; bool scored; };
Pipe pipes[MAX_PIPES];
int nPipes = 0;

float birdY = READY_Y, birdVy = 0;
int score = 0, hiScore = 0;
bool newBest = false;
float scrollDist = 0;
float scrollSpeed = SCROLL_START;
int pipesSpawned = 0, lastGapY = -1;
unsigned long lastFlapMs = 0, scoreFlashMs = 0, overMs = 0;
int idleLevel = HIGH;
bool btnWas = false;
int pressRun = 0;
unsigned long lastBtnMs = 0;

void loadHiScore(){
  prefs.begin("flappy", true);
  hiScore = prefs.getUShort("hi", 0);
  prefs.end();
}
void saveHiScore(){
  prefs.begin("flappy", false);
  prefs.putUShort("hi", (uint16_t)hiScore);
  prefs.end();
}

void resetGame(){
  score = 0;
  nPipes = 0;
  scrollDist = 0;
  birdY = HORIZON/2;
  birdVy = 0;
  newBest = false;
  scrollSpeed = SCROLL_START;
  pipesSpawned = 0;
  lastGapY = -1;
}

int gapAt(int n){ int g = GAP_START - n / 3; return g < GAP_MIN ? GAP_MIN : g; }

void spawnPipe(float x){
  if(nPipes >= MAX_PIPES) return;
  int g = gapAt(pipesSpawned);
  int lo = g/2 + 14, hi = HORIZON - g/2 - 14;
  int gy = random(lo, hi);
  if(lastGapY >= 0){
    int l = lastGapY - MAX_GAP_DY, h = lastGapY + MAX_GAP_DY;
    if(l < lo) l = lo;
    if(h > hi) h = hi;
    gy = random(l, h + 1);
  }
  if(gy < lo) gy = lo; if(gy > hi) gy = hi;
  pipes[nPipes].x = x;
  pipes[nPipes].gapY = gy;
  pipes[nPipes].gap = g;
  pipes[nPipes].scored = false;
  nPipes++;
  pipesSpawned++;
  lastGapY = gy;
}

void onFlap(const char* src){
  unsigned long now = millis();
  if(state == S_READY){
    Serial.printf("[flappy] FLAP %s @%lu start\n", src, now);
    resetGame(); state = S_PLAY; birdVy = FLAP_V; lastFlapMs = now;
  } else if(state == S_PLAY){
    birdVy = FLAP_V; lastFlapMs = now;
  } else if(state == S_OVER && now - overMs > 400){
    Serial.printf("[flappy] FLAP %s @%lu retry\n", src, now);
    resetGame(); state = S_PLAY; birdVy = FLAP_V; lastFlapMs = now;
  }
}

void stepWorld(){
  scrollDist += scrollSpeed * PHYS_H;
  for(int i=0;i<nPipes;i++) pipes[i].x -= scrollSpeed * PHYS_H;
  if(nPipes && pipes[0].x < -(float)(CAP_W+2)){
    for(int i=1;i<nPipes;i++) pipes[i-1] = pipes[i];
    nPipes--;
  }
  if(nPipes == 0) spawnPipe(WIDTH + RUNWAY);
  else if(pipes[nPipes-1].x <= WIDTH - SPACING) spawnPipe(pipes[nPipes-1].x + SPACING);

  birdVy += GRAV * PHYS_H;
  if(birdVy > MAX_FALL) birdVy = MAX_FALL;
  birdY += birdVy * PHYS_H;
  if(birdY - BIRD_R < 2){ birdY = 2 + BIRD_R; if(birdVy < 0) birdVy = 0; }

  for(int i=0;i<nPipes;i++){
    if(!pipes[i].scored && pipes[i].x + PIPE_R < BIRD_X - HIT_R){
      pipes[i].scored = true;
      score++;
      scoreFlashMs = millis();
      scrollSpeed = SCROLL_START + score * SCROLL_STEP;
      if(scrollSpeed > SCROLL_MAX) scrollSpeed = SCROLL_MAX;
      Serial.printf("[flappy] SCORE %d\n", score);
    }
  }

  bool hit = (birdY + HIT_R >= HORIZON);
  for(int i=0;i<nPipes && !hit;i++){
    int px = (int)pipes[i].x;
    int capX = px - (CAP_W - PIPE_W)/2;
    int topY = pipes[i].gapY - pipes[i].gap/2;
    int botY = pipes[i].gapY + pipes[i].gap/2;
    bool xBody = (BIRD_X + HIT_R >= px) && (BIRD_X - HIT_R <= px + PIPE_W);
    bool xCap  = (BIRD_X + HIT_R >= capX) && (BIRD_X - HIT_R <= capX + CAP_W);
    if(!xBody && !xCap) continue;
    bool yBody = (birdY - HIT_R < topY - CAP_H) || (birdY + HIT_R > botY + CAP_H);
    bool yCap  = (birdY - HIT_R < topY && birdY + HIT_R > topY - CAP_H) ||
                 (birdY - HIT_R < botY + CAP_H && birdY + HIT_R > botY);
    if((xBody && yBody) || (xCap && yCap)) hit = true;
  }
  if(hit){
    state = S_DYING;
    birdVy = -140.0f;
    Serial.printf("[flappy] HIT at score %d\n", score);
  }
}

void updatePhysics(unsigned long t){
  static unsigned long prev = 0;
  static float acc = 0;
  if(prev == 0) prev = t;
  float dt = (t - prev) * 0.001f;
  prev = t;
  if(dt < 0) dt = 0; else if(dt > 0.1f) dt = 0.1f;

  if(state == S_READY){ birdY = READY_Y + sinf((t % 1571u) * 0.004f) * 3.0f; birdVy = 0; acc = 0; return; }
  if(state == S_OVER) return;

  acc += dt * TIME_SCALE;
  int steps = 0;
  while(acc >= PHYS_H && steps < 6){
    acc -= PHYS_H; steps++;
    if(state == S_PLAY){
      stepWorld();
    } else if(state == S_DYING){
      birdVy += GRAV * PHYS_H;
      birdY += birdVy * PHYS_H;
      if(birdY + HIT_R >= HORIZON){
        birdY = HORIZON - HIT_R;
        state = S_OVER;
        overMs = millis();
        if(score > hiScore){ hiScore = score; newBest = true; saveHiScore(); }
        Serial.printf("[flappy] GAME OVER score=%d hi=%d%s\n", score, hiScore, newBest ? " NEW BEST" : "");
      }
    }
  }
}

// ── Drawing ──
void drawSky(){
  // dot grid over the playfield (dim texture, never fights the pipes)
  for(int y = PLAY_T + 2; y < HORIZON; y += 8)
    for(int x = ((y / 8) & 1) * 4; x < WIDTH; x += 8) FPIX(x, y, panelGhost());
  // parallax cloud dashes at half the scroll speed
  int off = ((int)(scrollDist * 0.5f)) % 48;
  for(int x = -48 - off; x < WIDTH; x += 48){
    fbFillRect(x + 8, 30, 14, 2, panelGhost());
    fbFillRect(x + 30, 46, 10, 2, panelGhost());
  }
}

void pipeSection(int x,int y,int w,int h,uint16_t fill,bool sheen){
  if(h <= 0 || w <= 0) return;
  fbFillRect(x, y, w, h, fill);                        // light grey body
  fbFillRect(x, y, w, LINE, panelInk());               // white highlights
  if(h > 2*LINE) fbFillRect(x, y+h-LINE, w, LINE, panelInk());
  fbFillRect(x, y, LINE, h, panelInk());
  fbFillRect(x+w-LINE, y, LINE, h, panelInk());
  if(sheen && h > 6*LINE) fbFillRect(x+3*LINE, y+2*LINE, 2*LINE, h-4*LINE, panelInk());
}
void drawPipe(float px,int gapY,int gap){
  int x = (int)px;
  int topB = gapY - gap/2;
  int botT = gapY + gap/2;
  int capX = x - (CAP_W - PIPE_W)/2;
  pipeSection(x, 0, PIPE_W, topB - CAP_H, panelEdge(), true);
  pipeSection(capX, topB - CAP_H, CAP_W, CAP_H, panelInk(), false);
  pipeSection(x, botT + CAP_H, PIPE_W, HORIZON - (botT + CAP_H), panelEdge(), true);
  pipeSection(capX, botT, CAP_W, CAP_H, panelInk(), false);
}

void drawGround(){
  // grey band footer with a light-grey top rule and dark scrolling treads
  fbFillRect(0, HORIZON, WIDTH, HEIGHT - HORIZON, panelGhost());
  fbFillRect(0, HORIZON, WIDTH, 2, panelEdge());
  int off = ((int)scrollDist) % 12;
  for(int x = -12 - off; x < WIDTH; x += 12){
    fbFillRect(x + 3, HORIZON + 6, 7, 2, panelBG());
    fbFillRect(x + 9, HORIZON + 11, 7, 2, panelBG());
  }
}

void drawBird(int x,int y,bool wingUp,int pitch){
  fbFillCircle(x, y, BIRD_R + 1, accent());   // magenta rim pops on grey pipes
  fbFillCircle(x, y, BIRD_R, panelInk());     // white body
  if(wingUp) fbFillCircle(x-2, y-2, 3, panelGhost());
  else       fbFillCircle(x-2, y+2, 3, panelGhost());
  fbFillCircle(x+2, y-2+pitch, 2, panelBG()); // dark eye + white glint
  fbFillCircle(x+3, y-2+pitch, 1, panelInk());
  fbFillRect(x+3, y+pitch, 4, 3, accent());   // beak (pitches with velocity)
}

// HUD row: state left, BEST green centre, score (11px raster) right. Small
// deck chips keep the labels readable when a pipe crosses the top rows.
void drawHud(){
  const char* st = (state == S_READY) ? "READY" : (state == S_OVER ? "OVER" : "PLAY");
  uint16_t stc = (state == S_OVER) ? accent() : accentGN();
  char buf[8]; snprintf(buf,sizeof(buf),"%d",score);
  char hb[16]; snprintf(hb,sizeof(hb),"BEST %d",hiScore);
  int ws = textW11(buf), wb = textW57(hb), wl = textW57(st);
  int xs = WIDTH - 6 - ws, xb = (WIDTH - wb) / 2;
  fbFillRect(4, HDR_Y - 2, wl + 4, 10, panelBG());
  fbFillRect(xb - 2, HDR_Y - 2, wb + 4, 10, panelBG());
  fbFillRect(xs - 2, 0, ws + 4, 13, panelBG());
  fbText(6, HDR_Y, st, stc);
  fbText(xb, HDR_Y, hb, accentGN());
  bool flash = (scoreFlashMs && millis() - scoreFlashMs < 160);
  fbText11(xs, 1, buf, flash ? accent() : panelInk());
}

void drawReady(unsigned long t){
  const char* title = "FLAPPY";
  int w = textW57(title, 2);
  fbText((WIDTH - w) / 2, 18, title, panelInk(), 2);
  fbHLine((WIDTH - w) / 2, 44, w, accentDK());
  fbFillRect((WIDTH - w) / 2, 45, w, 3, accent());
  if((t/500)%2==0)
    fbText((WIDTH - textW57("TAP TO FLAP")) / 2, 52, "TAP TO FLAP", panelInk());
}

void drawOver(unsigned long t){
  fbFillRect(16, 12, 128, 104, panelBG());
  fbRect(16, 12, 128, 104, panelEdge());
  fbHLine(16, 14, 128, accentDK());
  fbFillRect(16, 15, 128, 3, accent());
  const char* go = "GAME OVER";
  fbText((WIDTH - textW57(go, 2)) / 2, 24, go, accent(), 2);
  char sbuf[8]; snprintf(sbuf,sizeof(sbuf),"%d",score);
  fbText((WIDTH - textW57(sbuf, 2)) / 2, 50, sbuf, newBest ? accent() : panelInk(), 2);
  if(newBest){
    uint16_t cb = ((t/250)%2==0) ? accentGN() : accent();
    fbText((WIDTH - textW57("NEW BEST")) / 2, 68, "NEW BEST", cb);
  }
  char buf[16]; snprintf(buf,sizeof(buf),"BEST %d",hiScore);
  fbText((WIDTH - textW57(buf)) / 2, 78, buf, accentGN());
  if((t/500)%2==0)
    fbText((WIDTH - textW57("TAP TO RETRY")) / 2, 94, "TAP TO RETRY", accentDK());
}

void render(unsigned long t){
  fbClear(panelBG());
  drawSky();
  fbHLine(4, ACC_Y, WIDTH - 8, accentDK());        // magenta accent strip
  fbFillRect(4, ACC_Y + 1, WIDTH - 8, 4, accent());
  if(state != S_READY) for(int i=0;i<nPipes;i++) drawPipe(pipes[i].x, pipes[i].gapY, pipes[i].gap);
  drawGround();
  bool wingUp;
  if(state == S_READY) wingUp = ((t/220)%2==0);
  else if(state == S_PLAY) wingUp = ((t - lastFlapMs) < 130 || birdVy < 0);
  else wingUp = false;
  int pitch = (int)(birdVy * 0.022f);
  if(pitch < -3) pitch = -3; if(pitch > 3) pitch = 3;
  drawBird(BIRD_X, (int)birdY, wingUp, pitch);
  drawHud();
  if(state == S_READY) drawReady(t);
  if(state == S_OVER) drawOver(t);
  vhsFlush();
}

// ── LED ──
void updateLED(){
  static unsigned long last = 0;
  unsigned long now = millis();
  if(now - last < 40) return;
  last = now;
  if(state == S_DYING || state == S_OVER){
    bool on = (now/150)%2==0;
    pixels.setPixelColor(0, on ? pixels.Color(255,30,20) : pixels.Color(0,0,0));
  } else if(state == S_PLAY){
    if(now - scoreFlashMs < 120) pixels.setPixelColor(0, pixels.Color(210,255,210));
    else if(now - lastFlapMs < 90) pixels.setPixelColor(0, pixels.Color(0,180,60));
    else { float p=(sinf((now%1571u)*0.004f)+1)*0.5f; pixels.setPixelColor(0, pixels.Color(0, 25+p*25, 8+p*8)); }
  } else {
    float p=(sinf((now%2094u)*0.003f)+1)*0.5f;
    pixels.setPixelColor(0, pixels.Color(0, 60+p*120, 20+p*40));
  }
  pixels.show();
}

// ── Input ──
void calibrateButton(){
  delay(300);
  int highCount = 0;
  for(int i=0;i<20;i++){ if(digitalRead(TOUCH_PIN)==HIGH) highCount++; delay(10); }
  idleLevel = (highCount>10) ? HIGH : LOW;
  Serial.printf("[btn] calibrate highCount=%d idle=%s\n", highCount, idleLevel==HIGH?"HIGH":"LOW");
}
void handleButton(){
  bool pressed = (digitalRead(TOUCH_PIN) != idleLevel);
  unsigned long now = millis();
  // Leaky debounce: a real press accumulates samples fast, noise spikes decay.
  if(pressed){ if(pressRun < 6) pressRun++; }
  else { if(pressRun > 0) pressRun--; }
  if(pressRun >= 2 && !btnWas && now - lastBtnMs > 100){
    btnWas = true;
    lastBtnMs = now;
    onFlap("btn");
  }
  if(!pressed && pressRun == 0) btnWas = false;
}
void handleDebugSerial(){
  if(Serial.available() && (Serial.peek()=='F' || Serial.peek()=='f')){
    Serial.read();
    onFlap("serial");
  }
}

#include "Dm01Intro.h"

void setup(){
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== FLAPPY ===");

  pinMode(TOUCH_PIN, INPUT_PULLDOWN);  // active-high input: hold idle LOW
  pixels.begin(); pixels.setBrightness(40); pixels.clear(); pixels.show();

  SPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);
  tft.initR(INITR_BLACKTAB);
  tft.setSPISpeed(40000000);
  tft.setRotation(1);
  tft.fillScreen(ST7735_BLACK);

  sinInit();
  loadHiScore();
  calibrateButton();
  resetGame();
  birdY = READY_Y;
  randomSeed(esp_random());
  Serial.printf("[flappy] ready, hi=%d\n", hiScore);
  dm01Start();
}

void loop(){
  bool introSkip = (digitalRead(TOUCH_PIN)==HIGH);
  if (dm01Frame(introSkip)) {
    pixels.setBrightness(70);
    pixels.setPixelColor(0, dm01Pal((int)(millis()/150)));
    pixels.show();
    fbFlush();
    screenshotHandle(fb, WIDTH, HEIGHT);
    delay(16);
    return;
  }
  static bool introEnded = false;
  if (!introEnded && !dm01IsActive()) { introEnded = true; pixels.setBrightness(40); }

  unsigned long t = millis();
  handleDebugSerial();
  handleButton();
  updatePhysics(t);
  render(t);
  updateLED();
  screenshotHandle(fb, WIDTH, HEIGHT);
  delay(1);
}
