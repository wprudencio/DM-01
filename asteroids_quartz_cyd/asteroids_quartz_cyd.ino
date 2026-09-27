// ─── ASTEROIDS · CYD (ESP32-2432S028R) — SIGNAL/VHS edition ─────────────────
// Port of asteroids/asteroids.ino (ESP32-C3 + ST7735 160x128) to the Cheap
// Yellow Display: ST7789 320x240 + XPT2046 resistive touch. The game world is
// the 2x C3 layout: playfield inset between the header strip and the footer.
//
// A vector Asteroids on the same 3D wireframe renderer as 3d_cube: the
// rocks are spinning low-poly wireframe asteroids drawn with the rotation
// matrices + line renderer, bullets sweep the screen, and the ship shoots
// automatically along its aim.
//
// Controls: tap the touch screen (or BOOT) = rotate the ship 45°, hold = thrust
// in the facing direction. Firing is automatic every ~380 ms. Rocks split when
// hit (big -> 2 medium -> 2 small), waves grow, 3 lives; a rock touching the
// ship costs a life. No NeoPixel on CYD — hits/life/wave flash a frame around
// the playfield instead.
//
// Renders in the SIGNAL/VHS theme: near-black deck, white ink vectors and
// light-grey rules, magenta danger/explosions and a signal-green score, with
// a twinkling starfield, a WAVE banner, a game-over card and a per-element
// VHS glitch on every frame.

#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include "Screenshot.h"
#include <math.h>
#include <string.h>
#include <stdio.h>

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

// ── Entity types declared early: Arduino's auto-generated prototypes
//    reference them before the definitions further down ──
typedef struct { float x, y, z; } Vec3;

#define MAX_ROCKS 14
#define ROCK_VERTS 8
#define ROCK_EDGES 12
#define MAX_BULLETS 6
#define MAX_PARTS 18
#define MAX_STARS 60

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
struct Star { int16_t x, y; uint8_t ph; };

Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);
SPIClass touchSPI(HSPI);

#define WIDTH  320
#define HEIGHT 240

// ─── Layout (2x the 160x128 face) ───
#define HDR_Y     8    // header text (11px raster at scale 2)
#define ACC_Y     38   // dim rule; magenta bar at ACC_Y+1
#define PLAY_TOP  48   // playfield top
#define PLAY_BOT  204  // playfield floor
#define FOOT_Y    212  // footer hints + lives glyphs

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

// ─── SIGNAL palette — near-black deck, magenta + signal green (VHS theme) ───
static uint16_t palBG, palInk, palGhost, palHI, palEdge, palAcc, palAccDK, palAccGN;
void palRefresh() {
  palBG    = rgb( 16,  16,  16); // #101010 deck
  palInk   = rgb(255, 255, 255); // white ink
  palGhost = rgb( 74,  74,  74); // stale digits / dim stars
  palHI    = rgb(170, 170, 170); // twinkle / light grey rule
  palEdge  = rgb(170, 170, 170); // light grey rules + borders
  palAcc   = rgb(236,   0, 140); // #EC008C magenta
  palAccDK = rgb(104,   0,  62); // dimmed magenta
  palAccGN = rgb(  0, 230, 118); // #00E676 signal green
}
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
unsigned long flashUntil = 0;
uint16_t flashCol = 0;
float shipX = WIDTH / 2, shipY = PLAY_BOT - 28, shipVX = 0, shipVY = 0;
int aim = 0;                    // 0..7, 0 = up
bool thrusting = false;

static const int8_t ROCK_EDGES_L[ROCK_EDGES][2] = {
  {0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},{0,4},{1,5},{2,6},{3,7}
};

bool wasPressed = false;
unsigned long pressStartMs = 0;
bool holdFired = false;
#define HOLD_MS 1200

float aimAngle() { return aim * (PI / 4.0f) - PI / 2.0f; }

void flash(uint16_t c, unsigned long ms) { flashCol = c; flashUntil = millis() + ms; }

void makeRock(Rock* r, int size) {
  r->size = size;
  r->scale = (size == 2) ? 30.0f : (size == 1 ? 18.0f : 11.0f);
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
    float sp = 24.0f + (esp_random() % 36) + wave * 3.0f;
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
      x = 32 + esp_random() % (WIDTH - 64);
      y = PLAY_TOP + 12 + esp_random() % 100;
    } while (fabsf(x - shipX) < 80 && fabsf(y - shipY) < 80);
    spawnRock(2, x, y);
  }
  waveBannerN = wave;
  waveBannerUntil = millis() + 1600;
}

void burst(float x, float y, int n, uint16_t color) {
  (void)color;
  for (int i = 0; i < n; i++) {
    for (int k = 0; k < MAX_PARTS; k++) {
      if (millis() < parts[k].until) continue;
      float a = (esp_random() % 628) * 0.01f;
      float sp = 50.0f + esp_random() % 90;
      parts[k].x = x; parts[k].y = y;
      parts[k].vx = cosf(a) * sp;
      parts[k].vy = sinf(a) * sp;
      parts[k].until = millis() + 350 + esp_random() % 250;
      break;
    }
  }
}

