// 3D Cube — CYD (ESP32-2432S028R). The engine (physics, camera, director,
// colours) mirrors 3d_cube/3d_cube.ino; only the output differs: a 320x240
// ST7789 driven in 64-row strips and an XPT2046 touch screen.

#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <math.h>
#include "Screenshot.h"

// --- CYD (ESP32-2432S028R) pins ---
#define TFT_CS    15
#define TFT_DC    2
#define TFT_RST   4
#define TFT_MOSI  13
#define TFT_SCLK  14
#define TFT_MISO  12
#define TFT_BL    21

// --- Touch (XPT2046) ---
#define TOUCH_CS   33
#define TOUCH_CLK  25
#define TOUCH_MOSI 32
#define TOUCH_MISO 39
#define TOUCH_IRQ  36

Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);
SPIClass touchSPI(HSPI);

#define WIDTH 320
#define HEIGHT 240
#define CX (WIDTH/2)
#define CY (HEIGHT/2)
#define MAX_BALLS 500
int nb = 1;
#define CUBE_BOUND 0.85
#define DMAX_X 0.42f        // widest ball pair; lets the collision sweep break early
#define REST_V 0.015f       // below this normal speed, contacts rest instead of bounce
#define PHYS_H (1.0f/60.0f) // fixed physics step: motion independent of frame rate
#define PAN_S 2.0f          // C3 pan/focal are 160x128 values; CYD is 2x
#define FOCAL_S 2.0f

typedef struct { float x,y,z; } Vec3;

// ─── Framebuffer (strip-based: 64 rows; a full 150 KB frame doesn't fit
//     ESP32 DRAM, so screenshots stream band by band) ────
#define FB_H 64
static uint16_t fb[WIDTH*FB_H];
static int fbTop = 0;
static ScreenshotStripSession shot;

#define FPIX(x,y,c) if((x)>=0&&(x)<WIDTH&&(y)>=fbTop&&(y)<fbTop+FB_H) fb[(y-fbTop)*WIDTH+(x)]=(c)

uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) { return tft.color565(r, g, b); }

// ─── Palette (SIGNAL theme) ────
uint16_t cBlack()   { return rgb(148, 158, 130); } // light LCD deck
uint16_t cWhite()   { return rgb( 28,  34,  24); } // dark ink
uint16_t cMagenta() { return rgb(200,  40,  40); } // alarm red
uint16_t cGreen()   { return rgb( 28,  34,  24); } // ink
uint16_t cLine()    { return rgb( 96, 104,  82); } // dark olive rules
uint16_t cMagentaD(){ return rgb(120,  24,  24); } // dark red edging

void fbClear(uint16_t c = 0) {
  for (int i=0; i<WIDTH*FB_H; i++) fb[i]=c;
}

void fbHLine(int x, int y, int w, uint16_t c) {
  if (y<fbTop||y>=fbTop+FB_H) return;
  if (x<0) { w+=x; x=0; }
  if (x+w>WIDTH) w=WIDTH-x;
  if (w<=0) return;
  uint16_t* p=&fb[(y-fbTop)*WIDTH+x];
  while (w--) *p++=c;
}

void fbFillRect(int x, int y, int w, int h, uint16_t c) {
  for (int i=0; i<h; i++) fbHLine(x, y+i, w, c);
}

