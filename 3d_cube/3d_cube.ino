#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>
#include <Adafruit_NeoPixel.h>
#include "Screenshot.h"
#include <math.h>
#include <string.h>

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
#define CX (WIDTH/2)
#define CY (HEIGHT/2)
#define MAX_BALLS 248
int nb = 1;
#define CUBE_BOUND 0.85
#define GRID_SUBDIV 2
#define DMAX_X 0.42f
#define REST_V 0.015f       // below this normal speed, contacts rest instead of bounce
#define PHYS_H (1.0f/60.0f) // fixed physics step: motion independent of frame rate

typedef struct { float x,y,z; } Vec3;

uint16_t fb[WIDTH*HEIGHT];
#define FPIX(x,y,c) if((x)>=0&&(x)<WIDTH&&(y)>=0&&(y)<HEIGHT) fb[(y)*WIDTH+(x)]=(c)

void fbClear(uint16_t c) { for (int i = 0; i < WIDTH * HEIGHT; i++) fb[i] = c; }

void fbHLine(int x, int y, int w, uint16_t c) {
  if (y<0||y>=HEIGHT) return;
  if (x<0) { w+=x; x=0; }
  if (x+w>WIDTH) w=WIDTH-x;
  if (w<=0) return;
  uint16_t* p=&fb[y*WIDTH+x];
  while (w--) *p++=c;
}

void fbFillCircle(int cx, int cy, int r, uint16_t c) {
  int x=0, y=r, d=3-2*r;
  while (x<=y) {
    fbHLine(cx-x,cy-y,2*x+1,c); fbHLine(cx-x,cy+y,2*x+1,c);
    fbHLine(cx-y,cy-x,2*y+1,c); fbHLine(cx-y,cy+x,2*y+1,c);
    if (d<0) d+=4*x+6; else { d+=4*(x-y)+10; y--; }
    x++;
  }
}

void fbDrawCircle(int cx, int cy, int r, uint16_t c) {
  int x=0, y=r, d=3-2*r;
  while (x<=y) {
    FPIX(cx+x,cy+y,c); FPIX(cx-x,cy+y,c);
    FPIX(cx+x,cy-y,c); FPIX(cx-x,cy-y,c);
    FPIX(cx+y,cy+x,c); FPIX(cx-y,cy+x,c);
    FPIX(cx+y,cy-x,c); FPIX(cx-y,cy-x,c);
    if (d<0) d+=4*x+6; else { d+=4*(x-y)+10; y--; }
    x++;
  }
}

void fbDrawLine(int x0, int y0, int x1, int y1, uint16_t c) {
  int dx=abs(x1-x0), sx=x0<x1?1:-1;
  int dy=-abs(y1-y0), sy=y0<y1?1:-1;
  int err=dx+dy, e2;
  for (;;) {
    FPIX(x0,y0,c);
    if (x0==x1 && y0==y1) break;
    e2=2*err;
    if (e2>=dy) { err+=dy; x0+=sx; }
    if (e2<=dx) { err+=dx; y0+=sy; }
  }
}

void fbFillRect(int x, int y, int w, int h, uint16_t c) {
  for (int i = 0; i < h; i++) fbHLine(x, y + i, w, c);
}

void fbFlush() {
  tft.startWrite();
  tft.setAddrWindow(0, 0, WIDTH, HEIGHT);
  tft.writePixels(fb, WIDTH*HEIGHT);
  tft.endWrite();
}

// ─── Departure Mono (rasterized). Labels use the 9px set, counters the 11px set.
//     y is the cap/digit top; cells are drawn at y - <PFX>_TOP. ────
#include "DepartureMono.h"
#include "DepartureMonoSmall.h"

static inline const uint8_t* dmsGlyph(char ch){
  if (ch < 32 || ch > 90) return dmsFont[0];
  return dmsFont[ch - 32];
}
static inline const uint8_t* dmGlyph(char ch){
  if (ch < 32 || ch > 90) return dmFont[0];
  return dmFont[ch - 32];
}

void fbDrawChar57(int x, int y, char ch, uint16_t c) {   // small label font
  if (ch == ' ') return;
  const uint8_t* g = dmsGlyph(ch);
  int cy = y - DMS_TOP;
  for (int row = 0; row < DMS_H; row++) {
    uint8_t bits = g[row];
    if (!bits) continue;
    for (int col = 0; col < DMS_ADV; col++)
      if (bits & (0x40 >> col)) FPIX(x + col, cy + row, c);
  }
}

