#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>
#include <Adafruit_NeoPixel.h>
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

#define WIDTH 160
#define HEIGHT 128

enum State : uint8_t { S_IDLE, S_WORK, S_BREAK, S_LONG_BREAK, S_PAUSED, S_DONE_FLASH };

// ── Framebuffer ──
static uint16_t fb[WIDTH * HEIGHT];
#define FPIX(x,y,c) if((x)>=0&&(x)<WIDTH&&(y)>=0&&(y)<HEIGHT) fb[(y)*WIDTH+(x)]=(c)

void fbClear(uint16_t c = ST7735_BLACK) {
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
  tft.startWrite();
  tft.setAddrWindow(0, 0, WIDTH, HEIGHT);
  tft.writePixels(fb, WIDTH * HEIGHT);
  tft.endWrite();
}

// ── 4x5 Font (subset: 0-9, A-Z, !, :) ──
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
static const uint8_t gDash45[5]  = {0b0000,0b0000,0b1110,0b0000,0b0000};
static const uint8_t gDot45[5]   = {0b0000,0b0000,0b0000,0b0000,0b0100};
static const uint8_t gGt45[5]    = {0b1000,0b0100,0b0010,0b0100,0b1000};
static const uint8_t gSlash45[5] = {0b0001,0b0010,0b0100,0b1000,0b0000};

