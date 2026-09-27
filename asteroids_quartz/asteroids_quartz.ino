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
// light-grey rules, red danger/explosions and a signal-green score, with
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

// ── QUARTZ palette — light LCD only, no state switching ──
// ── QUARTZ palette — light LCD only, no state switching ──
uint16_t panelBG()    { return rgb(148, 158, 130); } // light LCD green-grey
uint16_t panelInk()   { return rgb( 28,  34,  24); } // dark ink
uint16_t panelGhost() { return rgb(138, 148, 120); } // unlit segments / dot grid
uint16_t panelHI()    { return rgb(178, 186, 160); } // light inner rule
uint16_t panelEdge()  { return rgb( 96, 104,  82); } // dark olive rules
uint16_t accent()     { return rgb(200,  40,  40); } // alarm red
uint16_t accentDK()   { return rgb(120,  24,  24); } // dark red

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
    uint16_t c = on ? panelInk() : accentDK();
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
  fbFillRect(20, 36, WIDTH - 40, 2, accent());          // red cap
  fbText11((WIDTH - textW11("GAME OVER")) / 2, 46, "GAME OVER", panelInk());
  char sbuf[8];
  snprintf(sbuf, sizeof(sbuf), "%04d", score);
  fbText11((WIDTH - textW11(sbuf)) / 2, 64, sbuf, panelInk());
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
  fbText((WIDTH - textW57(wbuf)) / 2, 3, wbuf, panelInk());
  char sbuf[8];
  snprintf(sbuf, sizeof(sbuf), "%04d", score);
  fbText11(WIDTH - 6 - textW11(sbuf), 1, sbuf, panelInk());
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

  // lives (mini ship glyphs) + footer; last life blinks red
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

  fbFlush();
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