void fbText57(int x, int y, const char* s, uint16_t c) {
  while (*s) { fbDrawChar57(x, y, *s, c); x += DMS_ADV; s++; }
}

int textW57(const char* s) { int n = 0; while (*s++) n++; return n ? n * DMS_ADV - 1 : 0; }

void fbDrawNum11(int x, int y, int num, uint16_t c) {    // 11px counters
  char buf[8];
  snprintf(buf, sizeof(buf), "%d", num < 0 ? 0 : num);
  const char* s = buf;
  int cy = y - DM_TOP;
  while (*s) {
    const uint8_t* g = dmGlyph(*s);
    for (int row = 0; row < DM_H; row++) {
      uint8_t bits = g[row];
      if (!bits) continue;
      for (int col = 0; col < DM_W; col++)
        if (bits & (0x40 >> col)) FPIX(x + col, cy + row, c);
    }
    x += DM_ADV; s++;
  }
}
int numW11(const char* s) { int n = 0; while (*s++) n++; return n ? n * DM_ADV - 1 : 0; }

// ─── Palette (SIGNAL theme) ────
uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) { return tft.color565(r,g,b); }
uint16_t cBlack()   { return rgb( 16,  16,  16); } // #101010 deck
uint16_t cWhite()   { return rgb(255, 255, 255); }
uint16_t cMagenta() { return rgb(236,   0, 140); } // #EC008C
uint16_t cGreen()   { return rgb(  0, 230, 118); } // #00E676
uint16_t cLine()    { return rgb(170, 170, 170); } // light grey rules
uint16_t cMagentaD(){ return rgb(104,   0,  62); } // dimmed magenta edging

// ─── VHS filter (colour-only): slow chroma ripple + per-element glitch hits.
//     Hits fire on their own 6s slots so HUD rows never tear at the same time.
//     Only a single row is buffered (the taps read horizontal neighbours). ────
static uint16_t vhsRow[WIDTH];

static inline uint32_t vhsNoise(uint32_t x){
  x ^= x>>16; x *= 0x7feb352dU; x ^= x>>15; x *= 0x846ca68bU; x ^= x>>16;
  return x;
}

static bool elemHit(int i, uint32_t t, int &dx){
  const uint32_t SLOT = 6000;
  uint32_t phased = t + (uint32_t)(i * 1013u);
  uint32_t s = vhsNoise(phased / SLOT * 2654435761u ^ ((uint32_t)(i + 3) * 97u));
  if ((s % 6u) != 0) return false;
  uint32_t start = (s >> 8) % (SLOT - 400u);
  uint32_t dur   = 140u + ((s >> 20) % 160u);
  uint32_t local = phased % SLOT;
  if (local < start || local >= start + dur) return false;
  dx = (int)((s >> 12) % 7u) - 3;
  return true;
}

