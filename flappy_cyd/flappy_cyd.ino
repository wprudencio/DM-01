// Flappy Bird — CYD (ESP32-2432S028R) version.
// Port of flappy/flappy.ino (ESP32-C3 + ST7735 160x128 + button) to the
// Cheap Yellow Display: ST7789 320x240 + XPT2046 resistive touch.
//
// Renders in the SIGNAL/VHS theme: #242424 deck, white ink, a magenta accent
// strip, signal-green best score and a per-strip VHS glitch filter. Pipes are
// light grey with white highlights, the bird is white with a magenta rim, the
// ground is a grey band with light-grey rules. All type is the 11px Departure
// Mono raster, scaled per element.
//
// Controls: tap anywhere on the touch screen = flap / start / retry.
// No NeoPixel on CYD — state feedback is on-screen + serial only.
// High score persisted in NVS. Serial 'F' simulates a flap (screenshots/tests).
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <Preferences.h>
#include <string.h>
#include "Screenshot.h"

// ── Pins: CYD (ESP32-2432S028R) ──
#define TFT_CS    15
#define TFT_DC    2
#define TFT_RST   4
#define TFT_MOSI  13
#define TFT_SCLK  14
#define TFT_MISO  12
#define TFT_BL    21

// ── Touch (XPT2046 on separate HSPI bus) ──
#define TOUCH_CS   33
#define TOUCH_CLK  25
#define TOUCH_MOSI 32
#define TOUCH_MISO 39
#define TOUCH_IRQ  36

Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);
SPIClass touchSPI(HSPI);
Preferences prefs;

#define WIDTH 320
#define HEIGHT 240
#define HORIZON 208

// ── Framebuffer (strip-based: 64 rows per strip; 150 KB full frame does not
//    fit ESP32 DRAM, so screenshots stream band by band — see render()) ──
#define FB_H 64
static uint16_t fb[WIDTH * FB_H];
static int fbTop = 0;
static bool introMode = false;
static ScreenshotStripSession shot;

#define FPIX(x,y,c) if((x)>=0&&(x)<WIDTH&&(y)>=fbTop&&(y)<fbTop+FB_H) fb[(y-fbTop)*WIDTH+(x)]=(c)

uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) { return tft.color565(r, g, b); }

