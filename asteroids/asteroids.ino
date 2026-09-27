// Asteroids Vector — ESP32-C3 + ST7735 160x128.
//
// A vector Asteroids on the same 3D wireframe renderer as 3d_cube: the
// rocks are spinning low-poly wireframe asteroids drawn with the rotation
// matrices + line renderer, bullets sweep the screen, and the ship shoots
// automatically along its aim.
//
// Controls: tap pad (GPIO 0) / BOOT (GPIO 9) = rotate the ship 45°, hold =
// thrust in the facing direction. Firing is automatic every ~380 ms. Rocks
// split when hit (big -> 2 medium -> 2 small), waves grow, 3 lives; a rock
// touching the ship costs a life. WS2812 flashes red on the hit and green on
// a cleared wave.
//
// Renders in the SIGNAL/VHS theme: near-black deck, white ink vectors and
// light-grey rules, magenta danger/explosions and a signal-green score, with
// a twinkling starfield, a WAVE banner, a game-over card and a per-element
// VHS glitch on every frame. Reuses 3d_cube's rotation/projection primitives,
// the pomodoro touch calibration and the DM-01 intro. No WiFi needed.
//
// Serial is kept non-blocking (setTxTimeoutMs(0)): with the USB CDC connected
// but the host not draining, a debug log would otherwise stall the loop for
// up to 2s (20 x 100ms TX timeout) — that was the periodic freeze.

#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>
#include <Adafruit_NeoPixel.h>
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

#define WIDTH  160
#define HEIGHT 128

typedef struct { float x, y, z; } Vec3;

// entity types declared early: function prototypes reference them
#define MAX_ROCKS 14
#define ROCK_VERTS 8
#define ROCK_EDGES 12
#define MAX_BULLETS 6
#define MAX_PARTS 18

struct Rock {
  float x, y, vx, vy, ang, av;
  int size;                 // 2 big, 1 medium, 0 small
  bool alive;
  float scale;
  float radius;
  Vec3 mv[ROCK_VERTS];
};
struct Bullet { float x, y, vx, vy; unsigned long born; bool alive; };
struct Part { float x, y, vx, vy; unsigned long until; };

// static starfield — gives the black deck depth without stealing attention
#define MAX_STARS 30
struct Star { int16_t x, y; uint8_t ph; };

static uint16_t fb[WIDTH * HEIGHT];
#define FPIX(x,y,c) if((x)>=0&&(x)<WIDTH&&(y)>=0&&(y)<HEIGHT) fb[(y)*WIDTH+(x)]=(c)

void fbClear(uint16_t c) { for (int i = 0; i < WIDTH * HEIGHT; i++) fb[i] = c; }
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
uint16_t accentGN()   { return rgb(  0, 230, 118); } // #00E676 signal green (up)

#include "Dm01Intro.h"

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

// ── VHS filter: slow chroma ripple + per-element glitch hits. Each UI band
//    tears on its own 6s slots and its rip rows are washed with cycling
//    primaries (yellow/cyan/green/magenta/red/blue). Only a single row is
//    buffered (the taps read horizontal neighbours), keeping RAM tiny.
//    This is the frame's hot loop: the row loop is integer-only (LUT ripple
//    instead of soft-float sinf — the C3 has no FPU), taps are pre-clamped
//    via a padded row, nearest chroma taps (the deck is solid, no dot grid
//    to stripe), pure-deck rows are skipped, and hit/non-hit rows take
//    separate paths (untouched rows skip boost, clamp and wash entirely).
//    The per-pixel brightness flicker is gone too — it was the slowest part
//    of the old animation for almost no visible effect. 12ms -> 3.5ms. ──
// padded source row: the taps can reach PAD px past either edge, so the hot
// loop needs no per-pixel clamping (the pad repeats the edge pixels)
#define VHS_PAD 20   // covers the widest chroma tap (li/ri up to ~10 during a hit)
static uint16_t vhsRow[WIDTH + 2 * VHS_PAD];

