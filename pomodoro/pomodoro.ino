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
static uint16_t vhsSrc[WIDTH * HEIGHT];
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

// ── Departure Mono pixel fonts. Labels use the 9px set
// (DepartureMonoSmall.h); the countdown and DONE use the 11px set
// (DepartureMono.h). Both sit behind the old function names. ──
#include "DepartureMono.h"
#include "DepartureMonoSmall.h"

static inline const uint8_t* dmGlyph(char ch){
  if(ch < 32 || ch > 90) return dmFont[0];       // space fallback
  return dmFont[ch - 32];
}
static inline const uint8_t* dmsGlyph(char ch){
  if(ch < 32 || ch > 90) return dmsFont[0];      // space fallback
  return dmsFont[ch - 32];
}
void fbDrawChar57(int x,int y,char ch,uint16_t c,int scale){
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
void fbDrawText1x(int x,int y,const char* s,uint16_t c){   // small label font
  while(*s){
    if(*s != ' '){
      const uint8_t* g = dmsGlyph(*s);
      int cy = y - DMS_TOP;
      for(int row=0; row<DMS_H; row++){
        uint8_t bits = g[row];
        if(!bits) continue;
        for(int col=0; col<DMS_ADV; col++)
          if(bits & (0x40>>col)) fbFillRect(x+col, cy+row, 1, 1, c);
      }
    }
    x += DMS_ADV; s++;
  }
}
void fbDrawTextScaled(int x,int y,const char* s,uint16_t c,int scale){
  while(*s){ fbDrawChar57(x,y,*s,c,scale); x += DM_ADV*scale; s++; }
}
int textW57(const char* s,int scale){
  int n=0; while(*s++)n++;
  if(!n) return 0;
  return (scale==1)? (n*DMS_ADV - 1) : (n*DM_ADV*scale - scale);
}

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

// ── Rendering palette: dark-grey deck, white ink, magenta + signal-green
// accents. Palette: #101010, white, magenta #EC008C, signal green #00E676. ──
uint16_t cBlack(){   return rgb( 16,  16,  16); } // #101010
uint16_t cWhite(){   return rgb(255, 255, 255); }
uint16_t cMagenta(){ return rgb(236,   0, 140); } // #EC008C
uint16_t cGreen(){   return rgb(  0, 230, 118); } // #00E676
uint16_t cLine(){    return rgb(170, 170, 170); } // light grey rules
uint16_t cMagentaD(){return rgb(104,   0,  62); } // dimmed magenta edging

uint16_t scale565(uint16_t c, int pct){    // pct/256 brightness (over 256 brightens)
  int r=(c>>11)&0x1F, g=(c>>5)&0x3F, b=c&0x1F;
  r=r*pct>>8; g=g*pct>>8; b=b*pct>>8;
  if(r>31)r=31; if(g>63)g=63; if(b>31)b=31;
  return (uint16_t)((r<<11)|(g<<5)|b);
}
static inline uint32_t vhsNoise(uint32_t x){
  x ^= x>>16; x *= 0x7feb352dU; x ^= x>>15; x *= 0x846ca68bU; x ^= x>>16;
  return x;
}

// ── Animated VHS filter (colour-only): a soft global chroma ripple plus
// per-element glitch hits. Each UI band — header, strip, countdown, progress,
// SET, footer — tears sideways on its own random schedule, so no full-width
// bars appear and no two elements glitch at the same moment. ──
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

void vhsApply(){
  memcpy(vhsSrc, fb, sizeof(fb));
  uint32_t t = millis();
  float ph = (float)t * 0.0018f;        // slow chroma ripple

  // per-element row bands: header, strip, countdown, progress, SET, footer
  static const int bandY0[6] = {  2, 12, 38, 85,  99, 113 };
  static const int bandY1[6] = { 12, 19, 74, 95, 109, 128 };
  int bDx[6], bBoost[6]; float bChroma[6];
  for(int i=0; i<6; i++){
    bool hit = elemHit(i, t, bDx[i]);
    bDx[i]     = hit ? bDx[i] : 0;
    bChroma[i] = hit ? 5.0f : 0.0f;
    bBoost[i]  = hit ? 3 : 0;
  }

  int flick = 246 + (int)(vhsNoise(t / 110u) % 11); // 246..256 tape flicker

  for(int y=0; y<HEIGHT; y++){
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
    int li0=(int)lsh, lfr=(int)((lsh-li0)*256.0f);
    int ri0=(int)rsh, rfr=(int)((rsh-ri0)*256.0f);

    const uint16_t* s = &vhsSrc[y*WIDTH];
    uint16_t* d = &fb[y*WIDTH];
    for(int x=0; x<WIDTH; x++){
      int cx = x + xOff;
      if(cx < 0) cx = 0; if(cx > WIDTH-1) cx = WIDTH-1;
      uint16_t cC = s[cx];
      int lp = cx-li0, lq = lp-1;
      if(lp < 0) lp = 0; if(lq < 0) lq = 0;
      int rp = cx+ri0, rq = rp+1;
      if(rp > WIDTH-1) rp = WIDTH-1; if(rq > WIDTH-1) rq = WIDTH-1;
      uint16_t la=s[lp], lb=s[lq], ra=s[rp], rb=s[rq];
      int rSrc=(int)(((((la>>11)&0x1F)*(256-lfr))+(((lb>>11)&0x1F)*lfr))>>8);
      int bSrc=(int)((((ra&0x1F)*(256-rfr))+((rb&0x1F)*rfr))>>8);
      int r=(3*rSrc+((cC>>11)&0x1F))>>2;  // 75% blend = stronger fringe
      int g=(cC>>5)&0x3F;
      int b=(3*bSrc+(cC&0x1F))>>2;
      r=(r*flick)>>8; g=(g*flick)>>8; b=(b*flick)>>8;
      r+=boost; g+=boost; b+=boost;
      if(r<0)r=0; if(r>31)r=31;
      if(g<0)g=0; if(g>63)g=63;
      if(b<0)b=0; if(b>31)b=31;
      d[x]=(uint16_t)((r<<11)|(g<<5)|b);
    }
  }
}

void render(){
  State eff = state==S_PAUSED? pausedPrev : state;
  bool running = (state==S_WORK||state==S_BREAK||state==S_LONG_BREAK);
  uint16_t bg=cBlack(), ink=cWhite(), hi=cLine(), edge=cLine();
  uint16_t accent=cMagenta(), accentDK=cMagentaD();
  uint16_t strip = accent;
  if(running){                                  // strip breathes while running
    int p = 205 + (int)(50.0f*(0.5f+0.5f*sinf(millis()*0.004f)));
    strip = scale565(accent, p);
  }

  // DONE: magenta/black strobe card
  if(state==S_DONE_FLASH){
    bool on = ((millis()-doneFlashStart)/200)%2==0;
    fbClear(on? accent : bg);
    fbDrawTextScaled((WIDTH-textW57("DONE!",2))/2, 30, "DONE!", on? bg : ink, 2);
    const char* msg = (pausedPrev==S_WORK) ? "WORK DONE" : "BREAK DONE";
    fbDrawText1x((WIDTH-textW57(msg,1))/2, 66, msg, on? bg : accent);
    fbDrawText1x((WIDTH-textW57("TAP TO CONTINUE",1))/2, 88, "TAP TO CONTINUE", on? bg : accentDK);
  } else {

    fbClear(bg);

    // header: POMO + live pip left, preset in the middle gap, state right
    fbDrawText1x(6, 3, "POMO", ink);
    {
      bool on = running ? ((millis()/500)%2==0) : true;
      fbFillRect(34, 5, 4, 4, on? cGreen() : hi);
    }
    const char* sn = stateName(eff); if(state==S_PAUSED) sn="PAUSED";
    int sw = textW57(sn,1);
    fbDrawText1x(WIDTH-6-sw, 3, sn, cGreen());
    {
      const char* pl = presets[curPreset].label;
      int pw = textW57(pl,1);
      int lo = 6 + textW57("POMO",1) + 14;   // after POMO + pip
      int hiRight = WIDTH-6-sw;
      fbDrawText1x(lo + (hiRight-lo-pw)/2, 3, pl, ink);
    }

    // magenta accent strip
    fbHLine(4, 13, WIDTH-8, accentDK);
    fbFillRect(4, 14, WIDTH-8, 4, strip);

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

    // ── typographic countdown (block font, scale 6) ──
    char tbuf[8];
    snprintf(tbuf, sizeof(tbuf), "%02d:%02d", mins, secs);
    if(!showColon) tbuf[2]=' ';
    fbDrawTextScaled((WIDTH-textW57(tbuf,4))/2, 40, tbuf, ink, 4);

    // ── progress bar (framed, green fill + shimmer) ──
    int barX=6, barY=86, barW=WIDTH-12, barH=8;
    fbRect(barX, barY, barW, barH, edge);
    fbRect(barX+1, barY+1, barW-2, barH-2, hi);
    int innerW = barW-4, innerH = barH-4;
    fbFillRect(barX+2, barY+2, innerW, innerH, bg);
    int fillW = (int)(innerW * progress);
    if(fillW > 0) fbFillRect(barX+2, barY+2, fillW, innerH, cGreen());
    if(fillW > 12){                               // shimmer sweeps the fill
      int span = fillW - 10;
      int hx = barX+2 + (int)((millis()/10) % span);
      fbFillRect(hx, barY+3, 10, innerH-2, scale565(cGreen(), 340));
    }

    // ── SET row with 1/4 fraction ──
    int dotY = 100;
    fbDrawText1x(6, dotY, "SET", ink);
    for(int i=0;i<DOT_COUNT;i++){
      int x = 52 + i*16;
      if(i<completedInSet) fbFillRect(x,dotY,8,8,accent);
      else if(i==completedInSet && (state==S_WORK||(state==S_PAUSED&&pausedPrev==S_WORK))){
        if((millis()/400)%2==0) fbFillRect(x,dotY,8,8,accent);
        else fbRect(x,dotY,8,8,hi);
      } else fbRect(x,dotY,8,8,hi);
    }
    {
      int cur = completedInSet + (state==S_WORK||state==S_LONG_BREAK|| (state==S_PAUSED&&pausedPrev==S_WORK) ? 1 : 0);
      if(cur>4) cur=4; if(cur<1) cur=1;
      if(state==S_IDLE) cur=1;
      char frac[8]; snprintf(frac,sizeof(frac),"%d/4",cur);
      fbDrawText1x(WIDTH-6-textW57(frac,1), dotY, frac, ink);
    }

    // ── footer ──
    fbHLine(0, 114, WIDTH, hi);
    int fy = 119;
    const char* a; const char* b;
    if(state==S_IDLE){ a="TAP START"; b="HOLD PRESET"; }
    else if(state==S_PAUSED){ a="TAP RESUME"; b="HOLD RESET"; }
    else { a="TAP PAUSE"; b="HOLD PRESET"; }
    fbDrawText1x(6, fy, a, ink);
    fbDrawText1x(WIDTH-6-textW57(b,1), fy, b, ink);
  }
  vhsApply();
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
