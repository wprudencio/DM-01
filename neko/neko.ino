// ─── NEKO · ESP32-C3 LCD Tamagotchi — SIGNAL/VHS edition ────────
// One-button virtual cat for ESP32-C3 + ST7735 160x128 + WS2812.
//
// Renders in the SIGNAL/VHS theme: near-black deck, white ink, a magenta
// accent strip, signal-green stats and a VHS filter that tears one UI band at
// a time with the monitor primaries. The cat is drawn as an etched
// illustration: auto-outlined silhouette with halftone shading (solid /
// checker / sparse ink). Labels use the hand-drawn 5x6 Departure Mono set,
// stat digits the 11px raster at 1x.
//
// The cat is alive between actions: it wanders the floor, sways its tail,
// twitches its ears, meows, yawns when tired and naps on its own when
// exhausted. Petting makes it purr; in PLAY a tap makes it pounce at the ball;
// in FEED a tap sneaks an extra bite.
//
// Hardware (same wiring as pomodoro / 3d_cube):
//   TFT_CS=5 RST=4 DC=3 MOSI=2 SCLK=1 · landscape (rotation 1)
//   Button = BOOT (GPIO 9, active LOW) or touch pad on GPIO 0
//   LED = WS2812 on GPIO 10
//
// Controls:
//   TAP  (<700ms) … cycle FEED > PLAY > WASH > SLEEP > HEAL (the cat MROWs)
//   TAP x2        … pet the cat (purr, +happy)
//   TAP in PLAY   … pounce at the ball
//   TAP in FEED   … extra bite (NOM)
//   HOLD (>=700ms) … run the selected action (when dead: new life)

#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>
#include <Adafruit_NeoPixel.h>
#include <Preferences.h>
#include "Screenshot.h"
#include <math.h>
#include <string.h>
#include <stdio.h>

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
Preferences prefs;

#define WIDTH  160
#define HEIGHT 128

// ─── Layout ───
#define HDR_Y     4    // header labels
#define ACC_Y     11   // dim line; magenta bar at ACC_Y+1
#define PET_FLOOR 76   // ground line for the cat
#define LBL_Y     82   // stat labels
#define DIG_Y     93   // stat digits (11px raster at 1x, ink 93-100); labels ink ends at 88
#define BAR_Y     110  // divider above the button bar
#define DIV_Y     114  // splash divider
#define FTR_Y     116  // splash hints

#define SPR_W  68      // sprite canvas width  (chars)
#define SPR_H  29      // sprite canvas height (chars)
#define SPR_SC 2       // on-screen pixel scale
#define SPR_X  ((WIDTH - SPR_W * SPR_SC) / 2)  // 12
#define SPR_Y  (PET_FLOOR - SPR_H * SPR_SC)    // 18
#define CAT_DX (-4)    // nudge left so body+tail read centered

// ─── Framebuffer (global — never on the stack) ───
static uint16_t fb[WIDTH * HEIGHT];
#define FPIX(x, y, c) if ((x) >= 0 && (x) < WIDTH && (y) >= 0 && (y) < HEIGHT) fb[(y) * WIDTH + (x)] = (c)

uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) { return tft.color565(r, g, b); }