void resetShip() {
  shipX = WIDTH / 2; shipY = PLAY_BOT - 28;
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
    bullets[i].x = shipX + cosf(a) * 18.0f;
    bullets[i].y = shipY + sinf(a) * 18.0f;
    bullets[i].vx = cosf(a) * 340.0f;
    bullets[i].vy = sinf(a) * 340.0f;
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
  flash(accent(), 90);
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
  flash(accent(), 400);
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
    shipVX += cosf(a) * 300.0f * dt;
    shipVY += sinf(a) * 300.0f * dt;
  }
  // drag is per-second, so the ship's top speed doesn't depend on the frame rate
  float drag = expf(-0.816f * dt);
  shipVX *= drag;
  shipVY *= drag;
  shipX += shipVX * dt;
  shipY += shipVY * dt;
  if (shipX < 16) { shipX = 16; shipVX = 0; }
  if (shipX > WIDTH - 16) { shipX = WIDTH - 16; shipVX = 0; }
  if (shipY < PLAY_TOP + 8) { shipY = PLAY_TOP + 8; shipVY = 0; }
  if (shipY > PLAY_BOT - 8) { shipY = PLAY_BOT - 8; shipVY = 0; }

  // auto fire
  if (playing && now - lastFire > 380) { lastFire = now; fireBullet(); }

  // bullets
  for (int i = 0; i < MAX_BULLETS; i++) {
    Bullet& b = bullets[i];
    if (!b.alive) continue;
    b.x += b.vx * dt;
    b.y += b.vy * dt;
    if (b.x < -8 || b.x > WIDTH + 8 || b.y < PLAY_TOP - 8 || b.y > PLAY_BOT + 8 ||
        now - b.born > 1600) b.alive = false;
  }

  // rocks: drift, wrap inside the playfield, spin
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
    if (r.y < PLAY_TOP - m) r.y += (PLAY_BOT - PLAY_TOP) + 2 * m;
    if (r.y > PLAY_BOT + m) r.y -= (PLAY_BOT - PLAY_TOP) + 2 * m;
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
      if (dx * dx + dy * dy < (rocks[k].radius + 8.0f) * (rocks[k].radius + 8.0f)) {
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
    flash(panelInk(), 400);
  }

  // particles
  for (int i = 0; i < MAX_PARTS; i++) {
    if (now >= parts[i].until) continue;
    parts[i].x += parts[i].vx * dt;
    parts[i].y += parts[i].vy * dt;
  }
}

