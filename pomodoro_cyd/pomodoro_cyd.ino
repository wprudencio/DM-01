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
static uint16_t vhsSrc[WIDTH * FB_H];
static uint32_t vhsT = 0;
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

// ── Departure Mono pixel font (rasterized at 11px — see DepartureMono.h).
// Kept behind the old function names so the layout code reads unchanged. ──
#include "DepartureMono.h"

static inline const uint8_t* dmGlyph(char ch){
  if(ch < 32 || ch > 90) return dmFont[0];       // space fallback
  return dmFont[ch - 32];
}
void fbDrawCharScaled(int x,int y,char ch,uint16_t c,int scale){
  if(ch==' ') return;
  const uint8_t* g = dmGlyph(ch);
  int cy = y - DM_TOP*scale;                      // y = cap/digit top
  for(int row=0; row<DM_H; row++){
    uint8_t bits = g[row];
    if(!bits) continue;
    for(int col=0; col<DM_W; col++)
      if(bits & (0x40>>col)) fbFillRect(x+col*scale, cy+row*scale, scale, scale, c);
  }
}
void fbDrawTextScaled(int x,int y,const char* s,uint16_t c,int scale){
  while(*s){ fbDrawCharScaled(x,y,*s,c,scale); x += DM_ADV*scale; s++; }
}
int textW(const char* s, int scale){
  int len=0; const char* q=s; while(*q++) len++;
  if(len==0) return 0;
  return len*DM_ADV*scale - scale;
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

// ── Rendering palette: dark-grey deck, white ink, magenta + signal-green
// accents. Palette: #242424, white, magenta #EC008C, signal green #00E676. ──
uint16_t cBlack(){   return rgb( 36,  36,  36); } // #242424
uint16_t cWhite(){   return rgb(255, 255, 255); }
uint16_t cMagenta(){ return rgb(236,   0, 140); } // #EC008C
uint16_t cGreen(){   return rgb(  0, 230, 118); } // #00E676
uint16_t cInk(){     return rgb(205, 205, 205); }
uint16_t cMute(){    return rgb(110, 110, 110); }
uint16_t cRule(){    return rgb( 45,  45,  45); }
uint16_t cLine(){    return rgb(170, 170, 170); } // light grey rules
uint16_t cGhost(){   return rgb( 52,  52,  52); } // unlit 7-seg segments
uint16_t cMagentaD(){return rgb(104,   0,  62); } // dimmed magenta edging

uint16_t scale565(uint16_t c, int pct){    // pct/256 brightness (>256 brightens)
  int r=(c>>11)&0x1F, g=(c>>5)&0x3F, b=c&0x1F;
  r=r*pct>>8; g=g*pct>>8; b=b*pct>>8;
  if(r>31)r=31; if(g>63)g=63; if(b>31)b=31;
  return (uint16_t)((r<<11)|(g<<5)|b);
}
static inline uint32_t vhsNoise(uint32_t x){
  x ^= x>>16; x *= 0x7feb352dU; x ^= x>>15; x *= 0x846ca68bU; x ^= x>>16;
  return x;
}

// Layout (320x240) — the original timer layout, but with a typographic block
// countdown instead of 7-seg: header rail + magenta accent strip, giant block
// digits, framed progress bar, SET row, footer hints.
#define FS2 1
#define HDR_Y 9
#define BAR_Y 26
#define DIG_Y 64
#define PROG_Y 168
#define SET_Y 192
#define DIV_Y 216
#define FTR_Y 224

void drawFrame(){
  State eff = state==S_PAUSED? pausedPrev : state;
  bool running = (state==S_WORK||state==S_BREAK||state==S_LONG_BREAK);

  uint16_t bg=cBlack(), ink=cWhite(), hi=cLine(), edge=cLine();
  uint16_t accent=cMagenta(), accentDK=cMagentaD();
  uint16_t strip = accent;
  if(running){                                  // strip breathes while running
    int p = 205 + (int)(50.0f * (0.5f + 0.5f * sinf(millis() * 0.004f)));
    strip = scale565(accent, p);
  }

  // DONE: magenta/black strobe card
  if(state==S_DONE_FLASH){
    bool on = ((millis()-doneFlashStart)/200)%2==0;
    fbClear(on? accent : bg);
    const char* msg = (pausedPrev==S_WORK) ? "WORK DONE" : "BREAK DONE";
    int dw = textW("DONE!", 4);
    fbDrawTextScaled((WIDTH-dw)/2, 56, "DONE!", on? bg : ink, 4);
    int mw = textW(msg, FS2);
    fbDrawTextScaled((WIDTH-mw)/2, 120, msg, on? bg : accent, FS2);
    int hw = textW("TAP TO CONTINUE", FS2);
    fbDrawTextScaled((WIDTH-hw)/2, 160, "TAP TO CONTINUE", on? bg : accentDK, FS2);
    return;
  }

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
  if(running) showColon=(millis()%1000)<750;
  else if(state==S_PAUSED) showColon=(millis()/400)%2==0;
  else showColon=true;

  fbClear(bg);

  // header: POMO + live pip left, preset centered, state right (2x)
    fbDrawTextScaled(12, HDR_Y, "POMO", ink, FS2);
    {
      bool on = running ? ((millis()/500)%2==0) : true;
      fbFillRect(46, HDR_Y+1, 6, 6, on? cGreen() : hi);
    }
    const char* sn = stateName(eff); if(state==S_PAUSED) sn="PAUSED";
    int sw = textW(sn, FS2);
    fbDrawTextScaled(WIDTH-12-sw, HDR_Y, sn, cGreen(), FS2);
    {
      const char* pl = presets[curPreset].label;
      int pw = textW(pl, FS2);
      fbDrawTextScaled((WIDTH-pw)/2, HDR_Y, pl, ink, FS2);   // centered on screen
    }

  // magenta accent strip
  fbHLine(8, BAR_Y, WIDTH-16, accentDK);
  fbHLine(8, BAR_Y+1, WIDTH-16, accentDK);
  fbFillRect(8, BAR_Y+2, WIDTH-16, 8, strip);

  // ── typographic countdown (block font, scale 14) ──
  char tbuf[8];
  snprintf(tbuf, sizeof(tbuf), "%02d:%02d", mins, secs);
  if(!showColon) tbuf[2]=' ';
  fbDrawTextScaled((WIDTH-textW(tbuf,8))/2, DIG_Y, tbuf, ink, 8);

  // ── progress bar (framed, green fill) ──
  int barX=12, barY=PROG_Y, barW=WIDTH-24, barH=16;
  fbRect(barX, barY, barW, barH, edge);
  fbRect(barX+1, barY+1, barW-2, barH-2, hi);
  int innerW = barW-4, innerH = barH-4;
  fbFillRect(barX+2, barY+2, innerW, innerH, bg);
  int fillW = (int)(innerW * progress);
  if(fillW > 0) fbFillRect(barX+2, barY+2, fillW, innerH, cGreen());
  if(fillW > 24){                               // shimmer sweeps the fill
    int span = fillW - 20;
    int hx = barX + 2 + (int)((millis()/10) % span);
    fbFillRect(hx, barY+3, 20, innerH-2, scale565(cGreen(), 340));
  }

  // ── SET row with n/4 fraction ──
  fbDrawTextScaled(12, SET_Y+4, "SET", ink, FS2);
  for(int i=0;i<DOT_COUNT;i++){
    int x = 104 + i*32;
    if(i<completedInSet) fbFillRect(x,SET_Y,16,16,accent);
    else if(i==completedInSet && (state==S_WORK||(state==S_PAUSED&&pausedPrev==S_WORK))){
      if((millis()/400)%2==0) fbFillRect(x,SET_Y,16,16,accent);
      else fbRect(x,SET_Y,16,16,hi);
    } else fbRect(x,SET_Y,16,16,hi);
  }
  {
    int cur = completedInSet + (state==S_WORK||state==S_LONG_BREAK|| (state==S_PAUSED&&pausedPrev==S_WORK) ? 1 : 0);
    if(cur>4) cur=4; if(cur<1) cur=1;
    if(state==S_IDLE) cur=1;
    char frac[8]; snprintf(frac,sizeof(frac),"%d/4",cur);
    fbDrawTextScaled(WIDTH-12-textW(frac,FS2), SET_Y+4, frac, ink, FS2);
  }

  // ── footer: left = tap action, right = hold action ──
  fbHLine(0, DIV_Y, WIDTH, hi);
  const char* a; const char* b;
  if(state==S_IDLE){ a="TAP START"; b="HOLD PRESET"; }
  else if(state==S_PAUSED){ a="TAP RESUME"; b="HOLD RESET"; }
  else { a="TAP PAUSE"; b="HOLD RESET"; }
  fbDrawTextScaled(12, FTR_Y, a, touchDown? cGreen() : ink, FS2);
  fbDrawTextScaled(WIDTH-12-textW(b,FS2), FTR_Y, b, ink, FS2);
}

// A single element glitches in short bursts. Time is cut into 6s slots; only
// a fraction of an element's slots fire, each burst lands at a random offset
// inside its slot, so events stay ~6s apart on average and never line up. ──
static bool elemHit(int i, uint32_t t, int &dx){
  const uint32_t SLOT = 6000;
  uint32_t phased = t + (uint32_t)(i * 1013u);        // stagger the elements
  uint32_t slot = phased / SLOT;
  uint32_t s = vhsNoise(slot * 2654435761u ^ ((uint32_t)(i + 3) * 97u));
  if((s % 6u) != 0) return false;                     // most slots stay quiet
  uint32_t start = (s >> 8) % (SLOT - 400u);
  uint32_t dur   = 140u + ((s >> 20) % 160u);         // 140..300 ms burst
  uint32_t local = phased % SLOT;
  if(local < start || local >= start + dur) return false;
  dx = (int)((s >> 12) % 7u) - 3;
  return true;
}

// ── Animated VHS filter (colour-only): a soft global chroma ripple plus
// per-element glitch hits. Each UI band — header, strip, countdown, progress,
// SET, footer — tears sideways on its own random schedule, so no full-width
// bars appear and no two elements glitch at the same moment. Time is taken
// from vhsT so all strips of a frame stay in sync. ──
void vhsApply(int h){
  memcpy(vhsSrc, fb, (size_t)WIDTH * h * sizeof(uint16_t));

  uint32_t t = vhsT;
  float ph = (float)t * 0.0018f;        // slow chroma ripple

  // per-element row bands: header, strip, countdown, progress, SET, footer
  static const int bandY0[6] = {   8, 25, 62, 166, 190, 222 };
  static const int bandY1[6] = {  18, 36,129, 186, 210, 234 };
  int bDx[6], bBoost[6]; float bChroma[6];
  for(int i=0; i<6; i++){
    bool hit = elemHit(i, t, bDx[i]);
    bDx[i]     = hit ? bDx[i] : 0;
    bChroma[i] = hit ? 5.0f : 0.0f;
    bBoost[i]  = hit ? 3 : 0;
  }

  int flick = 246 + (int)(vhsNoise(t / 110u) % 11); // 246..256 tape flicker

  for(int row=0; row<h; row++){
    int y = fbTop + row;
    float lsh = 1.4f + 1.3f * sinf(y * 0.085f + ph);
    float rsh = 1.8f + 2.1f * sinf(y * 0.061f + 1.7f + ph * 1.3f);
    int boost = 0, xOff = 0;

    for(int i=0; i<6; i++){
      if(y >= bandY0[i] && y < bandY1[i]){
        lsh += bChroma[i]; rsh += bChroma[i];
        boost += bBoost[i];
        xOff  += bDx[i];
        break;
      }
    }
    if(lsh < 0.0f) lsh = 0.0f;
    if(rsh < 0.0f) rsh = 0.0f;
    int li0 = (int)lsh, lfr = (int)((lsh - li0) * 256.0f);
    int ri0 = (int)rsh, rfr = (int)((rsh - ri0) * 256.0f);

    const uint16_t* s = &vhsSrc[row * WIDTH];
    uint16_t* d = &fb[row * WIDTH];
    for(int x=0; x<WIDTH; x++){
      int cx = x + xOff;
      if(cx < 0) cx = 0; if(cx > WIDTH-1) cx = WIDTH-1;
      uint16_t cC = s[cx];
      int lp = cx - li0, lq = lp - 1;
      if(lp < 0) lp = 0; if(lq < 0) lq = 0;
      int rp = cx + ri0, rq = rp + 1;
      if(rp > WIDTH-1) rp = WIDTH-1; if(rq > WIDTH-1) rq = WIDTH-1;
      uint16_t la = s[lp], lb = s[lq], ra = s[rp], rb = s[rq];
      int rSrc = (int)(((((la>>11)&0x1F) * (256-lfr)) + (((lb>>11)&0x1F) * lfr)) >> 8);
      int bSrc = (int)((((ra&0x1F) * (256-rfr)) + ((rb&0x1F) * rfr)) >> 8);
      int r = (3*rSrc + ((cC>>11)&0x1F)) >> 2;  // 75% shift = stronger fringe
      int g = (cC>>5)&0x3F;
      int b = (3*bSrc + (cC&0x1F)) >> 2;

      r = (r * flick) >> 8; g = (g * flick) >> 8; b = (b * flick) >> 8;
      r += boost; g += boost; b += boost;

      if(r < 0) r = 0; if(r > 31) r = 31;
      if(g < 0) g = 0; if(g > 63) g = 63;
      if(b < 0) b = 0; if(b > 31) b = 31;
      d[x] = (uint16_t)((r<<11) | (g<<5) | b);
    }
  }
}

void render(){
  vhsT = millis();
  for (fbTop = 0; fbTop < HEIGHT; fbTop += FB_H) {
    if (introMode) dm01Draw(dm01Elapsed()); else drawFrame();
    int h = FB_H;
    if (fbTop + h > HEIGHT) h = HEIGHT - fbTop;
    if (!introMode) vhsApply(h);
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
