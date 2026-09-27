// Flappy Bird — ESP32-C3 + ST7735 160x128, QUARTZ LCD style.
// GPIO 0 button = flap / start / retry. WS2812 on GPIO 10 = state feedback.
// High score persisted in NVS. Serial 'F' simulates a flap (screenshots/tests).
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>
#include <Adafruit_NeoPixel.h>
#include <Preferences.h>
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

// ── 4x5 Font (0-9, A-Z, !, :) ──
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

static inline const uint8_t* glyph45(char ch){
  if(ch>='0'&&ch<='9') return gDig45[ch-'0'];
  if(ch>='A'&&ch<='Z') return gLet45[ch-'A'];
  if(ch=='!') return gExcl45;
  if(ch==':') return gColon45;
  return gLet45[0];
}
void fbDrawChar57(int x,int y,char ch,uint16_t c,int scale){
  if(ch==' ') return;
  const uint8_t* g=glyph45(ch);
  for(int row=0;row<5;row++){
    uint8_t bits=g[row];
    for(int col=0;col<4;col++)
      if(bits&(0b1000>>col)) fbFillRect(x+col*scale,y+row*scale,scale,scale,c);
  }
}
void fbText57(int x,int y,const char* s,uint16_t c,int scale){
  while(*s){ fbDrawChar57(x,y,*s,c,scale); x+=5*scale; s++; }
}
int textW57(const char* s,int scale){ int n=0; while(*s++)n++; return n?(n*5*scale-scale):0; }

// ── 7-seg digits with ghost segments (QUARTZ LCD) ──
static const uint8_t segBits[10] = {
  0x3F, 0x06, 0x5B, 0x4F, 0x66,
  0x6D, 0x7D, 0x07, 0x7F, 0x6F
};
void sevenSegSeg(int x,int y,int Wc,int Hc,int T,int seg,uint16_t c){
  int vH = (Hc - 3*T + 1)/2;
  int midT = (Hc - T)/2;
  switch(seg){
    case 0: fbFillRect(x, y, Wc, T, c); return;                // a (top)
    case 1: fbFillRect(x+Wc-T, y+T, T, vH, c); return;         // b (top-right)
    case 2: fbFillRect(x+Wc-T, y+midT+T, T, vH, c); return;    // c (bottom-right)
    case 3: fbFillRect(x, y+Hc-T, Wc, T, c); return;           // d (bottom)
    case 4: fbFillRect(x, y+midT+T, T, vH, c); return;         // e (bottom-left)
    case 5: fbFillRect(x, y+T, T, vH, c); return;              // f (top-left)
    case 6: fbFillRect(x, y+midT, Wc, T, c); return;           // g (middle)
  }
}
void sevenSegDigit(int x,int y,int Wc,int Hc,int T,uint8_t d,uint16_t lit,uint16_t ghost){
  for(int s=0;s<7;s++) sevenSegSeg(x,y,Wc,Hc,T,s,ghost);
  if(d>9) return;
  uint8_t m = segBits[d];
  for(int s=0;s<7;s++) if(m & (1<<s)) sevenSegSeg(x,y,Wc,Hc,T,s,lit);
}
void draw7SegNumber(int cx,int y,int value,int Wc,int Hc,int T,int gap,uint16_t lit,uint16_t ghost){
  int n = value>=100?3:(value>=10?2:1);
  int totalW = n*Wc + (n-1)*gap;
  int x = cx - totalW/2;
  int div = (n==3)?100:((n==2)?10:1);
  for(int i=0;i<n;i++){
    sevenSegDigit(x,y,Wc,Hc,T,(value/div)%10,lit,ghost);
    div/=10; x += Wc+gap;
  }
}

// ── Palette (single light LCD, never inverted) ──
uint16_t C_BG, C_INK, C_GHOST, C_HI, C_EDGE, C_RED, C_REDDK;
void initColors(){
  C_BG    = tft.color565(148,158,130);
  C_INK   = tft.color565(28,34,24);
  C_GHOST = tft.color565(138,148,120);
  C_HI    = tft.color565(178,186,160);
  C_EDGE  = tft.color565(96,104,82);
  C_RED   = tft.color565(200,40,40);
  C_REDDK = tft.color565(120,24,24);
}

