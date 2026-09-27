// Pomodoro Timer — CYD (ESP32-2432S028R) version.
// Port of pomodoro/pomodoro.ino (ESP32-C3 + ST7735 160x128 + touch button)
// to the Cheap Yellow Display: ST7789 320x240 + XPT2046 resistive touch.
//
// Controls: the TOUCH SCREEN replaces the touch button.
//   - Quick tap anywhere on screen  = TAP action (start / pause / resume / continue)
//   - Press and hold >= 1.5s        = HOLD action (cycle preset from READY, else reset)
// Footer hints show the current TAP / HOLD meaning, same as the C3 version.
// No NeoPixel on CYD — state feedback is on-screen + serial only.

#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
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

#define WIDTH 320
#define HEIGHT 240

enum State : uint8_t { S_IDLE, S_WORK, S_BREAK, S_LONG_BREAK, S_PAUSED, S_DONE_FLASH };

// ── Framebuffer (strip-based: 64 rows per strip; 150 KB full frame does not
//    fit ESP32 DRAM, so screenshots stream band by band — see render()) ──
#define FB_H 64
static uint16_t fb[WIDTH * FB_H];
static int fbTop = 0;
static ScreenshotStripSession shot;

#define FPIX(x,y,c) if((x)>=0&&(x)<WIDTH&&(y)>=fbTop&&(y)<fbTop+FB_H) fb[(y-fbTop)*WIDTH+(x)]=(c)

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
void fbDrawCircle(int cx, int cy, int r, uint16_t c) {
  int x = 0, y = r, d = 3 - 2 * r;
  while (x <= y) {
    FPIX(cx + x, cy + y, c); FPIX(cx - x, cy + y, c);
    FPIX(cx + x, cy - y, c); FPIX(cx - x, cy - y, c);
    FPIX(cx + y, cy + x, c); FPIX(cx - y, cy + x, c);
    FPIX(cx + y, cy - x, c); FPIX(cx - y, cy - x, c);
    if (d < 0) d += 4 * x + 6;
    else { d += 4 * (x - y) + 10; y--; }
    x++;
  }
}
void fbDrawLine(int x0,int y0,int x1,int y1,uint16_t c){
  int dx=abs(x1-x0),sx=x0<x1?1:-1;
  int dy=-abs(y1-y0),sy=y0<y1?1:-1;
  int err=dx+dy,e2;
  for(;;){FPIX(x0,y0,c); if(x0==x1&&y0==y1)break; e2=2*err; if(e2>=dy){err+=dy;x0+=sx;} if(e2<=dx){err+=dx;y0+=sy;}}
}
void fbFlush() {
  int h = FB_H;
  if (fbTop + h > HEIGHT) h = HEIGHT - fbTop;
  tft.startWrite();
  tft.setAddrWindow(0, fbTop, WIDTH, h);
  tft.writePixels(fb, WIDTH * h);
  tft.endWrite();
}

// ── 3x5 Font (same glyphs as pomodoro.ino) ──
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

// ── Pomodoro Logic (same as pomodoro.ino) ──
struct Preset { uint8_t work; uint8_t brk; uint8_t longBrk; const char* label; };
Preset presets[3] = {
  {25, 5, 15, "25/5"},
  {50,10,30, "50/10"},
  {15, 3,10, "15/3"}
};
int curPreset = 0;

State state = S_IDLE;
State pausedPrev = S_WORK;

unsigned long stateStartMs = 0;
unsigned long durationMs = 0;
unsigned long pauseRemainingMs = 0;
unsigned long doneFlashStart = 0;

int completedInSet = 0; // 0..3
int totalCompleted = 0;

#define HOLD_MS 1500
#define DOT_COUNT 4

bool wasPressed = false;
bool touchDown = false; // currently touching (for footer highlight)
bool introMode = false;
unsigned long pressStartMs = 0;
bool holdFired = false;

// helper
uint16_t rgb(uint8_t r,uint8_t g,uint8_t b){ return tft.color565(r,g,b); }

#define DM01_SCALE 2
#include "Dm01Intro.h"
const char* stateName(State s){
  switch(s){ case S_IDLE: return "READY"; case S_WORK: return "WORK"; case S_BREAK: return "BREAK"; case S_LONG_BREAK: return "LONG BREAK"; case S_PAUSED: return "PAUSED"; case S_DONE_FLASH: return "DONE!"; }
  return "";
}

void enterIdle(){
  state = S_IDLE;
  durationMs = (unsigned long)presets[curPreset].work * 60000UL;
  Serial.printf("[pomo] IDLE preset %s (%d/%d long %d) doneInSet=%d total=%d\n",
    presets[curPreset].label, presets[curPreset].work, presets[curPreset].brk, presets[curPreset].longBrk, completedInSet, totalCompleted);
}