void fbClear(uint16_t c) {
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
  for (int i = 1; i < h - 1; i++) { FPIX(x, y + i, c); FPIX(x + w - 1, y + i, c); }
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
void fbFlush() {
  tft.startWrite();
  tft.setAddrWindow(0, 0, WIDTH, HEIGHT);
  tft.writePixels(fb, WIDTH * HEIGHT);
  tft.endWrite();
}

// ─── SIGNAL palette — near-black deck, magenta + signal green (VHS theme).
//     Cached once per frame so the VHS brightness flicker lives in the palette
//     instead of multiplying every filtered pixel (and the dot grid stops
//     calling color565 6k times a frame). ───
static int vhsFlick = 256;   // 246..256, set per frame before drawing
static uint16_t palBG, palInk, palGhost, palHI, palEdge, palAcc, palAccDK, palAccGN;
static inline uint16_t palGrey(uint8_t v) {
  uint8_t q = (uint8_t)((v * vhsFlick) >> 8);
  return rgb(q, q, q);
}
void palRefresh() {
  palBG    = palGrey(16);
  palInk   = palGrey(255);
  palGhost = palGrey(74);
  palHI    = palGrey(170);
  palEdge  = palGrey(170);
  palAcc   = rgb((uint8_t)((236 * vhsFlick) >> 8), 0, (uint8_t)((140 * vhsFlick) >> 8));
  palAccDK = rgb((uint8_t)((104 * vhsFlick) >> 8), 0, (uint8_t)(( 62 * vhsFlick) >> 8));
  palAccGN = rgb(0, (uint8_t)((230 * vhsFlick) >> 8), (uint8_t)((118 * vhsFlick) >> 8));
}
uint16_t panelBG()    { return palBG; }     // #101010 deck
uint16_t panelInk()   { return palInk; }    // white ink
uint16_t panelGhost() { return palGhost; }  // grid dots / stale digits
uint16_t panelHI()    { return palHI; }     // light grey rule
uint16_t panelEdge()  { return palEdge; }   // light grey rules + borders
uint16_t accent()     { return palAcc; }    // #EC008C magenta
uint16_t accentDK()   { return palAccDK; }  // dimmed magenta
uint16_t accentGN()   { return palAccGN; }  // #00E676 signal green (up)

// ── Departure Mono: labels use the hand-drawn 5x6 pixel set (crisp at any
//    size); scale >= 2 uses the pixel-perfect 11px raster. y is the cap top. ──
#include "DepartureMono.h"
#include "DepartureMonoSmall.h"

static inline const uint8_t* dmsGlyph(char ch) {
  if (ch < 32 || ch > 90) return dmsFont[0];
  return dmsFont[ch - 32];
}
static inline const uint8_t* dmGlyphBig(char ch) {
  if (ch < 32 || ch > 90) return dmFont[0];
  return dmFont[ch - 32];
}
void fbChar(int x, int y, char ch, uint16_t c, int scale) {
  if (ch == ' ') return;
  if (ch >= 'a' && ch <= 'z') ch -= 32; // normalize — labels are UPPER
  if (scale >= 2) {
    const uint8_t* g = dmGlyphBig(ch);
    int cy = y - DM_TOP * scale;                    // y = cap/digit top
    for (int row = 0; row < DM_H; row++) {
      uint8_t bits = g[row];
      if (!bits) continue;
      for (int col = 0; col < DM_W; col++)
        if (bits & (0x40 >> col)) fbFillRect(x + col * scale, cy + row * scale, scale, scale, c);
    }
  } else {
    const uint8_t* g = dmsGlyph(ch);
    int cy = y - DMS_TOP;
    for (int row = 0; row < DMS_H; row++) {
      uint8_t bits = g[row];
      if (!bits) continue;
      for (int col = 0; col < DMS_ADV; col++)
        if (bits & (0x40 >> col)) fbFillRect(x + col, cy + row, 1, 1, c);
    }
  }
}
void fbText(int x, int y, const char* s, uint16_t c, int scale = 1) {
  while (*s) {
    fbChar(x, y, *s, c, scale);
    x += (scale >= 2) ? DM_ADV * scale : DMS_ADV;
    s++;
  }
}
int textW57(const char* s, int scale = 1) {
  int n = 0; while (*s++) n++;
  if (!n) return 0;
  return (scale >= 2) ? (n * DM_ADV * scale - scale) : (n * DMS_ADV - 1);
}
// 11px raster at 1x — the "medium" size between the 5x6 labels and scale 2
void fbText11(int x, int y, const char* s, uint16_t c) {
  while (*s) {
    if (*s != ' ') {
      char ch = (*s >= 'a' && *s <= 'z') ? (char)(*s - 32) : *s;
      const uint8_t* g = dmGlyphBig(ch);
      int cy = y - DM_TOP;
      for (int row = 0; row < DM_H; row++) {
        uint8_t bits = g[row];
        if (!bits) continue;
        for (int col = 0; col < DM_W; col++)
          if (bits & (0x40 >> col)) fbFillRect(x + col, cy + row, 1, 1, c);
      }
    }
    x += DM_ADV;
    s++;
  }
}
int textW11(const char* s) { int n = 0; while (*s++) n++; return n ? n * DM_ADV - 1 : 0; }

// ── VHS filter: slow chroma ripple + per-element glitch hits. Each UI band
//    tears on its own 6s slots and its rip rows are washed with cycling
//    primaries (yellow/cyan/green/magenta/red/blue). Only a single row is
//    buffered (the taps read horizontal neighbours), keeping RAM tiny.
//    This is the frame's hot loop: chroma taps are nearest (no sub-pixel
//    blend), row constants are hoisted and the math stays integer-only. ──
// padded source row: the taps can reach PAD px past either edge, so the hot
// loop needs no per-pixel clamping (the pad repeats the edge pixels)
#define VHS_PAD 20   // covers the widest chroma tap (li/ri up to ~10 during a hit)
static uint16_t vhsRow[WIDTH + 2 * VHS_PAD];

static inline uint32_t vhsNoise(uint32_t x) {
  x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
  return x;
}

// saturated monitor-test primaries for the rip rows
static uint16_t vhsPal(uint8_t i) {
  switch (i % 6) {
    case 0: return rgb(255, 255,   0); // yellow
    case 1: return rgb(  0, 255, 255); // cyan
    case 2: return rgb(  0, 255,   0); // green
    case 3: return rgb(255,   0, 255); // magenta
    case 4: return rgb(255,   0,   0); // red
    default:return rgb(  0,   0, 255); // blue
  }
}

static bool elemHit(int i, uint32_t t, int &dx, uint8_t &pal) {
  const uint32_t SLOT = 6000;
  uint32_t phased = t + (uint32_t)(i * 1013u);
  uint32_t s = vhsNoise(phased / SLOT * 2654435761u ^ ((uint32_t)(i + 3) * 97u));
  if ((s % 5u) != 0) return false;
  uint32_t start = (s >> 8) % (SLOT - 500u);
  uint32_t dur   = 200u + ((s >> 20) % 240u);
  uint32_t local = phased % SLOT;
  if (local < start || local >= start + dur) return false;
  dx  = (int)((s >> 12) % 11u) - 5;   // ±5px tears
  pal = (uint8_t)(s >> 24);
  return true;
}

void vhsApply() {
  uint32_t t = millis();
  float ph = (float)t * 0.0018f;

  // per-element bands: header, accent strip, cat, stat row, button row
  static const int bandY0[5] = {   2, 12,  16,  80, 108 };
  static const int bandY1[5] = {  12, 16,  80, 108, 128 };
  int bDx[5], bBoost[5]; float bChroma[5]; uint8_t bPal[5]; bool bHit[5];
  for (int i = 0; i < 5; i++) {
    bHit[i]    = elemHit(i, t, bDx[i], bPal[i]);
    bDx[i]     = bHit[i] ? bDx[i] : 0;
    bChroma[i] = bHit[i] ? 7.0f : 0.0f;
    bBoost[i]  = bHit[i] ? 4 : 0;
  }

  for (int y = 0; y < HEIGHT; y++) {
    int bi = -1;
    for (int i = 0; i < 5; i++) if (y >= bandY0[i] && y < bandY1[i]) { bi = i; break; }
    if (bi < 0) continue;
    const bool hit = bHit[bi];
    const int xOff = bDx[bi], boost = bBoost[bi];
    int lsh = (int)(256.0f * (1.4f + 1.3f * sinf(y * 0.085f + ph) + bChroma[bi]));
    int rsh = (int)(256.0f * (1.8f + 2.1f * sinf(y * 0.061f + 1.7f + ph * 1.3f) + bChroma[bi]));
    if (lsh < 0) lsh = 0; if (rsh < 0) rsh = 0;
    const int li = lsh >> 8, ri = rsh >> 8;      // tap distances
    const int lf = lsh & 0xFF, rf = rsh & 0xFF;  // sub-pixel blend weights
    // rip colour: shifts every 60ms and every 8 rows inside the hit
    uint16_t pc = vhsPal((uint8_t)(bPal[bi] + t / 60u + (y >> 3)));
    const int pr = (pc >> 11) & 0x1F, pg = (pc >> 5) & 0x3F, pb = pc & 0x1F;
    memcpy(&vhsRow[VHS_PAD], &fb[y * WIDTH], WIDTH * sizeof(uint16_t));
    for (int k = 0; k < VHS_PAD; k++) {
      vhsRow[k] = vhsRow[VHS_PAD];
      vhsRow[VHS_PAD + WIDTH + k] = vhsRow[VHS_PAD + WIDTH - 1];
    }
    const uint16_t* __restrict s = &vhsRow[VHS_PAD];
    uint16_t* __restrict d = &fb[y * WIDTH];

    // interpolated chroma taps: the 1px blend is what keeps the deck's dot
    // grid from striping under the shifted chroma
#define VHS_CHROMA()                                                          \
    int cx = x + xOff;                                                        \
    uint16_t cC = s[cx];                                                      \
    int l0 = s[cx - li], l1 = s[cx - li - 1];                                 \
    int r0 = s[cx + ri], r1 = s[cx + ri + 1];                                 \
    int rSrc = ((((l0 >> 11) & 0x1F) * (256 - lf)) + (((l1 >> 11) & 0x1F) * lf)) >> 8; \
    int bSrc = (((r0 & 0x1F) * (256 - rf)) + ((r1 & 0x1F) * rf)) >> 8;        \
    int r = (3 * rSrc + ((cC >> 11) & 0x1F)) >> 2;   /* 75% shift = stronger fringe */ \
    int g = (cC >> 5) & 0x3F;                                                 \
    int b = (3 * bSrc + (cC & 0x1F)) >> 2;

    if (hit) {                                             // wash the band with the palette
      for (int x = 0; x < WIDTH; x++) {
        VHS_CHROMA();
        r += boost; g += boost; b += boost;
        if (r > 31) r = 31;
        if (g > 63) g = 63;
        if (b > 31) b = 31;
        int w = (g > 22) ? 3 : 2;                          // 75% on ink, 50% on the deck
        r = (r * (4 - w) + pr * w) >> 2;
        g = (g * (4 - w) + pg * w) >> 2;
        b = (b * (4 - w) + pb * w) >> 2;
        d[x] = (uint16_t)((r << 11) | (g << 5) | b);
      }
    } else {                                               // untouched rows: no boost/clamp
      for (int x = 0; x < WIDTH; x++) {
        VHS_CHROMA();
        d[x] = (uint16_t)((r << 11) | (g << 5) | b);
      }
    }
#undef VHS_CHROMA
  }
}

void vhsFlush() { vhsApply(); fbFlush(); }

// ─── Sprites: etched cat + props (2x pixels, 3 ink tones) ───
const char SPR_KITTEN[29][69] PROGMEM = {
  "....................................................................",
  "....................................................................",
  ".............................##......##.............................",
  "............................#.#......#+#............................",
  "...........................#..#......#:+#...........................",
  "..........................#....######.::+#..........................",
  "..........................#...........::+#..........................",
  ".........................#.............::+#.........................",
  "........................#...............::+#........................",
  ".......................#.................::+#.......................",
  "......................##.....##......##...::##......................",
  "........................#...#.##....#.##::+#........................",
  "........................#....##......##.::+#........................",
  "........................#...............::+#........................",
  "........#.#.#...........#...............::+#...........#.#.#........",
  ".........................#.......##....::+#.........................",
  "..........................##.....##...::##....................##....",
  "........#.#.#...............#...####...#........###....#.#.#........",
  "..........................##............##......#+#.................",
  "........................##................##...#:+#.................",
  ".......................#......:.:...:.:.....#..#:+#.................",
  "......................#....#...:.....:..#....##:+#........##........",
  "......................#....#............#.....::+#...##.............",
  "......................#....#............#....::+#...................",
  "......................#....#............#...:###....................",
  ".......................#...#............#::+#.......................",
  "........................#..#..#......#..#:+#........................",
  "........................#..##.##....##.##:+#........................",
  ".........................##################.........................",
};
const char SPR_CHILD[29][69] PROGMEM = {
  "....................................................................",
  "............................##........##............................",
  "...........................#.#........#+#...........................",
  "..........................#..#........#:+#..........................",
  ".........................#....########.::+#.........................",
  "........................#...............::+#........................",
  "........................#...............::+#........................",
  ".......................#.................::+#.......................",
  "......................#...................::+#......................",
  ".....................#......###......###...::+#.....................",
  "....................##.....#.###....#.###...::##....................",
  "......................#.....###......###..::+#......................",
  "......................#...................::+#......................",
  "........#.#.#.........#...................::+#.........#.#.#........",
  ".......................#.........##.........#..........###..........",
  "........................#........##........#...........#+#..........",
  "........#.#.#............##.....####.....##............#+#.#..##....",
  "...........................#............#..............#+#..........",
  "........................###..............###..........#:+#..........",
  "......................##......:.:...:.:.....##........#:+#..........",
  ".....................#.........:.....:........#.....##:+#...........",
  "....................#......#............#......#..##.::+#.##........",
  "....................#......#............#.......##..:###............",
  "....................#......#............#..........::##.............",
  "....................#......#............#......######...............",
  ".....................#.....#............#..::+#.....................",
  "......................#....#..#......#..#.::+#......................",
  "......................#....##.##....##.##.::+#......................",
  ".......................######################.......................",
};
const char SPR_ADULT[29][69] PROGMEM = {
  "...........................##..........##...........................",
  "..........................#.#..........#+#..........................",
  ".........................#...#........#::+#.........................",
  ".........................#...#........#::+#.........................",
  "........................#.....########..::+#........................",
  ".......................#.................::+#.......................",
  "......................#...................::+#......................",
  ".....................#.....................::+#.....................",
  ".....................#.....................::+#.....................",
  "....................#.......###......###....::+#....................",
  "...................##......#.###....#.###....::##...................",
  ".....................#......###......###......#................#....",
  ".....................#........................#...............#+#...",
  "........#.#.#........#........................#........#.#.#.#::+#..",
  "......................#..........##..........#...............#::+#..",
  ".......................#.........##.........#................#:+#...",
  "........#.#.#...........#.......####.......#...........#.#.#.####...",
  ".........................#................#.................#::+#...",
  "......................###..................###..............#::+#...",
  "....................##........:.:...:.:.......##...........#::+#....",
  "...................#...........:.....:..........#.........#.::+#....",
  "..................#........#............#........#......####:+#.....",
  "..................#........#............#.........######..::+#......",
  "..................#........#............#................::+#.......",
  "..................#........#............#...............::##........",
  "...................#.......#............#.............:###..........",
  "....................#......#..#......#..#.......#######.............",
  "....................#......##.##....##.##...::+#....................",
  ".....................##########################.....................",
};
const char SPR_SENIOR[29][69] PROGMEM = {
  "...........................##..........##...........................",
  "..........................#.#..........#+#..........................",
  ".........................#...#........#::+#.........................",
  ".........................#...#........#::+#.........................",
  "........................#.....########..::+#........................",
  ".......................#.................::+#.......................",
  "......................#...................::+#......................",
  ".....................#.....................::+#.....................",
  ".....................#.....................::+#.....................",
  "....................#......#####....#####...::+#....................",
  "...................##......#.###....#.###....::##...................",
  ".....................#......###......###......#................#....",
  ".....................#........................#...............#+#...",
  "........#.#.#........#........................#........#.#.#.#::+#..",
  "......................#..........##..........#...............#::+#..",
  ".......................#.........##.........#................#:+#...",
  "........#.#.#...........#.......####.......#...........#.#.#.####...",
  ".........................#................#.................#::+#...",
  "......................###..................###..............#::+#...",
  "....................##........:.:...:.:.......##...........#::+#....",
  "...................#...........:.....:..........#.........#.::+#....",
  "..................#........#............#........#......####:+#.....",
  "..................#........#............#.........######..::+#......",
  "..................#........#............#................::+#.......",
  "..................#........#............#...............::##........",
  "...................#.......#............#.............:###..........",
  "....................#......#..#......#..#.......#######.............",
  "....................#......##.##....##.##...::+#....................",
  ".....................##########################.....................",
};
const char SPR_GHOST[16][19] PROGMEM = {
  ".....########.....",
  "...############...",
  "..##############..",
  ".################.",
  ".################.",
  ".###..######..###.",
  ".###..######..###.",
  ".################.",
  ".################.",
  ".################.",
  ".################.",
  ".################.",
  ".################.",
  ".################.",
  ".################.",
  ".#.#.#.#.#.#.#.#..",
};
const char SPR_BALL[13][14] PROGMEM = {
  "......#......",
  "...#######...",
  "..#########..",
  ".##..#######.",
  ".##..#######.",
  ".###########.",
  "#############",
  ".###########.",
  ".###########.",
  ".###########.",
  "..#########..",
  "...#######...",
  "......#......",
};
const char SPR_POOP[10][9] PROGMEM = {
  "....##..",
  "...####.",
  "...####.",
  "..#####.",
  ".######.",
  "########",
  "########",
  "########",
  ".######.",
  "..####..",
};
const char SPR_BOWL[7][17] PROGMEM = {
  "....########....",
  "..############..",
  "..############..",
  "..#..........#..",
  "..#..........#..",
  "...#........#...",
  "....########....",
};
const char SPR_HEART[8][10] PROGMEM = {
  ".##...##.",
  "#########",
  "#########",
  "#########",
  ".#######.",
  "..#####..",
  "...###...",
  "....#....",
};
const char SPR_SPARK[5][6] PROGMEM = {
  "..#..",
  "..#..",
  "#####",
  "..#..",
  "..#..",
};

struct CatStage { const char* spr; uint8_t ex, ey, ew, eh; };
const CatStage CATS[4] = {
  { SPR_KITTEN[0], 28, 10, 4, 3 },
  { SPR_CHILD[0],  27,  9, 5, 3 },
  { SPR_ADULT[0],  27,  9, 5, 3 },
  { SPR_SENIOR[0], 27,  9, 5, 3 },
};
const char* STAGE_NAMES[4] = { "KITTEN", "CHILD", "ADULT", "SENIOR" };

// '#' solid, '+' checker, ':' sparse — engraving tones at scale 2.
// flip mirrors the sprite (walking direction); shTop/shBot shear the first and
// last row horizontally, interpolated in between (sway, lean, stretch, twitch).
void drawSprite(int x, int y, const char* spr, int rows, int cols, uint16_t c,
                bool flip = false, int shTop = 0, int shBot = 0) {
  for (int r = 0; r < rows; r++) {
    int sx = 0;
    if (shTop || shBot) sx = shTop + (int)lroundf((shBot - shTop) * (float)r / (float)(rows - 1));
    const char* row = spr + r * (cols + 1);
    for (int cc = 0; cc < cols; cc++) {
      char ch = (char)pgm_read_byte(&row[flip ? (cols - 1 - cc) : cc]);
      if (ch == '.') continue;
      int px0 = x + sx + cc * SPR_SC, py0 = y + r * SPR_SC;
      if (ch == '#') fbFillRect(px0, py0, SPR_SC, SPR_SC, c);
      else if (ch == '+') { FPIX(px0, py0, c); FPIX(px0 + 1, py0 + 1, c); }
      else if (ch == ':') FPIX(px0, py0, c);
    }
  }
}

// ─── Pet state ───
struct Pet {
  int8_t hunger = 80, happy = 80, energy = 90, clean = 90, health = 99;
  int16_t age = 0;
  bool sick = false, sleeping = false, dead = false;
  uint8_t poops = 0;
  int poopTimer = 180;
  int starveSecs = 0;
};
Pet pet;

#define ACT_NONE 0
#define ACT_EAT  1
#define ACT_PLAY 2
#define ACT_WASH 3
#define ACT_HEAL 4
uint8_t actKind = ACT_NONE;
unsigned long actUntil = 0;
int eatBoost = 0;   // extra milliseconds of chewing from taps during FEED

int lastStageShown = 0;

#define M_IDLE  0
#define M_HAPPY 1
#define M_SAD   2
#define M_SICK  3
#define M_SLEEP 4
#define M_EAT   5
#define M_PLAY  6
#define M_DEAD  7

const char* MENU[5] = { "FEED", "PLAY", "WASH", "SLEEP", "HEAL" };
int menuSel = 0;

char msgBuf[24] = "HELLO.";
unsigned long msgUntil = 0;
void say(const char* s, unsigned long dur = 2500) {
  strncpy(msgBuf, s, sizeof(msgBuf) - 1);
  msgBuf[sizeof(msgBuf) - 1] = 0;
  msgUntil = millis() + dur;
}

static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline bool blinkOn(unsigned long t) { return (t / 380) % 2 == 0; }
int stageIdx() { return pet.age < 3 ? 0 : pet.age < 7 ? 1 : pet.age < 13 ? 2 : 3; }

// ─── Cat motion: wander, fidgets, purr, pounce, stretch, yawn ───
struct CatAnim {
  float x;                 // sprite canvas left edge (walk range 2..22)
  int8_t dir;              // +1 faces right, -1 faces left
  float targetX;
  bool walking;
  int16_t ballX;           // ball position while playing
  uint32_t walkEnd, nextWander;
  uint32_t purrUntil, pounceUntil, stretchUntil, yawnUntil, ballHidden;
  uint32_t tailUntil, tailNext, earUntil, earNext;
  uint32_t meowUntil, meowNext, yawnNext;
};
CatAnim cat = { (float)(SPR_X + CAT_DX), 1, (float)(SPR_X + CAT_DX), false, 80,
                0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

// speech bubble next to the cat's head (MEOW / PURR / NOM …)
char bubbleText[14] = "";
unsigned long bubbleUntil = 0;
void bubbleSay(const char* s, unsigned long dur) {
  strncpy(bubbleText, s, sizeof(bubbleText) - 1);
  bubbleText[sizeof(bubbleText) - 1] = 0;
  bubbleUntil = millis() + dur;
}

void catBrain(unsigned long t) {
  static unsigned long lastB = 0;
  float dt = (t - lastB) / 1000.0f; lastB = t;
  if (dt > 0.1f) dt = 0.1f;
  bool busy = pet.dead || pet.sleeping || actKind != ACT_NONE;

  // wander: pick a spot on the floor and pad over to it
  if (!cat.walking && t >= cat.nextWander) {
    cat.nextWander = t + 6000 + random(9000);
    if (!busy) {
      cat.targetX = (float)(2 + random(21));
      if (fabsf(cat.targetX - cat.x) > 6) { cat.walking = true; cat.walkEnd = t + 7000; }
    }
  }
  if (cat.walking) {
    float dx = cat.targetX - cat.x;
    float step = 15.0f * dt;
    if (busy) cat.walking = false;
    else if (fabsf(dx) <= step || t >= cat.walkEnd) { cat.x = cat.targetX; cat.walking = false; }
    else { cat.x += (dx > 0 ? step : -step); cat.dir = (dx > 0) ? 1 : -1; }
  }

  // pad up to the bowl while eating, face the ball while playing
  if (actKind == ACT_EAT && !pet.dead) {
    cat.x += (12.0f - cat.x) * 0.12f;
    cat.dir = 1;
  } else if (actKind == ACT_PLAY && !pet.dead) {
    cat.ballX = 80 + (int)(sinf(t * 0.004f + 1.2f) * 46.0f);
    cat.dir = (cat.ballX > (int)cat.x + 68) ? 1 : -1;
  }

  // pounce arc finished → the cat caught the ball
  if (cat.pounceUntil && t >= cat.pounceUntil) {
    cat.pounceUntil = 0;
    cat.ballHidden = t + 800;
    cat.purrUntil = t + 900;
    pet.happy = clampi(pet.happy + 4, 0, 99);
    bubbleSay("CAUGHT!", 900);
    Serial.println("[neko] caught the ball");
  }

  // idle fidgets: tail flick, ear twitch, meow, yawn
  if (t >= cat.tailNext) { cat.tailUntil = t + 900 + random(700); cat.tailNext = t + 4000 + random(7000); }
  if (t >= cat.earNext)  { cat.earUntil  = t + 170;              cat.earNext  = t + 2800 + random(5000); }
  if (t >= cat.meowNext && !busy && pet.happy > 55) {
    cat.meowUntil = t + 800; bubbleSay("MEOW", 800);
    cat.meowNext = t + 12000 + random(18000);
  }
  if (t >= cat.yawnNext && !busy && pet.energy < 40) {
    cat.yawnUntil = t + 1100; bubbleSay("HAA...", 1100);
    cat.yawnNext = t + 25000 + random(20000);
  }
  // an exhausted cat naps on its own
  if (!busy && pet.energy <= 4) {
    pet.sleeping = true;
    bubbleSay("ZZZ", 1500);
    say("ZONKED OUT.", 3000);
  }
}

// ─── Actions ───
void savePet();
void doAction(int sel) {
  if (pet.dead) {
    pet = Pet();
    actKind = ACT_NONE;
    lastStageShown = 0;
    say("A NEW KITTEN.");
    savePet();
    return;
  }
  unsigned long now = millis();
  cat.walking = false;
  switch (sel) {
    case 0: // FEED
      if (pet.sleeping) { say("WAKE UP FIRST."); break; }
      if (pet.hunger > 92) { say("FULL. NO MORE."); pet.happy = clampi(pet.happy - 3, 0, 99); break; }
      pet.hunger = clampi(pet.hunger + 26, 0, 99);
      pet.happy  = clampi(pet.happy + 6, 0, 99);
      pet.poopTimer = clampi(pet.poopTimer - 40, 20, 9999);
      actKind = ACT_EAT; actUntil = now + 3200; eatBoost = 0;
      bubbleSay("NOM NOM", 1400);
      say("YUM YUM.");
      break;
    case 1: // PLAY
      if (pet.sleeping) { say("SHH. SLEEPING."); break; }
      if (pet.energy < 12) { say("TOO TIRED."); break; }
      pet.happy  = clampi(pet.happy + 18, 0, 99);
      pet.hunger = clampi(pet.hunger - 8, 0, 99);
      pet.energy = clampi(pet.energy - 10, 0, 99);
      actKind = ACT_PLAY; actUntil = now + 5000;
      cat.pounceUntil = 0; cat.ballHidden = 0;
      bubbleSay("WHEE!", 1200);
      say("WHEEE!");
      break;
    case 2: // WASH
      if (pet.poops == 0 && pet.clean > 80) { say("ALREADY CLEAN."); break; }
      pet.poops = 0;
      pet.clean = 99;
      pet.happy = clampi(pet.happy + 5, 0, 99);
      if (pet.sick && random(100) < 30) { pet.sick = false; say("ALL BETTER."); }
      else say("SPARKLY CLEAN.");
      actKind = ACT_WASH; actUntil = now + 2200;
      bubbleSay("MRRROW!", 1400);
      break;
    case 3: // SLEEP / WAKE
      pet.sleeping = !pet.sleeping;
      actKind = ACT_NONE;
      if (!pet.sleeping) { cat.stretchUntil = now + 900; bubbleSay("MRROW.", 1000); }
      else bubbleSay("Zzz", 1200);
      say(pet.sleeping ? "NIGHT NIGHT." : "GOOD MORNING.");
      break;
    case 4: // HEAL
      if (pet.sleeping) { say("WAKE UP FIRST."); break; }
      if (pet.sick) {
        pet.sick = false;
        pet.happy = clampi(pet.happy - 4, 0, 99);
        bubbleSay("MMM.", 1000);
        say("ALL CURED.");
      } else {
        bubbleSay("EW.", 1000);
        pet.happy  = clampi(pet.happy - 8, 0, 99);
        pet.health = clampi(pet.health - 4, 0, 99);
        say("YUK. NOT SICK.");
      }
      actKind = ACT_HEAL; actUntil = now + 2000;
      break;
  }
  Serial.printf("[neko] action %d HNG=%d HAP=%d NRG=%d CLN=%d\n",
                sel, pet.hunger, pet.happy, pet.energy, pet.clean);
}

// ─── Input: BOOT button or GPIO0 touch pad ───
int idleLevel = HIGH;
bool wasPressed = false, holdFired = false;
unsigned long pressStartMs = 0;
const unsigned long HOLD_MS = 700;
const unsigned long DBL_MS  = 320;   // two taps inside this window = pet
bool tapPending = false;
unsigned long tapPendingAt = 0, lastTapMs = 0;

void onShortTap() {
  if (pet.dead) return;
  unsigned long now = millis();
  if (actKind == ACT_PLAY && now < actUntil) {      // pounce at the ball
    if (!cat.pounceUntil) { cat.pounceUntil = now + 750; bubbleSay("YIP!", 700); }
    Serial.println("[input] pounce");
    return;
  }
  if (actKind == ACT_EAT && now < actUntil) {       // sneak an extra bite
    if (eatBoost < 1600) eatBoost += 450;
    pet.happy = clampi(pet.happy + 1, 0, 99);
    bubbleSay("NOM", 600);
    Serial.println("[input] nom");
    return;
  }
  menuSel = (menuSel + 1) % 5;
  cat.earUntil = now + 180;
  bubbleSay("MROW?", 700);
  Serial.printf("[input] TAP menu=%d (%s)\n", menuSel, MENU[menuSel]);
}
void onDoubleTap() {
  if (pet.dead) return;
  pet.happy = clampi(pet.happy + 2, 0, 99);
  cat.purrUntil = millis() + 1500;
  cat.walking = false;
  bubbleSay(pet.sleeping ? "PURR..." : "PURR", 1500);
  Serial.println("[input] PET (purr)");
}
void onLongHold() {
  Serial.printf("[input] HOLD menu=%d\n", menuSel);
  doAction(menuSel);
}
void calibrateTouch() {
  pinMode(BOOT_PIN, INPUT_PULLUP);
  pinMode(TOUCH_PIN, INPUT);
  delay(300);
  int highCount = 0;
  for (int i = 0; i < 20; i++) { if (digitalRead(TOUCH_PIN) == HIGH) highCount++; delay(10); }
  idleLevel = (highCount > 10) ? HIGH : LOW;
}
static inline bool inputPressed() {
  if (digitalRead(BOOT_PIN) == LOW) return true;
  return digitalRead(TOUCH_PIN) != idleLevel;
}
void handleTouch() {
  bool pressed = inputPressed();
  unsigned long now = millis();
  if (pressed && !wasPressed) {
    pressStartMs = now; wasPressed = true; holdFired = false;
  } else if (!pressed && wasPressed) {
    unsigned long dur = now - pressStartMs;
    wasPressed = false;
    if (holdFired) {
      holdFired = false;
    } else if (dur >= 60 && dur < HOLD_MS) {
      if (actKind != ACT_NONE && now < actUntil) {
        onShortTap();                                // contextual, no defer
      } else if (now - lastTapMs < DBL_MS) {
        tapPending = false; onDoubleTap(); lastTapMs = 0;
      } else {
        tapPending = true; tapPendingAt = now + DBL_MS; lastTapMs = now;
      }
    }
    delay(20);
  } else if (pressed && wasPressed && !holdFired && (now - pressStartMs >= HOLD_MS)) {
    holdFired = true;
    tapPending = false;
    onLongHold();
  }
}

// ─── Simulation (1 Hz) ───
unsigned long lastTick = 0, lastSave = 0, lastAge = 0;

void simTick() {
  if (pet.dead) return;
  static uint16_t s = 0, h = 0;
  s++; h++;
  if (pet.sleeping) {
    if (s % 6 == 0)  pet.energy = clampi(pet.energy + 1, 0, 99);
    if (s % 45 == 0 && pet.hunger > 0) pet.hunger--;
    if (s % 90 == 0 && pet.clean > 0)  pet.clean--;
  } else {
    if (s % 20 == 0 && pet.hunger > 0) pet.hunger--;
    if (s % 35 == 0 && pet.happy > 0)  pet.happy--;
    if (s % 45 == 0 && pet.energy > 0) pet.energy--;
    if (s % 50 == 0 && pet.clean > 0)  pet.clean--;
    if (s % 20 == 0 && pet.energy == 0 && pet.happy > 0) pet.happy--;
    if (pet.poopTimer > 0) pet.poopTimer--;
    if (pet.poopTimer <= 0) {
      if (pet.poops < 3) {
        pet.poops++;
        pet.clean = clampi(pet.clean - 20, 0, 99);
        pet.happy = clampi(pet.happy - 6, 0, 99);
        say("POOP! CLEAN IT.");
      }
      pet.poopTimer = 150 + random(120);
    }
    if (s % 10 == 0 && !pet.sick) {
      bool risky = pet.clean < 25 || pet.poops >= 2 || pet.hunger < 15;
      if (risky && random(100) < 8) { pet.sick = true; say("SICK! USE HEAL."); }
    }
  }
  if (pet.sick && s % 10 == 0 && pet.happy > 0) pet.happy--;

  bool bad  = pet.hunger == 0 || pet.happy == 0 || pet.energy == 0 || pet.sick;
  bool good = pet.hunger > 30 && pet.happy > 30 && pet.energy > 20 && !pet.sick;
  if (bad && h % 5 == 0 && pet.health > 0) pet.health--;
  if (good && h % 8 == 0 && pet.health < 99) pet.health++;

  if (pet.hunger == 0) pet.starveSecs++; else pet.starveSecs = 0;
  if (pet.health <= 0 || pet.starveSecs > 300) {
    pet.dead = true; pet.sleeping = false; actKind = ACT_NONE;
    say("R.I.P.", 60000);
    savePet();
  }
}

void ageTick() {
  if (pet.dead) return;
  pet.age++;
  int st = stageIdx();
  if (st != lastStageShown) {
    lastStageShown = st;
    const char* names[4] = { "A BABY KITTEN.", "A CHILD CAT.", "AN ADULT CAT.", "A WISE SENIOR." };
    say(names[st], 4000);
  }
  if (pet.age >= 16) {
    pet.dead = true; pet.sleeping = false; actKind = ACT_NONE;
    say("OLD AGE. R.I.P.", 60000);
  }
  savePet();
}

// ─── Persistence (NVS) ───
void savePet() {
  prefs.begin("neko", false);
  prefs.putUChar("magic", 0xCA);
  prefs.putChar("hng", pet.hunger); prefs.putChar("hap", pet.happy);
  prefs.putChar("nrg", pet.energy); prefs.putChar("cln", pet.clean);
  prefs.putChar("hlt", pet.health);
  prefs.putShort("age", pet.age);
  prefs.putBool("sick", pet.sick); prefs.putBool("dead", pet.dead);
  prefs.putUChar("poop", pet.poops);
  prefs.end();
  lastSave = millis();
}
void loadPet() {
  prefs.begin("neko", true);
  if (prefs.getUChar("magic", 0) != 0xCA) { prefs.end(); return; }
  pet.hunger = prefs.getChar("hng", 80); pet.happy = prefs.getChar("hap", 80);
  pet.energy = prefs.getChar("nrg", 90); pet.clean = prefs.getChar("cln", 90);
  pet.health = prefs.getChar("hlt", 99);
  pet.age = prefs.getShort("age", 0);
  pet.sick = prefs.getBool("sick", false); pet.dead = prefs.getBool("dead", false);
  pet.poops = prefs.getUChar("poop", 0);
  prefs.end();
  pet.sleeping = false;
  pet.poopTimer = 180;
}

// ─── LED mood ───
void updateLED() {
  static unsigned long lastLed = 0;
  if (millis() - lastLed < 60) return;
  lastLed = millis();
  unsigned long t = millis();
  if (pet.dead) {
    float p = (sinf(t * 0.002f) + 1) * 0.5f;
    pixels.setPixelColor(0, pixels.Color((uint8_t)(10 + p * 45), 0, 0));
  } else if (t < cat.purrUntil) {
    float p = (sinf(t * 0.02f) + 1) * 0.5f;
    pixels.setPixelColor(0, pixels.Color((uint8_t)(20 + p * 28), (uint8_t)(10 + p * 14), (uint8_t)(4 + p * 6)));
  } else if (t < cat.pounceUntil) {
    pixels.setPixelColor(0, pixels.Color(50, 42, 10));
  } else if (actKind == ACT_PLAY && t < actUntil) {
    uint8_t w = (t / 120) % 3;
    if (w == 0) pixels.setPixelColor(0, pixels.Color(40, 0, 40));
    else if (w == 1) pixels.setPixelColor(0, pixels.Color(0, 40, 40));
    else pixels.setPixelColor(0, pixels.Color(40, 40, 0));
  } else if (pet.sleeping) {
    float p = (sinf(t * 0.003f) + 1) * 0.5f;
    pixels.setPixelColor(0, pixels.Color(0, 0, (uint8_t)(8 + p * 25)));
  } else if (pet.sick || pet.hunger < 20 || pet.happy < 20 || pet.clean < 25) {
    if ((t / 500) % 2 == 0) pixels.setPixelColor(0, pixels.Color(40, 25, 0));
    else pixels.clear();
  } else {
    float p = (sinf(t * 0.004f) + 1) * 0.5f;
    pixels.setPixelColor(0, pixels.Color((uint8_t)(15 + p * 20), (uint8_t)(8 + p * 10), 0));
  }
  pixels.show();
}

// ─── Render helpers ───
void drawDotGrid() {
  for (int y = SPR_Y; y < PET_FLOOR + 2; y += 2)
    for (int x = 0; x < WIDTH; x += 2)
      fb[y * WIDTH + x] = panelGhost();
}
void drawGroundShadow(int lift, int cx) {
  int hw = 54 - lift * 3;
  if (hw < 20) hw = 20;
  int y = PET_FLOOR - 1;
  for (int e = 0; e < 3; e++) {
    int w = hw - e * 9;
    for (int x = cx - w; x < cx + w; x++)
      if ((x + y + e) % 2 == 0) FPIX(x, y + e, panelGhost());
  }
}
void drawStrip() {
  fbHLine(4, ACC_Y, WIDTH - 8, accentDK());
  fbFillRect(4, ACC_Y + 1, WIDTH - 8, 4, accent());
}
const char* statusText(unsigned long t, bool* urgent) {
  *urgent = false;
  if (pet.dead) { *urgent = true; return "R.I.P."; }
  if (actKind == ACT_EAT  && t < actUntil) return "EAT";
  if (actKind == ACT_PLAY && t < actUntil) return "PLAY";
  if (actKind == ACT_WASH && t < actUntil) return "WASH";
  if (actKind == ACT_HEAL && t < actUntil) return "HEAL";
  if (pet.sleeping) return "SLEEP";
  if (pet.sick) { *urgent = true; return "SICK!"; }
  if (pet.hunger < 20) { *urgent = true; return "HUNGRY!"; }
  if (pet.poops > 0)   { *urgent = true; return "MESSY!"; }
  if (pet.clean < 25)  { *urgent = true; return "DIRTY!"; }
  if (pet.energy < 15) { *urgent = true; return "TIRED!"; }
  if (pet.happy < 20)  { *urgent = true; return "SAD!"; }
  return "AWAKE";
}
void drawHeader(unsigned long t) {
  fbText(6, HDR_Y, "NEKO", panelInk());
  {
    int a = pet.age < 0 ? 0 : (pet.age > 99 ? 99 : pet.age);
    char buf[4]; snprintf(buf, sizeof(buf), "%d", a);
    int w = textW57(buf, 1);
    fbText(29 + (14 - w) / 2, HDR_Y, buf, panelInk());
  }
  bool urgent = false;
  const char* st = statusText(t, &urgent);
  uint16_t stc = urgent ? (blinkOn(t) ? accent() : panelInk()) : accentGN();
  fbText(WIDTH - 6 - textW57(st, 1), HDR_Y, st, stc);
  const char* center = (t < msgUntil && msgBuf[0]) ? msgBuf : STAGE_NAMES[stageIdx()];
  fbText((WIDTH - textW57(center, 1)) / 2, HDR_Y, center, panelInk());
}
void drawStats(unsigned long t) {
  int vals[4] = { pet.hunger, pet.happy, pet.energy, pet.clean };
  const char* labs[4] = { "FOOD", "HAPPY", "REST", "CLEAN" };
  for (int i = 0; i < 4; i++) {
    int cx = 20 + i * 40;
    bool crit = vals[i] < (i == 3 ? 25 : 20);
    fbText(cx - textW57(labs[i], 1) / 2, LBL_Y, labs[i], panelInk());
    uint16_t lit = accentGN();                    // healthy = live green
    if (crit) lit = blinkOn(t) ? accent() : panelGhost();
    int v = vals[i] < 0 ? 0 : (vals[i] > 99 ? 99 : vals[i]);
    char buf[4]; snprintf(buf, sizeof(buf), "%d", v);
    int w = textW11(buf);
    fbText11(cx - w / 2, DIG_Y, buf, lit);
  }
}
void drawMenu(unsigned long t) {
  if (pet.dead) return;
  fbHLine(0, BAR_Y, WIDTH, panelEdge());
  // SLEEP is 5 chars (29px ink) — its cell gets +2px so the label clears the square
  const int cellX[6] = { 0, 31, 63, 95, 129, 160 };
  for (int i = 0; i < 5; i++) {
    int x0 = cellX[i], cw = cellX[i + 1] - x0;
    int cx = x0 + cw / 2;
    const char* lab = (i == 3 && pet.sleeping) ? "WAKE" : MENU[i];
    int w = textW57(lab, 1);
    int x = cx - w / 2;
    if (i == menuSel) {
      fbFillRect(x0 + 1, 112, cw - 2, 14, accent());   // selected = magenta
      fbText(x, 115, lab, panelBG());
    } else {
      fbRect(x0 + 1, 112, cw - 2, 14, panelEdge());
      fbText(x, 115, lab, panelEdge());
    }
  }
}
void drawDeadHint(unsigned long t) {
  if (!pet.dead) return;
  const char* s = "HOLD FOR NEW LIFE";
  fbText((WIDTH - textW57(s, 1)) / 2, 108, s, blinkOn(t) ? accent() : panelGhost());
}

// ─── The cat ───
uint8_t moodNow(unsigned long t) {
  if (pet.dead) return M_DEAD;
  if (actKind == ACT_EAT  && t < actUntil) return M_EAT;
  if (actKind == ACT_PLAY && t < actUntil) return M_PLAY;
  if (pet.sleeping) return M_SLEEP;
  if (pet.sick) return M_SICK;
  if (pet.hunger < 20 || pet.happy < 20 || pet.energy == 0) return M_SAD;
  if (pet.happy > 70 && pet.hunger > 40) return M_HAPPY;
  return M_IDLE;
}

void drawEyes(int x, int y, int st, uint8_t mood, unsigned long t) {
  int ex = CATS[st].ex, ey = CATS[st].ey, ew = CATS[st].ew, eh = CATS[st].eh;
  int exL = x + ex * SPR_SC;
  int exR = x + (SPR_W - ex - ew) * SPR_SC;
  int eyPx = y + ey * SPR_SC;
  int wpx = ew * SPR_SC, hpx = eh * SPR_SC;
  if (mood == M_SICK) {
    fbFillRect(exL, eyPx, wpx, hpx, panelBG());
    fbFillRect(exR, eyPx, wpx, hpx, panelBG());
    fbDrawLine(exL, eyPx, exL + wpx - 1, eyPx + hpx - 1, panelInk());
    fbDrawLine(exL + wpx - 1, eyPx, exL, eyPx + hpx - 1, panelInk());
    fbDrawLine(exR, eyPx, exR + wpx - 1, eyPx + hpx - 1, panelInk());
    fbDrawLine(exR + wpx - 1, eyPx, exR, eyPx + hpx - 1, panelInk());
    return;
  }
  bool closed = (mood == M_SLEEP) || (mood == M_EAT) || (t < cat.purrUntil) ||
                (t < cat.stretchUntil) || (t < cat.yawnUntil) ||
                ((mood == M_IDLE || mood == M_HAPPY) && (t % 4200) < 160);
  if (closed) {
    fbFillRect(exL, eyPx, wpx, hpx, panelBG());
    fbFillRect(exR, eyPx, wpx, hpx, panelBG());
    fbFillRect(exL, eyPx + hpx / 2 - 1, wpx, 2, panelInk());
    fbFillRect(exR, eyPx + hpx / 2 - 1, wpx, 2, panelInk());
  }
}

void drawMouth(int x, int y, int st, uint8_t m, unsigned long t) {
  int ex = CATS[st].ex, ey = CATS[st].ey, ew = CATS[st].ew, eh = CATS[st].eh;
  int exL = x + ex * SPR_SC;
  int exR = x + (SPR_W - ex - ew) * SPR_SC;
  int cx = (exL + exR) / 2 + ew * SPR_SC / 2;
  int my = y + (ey + eh) * SPR_SC + 7;
  if (t < cat.yawnUntil) {
    fbFillCircle(cx, my, 3, panelInk());
    fbFillCircle(cx, my, 1, panelBG());
  } else if (t < cat.meowUntil || t < cat.pounceUntil) {
    fbFillCircle(cx, my, 2, panelInk());
  } else if (m == M_EAT && t < actUntil) {
    fbFillCircle(cx, my, ((t / 150) % 2) ? 2 : 1, panelInk());
  }
}

void drawBubble(unsigned long t, int x, int y) {
  if (!bubbleText[0] || t >= bubbleUntil) return;
  int bw = textW57(bubbleText, 1) + 8, bh = 12;
  int bx = x + 96;
  if (bx + bw > WIDTH - 4) bx = WIDTH - 4 - bw;
  int by = 20;
  fbFillRect(bx, by, bw, bh, panelBG());
  fbRect(bx, by, bw, bh, panelInk());
  fbText(bx + 4, by + 4, bubbleText, panelInk());
  fbDrawLine(bx + 4, by + bh - 1, bx + 1, by + bh + 3, panelInk());
  fbDrawLine(bx + 5, by + bh - 1, bx + 2, by + bh + 3, panelInk());
}

void drawPet(unsigned long t) {
  uint8_t m = moodNow(t);
  int st = stageIdx();
  bool flip = cat.dir < 0;

  if (m == M_DEAD) {
    int gy = 20 + (int)(sinf(t * 0.002f) * 6.0f);
    drawSprite(62, gy, SPR_GHOST[0], 16, 18, ((t / 500) % 2) ? panelHI() : panelGhost());
    return;
  }

  bool purr    = t < cat.purrUntil;
  bool stretch = t < cat.stretchUntil;
  bool walking = cat.walking && actKind == ACT_NONE && !pet.sleeping;
  unsigned long pounceLeft = (cat.pounceUntil > t) ? (cat.pounceUntil - t) : 0;
  float pu = pounceLeft ? 1.0f - pounceLeft / 750.0f : 0.0f;

  int dy = 0, shTop = 0, shBot = (int)(sinf(t * 0.0011f) * 1.2f);   // idle sway
  if (m == M_SLEEP)      { dy = ((t / 1600) % 2) ? -2 : 0; shBot = 0; }
  else if (m == M_SICK)  { dy = (int)((t / 90) % 2) - 1; shBot = 0; }        // shiver
  else if (pounceLeft)   { dy = -(int)(sinf(pu * 3.14159f) * 26.0f); shBot = 0; }
  else if (m == M_PLAY)  { dy = -(int)(fabsf(sinf(t * 0.004f)) * 10.0f); shBot = 0; }
  else if (m == M_EAT)   { dy = ((3200 - (actUntil - t)) % 800 < 280) ? 2 : 0; }
  else if (purr)         { dy = -((t / 90) % 2); shBot = 0; }                // kneading
  else if (walking)      { dy = ((t / 170) % 2) ? -2 : 0; shBot = cat.dir * 2; }
  else if (stretch)      { dy = -1; shBot = (int)(sinf((1.0f - (cat.stretchUntil - t) / 900.0f) * 3.14159f) * 3.0f); }
  else                   { dy = ((t / 650) % 2) ? -3 : 0; }
  if (!walking && !purr && !pet.sleeping && !pounceLeft && t < cat.tailUntil)
    shBot += (int)(sinf(t * 0.02f) * 2);                                     // tail flick
  if (t < cat.earUntil) shTop = ((t / 70) % 2) ? 1 : -1;                     // ear twitch

  int x = (int)lroundf(cat.x) + (pounceLeft ? cat.dir * (int)(sinf(pu * 3.14159f) * 7.0f) : 0);
  int y = SPR_Y + dy;
  drawGroundShadow(-dy, x + SPR_W * SPR_SC / 2);
  drawSprite(x, y, CATS[st].spr, SPR_H, SPR_W, panelInk(), flip, shTop, shBot);
  drawEyes(x, y, st, m, t);
  drawMouth(x, y, st, m, t);

  if (purr && (t / 350) % 2 == 0)
    drawSprite(x + 102, y - 10, SPR_HEART[0], 8, 9, panelInk());

  // sparkles while washing
  if (actKind == ACT_WASH && t < actUntil) {
    for (int i = 0; i < 5; i++) {
      int sx = x + 14 + (i * 23 + t / 80) % 100;
      int sy = SPR_Y + 6 + (i * 13 + t / 110) % 44;
      drawSprite(sx, sy, SPR_SPARK[0], 5, 5, panelInk());
    }
  }
  // medicine plus while healing
  if (actKind == ACT_HEAL && t < actUntil && (t / 280) % 2 == 0) {
    fbText(x + 100, SPR_Y + 4, "+", accent(), 2);
  }
  // bowl, shrinking kibble pile and bite arc while eating
  if (m == M_EAT) {
    const int bcx = 80, bx = bcx - 16;
    unsigned long pos = 3200 - (actUntil - t) + eatBoost;
    int lvl = 3 - (int)(pos / 800);
    if (lvl < 0) lvl = 0;
    if (lvl > 3) lvl = 3;
    fbFillRect(bcx - 12, 68, 24, 4, panelBG());  // opaque bowl interior
    drawSprite(bx, PET_FLOOR - 14, SPR_BOWL[0], 7, 16, panelInk());
    if (lvl == 3) {
      fbFillRect(bcx - 10, 68, 20, 2, panelInk());
      fbFillRect(bcx - 12, 70, 24, 2, panelInk());
    } else if (lvl == 2) {
      fbFillRect(bcx -  8, 68, 16, 2, panelInk());
      fbFillRect(bcx - 10, 70, 20, 2, panelInk());
    } else if (lvl == 1) {
      fbFillRect(bcx - 6, 70, 12, 2, panelInk());
    }
    unsigned long tin = pos % 800;
    if (tin < 450 && pos < 2400) {
      float u = tin / 450.0f;
      int sy = 66;
      int kx = bcx + (int)((x + 68 - bcx) * u);
      int ky = sy + (int)((50 - sy) * u) - (int)(12 * sinf(u * 3.14159f));
      fbFillRect(kx, ky, 2, 2, panelInk());
    }
    if (pos > 1600 && (t / 400) % 2 == 0)
      drawSprite(96, 18, SPR_HEART[0], 8, 9, accent());
  }
  // bouncing ball while playing (hidden while the cat is "carrying" it)
  if (m == M_PLAY && t >= cat.ballHidden) {
    int by = PET_FLOOR - 30 - (int)(fabsf(sinf(t * 0.008f)) * 26.0f);
    drawSprite(cat.ballX - 13, by, SPR_BALL[0], 13, 13, panelInk());
  }
  // Zzz while sleeping
  if (m == M_SLEEP) {
    for (int i = 0; i < 3; i++) {
      int zx = x + 116 + i * 8;
      int zy = 42 - i * 11;
      fbText(zx, zy, "Z", panelInk(), i == 2 ? 2 : 1);
    }
  }
  // poops on the floor (in front)
  const int poopX[3] = { 2, 140, 22 };
  for (int i = 0; i < pet.poops && i < 3; i++)
    drawSprite(poopX[i], PET_FLOOR - 20, SPR_POOP[0], 10, 8, panelInk());
  // content hearts
  if (m == M_HAPPY && (t / 900) % 3 == 0)
    drawSprite(118, SPR_Y - 2, SPR_HEART[0], 8, 9, panelInk());

  drawBubble(t, x, y);
}

void render(unsigned long t) {
  vhsFlick = 246 + (int)(vhsNoise(t / 110u) % 11);
  palRefresh();
  fbClear(panelBG());
  drawDotGrid();
  drawStrip();
  drawHeader(t);
  drawPet(t);
  drawStats(t);
  drawMenu(t);
  if (pet.dead) drawDeadHint(t);
  vhsFlush();
}

// ─── Boot splash ───
void splash() {
  fbClear(panelBG());
  drawDotGrid();
  drawStrip();
  drawSprite(SPR_X + CAT_DX, SPR_Y + 2, CATS[2].spr, SPR_H, SPR_W, panelInk());
  fbText((WIDTH - textW57("NEKO", 3)) / 2, 82, "NEKO", panelInk(), 3);
  fbText((WIDTH - textW57("DIGITAL PET", 1)) / 2, 106, "DIGITAL PET", panelGhost());
  fbHLine(0, DIV_Y, WIDTH, panelEdge());
  fbText(6, FTR_Y, "TAP NEXT", panelInk());
  fbText(WIDTH - 6 - textW57("HOLD USE", 1), FTR_Y, "HOLD USE", accentDK());
  vhsFlush();
  delay(1700);
}

// ─── Setup / loop ───
#include "Dm01Intro.h"

void setup() {
  Serial.begin(115200);
  randomSeed(micros() + analogRead(0));
  pixels.begin(); pixels.setBrightness(40); pixels.clear(); pixels.show();

  SPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);
  tft.initR(INITR_BLACKTAB);
  tft.setSPISpeed(40000000);
  tft.setRotation(1);
  tft.fillScreen(ST7735_BLACK);
  palRefresh();

  loadPet();
  if (pet.dead) {
    pet = Pet();
    savePet();
  }
  calibrateTouch();
  dm01Start();
  Serial.println("[neko] SIGNAL edition boot");
  splash();
  msgUntil = millis() + 2000;
  lastTick = millis(); lastSave = millis(); lastAge = millis();
}

void loop() {
  bool introSkip = (digitalRead(TOUCH_PIN) != idleLevel) || (digitalRead(BOOT_PIN) == LOW);
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
  handleTouch();
  if (tapPending && t >= tapPendingAt) { tapPending = false; onShortTap(); }

  if (t - lastTick >= 1000) { lastTick += 1000; simTick(); }
  if (t - lastAge >= 120000UL) { lastAge += 120000UL; ageTick(); }
  if (t - lastSave >= 30000UL) savePet();
  if (actKind != ACT_NONE && t >= actUntil) actKind = ACT_NONE;

  catBrain(t);
  updateLED();
  render(t);
  screenshotHandle(fb, WIDTH, HEIGHT);

  // pace to ~60 FPS: the frame itself costs ~20ms, so this rarely waits
  static uint32_t nextFrame = 0;
  uint32_t now = millis();
  if ((int32_t)(nextFrame - now) > 0) delay(nextFrame - now);
  nextFrame += 16;
  if ((int32_t)(nextFrame - now) < -100) nextFrame = now + 16;
}