static inline const uint8_t* glyph45(char ch){
  if(ch>='0'&&ch<='9') return gDig45[ch-'0'];
  if(ch>='A'&&ch<='Z') return gLet45[ch-'A'];
  if(ch=='!') return gExcl45;
  if(ch==':') return gColon45;
  if(ch=='-') return gDash45;
  if(ch=='.') return gDot45;
  if(ch=='>') return gGt45;
  if(ch=='/') return gSlash45;
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
void fbDrawText1x(int x,int y,const char* s,uint16_t c){
  while(*s){ fbDrawChar57(x,y,*s,c,1); x+=5; s++; }
}
void fbDrawTextScaled(int x,int y,const char* s,uint16_t c,int scale){
  while(*s){ fbDrawChar57(x,y,*s,c,scale); x+=5*scale; s++; }
}
int textW57(const char* s,int scale){ int n=0; while(*s++)n++; return n?(n*5*scale-scale):0; }

// ── Pomodoro Logic ──
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
#define DONE_FLASH_MS 4000
#define DOT_COUNT 4

int idleLevel = HIGH;
bool wasPressed = false;
unsigned long pressStartMs = 0;
bool holdFired = false;

// helper
uint16_t rgb(uint8_t r,uint8_t g,uint8_t b){ return tft.color565(r,g,b); }
const char* stateName(State s){
  switch(s){ case S_IDLE: return "READY"; case S_WORK: return "WORK"; case S_BREAK: return "BREAK"; case S_LONG_BREAK: return "LONG BREAK"; case S_PAUSED: return "PAUSED"; case S_DONE_FLASH: return "DONE!"; }
  return "";
}

void enterIdle(){
  state = S_IDLE;
  durationMs = (unsigned long)presets[curPreset].work * 60000UL;
  // keep completedInSet/total as is, but show idle
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
  // flash feedback
  // reset to idle (keeps dot counts? reset completedInSet only on long break done, so keep)
  enterIdle();
}

void onShortTap(){
  Serial.println("[btn] SHORT TAP");
  // quick led feedback
  pixels.setPixelColor(0, pixels.Color(80,80,80)); pixels.show(); delay(60);
  switch(state){
    case S_IDLE: startWork(); break;
    case S_WORK:
    case S_BREAK:
    case S_LONG_BREAK: doPause(); break;
    case S_PAUSED: doResume(); break;
    case S_DONE_FLASH: {
      // skip flash, go to next
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
  Serial.println("[btn] LONG HOLD");
  // led flash
  for(int i=0;i<2;i++){ pixels.setPixelColor(0,pixels.Color(80,180,255)); pixels.show(); delay(90); pixels.clear(); pixels.show(); delay(90); }
  if(state==S_IDLE){
    // from READY: cycle to next preset
    cyclePreset();
    return;
  }
  // from WORK/BREAK/PAUSED/DONE: reset to idle, keep same preset
  Serial.printf("[pomo] RESET -> idle preset %s\n", presets[curPreset].label);
  enterIdle();
}

// ── Rendering: QUARTZ LCD (always light — skill §1, no negative mode) ──
#define F91_RED   rgb(200,40,40)
#define F91_RED_DK rgb(120,24,24)

uint16_t panelBG()    { return rgb(148,158,130); } // light LCD green-grey
uint16_t panelInk()   { return rgb(28,34,24); }    // dark ink
uint16_t panelGhost() { return rgb(138,148,120); } // unlit segments
uint16_t panelHI()    { return rgb(178,186,160); } // light inner border
uint16_t panelEdge()  { return rgb(96,104,82); }   // dark olive outer border
uint16_t accent()     { return F91_RED; }
uint16_t accentDK()   { return F91_RED_DK; }

// 7-segment digit: bit0=a bit1=b bit2=c bit3=d bit4=e bit5=f bit6=g
static const uint8_t segBits[10] = {
  0x3F, 0x06, 0x5B, 0x4F, 0x66,
  0x6D, 0x7D, 0x07, 0x7F, 0x6F
};
// single 7-seg segment (straight, clean, no gaps)
void sevenSegSeg(int x,int y,int Wc,int Hc,int T,int seg,uint16_t c){
  int vH = (Hc - 3*T + 1)/2;    // vertical segment height (round up to close gap)
  int midT = (Hc - T)/2;        // middle band top
  switch(seg){
    case 0: fbFillRect(x, y, Wc, T, c); return;            // a  (top)
    case 1: fbFillRect(x+Wc-T, y+T, T, vH, c); return;     // b  (top-right)
    case 2: fbFillRect(x+Wc-T, y+midT+T, T, vH, c); return;// c  (bottom-right)
    case 3: fbFillRect(x, y+Hc-T, Wc, T, c); return;       // d  (bottom)
    case 4: fbFillRect(x, y+midT+T, T, vH, c); return;     // e  (bottom-left)
    case 5: fbFillRect(x, y+T, T, vH, c); return;          // f  (top-left)
    case 6: fbFillRect(x, y+midT, Wc, T, c); return;       // g  (middle)
  }
}
// draw a 7-seg digit: all segments ghost, then light the active ones
void sevenSegDigit(int x,int y,int Wc,int Hc,int T,uint8_t d,uint16_t lit,uint16_t ghost){
  for(int s=0;s<7;s++) sevenSegSeg(x,y,Wc,Hc,T,s,ghost);
  if(d>9) return;
  uint8_t m = segBits[d];
  for(int s=0;s<7;s++) if(m & (1<<s)) sevenSegSeg(x,y,Wc,Hc,T,s,lit);
}
// full MM:SS with blinking colon (uniform gaps, colon centered in its cell)
void sevenSegTime(int x,int y,int Wc,int Hc,int T,int minutes,int seconds,bool showColon,uint16_t lit,uint16_t ghost){
  int gap = 7, colonW = 8;
  int m1=minutes/10,m0=minutes%10,s1=seconds/10,s0=seconds%10;
  int p = x;
  sevenSegDigit(p,y,Wc,Hc,T,m1,lit,ghost); p += Wc+gap;
  sevenSegDigit(p,y,Wc,Hc,T,m0,lit,ghost); p += Wc+gap;
  uint16_t cc = showColon ? lit : ghost;
  fbFillRect(p+1, y+14, 6, 6, cc);
  fbFillRect(p+1, y+Hc-20, 6, 6, cc);
  p += colonW+gap;
  sevenSegDigit(p,y,Wc,Hc,T,s1,lit,ghost); p += Wc+gap;
  sevenSegDigit(p,y,Wc,Hc,T,s0,lit,ghost);
}

void render(){
  int panelX=0, panelY=0, panelW=WIDTH, panelH=HEIGHT;
  State eff = state==S_PAUSED? pausedPrev : state;

  // palette (single light LCD in every state — skill §1)
  uint16_t bg=panelBG(), ink=panelInk(), ghost=panelGhost(), hi=panelHI(), edge=panelEdge();

  // DONE flash: white↔light-LCD-green with centered DONE! + subtitle (skill §6)
  if(state==S_DONE_FLASH){
    bool on = ((millis()-doneFlashStart)/200)%2==0;
    fbClear(on? rgb(255,255,255) : bg);
    fbDrawTextScaled((WIDTH-textW57("DONE!",1))/2, 48, "DONE!", on? accent() : ink, 1);
    const char* msg = (pausedPrev==S_WORK) ? "WORK DONE" : "BREAK DONE";
    fbDrawText1x((WIDTH-textW57(msg,1))/2, 62, msg, accent());
    fbDrawText1x((WIDTH-textW57("TAP TO CONTINUE",1))/2, 86, "TAP TO CONTINUE", accentDK());
    fbFlush();
    return;
  }

  // ── borderless LCD (no bezel) ──
  fbClear(bg);

  // header: POMO left, preset centered, state right (4x5 labels at y=3)
  fbDrawText1x(6, 3, "POMO", ink);
  { // preset centered
    const char* pl = presets[curPreset].label;
    fbDrawText1x((WIDTH - textW57(pl,1))/2, 3, pl, ink);
  }
  { // state right
    const char* sn = stateName(eff); if(state==S_PAUSED) sn="PAUSED";
    fbDrawText1x(WIDTH-6-textW57(sn,1), 3, sn, ink);
  }

  // red accent strip (alarm style: thin line + bar)
  fbHLine(4, 13, WIDTH-8, accentDK());
  fbFillRect(4, 14, WIDTH-8, 4, accent());

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

  // ── 7-seg digits (proportionate, centered) ──
  int Wc=26, Hc=44, T=5;
  int gap=8, colonW=8;
  int totalW = Wc*4 + 4*gap + colonW; // 144
  int timeX = (WIDTH - totalW)/2;
  // vertically center digits between red bar bottom (24) and progress bar top (86)
  int timeY = 24 + ((86-24) - Hc)/2;
  sevenSegTime(timeX, timeY, Wc, Hc, T, mins, secs, showColon, ink, ghost);
  // ── progress bar (smaller, outer border) ──
  int barX=6, barY=86, barW=WIDTH-12, barH=8;
  fbRect(barX, barY, barW, barH, edge);
  fbRect(barX+1, barY+1, barW-2, barH-2, hi);
  int innerW = barW-4, innerH = barH-4;
  fbFillRect(barX+2, barY+2, innerW, innerH, bg);
  int fillW = (int)(innerW * progress);
  if(fillW > 0) fbFillRect(barX+2, barY+2, fillW, innerH, accent());

  // ── SET row with 1/4 fraction ──
  // center vertically: progress bar bottom=94, divider=114, space=20px, squares=8px => dotY=100
  int dotY = 100;
  fbDrawText1x(6, dotY, "SET", ink);  // 5px text, shares the squares' top edge
  for(int i=0;i<DOT_COUNT;i++){
    int x = 52 + i*16;
    if(i<completedInSet) fbFillRect(x,dotY,8,8,accent());
    else if(i==completedInSet && (state==S_WORK||(state==S_PAUSED&&pausedPrev==S_WORK))){
      if((millis()/400)%2==0) fbFillRect(x,dotY,8,8,accent());
      else fbRect(x,dotY,8,8,hi);
    } else fbRect(x,dotY,8,8,hi);
  }
  // 1/4 fraction on right
  {
    int cur = completedInSet + (state==S_WORK||state==S_LONG_BREAK|| (state==S_PAUSED&&pausedPrev==S_WORK) ? 1 : 0);
    if(cur>4) cur=4; if(cur<1) cur=1;
    if(state==S_IDLE) cur=1;
    char frac[8]; snprintf(frac,sizeof(frac),"%d/4",cur);
    fbDrawText1x(WIDTH-6-textW57(frac,1), dotY, frac, ink);
  }

  // ── footer (screenshot: split left/right, divider) ──
  fbHLine(0, 114, WIDTH, edge);
  int fy = 119;
  const char* a; const char* b;
  if(state==S_IDLE){ a="TAP START"; b="HOLD PRESET"; }
  else if(state==S_PAUSED){ a="TAP RESUME"; b="HOLD RESET"; }
  else { a="TAP PAUSE"; b="HOLD PRESET"; }
  fbDrawText1x(6, fy, a, ink);
  fbDrawText1x(WIDTH-6-textW57(b,1), fy, b, accentDK());

  fbFlush();
}

// ── Touch handling with auto idle calibration ──
void calibrateTouch(){
  delay(300);
  int highCount=0;
  for(int i=0;i<20;i++){ if(digitalRead(TOUCH_PIN)==HIGH) highCount++; delay(10); }
  idleLevel = (highCount>10) ? HIGH : LOW;
  Serial.printf("[touch] calibrate highCount=%d idle=%s\n", highCount, idleLevel==HIGH?"HIGH":"LOW");
}

void handleTouch(){
  bool raw = digitalRead(TOUCH_PIN);
  bool pressed = (raw != idleLevel);
  unsigned long now = millis();

  // simple debounce: ignore very short glitches <30ms
  // we track stable
  if(pressed && !wasPressed){
    // new press
    pressStartMs = now;
    wasPressed = true;
    holdFired = false;
  } else if(!pressed && wasPressed){
    unsigned long dur = now - pressStartMs;
    if(!holdFired){
      if(dur >= 60 && dur < HOLD_MS){
        onShortTap();
      } else if(dur >= HOLD_MS){
        // already handled as hold
      }
    }
    wasPressed = false;
    holdFired = false;
    // small debounce delay
    delay(20);
  } else if(pressed && wasPressed && !holdFired && (now - pressStartMs >= HOLD_MS)){
    holdFired = true;
    onLongHold();
  }
}

// ── LED ──
void updateLED(){
  static unsigned long lastLed = 0;
  if(millis() - lastLed < 50) return;
  lastLed = millis();
  if(state==S_DONE_FLASH){
    bool on = (millis()/150)%2==0;
    if(on) pixels.setPixelColor(0, pixels.Color(255,255,255));
    else pixels.setPixelColor(0, pixels.Color(255,40,40));
    pixels.show(); return;
  }
  if(state==S_PAUSED){
    bool on = (millis()/400)%2==0;
    if(on) pixels.setPixelColor(0, pixels.Color(255,180,0));
    else pixels.clear();
    pixels.show(); return;
  }
  if(state==S_WORK){
    // pulsing red
    float p = (sin(millis()*0.004)+1)*0.5;
    uint8_t r = 120 + p*135;
    pixels.setPixelColor(0, pixels.Color(r, 0, 0));
    pixels.show(); return;
  }
  if(state==S_BREAK || state==S_LONG_BREAK){
    float p = (sin(millis()*0.004)+1)*0.5;
    uint8_t g = 120 + p*100;
    pixels.setPixelColor(0, pixels.Color(0,g, 60));
    pixels.show(); return;
  }
  if(state==S_IDLE){
    // breathing blue
    float p = (sin(millis()*0.003)+1)*0.5;
    uint8_t b = 40 + p*80;
    pixels.setPixelColor(0, pixels.Color(0, 20, b));
    pixels.show(); return;
  }
}

void checkTimerDone(){
  if(state==S_WORK || state==S_BREAK || state==S_LONG_BREAK){
    unsigned long elapsed = millis() - stateStartMs;
    if(elapsed >= durationMs){
      // finished
      State finished = state;
      Serial.printf("[pomo] FINISH %s\n", stateName(finished));
      if(finished==S_WORK){
        totalCompleted++;
        completedInSet++;
        if(completedInSet>=DOT_COUNT){
          // moved to long break next, but reset after long break
          // keep at 4 until long break done
        }
        // decide break type
        pausedPrev = finished; // for flash message
        state = S_DONE_FLASH;
        doneFlashStart = millis();
        // next state after flash will be break
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
  if(state==S_DONE_FLASH){
    // blink indefinitely until user taps
  }
}

#include "Dm01Intro.h"

void setup(){
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== POMODORO ===");

  pinMode(TOUCH_PIN, INPUT);
  pixels.begin(); pixels.setBrightness(40); pixels.clear(); pixels.show();

  SPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);
  tft.initR(INITR_BLACKTAB);
  tft.setRotation(1);
  tft.fillScreen(ST7735_BLACK);

  calibrateTouch();
  enterIdle();
  pixels.setBrightness(70);
  dm01Start();
}

void loop(){
  bool introSkip = (digitalRead(TOUCH_PIN) != idleLevel);
  if(dm01Frame(introSkip)){
    pixels.setPixelColor(0, dm01Pal((int)(millis()/150)));
    pixels.show();
    fbFlush();
    screenshotHandle(fb, WIDTH, HEIGHT);
    delay(16);
    return;
  }
  static bool introEnded = false;
  if(!introEnded && !dm01IsActive()){
    introEnded = true;
    pixels.setBrightness(40);
    Serial.printf("[intro] done t=%lums\n", millis() - dm01StartMs);
  }
  handleTouch();
  checkTimerDone();
  updateLED();
  render();
  screenshotHandle(fb, WIDTH, HEIGHT);
  delay(30);
}