// 8-bit sine table — the C3 has no FPU, so sinf() in the row loop is soft-float
static int8_t sinTab[256];
static void sinInit() {
  for (int i = 0; i < 256; i++) sinTab[i] = (int8_t)lroundf(sinf(i * 2.0f * PI / 256.0f) * 127.0f);
}

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
  // ripple phase as LUT indices (was sinf in the row loop — soft-float on C3)
  const int phL = (int)((t * 75u) >> 10);
  const int phR = (int)((t * 98u) >> 10);

  // per-element bands: header, accent strip, upper playfield, lower
  // playfield, footer/lives
  static const int bandY0[5] = {   1,  13,  19,  68, 114 };
  static const int bandY1[5] = {  13,  19,  68, 114, 128 };
  int bDx[5], bBoost[5]; uint8_t bPal[5]; bool bHit[5];
  for (int i = 0; i < 5; i++) {
    bHit[i] = elemHit(i, t, bDx[i], bPal[i]);
    bDx[i]  = bHit[i] ? bDx[i] : 0;
    bBoost[i] = bHit[i] ? 4 : 0;
  }

  const uint16_t bg = panelBG();   // solid deck — a pure deck row filters to itself

  for (int y = 0; y < HEIGHT; y++) {
    int bi = -1;
    for (int i = 0; i < 5; i++) if (y >= bandY0[i] && y < bandY1[i]) { bi = i; break; }
    if (bi < 0) continue;
    const bool hit = bHit[bi];
    const int xOff = bDx[bi], boost = bBoost[bi];
    if (!hit) {                    // empty row: nothing to shift or fringe
      const uint16_t* src = &fb[y * WIDTH];
      int x = 0;
      while (x < WIDTH && src[x] == bg) x++;
      if (x == WIDTH) continue;
    }
    // 1.4 + 1.3*sin(y*0.085 + ph), 1.8 + 2.1*sin(y*0.061 + 1.7 + ph*1.3) in 8.8
    int lsh = 358 + ((333 * sinTab[(((y * 3463) >> 10) + phL) & 255]) >> 7);
    int rsh = 461 + ((538 * sinTab[(((y * 2545) >> 10) + 69 + phR) & 255]) >> 7);
    if (lsh < 0) lsh = 0; if (rsh < 0) rsh = 0;
    const int li = lsh >> 8, ri = rsh >> 8;      // tap distances
    // rip colour: shifts every 60ms and every 8 rows inside the hit
    uint16_t pc = vhsPal((uint8_t)(bPal[bi] + t / 60u + (y >> 3)));
    const int pr = (pc >> 11) & 0x1F, pg = (pc >> 5) & 0x3F, pb = pc & 0x1F;
    memcpy(&vhsRow[VHS_PAD], &fb[y * WIDTH], WIDTH * sizeof(uint16_t));
    for (int k = 0; k < VHS_PAD; k++) {
      vhsRow[k] = vhsRow[VHS_PAD];
      vhsRow[VHS_PAD + WIDTH + k] = vhsRow[VHS_PAD + WIDTH - 1];
    }
    const uint16_t* __restrict s = &vhsRow[VHS_PAD];
    uint16_t* __restrict d = &fb[y * WIDTH];

    // nearest chroma taps — the deck is solid here (no dot grid to stripe),
    // so no sub-pixel blend is needed
#define VHS_CHROMA()                                                          \
    int cx = x + xOff;                                                        \
    uint16_t cC = s[cx];                                                      \
    int rSrc = (s[cx - li] >> 11) & 0x1F;                                     \
    int bSrc = s[cx + ri] & 0x1F;                                             \
    int r = (3 * rSrc + ((cC >> 11) & 0x1F)) >> 2;   /* 75% shift = stronger fringe */ \
    int g = (cC >> 5) & 0x3F;                                                 \
    int b = (3 * bSrc + (cC & 0x1F)) >> 2;

    if (hit) {                                             // wash the band with the palette
      for (int x = 0; x < WIDTH; x++) {
        VHS_CHROMA();
        r += boost; g += boost; b += boost;
        if (r > 31) r = 31;
        if (g > 63) g = 63;
        if (b > 31) b = 31;
        int w = (g > 22) ? 3 : 2;                          // 75% on ink, 50% on the deck
        r = (r * (4 - w) + pr * w) >> 2;
        g = (g * (4 - w) + pg * w) >> 2;
        b = (b * (4 - w) + pb * w) >> 2;
        d[x] = (uint16_t)((r << 11) | (g << 5) | b);
      }
    } else {                                               // untouched rows: no boost/clamp
      for (int x = 0; x < WIDTH; x++) {
        VHS_CHROMA();
        d[x] = (uint16_t)((r << 11) | (g << 5) | b);
      }
    }
#undef VHS_CHROMA
  }
}

