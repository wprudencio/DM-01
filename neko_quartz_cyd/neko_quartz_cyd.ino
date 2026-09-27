// ─── NEKO · CYD (ESP32-2432S028R) LCD Tamagotchi — QUARTZ edition ───────────
// Port of neko/neko.ino (ESP32-C3 + ST7735 160x128 + WS2812) to the Cheap
// Yellow Display: ST7789 320x240 + XPT2046 resistive touch.
//
// Renders in the QUARTZ LCD theme: a flat light greenish-grey panel, dark
// ink, a thin red accent strip and faint "ghost" unlit segments — the panel
// stays light in every state. The cat is drawn as an etched illustration:
// auto-outlined silhouette with halftone shading (solid / checker / sparse
// ink). Labels use the 4x5 bitmap font at scale 2; the stat digits are
// ghost-pass 7-segment cells (neko's compact pet-stat cell).
//
// The cat is alive between actions: it wanders the floor, sways its tail,
// twitches its ears, meows, yawns when tired and naps on its own when
// exhausted. Petting makes it purr; in PLAY a tap makes it pounce at the ball;
// in FEED a tap sneaks an extra bite.
//
// Controls (the touch screen replaces the button; BOOT works the same):
//   TAP  (<700ms) … cycle FEED > PLAY > WASH > SLEEP > HEAL (the cat MROWs)
//   TAP x2        … pet the cat (purr, +happy)
//   TAP in PLAY   … pounce at the ball
//   TAP in FEED   … extra bite (NOM)
//   HOLD (>=700ms) … run the selected action (when dead: new life)
// No NeoPixel on CYD — mood feedback is on-screen + serial only.

#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <Preferences.h>
#include "Screenshot.h"
#include <math.h>
#include <string.h>

// ── Pins: CYD (ESP32-2432S028R) ──
#define TFT_CS    15
#define TFT_DC    2
#define TFT_RST   4
#define TFT_MOSI  13
#define TFT_SCLK  14
#define TFT_MISO  12
#define TFT_BL    21

// ── Touch (XPT2046 on separate HSPI bus) + BOOT button (theme) ──
#define TOUCH_CS   33
#define TOUCH_CLK  25
#define TOUCH_MOSI 32
#define TOUCH_MISO 39
#define TOUCH_IRQ  36
#define BOOT_PIN   0

Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);
SPIClass touchSPI(HSPI);
Preferences prefs;

#define WIDTH  320
#define HEIGHT 240

// ─── Layout (2x of the 160x128 face, compressed to fit 240 rows) ───
#define HDR_Y     8    // header labels (4x5 font, scale 2)
#define ACC_Y     24   // dark line; red bar at ACC_Y+1
#define PET_FLOOR 156  // ground line for the cat
#define LBL_Y     170  // stat labels
#define DIG_Y     183  // stat digits (7-seg Hc=16)
#define BAR_Y     212  // divider above the action bar
#define DIV_Y     214  // splash divider
#define FTR_Y     224  // splash hints

#define SPR_W  68      // sprite canvas width  (chars)
#define SPR_H  29      // sprite canvas height (chars)
#define SPR_SC 4       // on-screen pixel scale
#define SPR_X  ((WIDTH - SPR_W * SPR_SC) / 2)  // 24
#define SPR_Y  (PET_FLOOR - SPR_H * SPR_SC)    // 40
#define CAT_DX (-8)    // nudge left so body+tail read centered

// ─── Framebuffer (64-row strips; a full 150 KB frame doesn't fit ESP32 DRAM) ───
#define FB_H 64
static uint16_t fb[WIDTH * FB_H];
static int fbTop = 0;
static bool introMode = false;
static ScreenshotStripSession shot;
#define FPIX(x, y, c) if ((x) >= 0 && (x) < WIDTH && (y) >= fbTop && (y) < fbTop + FB_H) fb[(y - fbTop) * WIDTH + (x)] = (c)

uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) { return tft.color565(r, g, b); }

