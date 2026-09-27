// Dm01Intro.h — DM-01 sunset pixel-rainbow boot intro.
//
// Master copy: tools/intro/Dm01Intro.h. Every sketch that plays the intro keeps
// a copy next to its .ino — edit the master, then re-copy (same rule as
// tools/screenshot/Screenshot.h).
//
// Requires from the sketch: WIDTH, HEIGHT, rgb(r,g,b), a framebuffer `fb` and
// the primitives FPIX, fbClear, fbHLine, fbFillRect, fbDrawLine, fbFillCircle
// (strip-framebuffer CYD sketches already clip those per band).
//
//   #define DM01_SCALE 2 before including on 320x240 (CYD); default 1 (160x128).
//
// Full-frame sketch (C3):
//   setup(): dm01Start();
//   loop():  bool skip = (digitalRead(TOUCH_PIN) != idleLevel);
//            if (dm01Frame(skip)) { fbFlush(); screenshotHandle(fb, WIDTH, HEIGHT); delay(16); return; }
//
// Strip-framebuffer sketch (CYD):
//   setup(): dm01Start();
//   loop():  bool skip = ...;
//            if (dm01Tick(skip)) {
//              unsigned long t = dm01Elapsed();
//              for (fbTop = 0; fbTop < HEIGHT; fbTop += FB_H) { dm01Draw(t); ...flush... }
//              return;
//            }

#pragma once

#ifndef DM01_SCALE
#define DM01_SCALE 1
#endif

// The choreography below is authored against DM01_T_END; DM01_INTRO_MS is the
// real duration, so changing it stretches the whole timeline.
#define DM01_INTRO_MS 4000
#define DM01_T_END    3450
#define DM01_HORIZON  (104 * DM01_SCALE)
#define DM01_COLS     23
#define DM01_ROWS     5

static bool dm01Active = false;
static unsigned long dm01StartMs = 0;
static int dm01SkipN = 0;

static inline uint32_t dm01Hash(uint32_t x){
  x ^= x>>16; x *= 0x7feb352dU; x ^= x>>15; x *= 0x846ca68bU; x ^= x>>16;
  return x;
}

static uint16_t dm01Pal(int i){
  switch(i&7){
    case 0: return rgb(240,50,50);
    case 1: return rgb(255,140,30);
    case 2: return rgb(255,220,50);
    case 3: return rgb(60,220,90);
    case 4: return rgb(60,220,220);
    case 5: return rgb(60,120,255);
    case 6: return rgb(220,60,200);
    default: return rgb(245,245,250);
  }
}

static uint16_t dm01Grad(int y){
  struct Stop { int y; uint8_t r,g,b; };
  static const Stop S[5] = {{0,8,5,22},{45,46,12,64},{75,108,20,76},{95,158,52,30},{103,200,124,42}};
  int gy = y * 103 / (DM01_HORIZON-1);
  int i = 0;
  while(i < 4 && gy > S[i+1].y) i++;
  const Stop* a = &S[i];
  const Stop* b = &S[i < 4 ? i+1 : 4];
  int span = b->y - a->y;
  if(span <= 0) return rgb(a->r, a->g, a->b);
  int f = ((gy - a->y) << 8) / span;
  uint8_t r = a->r + (uint8_t)(((int)(b->r - a->r) * f) >> 8);
  uint8_t g = a->g + (uint8_t)(((int)(b->g - a->g) * f) >> 8);
  uint8_t bl = a->b + (uint8_t)(((int)(b->b - a->b) * f) >> 8);
  return rgb(r, g, bl);
}

static uint16_t dm01StarX[40], dm01StarY[40];
static uint8_t dm01StarSeed[40];
static bool dm01StarsBuilt = false;

static void dm01BuildStars(){
  if(dm01StarsBuilt) return;
  dm01StarsBuilt = true;
  for(int i=0;i<40;i++){
    uint32_t h = dm01Hash(i*2654435761u + 12345u);
    dm01StarX[i] = h % WIDTH;
    dm01StarY[i] = (h>>8) % (DM01_HORIZON - 8*DM01_SCALE);
    dm01StarSeed[i] = (h>>16) & 0xFF;
  }
}