void fbFillCircle(int cx, int cy, int r, uint16_t c) {
  int x=0, y=r, d=3-2*r;
  while (x<=y) {
    fbHLine(cx-x,cy-y,2*x+1,c);
    fbHLine(cx-x,cy+y,2*x+1,c);
    fbHLine(cx-y,cy-x,2*y+1,c);
    fbHLine(cx-y,cy+x,2*y+1,c);
    if (d<0) d+=4*x+6;
    else { d+=4*(x-y)+10; y--; }
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
    if (d<0) d+=4*x+6;
    else { d+=4*(x-y)+10; y--; }
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

void fbFlush() {
  int h = FB_H;
  if (fbTop+h > HEIGHT) h = HEIGHT - fbTop;
  tft.startWrite();
  tft.setAddrWindow(0, fbTop, WIDTH, h);
  tft.writePixels(fb, WIDTH*h);
  tft.endWrite();
}

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
void fbDrawStr(int x, int y, const char* s, uint16_t c) {
  while (*s) {
    char ch = *s;
    if (ch >= 'a' && ch <= 'z') ch -= 32;
    if (ch != ' ') {
      const uint8_t* g = glyph45(ch);
      for (int row = 0; row < 5; row++) {
        uint8_t bits = g[row];
        for (int col = 0; col < 4; col++)
          if (bits & (0b1000 >> col)) fbFillRect(x + col * 2, y + row * 2, 2, 2, c);
      }
    }
    x += 10; s++;
  }
}
int strW(const char* s) { int n = 0; while (*s++) n++; return n ? n * 10 - 2 : 0; }


// ─── Cube ─────
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

// ─── Camera / director (same choreography as the C3 sketch) ────
float ax=0, ay=0, az=0;
int dir = 1;
bool lt = false;
float rot[9];
float camDist = 3.0, focalLen = 96.0*FOCAL_S;
bool director = false;
unsigned long directorStart = 0, segDur = 20000;
int flyKind = 0, flyS = 1;
float flyA = 0, flyB = 0, flyC = 0;
float spinCur = 1.0f;
int panX = 0, panY = 0;
float rollA = 0, cosR = 1.0f, sinR = 0.0f;
int fpsShown = 0;

static uint16_t hueLutBall[360], hueLutEdge[360];

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

static inline uint16_t scale565(uint16_t c, uint16_t b) {
  uint16_t r = (((c>>11)&0x1F)*b)>>8;
  uint16_t g = (((c>>5)&0x3F)*b)>>8;
  uint16_t bl = ((c&0x1F)*b)>>8;
  return (uint16_t)((r<<11)|(g<<5)|bl);
}

// Restitution ramp: gentle taps rest (0), hard hits bounce (1), smooth between
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

// ─── Touch ────
void touchInit() {
  touchSPI.begin(TOUCH_CLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
  pinMode(TOUCH_IRQ, INPUT);  // GPIO36 input-only: no internal pullup (external PU on CYD)
}
bool touchPressed() {
  return digitalRead(TOUCH_IRQ) == LOW;
}

#define DM01_SCALE 2
#include "Dm01Intro.h"

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== 3D CUBE CYD ===");

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  SPI.begin(TFT_SCLK, TFT_MISO, TFT_MOSI, TFT_CS);
  tft.init(240, 320);
  tft.setSPISpeed(40000000);
  tft.setRotation(1);
  // Panel needs inversion off: lib ST7789 init sends INVON which inverts colors.
  tft.invertDisplay(false);
  tft.fillScreen(0x0000);

  for (int h=0;h<360;h++) {
    hueLutBall[h] = hsv((float)h, 1.0f, 0.8f);
    hueLutEdge[h] = hsv((float)h, 1.0f, 0.62f);
  }

  touchInit();

  compRot(0,0,0); // identity rotation so physics has valid gravity on frame one
  addBall(0);
  for (int i=0;i<nb;i++) { orderX[i]=i; order[i]=i; }
  dm01Start();
}

void loop() {
  unsigned long t = millis();
  screenshotStripPoll(shot, WIDTH, HEIGHT);
  if (dm01Tick(touchPressed())) {
    for (fbTop = 0; fbTop < HEIGHT; fbTop += FB_H) {
      dm01Draw(t);
      int bh = FB_H;
      if (fbTop + bh > HEIGHT) bh = HEIGHT - fbTop;
      if (!screenshotStripSend(shot, fb, fbTop, bh)) fbFlush();
    }
    delay(16);
    return;
  }

  float tSec = t * 0.001f;

  // FPS — frames in each 1s window
  static unsigned long fpsLast = 0;
  static int fpsFrames = 0;
  fpsFrames++;
  if (t - fpsLast >= 1000) {
    fpsShown = (int)(fpsFrames * 1000UL / (t - fpsLast));
    fpsFrames = 0;
    fpsLast = t;
  }

  // Touch — tap to add a ball and reverse direction (debounced)
  bool touched = touchPressed();
  static unsigned long lastTap = 0;
  if (touched && !lt && (t - lastTap > 200)) {
    lastTap = t;
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

  // ─── Director: alternates free orbit and choreographed fly-throughs ────
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
  focalLen *= FOCAL_S;                 // 160x128 values -> 320x240
  panX = (int)(panX * PAN_S);
  panY = (int)(panY * PAN_S);
  cosR=cosf(rollA); sinR=sinf(rollA);

  float spd=0.020f+0.018f*sinf(tSec*0.09f);
  ax+=spd*dir*spinCur*(1.0f+0.7f*sinf(tSec*0.19f));
  ay+=spd*dir*spinCur*(1.0f+0.6f*cosf(tSec*0.23f));
  az+=spd*dir*spinCur*(0.6f+0.5f*sinf(tSec*0.11f));
  compRot(ax,ay,az);

  // Project once per frame, then draw the frame strip by strip
  Vec3 tv[8]; int sx[8],sy[8]; float sd[8];
  for (int i=0;i<8;i++) { tv[i]=cv[i]; aRot(&tv[i]); proj(tv[i],&sx[i],&sy[i],&sd[i]); }
  for (int i=0;i<nb;i++) { wp[i]=pos[i]; aRot(&wp[i]); proj(wp[i],&px[i],&py[i],&pd[i]); }

  // Sort edges back-to-front
  struct {int a,b;float z;} sed[12];
  for (int i=0;i<12;i++) {int a=ee[i][0],b=ee[i][1];sed[i]=(typeof(sed[0])){a,b,(sd[a]+sd[b])*0.5};}
  for (int i=1;i<12;i++) { typeof(sed[0]) es=sed[i]; int j=i-1; while(j>=0&&sed[j].z>es.z){sed[j+1]=sed[j];j--;} sed[j+1]=es; }

  sortDepth();

  // ─── RENDER (strip by strip) ────────
  const uint16_t haloC=tft.color565(20,20,50);
  for (fbTop = 0; fbTop < HEIGHT; fbTop += FB_H) {
    fbClear(cBlack());

    // ─── Cube edges: 5px thick rainbow, cycling, brightness pulsing ────
    {
      uint16_t edgePhase=(uint16_t)(((t/20UL)*3UL)%360UL);
      for (int i=0;i<12;i++) {
        int a=sed[i].a, b=sed[i].b;
        int ylo = (sy[a]<sy[b]?sy[a]:sy[b]) - 2;
        int yhi = (sy[a]>sy[b]?sy[a]:sy[b]) + 2;
        if (yhi < fbTop || ylo >= fbTop+FB_H) continue;   // off this strip
        float d=(sd[a]+sd[b])*0.5f;
        int bri=(int)((1.0f-(d-0.5f)/5.5f)*80.0f+175.0f);
        if(bri<0)bri=0; else if(bri>255)bri=255;
        bri=(bri*(uint16_t)(210+(int)(45.0f*sinf(tSec*2.5f+i*0.9f))))>>8;
        int ehIdx=i*30+edgePhase; if(ehIdx>=360)ehIdx-=360;
        uint16_t ec=scale565(hueLutEdge[ehIdx],(uint16_t)(bri+1));
        uint16_t ed=scale565(ec,155);
        fbDrawLine(sx[a],sy[a],sx[b],sy[b],ec);
        fbDrawLine(sx[a]+1,sy[a],sx[b]+1,sy[b],ec);
        fbDrawLine(sx[a]-1,sy[a],sx[b]-1,sy[b],ed);
        fbDrawLine(sx[a]+2,sy[a],sx[b]+2,sy[b],ec);
        fbDrawLine(sx[a]-2,sy[a],sx[b]-2,sy[b],ed);
      }
    }

    // ─── Balls: halo -> body -> core -> specular, depth sorted ────
    for (int si=0;si<nb;si++) {
      int i=order[si];
      if (pd[i]<=0.4f||pd[i]>=5.5f) continue;
      int vr=4+(int)(rad[i]*36.0f/pd[i]);
      if (vr<4)vr=4; if (vr>14)vr=14;
      if (py[i]+vr < fbTop || py[i]-vr >= fbTop+FB_H) continue;  // off this strip
      fbDrawCircle(px[i],py[i],vr+1,haloC);
      fbFillCircle(px[i],py[i],vr,((cols[i]>>1)&0x7BEF));
      fbFillCircle(px[i],py[i],vr-1,cols[i]);
      if (vr>=8) fbFillCircle(px[i]-1,py[i]-1,vr-3,0xFFFF);
    }

    // FPS counter (top-left corner): white label, green value
    for (int y=0;y<14;y++)
      for (int x=0;x<76;x++)
        FPIX(x,y,cBlack());
    fbDrawStr(4, 4, "FPS", cWhite());
    { char b[8]; snprintf(b, sizeof(b), "%d", fpsShown); fbDrawStr(36, 4, b, cGreen()); }

    // Direction indicator (top-right corner) — magenta
    for (int y=0;y<14;y++)
      for (int x=WIDTH-28;x<WIDTH;x++)
        FPIX(x,y,cBlack());
    uint16_t dc = cMagenta();
    int axp = WIDTH-16, ayp = 7;
    if (dir>0) {
      for (int r=0; r<4; r++)
        for (int c=0; c<=r; c++)
          fbFillRect(axp + r*2 - 4, ayp + c*2 - 6, 2, 2, dc);
    } else {
      for (int r=0; r<4; r++)
        for (int c=0; c<=r; c++)
          fbFillRect(axp - r*2 - 4, ayp + c*2 - 6, 2, 2, dc);
    }

    // Ball counter (bottom-right corner): green value over a magenta rail
    for (int y=HEIGHT-20; y<HEIGHT; y++)
      for (int x=WIDTH-56; x<WIDTH; x++)
        FPIX(x,y,cBlack());
    { char b[8]; snprintf(b, sizeof(b), "%d", nb); fbDrawStr(WIDTH-8-strW(b), HEIGHT-13, b, cGreen()); }
    fbFillRect(WIDTH-54, HEIGHT-4, 50, 2, cMagenta());

    int bh = FB_H;
    if (fbTop + bh > HEIGHT) bh = HEIGHT - fbTop;
    if (!screenshotStripSend(shot, fb, fbTop, bh)) fbFlush();
  }

  delay(1);
}