void startWork(){
  state = S_WORK;
  durationMs = (unsigned long)presets[curPreset].work * 60000UL;
  stateStartMs = millis();
  Serial.printf("[pomo] START WORK %d min\n", presets[curPreset].work);
}

void startBreak(bool isLong){
  if(isLong){ state = S_LONG_BREAK; durationMs = (unsigned long)presets[curPreset].longBrk * 60000UL; }
  else { state = S_BREAK; durationMs = (unsigned long)presets[curPreset].brk * 60000UL; }
  stateStartMs = millis();
  Serial.printf("[pomo] START %s %lu ms\n", isLong?"LONG BREAK":"BREAK", durationMs);
}

void doPause(){
  if(state==S_WORK || state==S_BREAK || state==S_LONG_BREAK){
    unsigned long elapsed = millis() - stateStartMs;
    if(elapsed >= durationMs) pauseRemainingMs = 0;
    else pauseRemainingMs = durationMs - elapsed;
    pausedPrev = state;
    state = S_PAUSED;
    Serial.printf("[pomo] PAUSED remaining %lu ms\n", pauseRemainingMs);
  }
}
void doResume(){
  if(state==S_PAUSED){
    state = pausedPrev;
    durationMs = pauseRemainingMs;
    stateStartMs = millis();
    Serial.printf("[pomo] RESUME %s remaining %lu\n", stateName(state), durationMs);
  }
}
void cyclePreset(){
  curPreset = (curPreset+1)%3;
  Serial.printf("[pomo] CYCLE preset -> %s\n", presets[curPreset].label);
  enterIdle();
}

void onShortTap(){
  Serial.println("[touch] SHORT TAP");
  switch(state){
    case S_IDLE: startWork(); break;
    case S_WORK:
    case S_BREAK:
    case S_LONG_BREAK: doPause(); break;
    case S_PAUSED: doResume(); break;
    case S_DONE_FLASH: {
      if(pausedPrev==S_WORK){
        bool isLong = (completedInSet >= DOT_COUNT);
        if(isLong) startBreak(true);
        else startBreak(false);
      } else {
        startWork();
      }
      break;
    }
  }
}
void onLongHold(){
  Serial.println("[touch] LONG HOLD");
  if(state==S_IDLE){
    cyclePreset();
    return;
  }
  Serial.printf("[pomo] RESET -> idle preset %s\n", presets[curPreset].label);
  enterIdle();
}

// ── Rendering: QUARTZ LCD, scaled 2x for 320x240 ──
// Single light LCD palette in every state (skill §1 — no negative mode).
#define F91_RED   rgb(200,40,40)
#define F91_RED_DK rgb(120,24,24)

uint16_t panelBG(){ return rgb(148,158,130); }
uint16_t panelInk(){ return rgb(28,34,24); }
uint16_t panelGhost(){ return rgb(138,148,120); }
uint16_t panelHI(){ return rgb(178,186,160); }
uint16_t panelEdge(){ return rgb(96,104,82); }
uint16_t accent(){ return F91_RED; }
uint16_t accentDK(){ return F91_RED_DK; }

// 7-segment digit: bit0=a bit1=b bit2=c bit3=d bit4=e bit5=f bit6=g
static const uint8_t segBits[10] = {
  0x3F, 0x06, 0x5B, 0x4F, 0x66,
  0x6D, 0x7D, 0x07, 0x7F, 0x6F
};
void sevenSegSeg(int x,int y,int Wc,int Hc,int T,int seg,uint16_t c){
  int vH = (Hc - 3*T + 1)/2;
  int midT = (Hc - T)/2;
  switch(seg){
    case 0: fbFillRect(x, y, Wc, T, c); return;
    case 1: fbFillRect(x+Wc-T, y+T, T, vH, c); return;
    case 2: fbFillRect(x+Wc-T, y+midT+T, T, vH, c); return;
    case 3: fbFillRect(x, y+Hc-T, Wc, T, c); return;
    case 4: fbFillRect(x, y+midT+T, T, vH, c); return;
    case 5: fbFillRect(x, y+T, T, vH, c); return;
    case 6: fbFillRect(x, y+midT, Wc, T, c); return;
  }
}
void sevenSegDigit(int x,int y,int Wc,int Hc,int T,uint8_t d,uint16_t lit,uint16_t ghost){
  for(int s=0;s<7;s++) sevenSegSeg(x,y,Wc,Hc,T,s,ghost);
  if(d>9) return;
  uint8_t m = segBits[d];
  for(int s=0;s<7;s++) if(m & (1<<s)) sevenSegSeg(x,y,Wc,Hc,T,s,lit);
}
void sevenSegTime(int x,int y,int Wc,int Hc,int T,int minutes,int seconds,bool showColon,uint16_t lit,uint16_t ghost){
  int gap = 14, colonW = 16;
  int m1=minutes/10,m0=minutes%10,s1=seconds/10,s0=seconds%10;
  int p = x;
  sevenSegDigit(p,y,Wc,Hc,T,m1,lit,ghost); p += Wc+gap;
  sevenSegDigit(p,y,Wc,Hc,T,m0,lit,ghost); p += Wc+gap;
  uint16_t cc = showColon ? lit : ghost;
  fbFillRect(p+2, y+28, 12, 12, cc);
  fbFillRect(p+2, y+Hc-40, 12, 12, cc);
  p += colonW+gap;
  sevenSegDigit(p,y,Wc,Hc,T,s1,lit,ghost); p += Wc+gap;
  sevenSegDigit(p,y,Wc,Hc,T,s0,lit,ghost);
}