static const char* DM01_GLYPHS[5][DM01_ROWS] = {
  {"###.","#..#","#..#","#..#","###."},           // D
  {"#...#","##.##","#.#.#","#...#","#...#"},     // M
  {"...","...","###","...","..."},                 // -
  {".##.","#.##","##.#","#..#",".##."},           // 0 (slashed)
  {".#.","##.",".#.",".#.","###"},                 // 1
};
static const uint8_t DM01_W[5] = {4,5,3,4,3};

static int8_t dm01Col[96], dm01Row[96];
static int dm01Count = 0;

static void dm01Build(){
  if(dm01Count) return;
  int col = 0;
  for(int it=0; it<5; it++){
    for(int r=0; r<DM01_ROWS; r++){
      const char* rowstr = DM01_GLYPHS[it][r];
      for(int c=0; c<DM01_W[it]; c++){
        if(rowstr[c]=='#'){ dm01Col[dm01Count]=col+c; dm01Row[dm01Count]=r; dm01Count++; }
      }
    }
    col += DM01_W[it] + 1;
  }
}

static void dm01Draw(unsigned long t){
  t = t * DM01_T_END / DM01_INTRO_MS;   // stretch the authored 3450ms timeline
  const int S = DM01_SCALE;
  const int B = 5*S;
  const int sunX = WIDTH/2;
  const int sunYc = DM01_HORIZON - 18*S;
  dm01BuildStars();
  dm01Build();

  float u = t/900.0f; if(u>1.0f) u=1.0f;
  float rise = 1.0f - powf(1.0f-u, 3.0f);
  int sunCy = HEIGHT + 14*S - (int)((HEIGHT + 14*S - sunYc) * rise);
  const bool finalFlash = (t >= 3300);

  fbClear(rgb(4,3,10));
  if(!finalFlash){
    for(int y=0; y<DM01_HORIZON; y++) fbHLine(0, y, WIDTH, dm01Grad(y));
    fbFillRect(0, DM01_HORIZON, WIDTH, HEIGHT-DM01_HORIZON, rgb(4,3,10));

    for(int i=0;i<40;i++){
      if(dm01StarY[i] > DM01_HORIZON - 40*S) continue;
      if(((t/180 + dm01StarSeed[i]) % 3) == 0) continue;
      uint16_t c = (dm01StarSeed[i]&3)==0 ? rgb(255,255,255)
                 : ((dm01StarSeed[i]&4) ? rgb(255,180,220) : rgb(150,140,220));
      FPIX(dm01StarX[i], dm01StarY[i], c);
    }

    int rad = 17*S;
    for(int y=sunCy-rad; y<=sunCy+rad; y++){
      if(y<0 || y>=DM01_HORIZON) continue;
      int dy = y - sunCy;
      if(dy*dy > rad*rad) continue;
      if(y > sunCy && ((y-sunCy)/(2*S))%2==0) continue;
      int half = (int)sqrtf((float)(rad*rad - dy*dy));
      fbHLine(sunX-half, y, half*2+1, (y < sunCy-6*S) ? rgb(240,200,70) : rgb(225,110,35));
    }

    for(int k=-5;k<=5;k++){
      fbDrawLine(sunX, DM01_HORIZON, sunX + k*34*S, HEIGHT-1,
                 (k&1) ? rgb(110,28,120) : rgb(42,100,130));
    }
    fbHLine(0, DM01_HORIZON, WIDTH, rgb(200,80,125));
    float off = (float)fmod(t/500.0, 1.0);
    for(int k=0;k<5;k++){
      float q = k + off;
      int y = DM01_HORIZON + (int)(q*q*3.2f*S);
      if(y < HEIGHT) fbHLine(0, y, WIDTH, (k&1) ? rgb(118,36,135) : rgb(38,96,130));
    }
  }

  const int shearMax=(DM01_ROWS-1)*2*S;
  const int x0=(WIDTH - (DM01_COLS*6*S + shearMax))/2;
  const int y0=(HEIGHT - DM01_ROWS*6*S)/2 - 12*S;
  const bool locked = (t >= 1450);
  const int waveRow = (int)((t/120) % (DM01_ROWS+6));

  bool occ[DM01_COLS][DM01_ROWS];
  memset(occ, 0, sizeof(occ));
  for(int i=0; i<dm01Count; i++){
    uint32_t h = dm01Hash(i*40503u + 991u);
    if(t >= 350 + (h%650) + 320 && t < 2550 + (h%10)*30) occ[dm01Col[i]][dm01Row[i]] = true;
  }
  const uint16_t bord = rgb(22,22,26);

  for(int pass=0; pass<3; pass++){
    for(int i=0; i<dm01Count; i++){
      uint32_t h = dm01Hash(i*40503u + 991u);
      int c = dm01Col[i], r = dm01Row[i];
      int tx = x0 + c*6*S + (DM01_ROWS-1-r)*2*S;
      int ty = y0 + r*6*S;
      unsigned long inStart = 350 + (h%650);
      unsigned long outStart = 2550 + (h%10)*30;
      int px = tx, py = ty, size = B, state = 2;
      if(t < inStart){
        state = 0;
      } else if(t < inStart + 320){
        float p = (t-inStart)/320.0f;
        float e = 1.0f - (1.0f-p)*(1.0f-p)*(1.0f-p);
        px = sunX + (int)((tx-sunX)*e);
        py = sunCy + (int)((ty-sunCy)*e);
        size = S + (int)((float)(B-S)*e);
        state = 1;
      } else if(t >= outStart){
        float p = (t-outStart)/520.0f;
        if(p >= 1.0f) state = 0;
        else {
          float e = p*p*p;
          px = tx + (int)((sunX-tx)*e);
          py = ty + (int)((sunCy-ty)*e);
          size = B - (int)((float)(B-S)*e);
          state = 3;
        }
      }
      if(state == 0 || size <= 0) continue;

      uint16_t cc = (state==2 && locked) ? dm01Pal(r + (int)(t/200)) : dm01Pal((h>>8) % 8);
      if(state==2 && locked && r == waveRow) cc = rgb(255,255,255);

      if(pass==0){
        if(state==2){
          bool up = (r>0) && occ[c][r-1];
          bool dn = (r<DM01_ROWS-1) && occ[c][r+1];
          bool lf = (c>0) && occ[c-1][r];
          bool rt = (c<DM01_COLS-1) && occ[c+1][r];
          if(!up) fbFillRect(px, py-S, B, S, bord);
          if(!dn) fbFillRect(px, py+B, B, S, bord);
          if(!lf) fbFillRect(px-S, py, S, B, bord);
          if(!rt) fbFillRect(px+B, py, S, B, bord);
          if(!up && !lf) fbFillRect(px-S, py-S, S, S, bord);
          if(!up && !rt) fbFillRect(px+B, py-S, S, S, bord);
          if(!dn && !lf) fbFillRect(px-S, py+B, S, S, bord);
          if(!dn && !rt) fbFillRect(px+B, py+B, S, S, bord);
        }
      } else if(pass==1){
        if(state==2) fbFillRect(px, py, B, B, cc);
      } else {
        if(state != 2){
          fbFillRect(px, py, size, size, cc);
          fbFillRect(px-S, py-S, size+2*S, S, bord);
          fbFillRect(px-S, py+size, size+2*S, S, bord);
          fbFillRect(px-S, py, S, size, bord);
          fbFillRect(px+size, py, S, size, bord);
        }
      }
    }
  }

  if(finalFlash){
    float p = (t-3300)/140.0f; if(p>1.0f) p=1.0f;
    fbFillCircle(sunX, sunYc, (int)(p*150.0f*S), rgb(245,245,250));
  }

  if(!finalFlash && locked){
    for(int s=0;s<3;s++){
      uint32_t h = dm01Hash(s*99991u + (uint32_t)(t/140));
      int sx = 14*S + (h % (WIDTH - 28*S));
      int sy = 16*S + ((h>>8) % (84*S));
      uint16_t c = dm01Pal((h>>16)&7);
      fbFillRect(sx-S, sy, 3*S, S, c);
      fbFillRect(sx, sy-S, S, 3*S, c);
    }
  }
}

static void dm01Start(){
  dm01Active = true;
  dm01StartMs = millis();
  dm01SkipN = 0;
}

static unsigned long dm01Elapsed(){ return millis() - dm01StartMs; }
static bool dm01IsActive(){ return dm01Active; }

static bool dm01Tick(bool skipPressed){
  if(!dm01Active) return false;
  unsigned long t = millis() - dm01StartMs;
  dm01SkipN = skipPressed ? dm01SkipN + 1 : 0;
  if(t >= DM01_INTRO_MS || (t > 500 && dm01SkipN >= 5)){
    dm01Active = false;
    return false;
  }
  return true;
}

static bool dm01Frame(bool skipPressed){
  if(!dm01Tick(skipPressed)) return false;
  dm01Draw(millis() - dm01StartMs);
  return true;
}