void vhsFlush() { vhsApply(); fbFlush(); }

// ── Rotation (3d_cube engine) ──
float rot[9];
void compRot(float a, float b, float c) {
  float cx = cosf(a), sx = sinf(a), cy = cosf(b), sy = sinf(b), cz = cosf(c), sz = sinf(c);
  rot[0] = cy * cz + sx * sy * sz; rot[1] = -cx * sz; rot[2] = sy * cz - sx * cy * sz;
  rot[3] = cy * sz - sx * sy * cz; rot[4] = cx * cz;  rot[5] = sy * sz + sx * cy * cz;
  rot[6] = -cx * sy;               rot[7] = sx;       rot[8] = cx * cy;
}
void aRot(Vec3* p) {
  float x = p->x * rot[0] + p->y * rot[1] + p->z * rot[2];
  float y = p->x * rot[3] + p->y * rot[4] + p->z * rot[5];
  float z = p->x * rot[6] + p->y * rot[7] + p->z * rot[8];
  p->x = x; p->y = y; p->z = z;
}

// ── Game state ──
Rock rocks[MAX_ROCKS];
Bullet bullets[MAX_BULLETS];
Part parts[MAX_PARTS];
Star stars[MAX_STARS];
int score = 0, lives = 3, wave = 1;
bool playing = true;
unsigned long invulnUntil = 0, lastFire = 0;
unsigned long waveBannerUntil = 0;
int waveBannerN = 0;
float shipX = WIDTH / 2, shipY = HEIGHT - 24, shipVX = 0, shipVY = 0;
int aim = 0;                    // 0..7, 0 = up
bool thrusting = false;

static const int8_t ROCK_EDGES_L[ROCK_EDGES][2] = {
  {0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},{0,4},{1,5},{2,6},{3,7}
};

int idleLevel = HIGH;
bool wasPressed = false;
unsigned long pressStartMs = 0;
bool holdFired = false;
#define HOLD_MS 1200

float aimAngle() { return aim * (PI / 4.0f) - PI / 2.0f; }

void makeRock(Rock* r, int size) {
  r->size = size;
  r->scale = (size == 2) ? 15.0f : (size == 1 ? 9.0f : 5.5f);
  r->radius = r->scale * 0.95f;
  for (int i = 0; i < ROCK_VERTS; i++) {
    float a = (i & 3) * (PI / 2.0f) + ((esp_random() % 100) - 50) * 0.008f;
    float rr = 0.72f + (esp_random() % 45) * 0.01f;
    r->mv[i] = {cosf(a) * rr, sinf(a) * rr, (i < 4) ? -0.55f : 0.55f};
  }
  r->ang = esp_random() % 314;
  r->av = ((int)(esp_random() % 100) - 50) * 0.02f;
  r->alive = true;
}

void spawnRock(int size, float x, float y) {
  for (int i = 0; i < MAX_ROCKS; i++) {
    if (rocks[i].alive) continue;
    makeRock(&rocks[i], size);
    rocks[i].x = x; rocks[i].y = y;
    float a = (esp_random() % 628) * 0.01f;
    float sp = 12.0f + (esp_random() % 18) + wave * 1.5f;
    rocks[i].vx = cosf(a) * sp;
    rocks[i].vy = sinf(a) * sp;
    return;
  }
}