void vhsApply(){
  uint32_t t = millis();
  float ph = (float)t * 0.0018f;

  // per-element bands: top HUD, accent strip, bottom hint (the scene keeps only
  // the global ripple — a full-scene sideways shift would hurt readability)
  static const int bandY0[3] = {   0, 14, 118 };
  static const int bandY1[3] = {  14, 19, 128 };
  int bDx[3], bBoost[3]; float bChroma[3];
  for (int i=0;i<3;i++){
    bool hit = elemHit(i, t, bDx[i]);
    bDx[i]     = hit ? bDx[i] : 0;
    bChroma[i] = hit ? 5.0f : 0.0f;
    bBoost[i]  = hit ? 3 : 0;
  }

  int flick = 246 + (int)(vhsNoise(t / 110u) % 11);

  for (int y=0;y<HEIGHT;y++){
    // Only the HUD bands are filtered: the 3D scene stays crisp, and skipping
    // its rows keeps the per-frame cost tiny (the physics runs real-time again).
    int bi = -1;
    for (int i=0;i<3;i++) if (y>=bandY0[i] && y<bandY1[i]) { bi=i; break; }
    if (bi < 0) continue;
    float lsh = 1.4f + 1.3f * sinf(y*0.085f + ph) + bChroma[bi];
    float rsh = 1.8f + 2.1f * sinf(y*0.061f + 1.7f + ph*1.3f) + bChroma[bi];
    int boost = bBoost[bi], xOff = bDx[bi];
    if (lsh<0) lsh=0; if (rsh<0) rsh=0;
    int li0=(int)lsh, lfr=(int)((lsh-li0)*256.0f);
    int ri0=(int)rsh, rfr=(int)((rsh-ri0)*256.0f);
    memcpy(vhsRow, &fb[y*WIDTH], WIDTH*sizeof(uint16_t));
    const uint16_t* s = vhsRow;
    uint16_t* d = &fb[y*WIDTH];
    for (int x=0;x<WIDTH;x++){
      int cx = x + xOff;
      if (cx<0) cx=0; if (cx>WIDTH-1) cx=WIDTH-1;
      uint16_t cC = s[cx];
      int lp=cx-li0, lq=lp-1; if(lp<0)lp=0; if(lq<0)lq=0;
      int rp=cx+ri0, rq=rp+1; if(rp>WIDTH-1)rp=WIDTH-1; if(rq>WIDTH-1)rq=WIDTH-1;
      uint16_t la=s[lp], lb=s[lq], ra=s[rp], rb=s[rq];
      int rSrc=(int)(((((la>>11)&0x1F)*(256-lfr))+(((lb>>11)&0x1F)*lfr))>>8);
      int bSrc=(int)((((ra&0x1F)*(256-rfr))+((rb&0x1F)*rfr))>>8);
      int r=(3*rSrc+((cC>>11)&0x1F))>>2;   // 75% shift = stronger fringe
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

// ─── Cube ────
Vec3 cv[8] = {{-1,-1,-1},{1,-1,-1},{1,1,-1},{-1,1,-1},{-1,-1,1},{1,-1,1},{1,1,1},{-1,1,1}};
int ee[12][2] = {{0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},{0,4},{1,5},{2,6},{3,7}};

// ─── Balls ────
Vec3 pos[MAX_BALLS], vel[MAX_BALLS];
float rad[MAX_BALLS], bounce[MAX_BALLS];
uint16_t cols[MAX_BALLS], hueBase[MAX_BALLS];
int nextHue = 0, orderX[MAX_BALLS];
Vec3 wp[MAX_BALLS];
int px[MAX_BALLS], py[MAX_BALLS];
float pd[MAX_BALLS];
int order[MAX_BALLS];

// ─── Camera ────
float ax=0, ay=0, az=0;
int dir = 1;
bool lt = false;
float rot[9];
float camDist = 3.0, focalLen = 96.0;
bool director = false;
unsigned long directorStart = 0, segDur = 20000;
int flyKind = 0, flyS = 1;
float flyA = 0, flyB = 0, flyC = 0;
float spinCur = 1.0f;
int panX = 0, panY = 0;
float rollA = 0, cosR = 1.0f, sinR = 0.0f;
int fpsShown = 0;

void compRot(float a, float b, float c) {
  float cx=cosf(a),sx=sinf(a),cy=cosf(b),sy=sinf(b),cz=cosf(c),sz=sinf(c);
  rot[0]=cy*cz+sx*sy*sz; rot[1]=-cx*sz; rot[2]=sy*cz-sx*cy*sz;
  rot[3]=cy*sz-sx*sy*cz; rot[4]=cx*cz;  rot[5]=sy*sz+sx*cy*cz;
  rot[6]=-cx*sy;         rot[7]=sx;     rot[8]=cx*cy;
}

void aRot(Vec3* p) {
  float x=p->x*rot[0]+p->y*rot[1]+p->z*rot[2];
  float y=p->x*rot[3]+p->y*rot[4]+p->z*rot[5];
  float z=p->x*rot[6]+p->y*rot[7]+p->z*rot[8];
  p->x=x; p->y=y; p->z=z;
}

void proj(Vec3 p, int* sx, int* sy, float* d) {
  float z = p.z + camDist;
  if (z < 0.3f) z = 0.3f;
  *d = z;
  float k = focalLen / z;
  float ox = p.x * k, oy = p.y * k;
  *sx = CX + panX + (int)(ox * cosR - oy * sinR);
  *sy = CY + panY + (int)(ox * sinR + oy * cosR);
}

uint16_t hsv(float h, float s, float v) {
  h = fmodf(h, 360.0f);
  float c = v * s;
  float x = c * (1 - fabsf(fmodf(h/60.0f, 2) - 1));
  float m = v - c;
  float r,g,b;
  if (h<60)       {r=c;g=x;b=0;}
  else if (h<120) {r=x;g=c;b=0;}
  else if (h<180) {r=0;g=c;b=x;}
  else if (h<240) {r=0;g=x;b=c;}
  else if (h<300) {r=x;g=0;b=c;}
  else            {r=c;g=0;b=x;}
  return tft.color565((uint8_t)((r+m)*255),(uint8_t)((g+m)*255),(uint8_t)((b+m)*255));
}

static uint16_t hueLutBall[360], hueLutEdge[360];

static inline uint16_t scale565(uint16_t c, uint16_t b) {
  uint16_t r = (((c>>11)&0x1F)*b)>>8;
  uint16_t g = (((c>>5)&0x3F)*b)>>8;
  uint16_t bl = ((c&0x1F)*b)>>8;
  return (uint16_t)((r<<11)|(g<<5)|bl);
}

// Restitution ramp: gentle taps rest (0), hard hits bounce (1), smooth in between
static inline float restRamp(float impact) {
  float q=(impact-REST_V)*20.0f;
  if (q<0) q=0; else if (q>1) q=1;
  return q*q*(3.0f-2.0f*q);
}

static inline void sortDepth() {
  for (int i=1;i<nb;i++) {
    int v=order[i]; float vd=pd[v]; int j=i-1;
    while (j>=0 && pd[order[j]]<vd) { order[j+1]=order[j]; j--; }
    order[j+1]=v;
  }
}

static inline void sortByX() {
  for (int i=1;i<nb;i++) {
    int v=orderX[i]; float vx=pos[v].x; int j=i-1;
    while (j>=0 && pos[orderX[j]].x>vx) { orderX[j+1]=orderX[j]; j--; }
    orderX[j+1]=v;
  }
}

void addBall(int i) {
  float h = i * (360.0f / MAX_BALLS);
  float radH = h * (PI / 180.0f);
  float weight = 1.1f + cosf(radH) * 0.7f;
  rad[i] = 0.07f + weight * 0.07f;
  bounce[i] = 0.88f - weight * 0.22f;
  // Spawn sampling: try 6 spots, keep the clearest (no interpenetration pop)
  float bestX=0, bestY=0, bestZ=0, bestD=-1;
  for (int k=0;k<6;k++) {
    float cx=random(2000)*0.0008f - 0.8f;
    float cy=random(2000)*0.0008f - 0.8f;
    float cz=random(2000)*0.0008f - 0.8f;
    float mind=1e9f;
    for (int m=0;m<i;m++) {
      float ox=cx-pos[m].x, oy=cy-pos[m].y, oz=cz-pos[m].z;
      float d2=ox*ox+oy*oy+oz*oz;
      if (d2<mind) mind=d2;
    }
    if (mind>bestD) { bestD=mind; bestX=cx; bestY=cy; bestZ=cz; }
  }
  pos[i].x=bestX; pos[i].y=bestY; pos[i].z=bestZ;
  float speedMul = 1.6f - weight * 0.6f;
  vel[i].x = (random(400)*0.0001f - 0.02f) * speedMul;
  vel[i].y = (random(400)*0.0001f - 0.02f) * speedMul;
  vel[i].z = (random(400)*0.0001f - 0.02f) * speedMul;
  hueBase[i] = (uint16_t)nextHue;
  nextHue += 137;
  if (nextHue >= 360) nextHue -= 360;
  cols[i] = hueLutBall[hueBase[i]];
}

#include "Dm01Intro.h"

void setup() {
  Serial.begin(115200);
  pinMode(TOUCH_PIN, INPUT);
  pixels.begin(); pixels.setBrightness(0); pixels.clear(); pixels.show();
  SPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);
  tft.initR(INITR_BLACKTAB);
  tft.setSPISpeed(40000000);
  tft.setRotation(1);
  tft.fillScreen(ST7735_BLACK);

  for (int h=0;h<360;h++) {
    hueLutBall[h] = hsv((float)h, 1.0f, 0.8f);
    hueLutEdge[h] = hsv((float)h, 1.0f, 0.62f);
  }

  compRot(0,0,0); // identity rotation so physics has valid gravity on frame one
  addBall(0);
  for (int i=0;i<nb;i++) { orderX[i]=i; order[i]=i; }
  dm01Start();
}

void loop() {
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
  if (!introEnded && !dm01IsActive()) { introEnded = true; pixels.setBrightness(0); }

  unsigned long t = millis();
  float tSec = t * 0.001f;

  static unsigned long fpsLast = 0;
  static int fpsFrames = 0;
  fpsFrames++;
  if (t - fpsLast >= 1000) {
    fpsShown = (int)(fpsFrames * 1000UL / (t - fpsLast));
    fpsFrames = 0;
    fpsLast = t;
  }

  bool touched = (digitalRead(TOUCH_PIN)==HIGH);
  if (touched && !lt) {
    dir = -dir;
    if (nb < MAX_BALLS) { addBall(nb); orderX[nb]=nb; order[nb]=nb; nb++; }
  }
  lt = touched;

  // ─── Physics: fixed 60Hz steps, decoupled from frame rate ────
  {
    static float physAcc = 0;
    static unsigned long physPrev = 0;
    if (physPrev == 0) physPrev = t;
    float dtF = (t - physPrev) * 0.001f;
    physPrev = t;
    if (dtF < 0) dtF = 0; else if (dtF > 0.1f) dtF = 0.1f;
    physAcc += dtF;
    const float dampF = 0.997f; // per-step air drag
    const float gravH = 0.012f; // per-step gravity: snappier, physical falls
    int steps = 0;
    while (physAcc >= PHYS_H && steps < 3) {
      physAcc -= PHYS_H;
      steps++;
      for (int i=0;i<nb;i++) {
        // world-locked gravity: screen-down (0,+g,0) rotated into the cube
        // frame via R^T (row 1 of rot[]) so balls slosh as the cube spins
        vel[i].x += gravH * rot[3];
        vel[i].y += gravH * rot[4];
        vel[i].z += gravH * rot[5];
        vel[i].x *= dampF; vel[i].y *= dampF; vel[i].z *= dampF;
      }
      sortByX();
      for (int it=0; it<2; it++) { // 2 relaxation passes → stable piles
        for (int ii=0;ii<nb;ii++) {
          int i=orderX[ii];
          for (int jj=ii+1;jj<nb;jj++) {
            int j=orderX[jj];
            if (pos[j].x-pos[i].x >= DMAX_X) break;
            float dx=pos[i].x-pos[j].x, dy=pos[i].y-pos[j].y, dz=pos[i].z-pos[j].z;
            float d2=dx*dx+dy*dy+dz*dz;
            float minDist=rad[i]+rad[j];
            if (d2<minDist*minDist && d2>0.000001f) {
              float d=sqrtf(d2);
              float nx=dx/d, ny=dy/d, nz=dz/d;
              float ri3=rad[i]*rad[i]*rad[i], rj3=rad[j]*rad[j]*rad[j];
              float mi=rj3/(ri3+rj3), mj=1-mi;
              float overlap=minDist-d;
              float corr=(overlap-0.004f)*0.8f; // slop + fractional push
              if (corr>0) {
                pos[i].x+=nx*corr*mi; pos[i].y+=ny*corr*mi; pos[i].z+=nz*corr*mi;
                pos[j].x-=nx*corr*mj; pos[j].y-=ny*corr*mj; pos[j].z-=nz*corr*mj;
              }
              float rvx=vel[i].x-vel[j].x, rvy=vel[i].y-vel[j].y, rvz=vel[i].z-vel[j].z;
              float rv=rvx*nx+rvy*ny+rvz*nz;
              if (rv<0) {
                float e=0.5f*(bounce[i]+bounce[j]); // per-pair restitution
                if (e>0.9f) e=0.9f;
                e*=restRamp(-rv);                   // taps thud, hits bounce
                float jImp=-(1.0f+e)*rv;
                vel[i].x-=jImp*mi*nx; vel[i].y-=jImp*mi*ny; vel[i].z-=jImp*mi*nz;
                vel[j].x+=jImp*mj*nx; vel[j].y+=jImp*mj*ny; vel[j].z+=jImp*mj*nz;
                // dry friction: bleed off tangential slip (mass-weighted, can't reverse)
                float tvx=rvx-rv*nx, tvy=rvy-rv*ny, tvz=rvz-rv*nz;
                vel[i].x-=tvx*0.08f*mi; vel[i].y-=tvy*0.08f*mi; vel[i].z-=tvz*0.08f*mi;
                vel[j].x+=tvx*0.08f*mj; vel[j].y+=tvy*0.08f*mj; vel[j].z+=tvz*0.08f*mj;
              }
            }
          }
        }
      }
      for (int i=0;i<nb;i++) {
        pos[i].x+=vel[i].x; pos[i].y+=vel[i].y; pos[i].z+=vel[i].z;
        float sp2=vel[i].x*vel[i].x+vel[i].y*vel[i].y+vel[i].z*vel[i].z;
        if (sp2>0.0625f) { // safety clamp: 0.25/step max
          float s=0.25f/sqrtf(sp2);
          vel[i].x*=s; vel[i].y*=s; vel[i].z*=s;
        } else if (sp2<1e-10f) {
          vel[i].x=0; vel[i].y=0; vel[i].z=0; // sleep snap: no drift
        }
        float b=CUBE_BOUND-rad[i];
        if (pos[i].x>b)       {pos[i].x=b;       vel[i].x=-vel[i].x*bounce[i]*restRamp(vel[i].x);  vel[i].y*=0.92f; vel[i].z*=0.92f;}
        else if (pos[i].x<-b) {pos[i].x=-b;      vel[i].x=-vel[i].x*bounce[i]*restRamp(-vel[i].x); vel[i].y*=0.92f; vel[i].z*=0.92f;}
        if (pos[i].y>b)       {pos[i].y=b;       vel[i].y=-vel[i].y*bounce[i]*restRamp(vel[i].y);  vel[i].x*=0.92f; vel[i].z*=0.92f;}
        else if (pos[i].y<-b) {pos[i].y=-b;      vel[i].y=-vel[i].y*bounce[i]*restRamp(-vel[i].y); vel[i].x*=0.92f; vel[i].z*=0.92f;}
        if (pos[i].z>b)       {pos[i].z=b;       vel[i].z=-vel[i].z*bounce[i]*restRamp(vel[i].z);  vel[i].x*=0.92f; vel[i].y*=0.92f;}
        else if (pos[i].z<-b) {pos[i].z=-b;      vel[i].z=-vel[i].z*bounce[i]*restRamp(-vel[i].z); vel[i].x*=0.92f; vel[i].y*=0.92f;}
      }
    }
    if (steps==3) physAcc=0; // overloaded: drop backlog, never spiral
  }

  uint16_t cycPhase = (uint16_t)((t/25UL)%360UL);
  for (int i=0;i<nb;i++) {
    int k=(int)hueBase[i]+cycPhase;
    if (k>=360) k-=360;
    cols[i]=hueLutBall[k];
  }

  if (t-directorStart>=segDur) {
    directorStart=t;
    if (director) { director=false; segDur=12000UL+random(13000); }
    else {
      director=true; segDur=10000UL+random(8000);
      flyKind=random(3); flyS=random(2)?1:-1;
      if (flyKind==0) { flyA=2.0f+random(80)*0.01f; flyB=random(20); flyC=random(25)*0.01f; }
      else if (flyKind==1) { flyA=15.0f+random(20); flyB=random(20); flyC=random(15)*0.01f; }
      else { flyA=0.5f+random(60)*0.01f; flyB=8.0f+random(10); }
    }
  }
  spinCur+=((director?0.25f:1.0f)-spinCur)*0.04f;

  if (director) {
    float p=(t-directorStart)/(float)segDur;
    float e=sinf(p*3.14159f);
    if (flyKind==0) { camDist=3.6f-(3.6f-flyA)*e; focalLen=112.0f; panX=(int)(flyB*sinf(p*6.2832f)*e*flyS); panY=(int)(flyB*0.5f*sinf(p*12.566f)*e); rollA=flyC*sinf(p*6.2832f)*e*flyS; }
    else if (flyKind==1) { camDist=2.4f+0.5f*sinf(tSec*0.13f); focalLen=112.0f; panX=(int)(flyB*sinf(p*6.2832f+1.0f)*e*flyS); panY=(int)(flyA*sinf(p*6.2832f)*e*flyS); rollA=flyC*sinf(p*12.566f)*e; }
    else { camDist=2.6f; focalLen=106.0f; panX=(int)(flyB*sinf(p*6.2832f)*e*flyS); panY=(int)(flyB*cosf(p*6.2832f)*e); rollA=flyA*sinf(p*6.2832f)*e*flyS; }
  } else {
    camDist=2.6+0.8*sinf(tSec*0.13f);
    focalLen=100.0+28.0*sinf(tSec*0.17f+1.0f);
    panX=0; panY=0; rollA=0;
  }
  cosR=cosf(rollA); sinR=sinf(rollA);
  float spd=0.020f+0.018f*sinf(tSec*0.09f);
  ax+=spd*dir*spinCur*(1.0f+0.7f*sinf(tSec*0.19f));
  ay+=spd*dir*spinCur*(1.0f+0.6f*cosf(tSec*0.23f));
  az+=spd*dir*spinCur*(0.6f+0.5f*sinf(tSec*0.11f));
  compRot(ax,ay,az);

  // Project cube
  Vec3 tv[8]; int sx[8],sy[8]; float sd[8];
  for (int i=0;i<8;i++) { tv[i]=cv[i]; aRot(&tv[i]); proj(tv[i],&sx[i],&sy[i],&sd[i]); }

  // Project balls
  for (int i=0;i<nb;i++) { wp[i]=pos[i]; aRot(&wp[i]); proj(wp[i],&px[i],&py[i],&pd[i]); }

  // ─── RENDER ────
  const uint16_t ink=cWhite(), bg=cBlack(), rule=cLine();
  fbClear(bg);

  // ─── Cube edges (rainbow) ────
  {
    struct {int a,b;float z;} sed[12];
    for (int i=0;i<12;i++) { int a=ee[i][0],b=ee[i][1]; sed[i]=(typeof(sed[0])){a,b,(sd[a]+sd[b])*0.5f}; }
    for (int i=1;i<12;i++) { typeof(sed[0]) es=sed[i]; int j=i-1; while(j>=0&&sed[j].z>es.z){sed[j+1]=sed[j];j--;} sed[j+1]=es; }

    uint16_t edgePhase=(uint16_t)(((t/20UL)*3UL)%360UL);
    for (int i=0;i<12;i++) {
      int a=sed[i].a, b=sed[i].b;
      float d=(sd[a]+sd[b])*0.5f;
      int bri=(int)((1.0f-(d-0.5f)/5.5f)*80.0f+175.0f);
      if(bri<0)bri=0; else if(bri>255)bri=255;
      bri=(bri*(uint16_t)(210+(int)(45.0f*sinf(tSec*2.5f+i*0.9f))))>>8;
      int ehIdx=i*30+edgePhase; if(ehIdx>=360)ehIdx-=360;
      uint16_t ec=scale565(hueLutEdge[ehIdx],(uint16_t)(bri+1));
      uint16_t ed=scale565(ec,155);              // dimmer outer lines
      // 5px-thick edge (CYD look): bright core + trailing pair
      fbDrawLine(sx[a],sy[a],sx[b],sy[b],ec);
      fbDrawLine(sx[a]+1,sy[a],sx[b]+1,sy[b],ec);
      fbDrawLine(sx[a]-1,sy[a],sx[b]-1,sy[b],ed);
      fbDrawLine(sx[a]+2,sy[a],sx[b]+2,sy[b],ec);
      fbDrawLine(sx[a]-2,sy[a],sx[b]-2,sy[b],ed);
    }
  }

  // ─── Balls ────
  sortDepth();
  const uint16_t haloC=tft.color565(20,20,50);
  for (int si=0;si<nb;si++) {
    int i=order[si];
    if(pd[i]<=0.4f||pd[i]>=5.5f) continue;
    int vr=2+(int)(rad[i]*18.0f/pd[i]);
    if(vr<2)vr=2; if(vr>7)vr=7;
    fbDrawCircle(px[i],py[i],vr+1,haloC);
    fbFillCircle(px[i],py[i],vr,((cols[i]>>1)&0x7BEF));
    fbFillCircle(px[i],py[i],vr-1,cols[i]);
    if(vr>=4) fbFillCircle(px[i]-1,py[i]-1,vr-3,0xFFFF);
  }

  // ── HUD: top bar ──
  fbFillRect(0,0,WIDTH,14,bg);
  fbText57(4,4,"BALLS",ink);
  fbDrawNum11(40,3,nb,cGreen());
  fbText57(105,4,"FPS",ink);
  fbDrawNum11(128,3,fpsShown,cGreen());
  fbHLine(4,14,WIDTH-8,cMagentaD());
  fbFillRect(4,15,WIDTH-8,3,cMagenta());

  // Bottom: single quiet hint, centered
  fbText57((WIDTH-textW57("TAP +BALL"))/2,HEIGHT-9,"TAP +BALL",rule);

  vhsApply();
  fbFlush();
  screenshotHandle(fb, WIDTH, HEIGHT);
  delay(1);
}