// Layout (320x240, ~2x of the 160x128 original)
#define FS2 2          // UI text scale
#define HDR_Y 8
#define BAR_Y 26       // thin line; thick bar BAR_Y+2 h=8 (bottom = 36)
#define DIG_Y 60       // 7-seg top (Hc=88 -> bottom 148)
#define PROG_Y 168     // progress bar top (h=16 -> bottom 184)
#define SET_Y 196      // SET squares top (16px -> bottom 212)
#define DIV_Y 224      // footer divider
#define FTR_Y 226      // footer text (2x, 10px tall)

void drawFrame(){
  State eff = state==S_PAUSED? pausedPrev : state;

  uint16_t bg=panelBG(), ink=panelInk(), ghost=panelGhost(), hi=panelHI(), edge=panelEdge();

  // DONE flash: red-on-LCD — blinks until tap
  if(state==S_DONE_FLASH){
    bool on = ((millis()-doneFlashStart)/200)%2==0;
    fbClear(on? rgb(255,255,255) : bg);
    const char* msg = (pausedPrev==S_WORK) ? "WORK DONE" : "BREAK DONE";
    int dw = textW("DONE!", 6);
    fbDrawTextScaled((WIDTH-dw)/2, 56, "DONE!", on? accent() : ink, 6);
    int mw = textW(msg, FS2);
    fbDrawTextScaled((WIDTH-mw)/2, 120, msg, accent(), FS2);
    int hw = textW("TAP TO CONTINUE", FS2);
    fbDrawTextScaled((WIDTH-hw)/2, 160, "TAP TO CONTINUE", accentDK(), FS2);
    return;
  }

  fbClear(bg);

  // header: POMO left, preset centered, state right (2x)
  fbDrawTextScaled(12, HDR_Y, "POMO", ink, FS2);
  {
    const char* pl = presets[curPreset].label;
    fbDrawTextScaled((WIDTH-textW(pl,FS2))/2, HDR_Y, pl, ink, FS2);
  }
  {
    const char* sn = stateName(eff); if(state==S_PAUSED) sn="PAUSED";
    fbDrawTextScaled(WIDTH-12-textW(sn,FS2), HDR_Y, sn, ink, FS2);
  }

  // red accent strip (alarm style)
  fbHLine(8, BAR_Y, WIDTH-16, accentDK());
  fbHLine(8, BAR_Y+1, WIDTH-16, accentDK());
  fbFillRect(8, BAR_Y+2, WIDTH-16, 8, accent());

  // compute remaining / progress
  unsigned long remainingMs; float progress;
  if(state==S_IDLE){ remainingMs=(unsigned long)presets[curPreset].work*60000UL; progress=0.0f; }
  else if(state==S_PAUSED){
    remainingMs=pauseRemainingMs; unsigned long orig=0;
    if(pausedPrev==S_WORK) orig=(unsigned long)presets[curPreset].work*60000UL;
    else if(pausedPrev==S_BREAK) orig=(unsigned long)presets[curPreset].brk*60000UL;
    else if(pausedPrev==S_LONG_BREAK) orig=(unsigned long)presets[curPreset].longBrk*60000UL;
    if(orig>0) progress=1.0f-(float)remainingMs/(float)orig; else progress=0;
  } else { unsigned long el=millis()-stateStartMs; if(el>=durationMs) remainingMs=0; else remainingMs=durationMs-el; progress=(float)el/(float)durationMs; if(progress>1) progress=1; }
  int totSec=remainingMs/1000; int mins=totSec/60, secs=totSec%60;
  bool showColon;
  if(state==S_WORK||state==S_BREAK||state==S_LONG_BREAK) showColon=(millis()%1000)<750;
  else if(state==S_PAUSED) showColon=(millis()/400)%2==0;
  else showColon=true;

  // ── 7-seg digits (2x proportions, centered) ──
  int Wc=54, Hc=88, T=10;
  int gap=14, colonW=16;
  int totalW = Wc*4 + 4*gap + colonW; // 288
  int timeX = (WIDTH - totalW)/2;
  sevenSegTime(timeX, DIG_Y, Wc, Hc, T, mins, secs, showColon, ink, ghost);

  // ── progress bar ──
  int barX=12, barY=PROG_Y, barW=WIDTH-24, barH=16;
  fbRect(barX, barY, barW, barH, edge);
  fbRect(barX+1, barY+1, barW-2, barH-2, hi);
  int innerW = barW-4, innerH = barH-4;
  fbFillRect(barX+2, barY+2, innerW, innerH, bg);
  int fillW = (int)(innerW * progress);
  if(fillW > 0) fbFillRect(barX+2, barY+2, fillW, innerH, accent());

  // ── SET row with n/4 fraction ──
  fbDrawTextScaled(12, SET_Y+3, "SET", ink, FS2);
  for(int i=0;i<DOT_COUNT;i++){
    int x = 104 + i*32;
    if(i<completedInSet) fbFillRect(x,SET_Y,16,16,accent());
    else if(i==completedInSet && (state==S_WORK||(state==S_PAUSED&&pausedPrev==S_WORK))){
      if((millis()/400)%2==0) fbFillRect(x,SET_Y,16,16,accent());
      else fbRect(x,SET_Y,16,16,hi);
    } else fbRect(x,SET_Y,16,16,hi);
  }
  {
    int cur = completedInSet + (state==S_WORK||state==S_LONG_BREAK|| (state==S_PAUSED&&pausedPrev==S_WORK) ? 1 : 0);
    if(cur>4) cur=4; if(cur<1) cur=1;
    if(state==S_IDLE) cur=1;
    char frac[8]; snprintf(frac,sizeof(frac),"%d/4",cur);
    fbDrawTextScaled(WIDTH-12-textW(frac,FS2), SET_Y+3, frac, ink, FS2);
  }

  // ── footer: left = tap action, right = hold action ──
  fbHLine(0, DIV_Y, WIDTH, edge);
  const char* a; const char* b;
  if(state==S_IDLE){ a="TAP START"; b="HOLD PRESET"; }
  else if(state==S_PAUSED){ a="TAP RESUME"; b="HOLD RESET"; }
  else { a="TAP PAUSE"; b="HOLD RESET"; }
  uint16_t leftC = touchDown ? accent() : ink; // press feedback
  fbDrawTextScaled(12, FTR_Y, a, leftC, FS2);
  fbDrawTextScaled(WIDTH-12-textW(b,FS2), FTR_Y, b, accentDK(), FS2);
}