void fbClear(uint16_t c) {
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
  int h = FB_H;
  if (fbTop + h > HEIGHT) h = HEIGHT - fbTop;
  tft.startWrite();
  tft.setAddrWindow(0, fbTop, WIDTH, h);
  tft.writePixels(fb, WIDTH * h);
  tft.endWrite();
}

#define DM01_SCALE 2
#include "Dm01Intro.h"

// ─── QUARTZ LCD palette — light-only, no state switching. The panel is
//     always the classic positive LCD; emphasis comes from the red accent,
//     labels and blink cadence, never from inverting to black. ───
uint16_t panelBG()    { return rgb(148, 158, 130); }  // light LCD green-grey
uint16_t panelInk()   { return rgb( 28,  34,  24); }  // dark ink (digits, labels)
uint16_t panelGhost() { return rgb(138, 148, 120); }  // unlit segments / dot grid
uint16_t panelEdge()  { return rgb( 96, 104,  82); }  // dark olive rules + borders
uint16_t panelDim()   { return rgb( 84,  92,  74); }  // resting art + hints
uint16_t panelSpirit(){ return rgb(118, 128, 102); }  // ghost cat
uint16_t accent()     { return rgb(200,  40,  40); }  // alarm red
uint16_t accentDK()   { return rgb(120,  24,  24); }  // dark red

// ─── 4x5 bitmap font (labels / messages) — the QUARTZ label face. Row bytes,
//     MSB = left column, 5px advance at scale 1 (10px at scale 2, the CYD
//     default). ~170 bytes for A-Z + 0-9 plus ! : . ? - +. ───
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
  return gQuest45;                     // never render an unknown char as nothing
}
void fbChar(int x, int y, char ch, uint16_t c, int scale) {
  if (ch == ' ') return;
  if (ch >= 'a' && ch <= 'z') ch -= 32; // normalize — labels are UPPER
  const uint8_t* g = glyph45(ch);
  for (int row = 0; row < 5; row++) {
    uint8_t bits = g[row];
    for (int col = 0; col < 4; col++)
      if (bits & (0b1000 >> col))
        fbFillRect(x + col * scale, y + row * scale, scale, scale, c);
  }
}
void fbText(int x, int y, const char* s, uint16_t c, int scale = 1) {
  while (*s) { fbChar(x, y, *s, c, scale); x += 5 * scale; s++; }
}
int textW57(const char* s, int scale = 1) {
  int n = 0; while (*s++) n++;
  return n ? n * 5 * scale - scale : 0;
}

// ─── 7-segment digits (ghost pass then lit pass — real LCD look) ───
static const uint8_t segBits[10] = {
  0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F
};
void sevenSegSeg(int x, int y, int Wc, int Hc, int T, int seg, uint16_t c) {
  int vH = (Hc - 3 * T + 1) / 2, midT = (Hc - T) / 2;
  switch (seg) {
    case 0: fbFillRect(x, y, Wc, T, c); return;                          // a (top)
    case 1: fbFillRect(x + Wc - T, y + T, T, vH, c); return;             // b (top-right)
    case 2: fbFillRect(x + Wc - T, y + midT + T, T, vH, c); return;      // c (bottom-right)
    case 3: fbFillRect(x, y + Hc - T, Wc, T, c); return;                 // d (bottom)
    case 4: fbFillRect(x, y + midT + T, T, vH, c); return;               // e (bottom-left)
    case 5: fbFillRect(x, y + T, T, vH, c); return;                      // f (top-left)
    case 6: fbFillRect(x, y + midT, Wc, T, c); return;                   // g (middle)
  }
}
void sevenSegDigit(int x, int y, int Wc, int Hc, int T, uint8_t d, uint16_t lit) {
  for (int s = 0; s < 7; s++) sevenSegSeg(x, y, Wc, Hc, T, s, panelGhost());
  if (d > 9) return;                                                   // >9 = ghost-only slot
  for (int s = 0; s < 7; s++)
    if (segBits[d] & (1 << s)) sevenSegSeg(x, y, Wc, Hc, T, s, lit);
}
// 2-digit pet stat cell: Wc=12 Hc=16 T=2 gap=4 → 28px wide (neko CYD cell)
void statNum(int x, int y, int val, uint16_t lit) {
  const int Wc = 12, Hc = 16, T = 2, gap = 4;
  if (val < 0) val = 0;
  if (val > 99) val = 99;
  int d1 = val / 10, d0 = val % 10;
  sevenSegDigit(x, y, Wc, Hc, T, d1 == 0 ? 10 : d1, lit);              // leading zero = blank
  sevenSegDigit(x + Wc + gap, y, Wc, Hc, T, d0, lit);
}

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

