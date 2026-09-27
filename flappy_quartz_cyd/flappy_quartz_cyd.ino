// Flappy Bird — CYD (ESP32-2432S028R) version.
// Port of flappy/flappy.ino (ESP32-C3 + ST7735 160x128 + button) to the
// Cheap Yellow Display: ST7789 320x240 + XPT2046 resistive touch.
//
// Controls: tap anywhere on the touch screen = flap / start / retry.
// No NeoPixel on CYD — state feedback is on-screen + serial only.
// High score persisted in NVS. Serial 'F' simulates a flap (screenshots/tests).
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <Preferences.h>
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

// ── 3x5 Font (same glyphs as pomodoro_cyd.ino) ──
const uint8_t digitBits[10][5] = {
  {0b111,0b101,0b101,0b101,0b111},{0b010,0b110,0b010,0b010,0b111},
  {0b111,0b001,0b111,0b100,0b111},{0b111,0b001,0b111,0b001,0b111},
  {0b101,0b101,0b111,0b001,0b001},{0b111,0b100,0b111,0b001,0b111},
  {0b111,0b100,0b111,0b101,0b111},{0b111,0b001,0b001,0b001,0b001},
  {0b111,0b101,0b111,0b101,0b111},{0b111,0b101,0b111,0b001,0b111},
};
const uint8_t letterBits[26][5] = {
  {0b010,0b101,0b111,0b101,0b101},{0b110,0b101,0b110,0b101,0b110},
  {0b011,0b100,0b100,0b100,0b011},{0b110,0b101,0b101,0b101,0b110},
  {0b111,0b100,0b110,0b100,0b111},{0b111,0b100,0b110,0b100,0b100},
  {0b011,0b100,0b101,0b101,0b011},{0b101,0b101,0b111,0b101,0b101},
  {0b111,0b010,0b010,0b010,0b111},{0b001,0b001,0b001,0b101,0b010},
  {0b101,0b110,0b100,0b110,0b101},{0b100,0b100,0b100,0b100,0b111},
  {0b101,0b111,0b111,0b101,0b101},{0b101,0b111,0b111,0b111,0b101},
  {0b010,0b101,0b101,0b101,0b010},{0b110,0b101,0b110,0b100,0b100},
  {0b010,0b101,0b101,0b110,0b011},{0b110,0b101,0b110,0b101,0b101},
  {0b011,0b100,0b010,0b001,0b110},{0b111,0b010,0b010,0b010,0b010},
  {0b101,0b101,0b101,0b101,0b010},{0b101,0b101,0b101,0b010,0b010},
  {0b101,0b101,0b111,0b111,0b101},{0b101,0b101,0b010,0b101,0b101},
  {0b101,0b101,0b010,0b010,0b010},{0b111,0b001,0b010,0b100,0b111},
};

void fbDrawDigitScaled(int x,int y,uint8_t d,uint16_t c,int scale){
  if(d>9) return;
  for(int row=0;row<5;row++){
    uint8_t bits=digitBits[d][row];
    for(int col=0;col<3;col++){
      if(bits & (0b100>>col)){
        fbFillRect(x+col*scale, y+row*scale, scale, scale, c);
      }
    }
  }
}
void fbDrawCharScaled(int x,int y,char ch,uint16_t c,int scale){
  if(ch>='0'&&ch<='9'){ fbDrawDigitScaled(x,y,ch-'0',c,scale); return; }
  if(ch>='A'&&ch<='Z'){
    uint8_t idx=ch-'A';
    for(int row=0;row<5;row++){
      uint8_t bits=letterBits[idx][row];
      for(int col=0;col<3;col++) if(bits & (0b100>>col)) fbFillRect(x+col*scale,y+row*scale,scale,scale,c);
    } return;
  }
  if(ch==' ' ) return;
  if(ch==':'){
    int cx = x + scale;
    fbFillRect(cx, y+1*scale, scale, scale, c);
    fbFillRect(cx, y+3*scale, scale, scale, c);
    return;
  }
  if(ch=='/'){
    for(int i=0;i<3;i++) fbFillRect(x+(2-i)*scale, y+i*scale, scale, scale, c);
    fbFillRect(x, y+3*scale, scale, scale, c);
    fbFillRect(x, y+4*scale, scale, scale, c);
    return;
  }
  if(ch=='.'){ fbFillRect(x+scale, y+4*scale, scale, scale, c); return; }
  if(ch=='-'){ fbFillRect(x, y+2*scale, 3*scale, scale, c); return; }
  if(ch=='!'){
    for(int i=0;i<4;i++) fbFillRect(x+scale, y+i*scale, scale, scale, c);
    fbFillRect(x+scale, y+4*scale, scale, scale, c);
    return;
  }
}
void fbDrawTextScaled(int x,int y,const char* s,uint16_t c,int scale){
  while(*s){ fbDrawCharScaled(x,y,*s,c,scale); x += (3*scale+1);
    if(*s==':') x-=1;
    s++;
  }
}
int textW(const char* s, int scale){
  int len=0; const char* q=s; while(*q++){len++;}
  if(len==0) return 0;
  return len*(3*scale+1)-1;
}

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
void pipeSection(int x,int y,int w,int h,uint16_t fill,bool sheen){
  if(h <= 0 || w <= 0) return;
  fbFillRect(x, y, w, h, fill);
  fbFillRect(x, y, w, LINE, C_INK);
  if(h > 2*LINE) fbFillRect(x, y+h-LINE, w, LINE, C_INK);
  fbFillRect(x, y, LINE, h, C_INK);
  fbFillRect(x+w-LINE, y, LINE, h, C_INK);
  if(sheen && h > 6*LINE) fbFillRect(x+3*LINE, y+2*LINE, 2*LINE, h-4*LINE, C_GHOST);
}
void drawPipe(float px,int gapY){
  int x = (int)px;
  int topB = gapY - GAP/2;
  int botT = gapY + GAP/2;
  int capX = x - (CAP_W - PIPE_W)/2;
  pipeSection(x, 0, PIPE_W, topB - CAP_H, C_EDGE, true);
  pipeSection(capX, topB - CAP_H, CAP_W, CAP_H, C_EDGE, false);
  pipeSection(x, botT + CAP_H, PIPE_W, HORIZON - (botT + CAP_H), C_EDGE, true);
  pipeSection(capX, botT, CAP_W, CAP_H, C_EDGE, false);
}