enum State : uint8_t { S_READY, S_PLAY, S_DYING, S_OVER };
State state = S_READY;

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
void pipeSection(int x,int y,int w,int h,uint16_t fill,bool sheen){
  if(h <= 0 || w <= 0) return;
  fbFillRect(x, y, w, h, fill);
  fbFillRect(x, y, w, LINE, C_INK);
  if(h > 2*LINE) fbFillRect(x, y+h-LINE, w, LINE, C_INK);
  fbFillRect(x, y, LINE, h, C_INK);
  fbFillRect(x+w-LINE, y, LINE, h, C_INK);
  if(sheen && h > 6*LINE) fbFillRect(x+3*LINE, y+2*LINE, 2*LINE, h-4*LINE, C_GHOST);
}
void drawPipe(float px,int gapY,int gap){
  int x = (int)px;
  int topB = gapY - gap/2;
  int botT = gapY + gap/2;
  int capX = x - (CAP_W - PIPE_W)/2;
  pipeSection(x, 0, PIPE_W, topB - CAP_H, C_EDGE, true);
  pipeSection(capX, topB - CAP_H, CAP_W, CAP_H, C_EDGE, false);
  pipeSection(x, botT + CAP_H, PIPE_W, HORIZON - (botT + CAP_H), C_EDGE, true);
  pipeSection(capX, botT, CAP_W, CAP_H, C_EDGE, false);
}

void drawGround(){
  fbFillRect(0, HORIZON, WIDTH, HEIGHT - HORIZON, C_HI);
  fbFillRect(0, HORIZON, WIDTH, 2, C_EDGE);
  int off = ((int)scrollDist) % 12;
  for(int x = -12 - off; x < WIDTH; x += 12){
    fbFillRect(x + 3, HORIZON + 6, 7, 2, C_GHOST);
    fbFillRect(x + 9, HORIZON + 11, 7, 2, C_GHOST);
  }
}

void drawBird(int x,int y,bool wingUp){
  fbFillCircle(x, y, BIRD_R, C_INK);
  if(wingUp) fbFillCircle(x-2, y-2, 3, C_GHOST);
  else       fbFillCircle(x-2, y+2, 3, C_GHOST);
  fbFillCircle(x+2, y-2, 2, C_HI);
  fbFillCircle(x+3, y-2, 1, C_INK);
  fbFillRect(x+3, y, 4, 3, C_RED);
}

void drawScoreModule(int cx,int y,int value){
  const int Wc=8, Hc=12, T=2, gap=3, padX=4, padY=3;
  int n = value>=100?3:(value>=10?2:1);
  int totalW = n*Wc + (n-1)*gap;
  int boxW = totalW + 2*padX, boxH = Hc + 2*padY;
  int bx = cx - boxW/2;
  fbFillRect(bx, y, boxW, boxH, C_HI);
  fbRect(bx, y, boxW, boxH, C_EDGE);
  draw7SegNumber(cx, y+padY, value, Wc, Hc, T, gap, C_INK, C_GHOST);
}

void drawReady(unsigned long t){
  const char* title = "FLAPPY";
  fbText57((WIDTH-textW57(title,3))/2, 14, title, C_INK, 3);
  fbHLine(36, 34, 88, C_REDDK);
  if((t/500)%2==0){
    const char* s = "TAP TO FLAP";
    fbText57((WIDTH-textW57(s,1))/2, 42, s, C_INK, 1);
  }
  char buf[16]; snprintf(buf,sizeof(buf),"HI %d",hiScore);
  fbText57((WIDTH-textW57(buf,2))/2, 54, buf, C_INK, 2);
}

void drawOver(unsigned long t){
  fbFillRect(16, 12, 128, 96, C_BG);
  fbRect(16, 12, 128, 96, C_EDGE);
  fbRect(17, 13, 126, 94, C_HI);
  const char* go = "GAME OVER";
  fbText57((WIDTH-textW57(go,2))/2, 20, go, C_RED, 2);
  draw7SegNumber(WIDTH/2, 38, score, 12, 20, 3, 4, C_INK, C_GHOST);
  char buf[16]; snprintf(buf,sizeof(buf),"HI %d",hiScore);
  fbText57((WIDTH-textW57(buf,1))/2, 64, buf, C_INK, 1);
  if((t/500)%2==0){
    const char* s = "TAP TO RETRY";
    fbText57((WIDTH-textW57(s,1))/2, 82, s, C_REDDK, 1);
  }
}

void render(unsigned long t){
  fbClear(C_BG);
  if(state != S_READY) for(int i=0;i<nPipes;i++) drawPipe(pipes[i].x, pipes[i].gapY, pipes[i].gap);
  drawGround();
  bool wingUp;
  if(state == S_READY) wingUp = ((t/220)%2==0);
  else if(state == S_PLAY) wingUp = ((t - lastFlapMs) < 130 || birdVy < 0);
  else wingUp = false;
  drawBird(BIRD_X, (int)birdY, wingUp);
  if(state == S_PLAY || state == S_DYING) drawScoreModule(WIDTH/2, 3, score);
  if(state == S_READY) drawReady(t);
  if(state == S_OVER) drawOver(t);
  fbFlush();
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

  initColors();
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