// '#' solid, '+' checker, ':' sparse — engraving tones at scale 4.
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
  float x;                 // sprite canvas left edge (walk range 4..44)
  int8_t dir;              // +1 faces right, -1 faces left
  float targetX;
  bool walking;
  int16_t ballX;           // ball position while playing
  uint32_t walkEnd, nextWander;
  uint32_t purrUntil, pounceUntil, stretchUntil, yawnUntil, ballHidden;
  uint32_t tailUntil, tailNext, earUntil, earNext;
  uint32_t meowUntil, meowNext, yawnNext;
};
CatAnim cat = { (float)(SPR_X + CAT_DX), 1, (float)(SPR_X + CAT_DX), false, 160,
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
      cat.targetX = (float)(4 + random(41));
      if (fabsf(cat.targetX - cat.x) > 10) { cat.walking = true; cat.walkEnd = t + 7000; }
    }
  }
  if (cat.walking) {
    float dx = cat.targetX - cat.x;
    float step = 30.0f * dt;
    if (busy) cat.walking = false;
    else if (fabsf(dx) <= step || t >= cat.walkEnd) { cat.x = cat.targetX; cat.walking = false; }
    else { cat.x += (dx > 0 ? step : -step); cat.dir = (dx > 0) ? 1 : -1; }
  }

  // pad up to the bowl while eating, face the ball while playing
  if (actKind == ACT_EAT && !pet.dead) {
    cat.x += (24.0f - cat.x) * 0.12f;
    cat.dir = 1;
  } else if (actKind == ACT_PLAY && !pet.dead) {
    cat.ballX = 160 + (int)(sinf(t * 0.004f + 1.2f) * 92.0f);
    cat.dir = (cat.ballX > (int)cat.x + 136) ? 1 : -1;
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

// ─── Input: XPT2046 touch screen, BOOT button is the same one-button input ───
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
void touchInit() {
  touchSPI.begin(TOUCH_CLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
  pinMode(TOUCH_CS, OUTPUT);
  digitalWrite(TOUCH_CS, HIGH);
  pinMode(TOUCH_IRQ, INPUT);  // GPIO36 input-only: no internal pullup (external PU on CYD)
  pinMode(BOOT_PIN, INPUT_PULLUP);
}
bool touchPressed() {
  // Debounce: a glitch must persist across 3 loop passes, so line noise can
  // neither fire taps nor the double-tap pet gesture.
  static uint8_t lowN = 0;
  if (digitalRead(TOUCH_IRQ) != LOW) { lowN = 0; return false; }
  if (lowN < 3) { lowN++; return false; }
  return true;
}
static inline bool inputPressed() {
  if (digitalRead(BOOT_PIN) == LOW) return true;
  return touchPressed();
}
bool touchRead(int &tx, int &ty) {          // false = idle-looking reading
  touchSPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  digitalWrite(TOUCH_CS, LOW);
  touchSPI.transfer(0xD0);
  uint16_t xRaw = ((touchSPI.transfer(0) << 8) | touchSPI.transfer(0)) >> 3;
  touchSPI.transfer(0x90);
  uint16_t yRaw = ((touchSPI.transfer(0) << 8) | touchSPI.transfer(0)) >> 3;
  digitalWrite(TOUCH_CS, HIGH);
  touchSPI.endTransaction();
  bool valid = xRaw > 150 && xRaw < 3950 && yRaw > 150 && yRaw < 3950;
  // CYD landscape: raw Y -> screen X, raw X -> screen Y
  tx = map(yRaw, 200, 3900, 0, 320);
  ty = map(xRaw, 3900, 200, 0, 240);
  if (tx < 0) tx = 0; if (tx > 319) tx = 319;
  if (ty < 0) ty = 0; if (ty > 239) ty = 239;
  return valid;
}
void handleTouch() {
  bool pressed = inputPressed();
  unsigned long now = millis();
  if (pressed && !wasPressed) {
    if (digitalRead(BOOT_PIN) == LOW) {
      Serial.println("[input] BOOT press");
    } else {
      int tx, ty;
      if (!touchRead(tx, ty)) return;              // line noise, not a real press
      Serial.printf("[touch] press @ %d,%d\n", tx, ty);
    }
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

// ─── LED: no NeoPixel on CYD (mood is on-screen) ───
void updateLED() {}

// ─── Render helpers ───
void drawDotGrid() {
  for (int y = SPR_Y; y < PET_FLOOR + 2; y += 4)
    for (int x = 0; x < WIDTH; x += 4)
      FPIX(x, y, panelGhost());
}
void drawGroundShadow(int lift, int cx) {
  int hw = 108 - lift * 6;
  if (hw < 40) hw = 40;
  int y = PET_FLOOR - 1;
  for (int e = 0; e < 3; e++) {
    int w = hw - e * 18;
    for (int x = cx - w; x < cx + w; x++)
      if ((x + y + e) % 2 == 0) FPIX(x, y + e, panelGhost());
  }
}
void drawStrip() {
  fbHLine(8, ACC_Y, WIDTH - 16, accentDK());
  fbFillRect(8, ACC_Y + 1, WIDTH - 16, 8, accent());
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
  fbText(12, HDR_Y, "NEKO", panelInk(), 2);
  statNum(58, HDR_Y - 4, pet.age, panelInk());         // age in ghost 7-seg
  bool urgent = false;
  const char* st = statusText(t, &urgent);
  uint16_t stc = urgent ? (blinkOn(t) ? accent() : panelInk()) : panelInk();
  fbText(WIDTH - 12 - textW57(st, 2), HDR_Y, st, stc, 2);
  const char* center = (t < msgUntil && msgBuf[0]) ? msgBuf : STAGE_NAMES[stageIdx()];
  fbText((WIDTH - textW57(center, 2)) / 2, HDR_Y, center, panelInk(), 2);
}
void drawStats(unsigned long t) {
  int vals[4] = { pet.hunger, pet.happy, pet.energy, pet.clean };
  const char* labs[4] = { "FOOD", "HAPPY", "REST", "CLEAN" };
  for (int i = 0; i < 4; i++) {
    int cx = 40 + i * 80;
    bool crit = vals[i] < (i == 3 ? 25 : 20);
    fbText(cx - textW57(labs[i], 2) / 2, LBL_Y, labs[i], panelInk(), 2);
    uint16_t lit = panelInk();                       // healthy = steady ink
    if (crit) lit = blinkOn(t) ? accent() : panelGhost();
    statNum(cx - 14, DIG_Y, vals[i], lit);           // ghost 7-seg, 2 digits
  }
}
void drawMenu(unsigned long t) {
  if (pet.dead) return;
  fbHLine(0, BAR_Y, WIDTH, panelEdge());
  const int cellW = WIDTH / 5;
  for (int i = 0; i < 5; i++) {
    int x0 = i * cellW;
    int cx = x0 + cellW / 2;
    const char* lab = (i == 3 && pet.sleeping) ? "WAKE" : MENU[i];
    int w = textW57(lab, 2);
    int x = cx - w / 2;
    if (i == menuSel) {
      fbFillRect(x0 + 4, 213, cellW - 8, 25, panelInk());  // selected = ink fill
      fbText(x, 219, lab, panelBG(), 2);
      fbFillRect(x0 + 12, 233, cellW - 24, 3, accent());   // red underline
    } else {
      fbRect(x0 + 4, 213, cellW - 8, 25, panelEdge());
      fbText(x, 219, lab, panelEdge(), 2);
    }
  }
}
void drawDeadHint(unsigned long t) {
  if (!pet.dead) return;
  const char* s = "HOLD FOR NEW LIFE";
  fbText((WIDTH - textW57(s, 2)) / 2, 196, s, blinkOn(t) ? accent() : panelDim(), 2);
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
  int my = y + (ey + eh) * SPR_SC + 14;
  if (t < cat.yawnUntil) {
    fbFillCircle(cx, my, 6, panelInk());
    fbFillCircle(cx, my, 2, panelBG());
  } else if (t < cat.meowUntil || t < cat.pounceUntil) {
    fbFillCircle(cx, my, 4, panelInk());
  } else if (m == M_EAT && t < actUntil) {
    fbFillCircle(cx, my, ((t / 150) % 2) ? 4 : 2, panelInk());
  }
}

void drawBubble(unsigned long t, int x, int y) {
  if (!bubbleText[0] || t >= bubbleUntil) return;
  int bw = textW57(bubbleText, 2) + 16, bh = 26;
  int bx = x + 192;
  if (bx + bw > WIDTH - 8) bx = WIDTH - 8 - bw;
  int by = 36;
  fbFillRect(bx, by, bw, bh, panelBG());
  fbRect(bx, by, bw, bh, panelInk());
  fbText(bx + 8, by + 8, bubbleText, panelInk(), 2);
  fbDrawLine(bx + 8, by + bh - 1, bx + 2, by + bh + 6, panelInk());
  fbDrawLine(bx + 9, by + bh - 1, bx + 3, by + bh + 6, panelInk());
}

void drawPet(unsigned long t) {
  uint8_t m = moodNow(t);
  int st = stageIdx();
  bool flip = cat.dir < 0;

  if (m == M_DEAD) {
    int gy = 40 + (int)(sinf(t * 0.002f) * 12.0f);
    drawSprite(124, gy, SPR_GHOST[0], 16, 18, ((t / 500) % 2) ? panelSpirit() : panelGhost());
    return;
  }

  bool purr    = t < cat.purrUntil;
  bool stretch = t < cat.stretchUntil;
  bool walking = cat.walking && actKind == ACT_NONE && !pet.sleeping;
  unsigned long pounceLeft = (cat.pounceUntil > t) ? (cat.pounceUntil - t) : 0;
  float pu = pounceLeft ? 1.0f - pounceLeft / 750.0f : 0.0f;

  int dy = 0, shTop = 0, shBot = (int)(sinf(t * 0.0011f) * 2.4f);   // idle sway
  if (m == M_SLEEP)      { dy = ((t / 1600) % 2) ? -4 : 0; shBot = 0; }
  else if (m == M_SICK)  { dy = (int)((t / 90) % 2) * 2 - 2; shBot = 0; }   // shiver
  else if (pounceLeft)   { dy = -(int)(sinf(pu * 3.14159f) * 52.0f); shBot = 0; }
  else if (m == M_PLAY)  { dy = -(int)(fabsf(sinf(t * 0.004f)) * 20.0f); shBot = 0; }
  else if (m == M_EAT)   { dy = ((3200 - (actUntil - t)) % 800 < 280) ? 4 : 0; }
  else if (purr)         { dy = -((t / 90) % 2) * 2; shBot = 0; }           // kneading
  else if (walking)      { dy = ((t / 170) % 2) ? -4 : 0; shBot = cat.dir * 4; }
  else if (stretch)      { dy = -2; shBot = (int)(sinf((1.0f - (cat.stretchUntil - t) / 900.0f) * 3.14159f) * 6.0f); }
  else                   { dy = ((t / 650) % 2) ? -6 : 0; }
  if (!walking && !purr && !pet.sleeping && !pounceLeft && t < cat.tailUntil)
    shBot += (int)(sinf(t * 0.02f) * 4);                                    // tail flick
  if (t < cat.earUntil) shTop = ((t / 70) % 2) ? 2 : -2;                    // ear twitch

  int x = (int)lroundf(cat.x) + (pounceLeft ? cat.dir * (int)(sinf(pu * 3.14159f) * 14.0f) : 0);
  int y = SPR_Y + dy;
  drawGroundShadow(-dy, x + SPR_W * SPR_SC / 2);
  drawSprite(x, y, CATS[st].spr, SPR_H, SPR_W, panelInk(), flip, shTop, shBot);
  drawEyes(x, y, st, m, t);
  drawMouth(x, y, st, m, t);

  if (purr && (t / 350) % 2 == 0)
    drawSprite(x + 204, y - 20, SPR_HEART[0], 8, 9, panelInk());

  // sparkles while washing
  if (actKind == ACT_WASH && t < actUntil) {
    for (int i = 0; i < 5; i++) {
      int sx = x + 28 + (i * 46 + t / 80) % 200;
      int sy = SPR_Y + 12 + (i * 26 + t / 110) % 88;
      drawSprite(sx, sy, SPR_SPARK[0], 5, 5, panelInk());
    }
  }
  // medicine plus while healing
  if (actKind == ACT_HEAL && t < actUntil && (t / 280) % 2 == 0) {
    fbText(x + 200, SPR_Y + 8, "+", accent(), 4);
  }
  // bowl, shrinking kibble pile and bite arc while eating
  if (m == M_EAT) {
    const int bcx = 160, bx = bcx - 32;
    unsigned long pos = 3200 - (actUntil - t) + eatBoost;
    int lvl = 3 - (int)(pos / 800);
    if (lvl < 0) lvl = 0;
    if (lvl > 3) lvl = 3;
    fbFillRect(bcx - 24, 136, 48, 8, panelBG());  // opaque bowl interior
    drawSprite(bx, PET_FLOOR - 28, SPR_BOWL[0], 7, 16, panelInk());
    if (lvl == 3) {
      fbFillRect(bcx - 20, 136, 40, 4, panelInk());
      fbFillRect(bcx - 24, 140, 48, 4, panelInk());
    } else if (lvl == 2) {
      fbFillRect(bcx - 16, 136, 32, 4, panelInk());
      fbFillRect(bcx - 20, 140, 40, 4, panelInk());
    } else if (lvl == 1) {
      fbFillRect(bcx - 12, 140, 24, 4, panelInk());
    }
    unsigned long tin = pos % 800;
    if (tin < 450 && pos < 2400) {
      float u = tin / 450.0f;
      int sy = 132;
      int kx = bcx + (int)((x + 136 - bcx) * u);
      int ky = sy + (int)((100 - sy) * u) - (int)(24 * sinf(u * 3.14159f));
      fbFillRect(kx, ky, 4, 4, panelInk());
    }
    if (pos > 1600 && (t / 400) % 2 == 0)
      drawSprite(192, 36, SPR_HEART[0], 8, 9, accent());
  }
  // bouncing ball while playing (hidden while the cat is "carrying" it)
  if (m == M_PLAY && t >= cat.ballHidden) {
    int by = PET_FLOOR - 60 - (int)(fabsf(sinf(t * 0.008f)) * 52.0f);
    drawSprite(cat.ballX - 26, by, SPR_BALL[0], 13, 13, panelInk());
  }
  // Zzz while sleeping
  if (m == M_SLEEP) {
    for (int i = 0; i < 3; i++) {
      int zx = x + 232 + i * 16;
      int zy = 84 - i * 22;
      fbText(zx, zy, "Z", panelInk(), i == 2 ? 4 : 2);
    }
  }
  // poops on the floor (in front)
  const int poopX[3] = { 4, 280, 44 };
  for (int i = 0; i < pet.poops && i < 3; i++)
    drawSprite(poopX[i], PET_FLOOR - 40, SPR_POOP[0], 10, 8, panelInk());
  // content hearts
  if (m == M_HAPPY && (t / 900) % 3 == 0)
    drawSprite(236, SPR_Y - 4, SPR_HEART[0], 8, 9, panelInk());

  drawBubble(t, x, y);
}

void drawFrame(unsigned long t) {
  fbClear(panelBG());
  drawDotGrid();
  drawStrip();
  drawHeader(t);
  drawPet(t);
  drawStats(t);
  drawMenu(t);
  if (pet.dead) drawDeadHint(t);
}

void render(unsigned long t) {
  for (fbTop = 0; fbTop < HEIGHT; fbTop += FB_H) {
    if (introMode) dm01Draw(dm01Elapsed()); else drawFrame(t);
    int h = FB_H;
    if (fbTop + h > HEIGHT) h = HEIGHT - fbTop;
    if (!screenshotStripSend(shot, fb, fbTop, h)) fbFlush();
  }
}

// ─── Boot splash ───
void splash() {
  for (fbTop = 0; fbTop < HEIGHT; fbTop += FB_H) {
    fbClear(panelBG());
    drawDotGrid();
    drawStrip();
    drawSprite(SPR_X + CAT_DX, SPR_Y + 4, CATS[2].spr, SPR_H, SPR_W, panelInk());
    fbText((WIDTH - textW57("NEKO", 4)) / 2, 166, "NEKO", panelInk(), 4);
    fbText((WIDTH - textW57("DIGITAL PET", 2)) / 2, 194, "DIGITAL PET", panelDim(), 2);
    fbHLine(0, DIV_Y, WIDTH, panelEdge());
    fbText(12, FTR_Y, "TAP NEXT", panelInk(), 2);
    fbText(WIDTH - 12 - textW57("HOLD USE", 2), FTR_Y, "HOLD USE", accentDK(), 2);
    fbFlush();
  }
  delay(1700);
}

// ─── Setup / loop ───
void setup() {
  Serial.begin(115200);
  randomSeed(micros() + analogRead(0));

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  SPI.begin(TFT_SCLK, TFT_MISO, TFT_MOSI, TFT_CS);
  tft.init(240, 320);
  tft.setSPISpeed(80000000);   // ST7789 takes 80 MHz — halves the strip flush
  tft.setRotation(1);
  tft.invertDisplay(false);
  tft.fillScreen(panelBG());

  touchInit();

  loadPet();
  if (pet.dead) {
    pet = Pet();
    savePet();
  }
  Serial.println("[neko] CYD QUARTZ edition boot");
  dm01Start();
  msgUntil = millis() + 2000;
  lastTick = millis(); lastSave = millis(); lastAge = millis();
}

void loop() {
  unsigned long t = millis();
  screenshotStripPoll(shot, WIDTH, HEIGHT);
  if (dm01Tick(inputPressed())) {
    introMode = true;
    render(t);
    delay(16);
    return;
  }
  introMode = false;
  handleTouch();
  if (tapPending && t >= tapPendingAt) { tapPending = false; onShortTap(); }

  if (t - lastTick >= 1000) { lastTick += 1000; simTick(); }
  if (t - lastAge >= 120000UL) { lastAge += 120000UL; ageTick(); }
  if (t - lastSave >= 30000UL) savePet();
  if (actKind != ACT_NONE && t >= actUntil) actKind = ACT_NONE;

  catBrain(t);
  render(t);

  // pace to ~60 FPS: the frame itself costs ~50ms, so this rarely waits
  static uint32_t nextFrame = 0;
  uint32_t now = millis();
  if ((int32_t)(nextFrame - now) > 0) delay(nextFrame - now);
  nextFrame += 16;
  if ((int32_t)(nextFrame - now) < -100) nextFrame = now + 16;
}