void drawGround(){
  fbFillRect(0, HORIZON, WIDTH, HEIGHT - HORIZON, C_HI);
  fbFillRect(0, HORIZON, WIDTH, 4, C_EDGE);
  int off = ((int)scrollDist) % 24;
  for(int x = -24 - off; x < WIDTH; x += 24){
    fbFillRect(x + 6, HORIZON + 12, 14, 4, C_GHOST);
    fbFillRect(x + 18, HORIZON + 22, 14, 4, C_GHOST);
  }
}

void drawBird(int x,int y,bool wingUp){
  fbFillCircle(x, y, BIRD_R, C_INK);
  if(wingUp) fbFillCircle(x-4, y-4, 6, C_GHOST);
  else       fbFillCircle(x-4, y+4, 6, C_GHOST);
  fbFillCircle(x+4, y-4, 4, C_HI);
  fbFillCircle(x+6, y-4, 2, C_INK);
  fbFillRect(x+7, y-1, 8, 6, C_RED);
}

void drawScoreModule(int cx,int y,int value){
  const int Wc=16, Hc=26, T=4, gap=6, padX=8, padY=6;
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
  fbDrawTextScaled((WIDTH-textW(title,6))/2, 34, title, C_INK, 6);
  fbFillRect((WIDTH-113)/2, 72, 113, 3, C_REDDK);
  if((t/500)%2==0){
    const char* s = "TAP TO FLAP";
    fbDrawTextScaled((WIDTH-textW(s,2))/2, 88, s, C_INK, 2);
  }
  char buf[16]; snprintf(buf,sizeof(buf),"HI %d",hiScore);
  fbDrawTextScaled((WIDTH-textW(buf,2))/2, 112, buf, C_INK, 2);
}

void drawOver(unsigned long t){
  fbFillRect(48, 24, 224, 184, C_BG);
  fbRect(48, 24, 224, 184, C_EDGE);
  fbRect(49, 25, 222, 182, C_HI);
  const char* go = "GAME OVER";
  fbDrawTextScaled((WIDTH-textW(go,4))/2, 40, go, C_RED, 4);
  draw7SegNumber(WIDTH/2, 76, score, 22, 40, 6, 8, C_INK, C_GHOST);
  char buf[16]; snprintf(buf,sizeof(buf),"HI %d",hiScore);
  fbDrawTextScaled((WIDTH-textW(buf,2))/2, 132, buf, C_INK, 2);
  if((t/500)%2==0){
    const char* s = "TAP TO RETRY";
    fbDrawTextScaled((WIDTH-textW(s,2))/2, 160, s, C_REDDK, 2);
  }
}

void drawFrame(unsigned long t){
  fbClear(C_BG);
  if(state != S_READY) for(int i=0;i<nPipes;i++) drawPipe(pipes[i].x, pipes[i].gapY);
  drawGround();
  bool wingUp;
  if(state == S_READY) wingUp = ((t/220)%2==0);
  else if(state == S_PLAY) wingUp = ((t - lastFlapMs) < 130 || birdVy < 0);
  else wingUp = false;
  drawBird(BIRD_X, (int)birdY, wingUp);
  if(state == S_PLAY || state == S_DYING) drawScoreModule(WIDTH/2, 6, score);
  if(state == S_READY) drawReady(t);
  if(state == S_OVER) drawOver(t);
}

void render(unsigned long t){
  for (fbTop = 0; fbTop < HEIGHT; fbTop += FB_H) {
    if (introMode) dm01Draw(dm01Elapsed()); else drawFrame(t);
    int h = FB_H;
    if (fbTop + h > HEIGHT) h = HEIGHT - fbTop;
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

  initColors();
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