void fbClear(uint16_t c = 0x0000) {
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

#define DM01_SCALE 2
#include "Dm01Intro.h"

// ── Departure Mono (rasterized, 11px). y is the cap/digit top. ──
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

// ── SIGNAL palette — near-black deck, magenta + signal green (VHS theme) ──
uint16_t panelBG()    { return rgb( 36,  36,  36); } // #242424 deck
uint16_t panelInk()   { return rgb(255, 255, 255); } // white ink
uint16_t panelGhost() { return rgb( 88,  88,  88); } // parallax ghosts / grid
uint16_t panelHI()    { return rgb(170, 170, 170); } // light grey
uint16_t panelEdge()  { return rgb(170, 170, 170); } // light grey rules + pipes
uint16_t accent()     { return rgb(236,   0, 140); } // #EC008C magenta
uint16_t accentDK()   { return rgb(104,   0,  62); } // dimmed magenta
uint16_t accentGN()   { return rgb(  0, 230, 118); } // #00E676 signal green

// ── VHS filter: slow chroma ripple + per-element glitch hits. Each UI band
//     tears on its own 6s slots and its rip rows are washed with cycling
//     primaries (yellow/cyan/green/magenta/red/blue); skipping the off-band
//     rows keeps the per-frame cost tiny. Only a single row is buffered. ──
static uint16_t vhsRow[WIDTH];
static uint32_t vhsT = 0;

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

void vhsApply(int h) {
  uint32_t t = vhsT;
  float ph = (float)t * 0.0018f;

  // per-element bands: header/HUD, accent strip, upper playfield,
  // lower playfield, ground/footer
  static const int bandY0[5] = {   2, 30,  40, 124, 208 };
  static const int bandY1[5] = {  30, 40, 124, 208, 240 };
  int bDx[5], bBoost[5]; float bChroma[5]; uint8_t bPal[5]; bool bHit[5];
  for (int i = 0; i < 5; i++) {
    bHit[i]    = elemHit(i, t, bDx[i], bPal[i]);
    bDx[i]     = bHit[i] ? bDx[i] : 0;
    bChroma[i] = bHit[i] ? 7.0f : 0.0f;
    bBoost[i]  = bHit[i] ? 4 : 0;
  }

  int flick = 246 + (int)(vhsNoise(t / 110u) % 11);

  for (int row = 0; row < h; row++) {
    int y = fbTop + row;
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
    int li0 = (int)lsh, lfr = (int)((lsh - li0) * 256.0f);
    int ri0 = (int)rsh, rfr = (int)((rsh - ri0) * 256.0f);
    memcpy(vhsRow, &fb[row * WIDTH], WIDTH * sizeof(uint16_t));
    const uint16_t* s = vhsRow;
    uint16_t* d = &fb[row * WIDTH];
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

// ── Layout (2x the C3 layout) ──
#define HDR_Y   8
#define ACC_Y   30   // dim lines; magenta bar 32..39
#define PLAY_T  40

// ── Game (2x scale of the C3 version) ──
#define MAX_PIPES 3
#define PIPE_W    32
#define CAP_W     44
#define CAP_H     10
#define LINE      2
#define GAP       88
#define SPACING   152
#define BIRD_X    80
#define BIRD_R    10
#define HIT_R     7
#define READY_Y   150

#define PHYS_H    (1.0f/60.0f)
#define GRAV      1800.0f
#define FLAP_V    -450.0f
#define MAX_FALL  520.0f
#define SCROLL    124.0f

enum State : uint8_t { S_READY, S_PLAY, S_DYING, S_OVER };
State state = S_READY;

struct Pipe { float x; int gapY; bool scored; };
Pipe pipes[MAX_PIPES];
int nPipes = 0;

float birdY = READY_Y, birdVy = 0;
int score = 0, hiScore = 0;
float scrollDist = 0;
unsigned long lastFlapMs = 0, scoreFlashMs = 0, overMs = 0;
bool wasPressed = false;
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
}

void spawnPipe(float x){
  if(nPipes >= MAX_PIPES) return;
  pipes[nPipes].x = x;
  pipes[nPipes].gapY = random(GAP/2 + 28, HORIZON - GAP/2 - 28);
  pipes[nPipes].scored = false;
  nPipes++;
}

void onFlap(){
  unsigned long now = millis();
  if(state == S_READY){
    resetGame(); state = S_PLAY; birdVy = FLAP_V; lastFlapMs = now;
  } else if(state == S_PLAY){
    birdVy = FLAP_V; lastFlapMs = now;
  } else if(state == S_OVER && now - overMs > 400){
    resetGame(); state = S_PLAY; birdVy = FLAP_V; lastFlapMs = now;
  }
}

void stepWorld(){
  scrollDist += SCROLL * PHYS_H;
  for(int i=0;i<nPipes;i++) pipes[i].x -= SCROLL * PHYS_H;
  if(nPipes && pipes[0].x < -(float)(CAP_W+4)){
    for(int i=1;i<nPipes;i++) pipes[i-1] = pipes[i];
    nPipes--;
  }
  if(nPipes == 0) spawnPipe(WIDTH + 48);
  else if(pipes[nPipes-1].x <= WIDTH - SPACING) spawnPipe(pipes[nPipes-1].x + SPACING);

  birdVy += GRAV * PHYS_H;
  if(birdVy > MAX_FALL) birdVy = MAX_FALL;
  birdY += birdVy * PHYS_H;
  if(birdY - BIRD_R < 4){ birdY = 4 + BIRD_R; if(birdVy < 0) birdVy = 0; }

  for(int i=0;i<nPipes;i++){
    if(!pipes[i].scored && pipes[i].x + PIPE_W < BIRD_X - HIT_R){
      pipes[i].scored = true;
      score++;
      scoreFlashMs = millis();
      Serial.printf("[flappy] SCORE %d\n", score);
    }
  }

  bool hit = (birdY + HIT_R >= HORIZON);
  for(int i=0;i<nPipes && !hit;i++){
    if(BIRD_X + HIT_R < pipes[i].x || BIRD_X - HIT_R > pipes[i].x + PIPE_W) continue;
    if(birdY - HIT_R < pipes[i].gapY - GAP/2 || birdY + HIT_R > pipes[i].gapY + GAP/2) hit = true;
  }
  if(hit){
    state = S_DYING;
    birdVy = -280.0f;
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

  if(state == S_READY){ birdY = READY_Y + sinf(t * 0.004f) * 6.0f; birdVy = 0; acc = 0; return; }
  if(state == S_OVER) return;

  acc += dt;
  int steps = 0;
  while(acc >= PHYS_H && steps < 4){
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
        if(score > hiScore){ hiScore = score; saveHiScore(); }
        Serial.printf("[flappy] GAME OVER score=%d hi=%d\n", score, hiScore);
      }
    }
  }
}

// ── Drawing ──
void drawSky(){
  // dot grid over the playfield (dim texture, never fights the pipes)
  for(int y = PLAY_T + 4; y < HORIZON; y += 12)
    for(int x = ((y / 12) & 1) * 6; x < WIDTH; x += 12) FPIX(x, y, panelGhost());
  // parallax cloud dashes at half the scroll speed
  int off = ((int)(scrollDist * 0.5f)) % 96;
  for(int x = -96 - off; x < WIDTH; x += 96){
    fbFillRect(x + 16, 60, 28, 4, panelGhost());
    fbFillRect(x + 60, 96, 20, 4, panelGhost());
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
void drawPipe(float px,int gapY){
  int x = (int)px;
  int topB = gapY - GAP/2;
  int botT = gapY + GAP/2;
  int capX = x - (CAP_W - PIPE_W)/2;
  pipeSection(x, 0, PIPE_W, topB - CAP_H, panelEdge(), true);
  pipeSection(capX, topB - CAP_H, CAP_W, CAP_H, panelInk(), false);
  pipeSection(x, botT + CAP_H, PIPE_W, HORIZON - (botT + CAP_H), panelEdge(), true);
  pipeSection(capX, botT, CAP_W, CAP_H, panelInk(), false);
}

void drawGround(){
  // grey band footer with a light-grey top rule and dark scrolling treads
  fbFillRect(0, HORIZON, WIDTH, HEIGHT - HORIZON, panelGhost());
  fbFillRect(0, HORIZON, WIDTH, 4, panelEdge());
  int off = ((int)scrollDist) % 24;
  for(int x = -24 - off; x < WIDTH; x += 24){
    fbFillRect(x + 6, HORIZON + 12, 14, 4, panelBG());
    fbFillRect(x + 18, HORIZON + 22, 14, 4, panelBG());
  }
}

void drawBird(int x,int y,bool wingUp){
  fbFillCircle(x, y, BIRD_R + 2, accent());   // magenta rim pops on grey pipes
  fbFillCircle(x, y, BIRD_R, panelInk());     // white body
  if(wingUp) fbFillCircle(x-4, y-4, 6, panelGhost());
  else       fbFillCircle(x-4, y+4, 6, panelGhost());
  fbFillCircle(x+4, y-4, 4, panelBG());       // dark eye + white glint
  fbFillCircle(x+6, y-4, 2, panelInk());
  fbFillRect(x+7, y-1, 8, 6, accent());       // beak
}

// HUD row: state left, BEST green centre, score right. Small deck chips keep
// the labels readable when a pipe crosses the top rows.
void drawHud(){
  const char* st = (state == S_READY) ? "READY" : (state == S_OVER ? "OVER" : "PLAY");
  uint16_t stc = (state == S_OVER) ? accent() : accentGN();
  char buf[8]; snprintf(buf,sizeof(buf),"%d",score);
  char hb[16]; snprintf(hb,sizeof(hb),"BEST %d",hiScore);
  int ws = textW57(buf, 2), wb = textW57(hb, 2), wl = textW57(st, 2);
  int xs = WIDTH - 12 - ws, xb = (WIDTH - wb) / 2;
  fbFillRect(6, 0, wl + 12, 30, panelBG());
  fbFillRect(xb - 6, 0, wb + 12, 30, panelBG());
  fbFillRect(xs - 6, 0, ws + 12, 30, panelBG());
  fbText(12, HDR_Y, st, stc, 2);
  fbText(xb, HDR_Y, hb, accentGN(), 2);
  bool flash = (scoreFlashMs && millis() - scoreFlashMs < 160);
  fbText(xs, HDR_Y, buf, flash ? accent() : panelInk(), 2);
}

void drawReady(unsigned long t){
  const char* title = "FLAPPY";
  int w = textW57(title, 4);
  fbText((WIDTH - w) / 2, 34, title, panelInk(), 4);
  fbHLine((WIDTH - w) / 2, 88, w, accentDK());
  fbFillRect((WIDTH - w) / 2, 89, w, 6, accent());
  if((t/500)%2==0)
    fbText((WIDTH - textW57("TAP TO FLAP", 2)) / 2, 104, "TAP TO FLAP", panelInk(), 2);
}

void drawOver(unsigned long t){
  fbFillRect(48, 24, 224, 184, panelBG());
  fbRect(48, 24, 224, 184, panelEdge());
  fbHLine(48, 26, 224, accentDK());
  fbFillRect(48, 27, 224, 6, accent());
  const char* go = "GAME OVER";
  fbText((WIDTH - textW57(go, 3)) / 2, 42, go, accent(), 3);
  char sbuf[8]; snprintf(sbuf,sizeof(sbuf),"%d",score);
  fbText((WIDTH - textW57(sbuf, 4)) / 2, 88, sbuf, panelInk(), 4);
  char buf[16]; snprintf(buf,sizeof(buf),"BEST %d",hiScore);
  fbText((WIDTH - textW57(buf, 2)) / 2, 136, buf, accentGN(), 2);
  if((t/500)%2==0){
    const char* s = "TAP TO RETRY";
    fbText((WIDTH - textW57(s, 2)) / 2, 168, s, accentDK(), 2);
  }
}

void drawFrame(unsigned long t){
  fbClear(panelBG());
  drawSky();
  fbHLine(8, ACC_Y, WIDTH - 16, accentDK());       // magenta accent strip
  fbHLine(8, ACC_Y + 1, WIDTH - 16, accentDK());
  fbFillRect(8, ACC_Y + 2, WIDTH - 16, 8, accent());
  if(state != S_READY) for(int i=0;i<nPipes;i++) drawPipe(pipes[i].x, pipes[i].gapY);
  drawGround();
  bool wingUp;
  if(state == S_READY) wingUp = ((t/220)%2==0);
  else if(state == S_PLAY) wingUp = ((t - lastFlapMs) < 130 || birdVy < 0);
  else wingUp = false;
  drawHud();
  drawBird(BIRD_X, (int)birdY, wingUp);   // bird over the HUD chip at the ceiling
  if(state == S_READY) drawReady(t);
  if(state == S_OVER) drawOver(t);
}

void render(unsigned long t){
  vhsT = millis();
  for (fbTop = 0; fbTop < HEIGHT; fbTop += FB_H) {
    if (introMode) dm01Draw(dm01Elapsed()); else drawFrame(t);
    int h = FB_H;
    if (fbTop + h > HEIGHT) h = HEIGHT - fbTop;
    if (!introMode) vhsApply(h);          // intro stays clean
    if (!screenshotStripSend(shot, fb, fbTop, h)) fbFlush();
  }
}

// ── Touch (XPT2046): tap anywhere = flap / start / retry ──
void touchInit() {
  touchSPI.begin(TOUCH_CLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
  pinMode(TOUCH_CS, OUTPUT);
  digitalWrite(TOUCH_CS, HIGH);
  pinMode(TOUCH_IRQ, INPUT);  // GPIO36 input-only: no internal pullup (external PU on CYD)
}
bool touchPressed() {
  return digitalRead(TOUCH_IRQ) == LOW;
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
  // Map raw to screen — CYD landscape: axes are SWAPPED (raw Y → screen X, raw X → screen Y)
  tx = map(yRaw, 200, 3900, 0, 320);
  ty = map(xRaw, 3900, 200, 0, 240);
  if (tx < 0) tx = 0; if (tx > 319) tx = 319;
  if (ty < 0) ty = 0; if (ty > 239) ty = 239;
}
void handleTouch(){
  bool pressed = touchPressed();
  unsigned long now = millis();
  if(pressed && !wasPressed && now - lastBtnMs > 80){
    lastBtnMs = now;
    int tx, ty; touchRead(tx, ty);
    Serial.printf("[touch] press @ %d,%d\n", tx, ty);
    onFlap();
  }
  wasPressed = pressed;
}
void handleDebugSerial(){
  if(Serial.available() && (Serial.peek()=='F' || Serial.peek()=='f')){
    Serial.read();
    onFlap();
  }
}

void setup(){
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== FLAPPY CYD ===");

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  SPI.begin(TFT_SCLK, TFT_MISO, TFT_MOSI, TFT_CS);
  tft.init(240, 320);
  tft.setSPISpeed(40000000);
  tft.setRotation(1);
  // Panel is RGB (lib default) but needs inversion off: the lib's
  // ST7789 init sends INVON which turned WORK black bg white.
  tft.invertDisplay(false);
  tft.fillScreen(0x0000);

  loadHiScore();
  touchInit();
  resetGame();
  birdY = READY_Y;
  randomSeed(esp_random());
  Serial.printf("[flappy] ready, hi=%d\n", hiScore);
  dm01Start();
}

void loop(){
  unsigned long t = millis();
  screenshotStripPoll(shot, WIDTH, HEIGHT);
  if (dm01Tick(touchPressed())) {
    introMode = true;
    render(t);
    delay(16);
    return;
  }
  introMode = false;
  handleDebugSerial();
  handleTouch();
  updatePhysics(t);
  render(t);
  delay(10);
}