void spawnWave() {
  int n = 3 + wave; if (n > 7) n = 7;
  for (int i = 0; i < n; i++) {
    float x, y;
    do {
      x = 16 + esp_random() % (WIDTH - 32);
      y = 22 + esp_random() % 60;
    } while (fabsf(x - shipX) < 40 && fabsf(y - shipY) < 40);
    spawnRock(2, x, y);
  }
  waveBannerN = wave;
  waveBannerUntil = millis() + 1600;
}

void burst(float x, float y, int n, uint16_t color) {
  for (int i = 0; i < n; i++) {
    for (int k = 0; k < MAX_PARTS; k++) {
      if (millis() < parts[k].until) continue;
      float a = (esp_random() % 628) * 0.01f;
      float sp = 25.0f + esp_random() % 45;
      parts[k].x = x; parts[k].y = y;
      parts[k].vx = cosf(a) * sp;
      parts[k].vy = sinf(a) * sp;
      parts[k].until = millis() + 350 + esp_random() % 250;
      break;
    }
  }
}

void resetShip() {
  shipX = WIDTH / 2; shipY = HEIGHT - 24;
  shipVX = shipVY = 0;
  invulnUntil = millis() + 1800;
}

void newGame() {
  for (int i = 0; i < MAX_ROCKS; i++) rocks[i].alive = false;
  for (int i = 0; i < MAX_BULLETS; i++) bullets[i].alive = false;
  for (int i = 0; i < MAX_PARTS; i++) parts[i].until = 0;
  score = 0; lives = 3; wave = 1;
  playing = true;
  aim = 0;
  resetShip();
  spawnWave();
  Serial.println("[ast] new game");
}

void fireBullet() {
  float a = aimAngle();
  for (int i = 0; i < MAX_BULLETS; i++) {
    if (bullets[i].alive) continue;
    bullets[i].x = shipX + cosf(a) * 9.0f;
    bullets[i].y = shipY + sinf(a) * 9.0f;
    bullets[i].vx = cosf(a) * 170.0f;
    bullets[i].vy = sinf(a) * 170.0f;
    bullets[i].born = millis();
    bullets[i].alive = true;
    return;
  }
}

void killRock(int i) {
  Rock& r = rocks[i];
  int pts = (r.size == 2) ? 20 : (r.size == 1 ? 50 : 100);
  score += pts;
  burst(r.x, r.y, (r.size == 2) ? 8 : 5, accent());
  if (r.size > 0) {
    float bx = r.x, by = r.y;
    r.alive = false;
    spawnRock(r.size - 1, bx, by);
    spawnRock(r.size - 1, bx, by);
  } else {
    r.alive = false;
  }
  Serial.printf("[ast] hit %d pts=%d score=%d\n", r.size, pts, score);
}

void loseLife() {
  lives--;
  burst(shipX, shipY, 10, accent());
  Serial.printf("[ast] hit ship, lives=%d\n", lives);
  if (lives <= 0) {
    playing = false;
    Serial.printf("[ast] game over score=%d\n", score);
  } else {
    resetShip();
  }
}