void render(){
  for (fbTop = 0; fbTop < HEIGHT; fbTop += FB_H) {
    if (introMode) dm01Draw(dm01Elapsed()); else drawFrame();
    int h = FB_H;
    if (fbTop + h > HEIGHT) h = HEIGHT - fbTop;
    if (!screenshotStripSend(shot, fb, fbTop, h)) fbFlush();
  }
}

// ── Touch (XPT2046): screen replaces the button ──
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
  touchDown = pressed;

  if(pressed && !wasPressed){
    pressStartMs = now;
    wasPressed = true;
    holdFired = false;
    int tx, ty; touchRead(tx, ty);
    Serial.printf("[touch] press @ %d,%d\n", tx, ty);
  } else if(!pressed && wasPressed){
    unsigned long dur = now - pressStartMs;
    if(!holdFired){
      if(dur >= 60 && dur < HOLD_MS){
        onShortTap();
      }
    }
    wasPressed = false;
    holdFired = false;
    delay(20);
  } else if(pressed && wasPressed && !holdFired && (now - pressStartMs >= HOLD_MS)){
    holdFired = true;
    onLongHold();
  }
}

void checkTimerDone(){
  if(state==S_WORK || state==S_BREAK || state==S_LONG_BREAK){
    unsigned long elapsed = millis() - stateStartMs;
    if(elapsed >= durationMs){
      State finished = state;
      Serial.printf("[pomo] FINISH %s\n", stateName(finished));
      if(finished==S_WORK){
        totalCompleted++;
        completedInSet++;
        pausedPrev = finished;
        state = S_DONE_FLASH;
        doneFlashStart = millis();
      } else if(finished==S_BREAK){
        pausedPrev = finished;
        state = S_DONE_FLASH;
        doneFlashStart = millis();
      } else if(finished==S_LONG_BREAK){
        completedInSet = 0;
        pausedPrev = finished;
        state = S_DONE_FLASH;
        doneFlashStart = millis();
      }
    }
  }
}

void setup(){
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== POMODORO CYD ===");

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

  touchInit();
  enterIdle();
  dm01Start();
}

void loop(){
  screenshotStripPoll(shot, WIDTH, HEIGHT);
  if (dm01Tick(touchPressed())) {
    introMode = true;
    render();
    delay(16);
    return;
  }
  introMode = false;
  handleTouch();
  checkTimerDone();
  render();
  delay(30);
}