// ── Touch: tap = rotate 45°, hold = thrust (BOOT works the same) ──
void touchInit() {
  touchSPI.begin(TOUCH_CLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
  pinMode(TOUCH_CS, OUTPUT);
  digitalWrite(TOUCH_CS, HIGH);
  pinMode(TOUCH_IRQ, INPUT);  // GPIO36 input-only: no internal pullup (external PU on CYD)
  pinMode(BOOT_PIN, INPUT_PULLUP);
}
bool touchPressed() {
  // Debounce: a glitch must persist across 3 loop passes, so line noise can
  // neither rotate the ship nor retry the game.
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
  thrusting = pressed && playing;
}

// ── Render ──
void drawShip() {
  if ((millis() < invulnUntil) && ((millis() / 150) % 2 == 0)) return;
  float a = aimAngle();
  float nx = shipX + cosf(a) * 18, ny = shipY + sinf(a) * 18;
  float lx = shipX + cosf(a + 2.6f) * 16, ly = shipY + sinf(a + 2.6f) * 16;
  float rx = shipX + cosf(a - 2.6f) * 16, ry = shipY + sinf(a - 2.6f) * 16;
  float tx = shipX - cosf(a) * 6, ty = shipY - sinf(a) * 6;
  fbDrawLine((int)nx, (int)ny, (int)lx, (int)ly, panelInk());
  fbDrawLine((int)nx, (int)ny, (int)rx, (int)ry, panelInk());
  fbDrawLine((int)lx, (int)ly, (int)tx, (int)ty, panelInk());
  fbDrawLine((int)rx, (int)ry, (int)tx, (int)ty, panelInk());
  if (thrusting && playing) {
    float fx = shipX - cosf(a) * 10, fy = shipY - sinf(a) * 10;
    float f1x = shipX - cosf(a + 2.4f) * 14, f1y = shipY - sinf(a + 2.4f) * 14;
    float f2x = shipX - cosf(a - 2.4f) * 14, f2y = shipY - sinf(a - 2.4f) * 14;
    bool on = (millis() / 80) % 2 == 0;
    uint16_t c = on ? panelInk() : accentDK();
    fbDrawLine((int)f1x, (int)f1y, (int)(fx + cosf(a) * 8), (int)(fy + sinf(a) * 8), c);
    fbDrawLine((int)f2x, (int)f2y, (int)(fx + cosf(a) * 8), (int)(fy + sinf(a) * 8), c);
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
    stars[i].y = PLAY_TOP + 4 + esp_random() % (PLAY_BOT - PLAY_TOP - 8);
    stars[i].ph = esp_random() % 8;
  }
}

void drawStars(unsigned long t) {
  int blip = (t / 220) & 7;
  for (int i = 0; i < MAX_STARS; i++)
    FPIX(stars[i].x, stars[i].y, (stars[i].ph == blip) ? panelHI() : panelGhost());
}

void drawGameOver(unsigned long t) {
  fbFillRect(60, 78, 200, 92, panelBG());
  fbFillRect(60, 78, 200, 1, panelEdge());       // top rule
  fbFillRect(60, 169, 200, 1, panelEdge());      // bottom rule
  fbFillRect(60, 78, 1, 92, panelEdge());        // left rule
  fbFillRect(259, 78, 1, 92, panelEdge());       // right rule
  fbFillRect(60, 78, 200, 4, accent());          // magenta cap
  fbText((WIDTH - textW57("GAME OVER", 2)) / 2, 92, "GAME OVER", panelInk(), 2);
  char sbuf[8];
  snprintf(sbuf, sizeof(sbuf), "%04d", score);
  fbText((WIDTH - textW57(sbuf, 2)) / 2, 124, sbuf, panelInk(), 2);
  const char* hint = "TAP RETRY";
  fbText((WIDTH - textW57(hint, 1)) / 2, 152, hint, ((t / 400) & 1) ? accent() : accentDK(), 1);
}

void drawFrame(unsigned long t) {
  fbClear(panelBG());

  // header
  fbText(12, HDR_Y, "ASTEROIDS", panelInk(), 2);
  char wbuf[8];
  snprintf(wbuf, sizeof(wbuf), "W%d", wave);
  fbText((WIDTH - textW57(wbuf, 2)) / 2, HDR_Y, wbuf, panelInk(), 2);
  char sbuf[8];
  snprintf(sbuf, sizeof(sbuf), "%04d", score);
  fbText(WIDTH - 12 - textW57(sbuf, 2), HDR_Y, sbuf, panelInk(), 2);
  fbHLine(8, ACC_Y, WIDTH - 16, accentDK());
  fbFillRect(8, ACC_Y + 1, WIDTH - 16, 6, accent());

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
    int x = 12 + i * 24, y = FOOT_Y;
    fbDrawLine(x + 6, y, x, y + 10, lc);
    fbDrawLine(x + 6, y, x + 12, y + 10, lc);
    fbDrawLine(x, y + 10, x + 12, y + 10, lc);
  }
  const char* h1 = "TAP TURN";
  const char* h2 = "HOLD THRUST";
  int w1 = textW57(h1, 1), w2 = textW57(h2, 1);
  fbText(WIDTH - 12 - w2, FOOT_Y, h2, accentDK(), 1);
  fbText(WIDTH - 12 - w2 - 16 - w1, FOOT_Y, h1, panelInk(), 1);

  // wave banner over the playfield
  if (playing && t < waveBannerUntil) {
    char wb[12];
    snprintf(wb, sizeof(wb), "WAVE %d", waveBannerN);
    int w = textW57(wb, 2), x = (WIDTH - w) / 2;
    fbText(x, 104, wb, accent(), 2);
    fbFillRect(x, 130, w, 2, accentDK());
  }

  if (!playing) drawGameOver(t);

  // hit / life / wave feedback (CYD has no NeoPixel)
  if (t < flashUntil) {
    fbFillRect(0, PLAY_TOP, WIDTH, 2, flashCol);
    fbFillRect(0, PLAY_BOT - 2, WIDTH, 2, flashCol);
    fbFillRect(0, PLAY_TOP, 2, PLAY_BOT - PLAY_TOP, flashCol);
    fbFillRect(WIDTH - 2, PLAY_TOP, 2, PLAY_BOT - PLAY_TOP, flashCol);
  }
}

void render(unsigned long t) {
  
  for (fbTop = 0; fbTop < HEIGHT; fbTop += FB_H) {
    if (introMode) dm01Draw(dm01Elapsed()); else drawFrame(t);
    int h = FB_H;
    if (fbTop + h > HEIGHT) h = HEIGHT - fbTop;
    if (!screenshotStripSend(shot, fb, fbTop, h)) fbFlush();
  }
}

// ─── Setup / loop ───
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
  palRefresh();

  touchInit();
  initStars();
  newGame();
  Serial.println("[ast] CYD SIGNAL edition boot");
  dm01Start();
}

void loop() {
  unsigned long t = millis();
  screenshotStripPoll(shot, WIDTH, HEIGHT);
  if (dm01Tick(inputPressed())) {
    introMode = true;
    render(t);
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
  updateGame(dt);
  render(t);

  // pace to ~60 FPS: the frame itself costs ~45ms, so this rarely waits
  static uint32_t nextFrame = 0;
  uint32_t ms = millis();
  if ((int32_t)(nextFrame - ms) > 0) delay(nextFrame - ms);
  nextFrame += 16;
  if ((int32_t)(nextFrame - ms) < -100) nextFrame = ms + 16;
}