void updateGame(float dt) {
  unsigned long now = millis();

  // ship: thrust + friction + bounds
  if (thrusting && playing) {
    float a = aimAngle();
    shipVX += cosf(a) * 150.0f * dt;
    shipVY += sinf(a) * 150.0f * dt;
  }
  // drag is per-second (matches 0.985/frame at ~54fps), so the ship's top
  // speed no longer depends on the frame rate
  float drag = expf(-0.816f * dt);
  shipVX *= drag;
  shipVY *= drag;
  shipX += shipVX * dt;
  shipY += shipVY * dt;
  if (shipX < 8) { shipX = 8; shipVX = 0; }
  if (shipX > WIDTH - 8) { shipX = WIDTH - 8; shipVX = 0; }
  if (shipY < 22) { shipY = 22; shipVY = 0; }
  if (shipY > HEIGHT - 10) { shipY = HEIGHT - 10; shipVY = 0; }

  // auto fire
  if (playing && now - lastFire > 380) { lastFire = now; fireBullet(); }

  // bullets
  for (int i = 0; i < MAX_BULLETS; i++) {
    Bullet& b = bullets[i];
    if (!b.alive) continue;
    b.x += b.vx * dt;
    b.y += b.vy * dt;
    if (b.x < -4 || b.x > WIDTH + 4 || b.y < -4 || b.y > HEIGHT + 4 || now - b.born > 1600) b.alive = false;
  }

  // rocks: drift, wrap, spin
  int aliveRocks = 0;
  for (int i = 0; i < MAX_ROCKS; i++) {
    Rock& r = rocks[i];
    if (!r.alive) continue;
    aliveRocks++;
    r.x += r.vx * dt;
    r.y += r.vy * dt;
    r.ang += r.av * dt;
    float m = r.radius;
    if (r.x < -m) r.x += WIDTH + 2 * m;
    if (r.x > WIDTH + m) r.x -= WIDTH + 2 * m;
    if (r.y < -m) r.y += HEIGHT + 2 * m;
    if (r.y > HEIGHT + m) r.y -= HEIGHT + 2 * m;
  }

  // bullet <-> rock
  for (int i = 0; i < MAX_BULLETS; i++) {
    if (!bullets[i].alive) continue;
    for (int k = 0; k < MAX_ROCKS; k++) {
      if (!rocks[k].alive) continue;
      float dx = bullets[i].x - rocks[k].x;
      float dy = bullets[i].y - rocks[k].y;
      if (dx * dx + dy * dy < rocks[k].radius * rocks[k].radius) {
        bullets[i].alive = false;
        killRock(k);
        break;
      }
    }
  }

  // rock <-> ship
  if (playing && now > invulnUntil) {
    for (int k = 0; k < MAX_ROCKS; k++) {
      if (!rocks[k].alive) continue;
      float dx = shipX - rocks[k].x;
      float dy = shipY - rocks[k].y;
      if (dx * dx + dy * dy < (rocks[k].radius + 4.0f) * (rocks[k].radius + 4.0f)) {
        loseLife();
        break;
      }
    }
  }

  // wave cleared
  if (playing && aliveRocks == 0) {
    wave++;
    score += 100;
    Serial.printf("[ast] wave %d\n", wave);
    spawnWave();
  }

  // particles
  for (int i = 0; i < MAX_PARTS; i++) {
    if (now >= parts[i].until) continue;
    parts[i].x += parts[i].vx * dt;
    parts[i].y += parts[i].vy * dt;
  }
}

// ── Touch: tap = rotate 45°, hold = thrust ──
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

void handleTouch() {
  bool pressed = inputPressed();
  unsigned long now = millis();
  thrusting = pressed && playing;   // hold = burn while held
  if (pressed && !wasPressed) {
    pressStartMs = now;
    wasPressed = true;
    holdFired = false;
  } else if (!pressed && wasPressed) {
    unsigned long dur = now - pressStartMs;
    if (!holdFired && dur >= 60 && dur < HOLD_MS) {
      if (playing) {
        aim = (aim + 1) & 7;
        Serial.printf("[btn] aim %d\n", aim);
      } else {
        newGame();
      }
    }
    wasPressed = false;
    holdFired = false;
    delay(20);
  } else if (pressed && wasPressed && !holdFired && (now - pressStartMs >= HOLD_MS)) {
    holdFired = true;   // long hold just keeps thrusting
  }
}

// ── LED ──
void updateLed() {
  static unsigned long last = 0;
  if (millis() - last < 50) return;
  last = millis();
  if (!playing) {
    bool on = (millis() / 300) % 2 == 0;
    pixels.setBrightness(70);
    if (on) pixels.setPixelColor(0, pixels.Color(200, 30, 30));
    else pixels.clear();
  } else if (thrusting) {
    float p = (sinf(millis() * 0.02f) + 1.0f) * 0.5f;
    pixels.setBrightness(60);
    pixels.setPixelColor(0, pixels.Color(255, 120 + p * 80, 0));
  } else {
    pixels.setBrightness(25);
    pixels.setPixelColor(0, pixels.Color(0, 60, 30));
  }
  pixels.show();
}

// ── Render ──
void drawShip() {
  if ((millis() < invulnUntil) && ((millis() / 150) % 2 == 0)) return;
  float a = aimAngle();
  float nx = shipX + cosf(a) * 9, ny = shipY + sinf(a) * 9;
  float lx = shipX + cosf(a + 2.6f) * 8, ly = shipY + sinf(a + 2.6f) * 8;
  float rx = shipX + cosf(a - 2.6f) * 8, ry = shipY + sinf(a - 2.6f) * 8;
  float tx = shipX - cosf(a) * 3, ty = shipY - sinf(a) * 3;
  fbDrawLine((int)nx, (int)ny, (int)lx, (int)ly, panelInk());
  fbDrawLine((int)nx, (int)ny, (int)rx, (int)ry, panelInk());
  fbDrawLine((int)lx, (int)ly, (int)tx, (int)ty, panelInk());
  fbDrawLine((int)rx, (int)ry, (int)tx, (int)ty, panelInk());
  if (thrusting && playing) {
    float fx = shipX - cosf(a) * 5, fy = shipY - sinf(a) * 5;
    float f1x = shipX - cosf(a + 2.4f) * 7, f1y = shipY - sinf(a + 2.4f) * 7;
    float f2x = shipX - cosf(a - 2.4f) * 7, f2y = shipY - sinf(a - 2.4f) * 7;
    bool on = (millis() / 80) % 2 == 0;
    uint16_t c = on ? accentGN() : accentDK();
    fbDrawLine((int)f1x, (int)f1y, (int)(fx + cosf(a) * 4), (int)(fy + sinf(a) * 4), c);
    fbDrawLine((int)f2x, (int)f2y, (int)(fx + cosf(a) * 4), (int)(fy + sinf(a) * 4), c);
  }
}

void drawRock(Rock& r) {
  compRot(r.ang * 0.6f, r.ang, r.ang * 0.35f);
  Vec3 tv[ROCK_VERTS];
  int px[ROCK_VERTS], py[ROCK_VERTS];
  for (int i = 0; i < ROCK_VERTS; i++) {
    tv[i] = r.mv[i];
    aRot(&tv[i]);
    px[i] = (int)(r.x + tv[i].x * r.scale);
    py[i] = (int)(r.y + tv[i].y * r.scale);
  }
  uint16_t c = (r.size == 2) ? panelInk() : (r.size == 1 ? panelEdge() : panelGhost());
  for (int e = 0; e < ROCK_EDGES; e++) {
    int a = ROCK_EDGES_L[e][0], b = ROCK_EDGES_L[e][1];
    fbDrawLine(px[a], py[a], px[b], py[b], c);
  }
}

void initStars() {
  for (int i = 0; i < MAX_STARS; i++) {
    stars[i].x = esp_random() % WIDTH;
    stars[i].y = 22 + esp_random() % (HEIGHT - 22 - 18);
    stars[i].ph = esp_random() % 8;
  }
}

void drawStars(unsigned long t) {
  int blip = (t / 220) & 7;
  for (int i = 0; i < MAX_STARS; i++)
    FPIX(stars[i].x, stars[i].y, (stars[i].ph == blip) ? panelHI() : panelGhost());
}

void drawGameOver(unsigned long t) {
  fbFillRect(20, 36, WIDTH - 40, 56, panelBG());
  fbFillRect(20, 36, WIDTH - 40, 1, panelEdge());       // top rule
  fbFillRect(20, 91, WIDTH - 40, 1, panelEdge());       // bottom rule
  fbFillRect(20, 36, 1, 56, panelEdge());               // left rule
  fbFillRect(WIDTH - 21, 36, 1, 56, panelEdge());       // right rule
  fbFillRect(20, 36, WIDTH - 40, 2, accent());          // magenta cap
  fbText11((WIDTH - textW11("GAME OVER")) / 2, 46, "GAME OVER", panelInk());
  char sbuf[8];
  snprintf(sbuf, sizeof(sbuf), "%04d", score);
  fbText11((WIDTH - textW11(sbuf)) / 2, 64, sbuf, accentGN());
  const char* hint = "TAP RETRY";
  fbText((WIDTH - textW57(hint)) / 2, 80, hint, ((t / 400) & 1) ? accent() : accentDK());
}

void render() {
  unsigned long t = millis();
  fbClear(panelBG());

  // header
  fbText(6, 3, "ASTEROIDS", panelInk());
  char wbuf[8];
  snprintf(wbuf, sizeof(wbuf), "W%d", wave);
  fbText((WIDTH - textW57(wbuf)) / 2, 3, wbuf, accentGN());
  char sbuf[8];
  snprintf(sbuf, sizeof(sbuf), "%04d", score);
  fbText11(WIDTH - 6 - textW11(sbuf), 1, sbuf, accentGN());
  fbHLine(4, 14, WIDTH - 8, accentDK());
  fbFillRect(4, 15, WIDTH - 8, 3, accent());

  drawStars(t);

  // particles
  for (int i = 0; i < MAX_PARTS; i++) {
    if (t >= parts[i].until) continue;
    fbDrawLine((int)parts[i].x, (int)parts[i].y,
               (int)(parts[i].x - parts[i].vx * 0.04f),
               (int)(parts[i].y - parts[i].vy * 0.04f), accent());
  }

  for (int i = 0; i < MAX_ROCKS; i++) if (rocks[i].alive) drawRock(rocks[i]);

  for (int i = 0; i < MAX_BULLETS; i++) {
    if (!bullets[i].alive) continue;
    fbDrawLine((int)bullets[i].x, (int)bullets[i].y,
               (int)(bullets[i].x - bullets[i].vx * 0.02f),
               (int)(bullets[i].y - bullets[i].vy * 0.02f), panelInk());
  }

  drawShip();

  // lives (mini ship glyphs) + footer; last life blinks magenta
  uint16_t lc = (lives == 1 && ((t / 400) & 1)) ? accent() : panelInk();
  for (int i = 0; i < lives; i++) {
    int x = 6 + i * 9, y = HEIGHT - 7;
    fbDrawLine(x + 3, y, x, y + 5, lc);
    fbDrawLine(x + 3, y, x + 6, y + 5, lc);
    fbDrawLine(x, y + 5, x + 6, y + 5, lc);
  }
  fbText(WIDTH - 6 - textW57("TAP TURN"), HEIGHT - 9, "TAP TURN", panelInk());
  fbText(WIDTH - 6 - textW57("TAP TURN") - 4 - textW57("HOLD THRUST"), HEIGHT - 9, "HOLD THRUST", accentDK());

  // wave banner over the playfield
  if (playing && t < waveBannerUntil) {
    char wb[12];
    snprintf(wb, sizeof(wb), "WAVE %d", waveBannerN);
    int w = textW11(wb), x = (WIDTH - w) / 2;
    fbText11(x, 50, wb, accent());
    fbFillRect(x, 62, w, 1, accentDK());
  }

  if (!playing) drawGameOver(t);

  vhsFlush();
}

void setup() {
  Serial.begin(115200);
#if defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT
  // never let a debug log stall the game: with the CDC connected but the host
  // not draining (no monitor open), a plain write() blocks up to 20x100ms
  Serial.setTxTimeoutMs(0);
#endif
  delay(300);
  Serial.println("\n=== ASTEROIDS VECTOR ===");

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

  calibrateTouch();
  sinInit();
  initStars();
  newGame();
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

  static bool introEnded = false;
  if (!introEnded && !dm01IsActive()) {
    introEnded = true;
    Serial.printf("[intro] done t=%lums\n", millis() - dm01StartMs);
  }

  static unsigned long lastFrame = 0;
  unsigned long now = millis();
  float dt = (now - lastFrame) / 1000.0f;
  lastFrame = now;
  if (dt > 0.06f) dt = 0.06f;

  handleTouch();
  updateGame(dt);
  updateLed();
  render();
  screenshotHandle(fb, WIDTH, HEIGHT);
  delay(1);   // just yield; the frame's own work paces the loop
}