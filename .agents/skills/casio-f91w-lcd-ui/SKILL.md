---
name: casio-f91w-lcd-ui
description: Reproduce the Casio F-91W LIGHT-LCD UI style for ESP32 TFT sketches (ST7735 160x128 and ST7789 320x240). Use when building or restyling watch-like timers, virtual pets and games with ghost 7-segment digits, a light greenish-grey LCD panel, dark ink, a red accent strip, LCD art sprites, and SET/progress indicators. Light palette in every state — no inverted/black negative mode. Covers pomodoro, 3d_cube, neko and flappy on C3 + CYD.
---

# Casio F-91W LCD UI Style

A faithful, framebuffer-based reproduction of the classic Casio F-91W digital
watch face, described generically so it can be dropped into any ESP32 TFT
sketch (ST7735 160x128, or ST7789 320x240 on the CYD).

The signature look: a flat **light greenish-grey LCD panel** with **dark
7-segment digits** and faint **"ghost" unlit segments**, a **thin red accent
strip**, a **SET indicator row** with literal squares, and dark-ink labels. The
panel stays light in **every** state — differentiate states with labels, colon
cadence, progress fill, SET squares, and sprites, never by inverting to black.

The style is the palette, the ghost digits, the 4x5 font, and the red accent —
not any particular widget. Progress bars, SET rows, headers and footers are
opt-in components: include one only when the app has something real to show
(see §4). Cut rows that never change user behavior — `3d_cube` dropped AUTO
CAM / MODE / GRAVITY / COLLISIONS down to BALLS + FPS + one hint and looked
better for it.

The hardware, rendering, strip framebuffer and touch details live in the
`esp32-c3-ws2812` skill.

## 1. The Palette (the heart of the style) — light only

One palette, no state switching. The panel is always the classic positive LCD.

```cpp
uint16_t panelBG()    { return rgb(148,158,130); } // light LCD green-grey
uint16_t panelInk()   { return rgb(28,34,24); }    // dark ink (digits, labels)
uint16_t panelGhost() { return rgb(138,148,120); } // unlit segments (darker than bg)
uint16_t panelHI()    { return rgb(178,186,160); } // light inner border
uint16_t panelEdge()  { return rgb(96,104,82); }   // dark olive outer border
uint16_t accent()     { return rgb(200,40,40); }   // F-91W red
uint16_t accentDK()   { return rgb(120,24,24); }   // dark red
```

Color notes:
- `panelBG` (`rgb(148,158,130)`) reads as classic LCD glass. Keep it light — it
  is the identity of the style.
- `panelInk` must stay very dark against the light bg for all digits/labels.
- `panelGhost` must be visibly darker than the bg but far lighter than ink, so
  unlit segments read as "off LCD", not as lit digits.
- `panelHI` / `panelEdge` are the recessed progress-bar frame. They are
  optional: `3d_cube` uses just BG/Ink/Ghost/accent, `neko` adds no HI.
- `accent` is the watch's red "ALARM" sweep. Always keep it red.
- **Never invert the panel.** Every project is light-only: `pomodoro`,
  `pomodoro_cyd`, `3d_cube`, `neko`, `neko_cyd`, `flappy`, `flappy_cyd`. If a
  state wants emphasis, use the red accent (progress fill, active square, right
  footer hint), not a black negative mode. (`pomodoro_cyd` carried a legacy
  inverted running mode until the 2026-09 cleanup — do not reintroduce it.)
- Sketch-local extras are fine: `neko` adds `C_DIM rgb(84,92,74)` for resting
  art and `C_SPIRIT rgb(118,128,102)` for the ghost cat.

## 2. 7-Segment Digits with Ghost Segments

Draw **all segments in ghost color first**, then overdraw the lit ones. This is
what makes it look like a real LCD.

```cpp
// bits: bit0=a top, bit1=b top-right, bit2=c bottom-right,
//        bit3=d bottom, bit4=e bottom-left, bit5=f top-left, bit6=g middle
static const uint8_t segBits[10] = {
  0x3F,0x06,0x5B,0x4F,0x66, 0x6D,0x7D,0x07,0x7F,0x6F
};

void sevenSegSeg(int x,int y,int Wc,int Hc,int T,int seg,uint16_t c){
  int vH=(Hc-3*T+1)/2, midT=(Hc-T)/2;
  switch(seg){
    case 0: fbFillRect(x,y,Wc,T,c); return;                          // a (top)
    case 1: fbFillRect(x+Wc-T,y+T,T,vH,c); return;                   // b (top-right)
    case 2: fbFillRect(x+Wc-T,y+midT+T,T,vH,c); return;              // c (bottom-right)
    case 3: fbFillRect(x,y+Hc-T,Wc,T,c); return;                     // d (bottom)
    case 4: fbFillRect(x,y+midT+T,T,vH,c); return;                   // e (bottom-left)
    case 5: fbFillRect(x,y+T,T,vH,c); return;                        // f (top-left)
    case 6: fbFillRect(x,y+midT,Wc,T,c); return;                     // g (middle)
  }
}
void sevenSegDigit(int x,int y,int Wc,int Hc,int T,uint8_t d,uint16_t lit,uint16_t ghost){
  for(int s=0;s<7;s++) sevenSegSeg(x,y,Wc,Hc,T,s,ghost);   // ghost all
  if(d>9) return;                                          // >9 = ghost-only slot
  uint8_t m=segBits[d];
  for(int s=0;s<7;s++) if(m&(1<<s)) sevenSegSeg(x,y,Wc,Hc,T,s,lit);
}
```

### MM:SS time with a blinking colon

```cpp
void sevenSegTime(int x,int y,int Wc,int Hc,int T,int min,int sec,bool showColon,uint16_t lit,uint16_t ghost){
  int gap=7, colonW=8;   // 160x128: gap 7, colonW 8. 320x240 (2x): gap 14, colonW 16.
  int p=x;
  sevenSegDigit(p,y,Wc,Hc,T,min/10,lit,ghost); p+=Wc+gap;
  sevenSegDigit(p,y,Wc,Hc,T,min%10,lit,ghost); p+=Wc+gap;
  uint16_t cc = showColon ? lit : ghost;
  fbFillRect(p+1, y+14, 6, 6, cc);
  fbFillRect(p+1, y+Hc-20, 6, 6, cc);
  p += colonW+gap;
  sevenSegDigit(p,y,Wc,Hc,T,sec/10,lit,ghost); p+=Wc+gap;
  sevenSegDigit(p,y,Wc,Hc,T,sec%10,lit,ghost);
}
```

Colon blink cadence is a signature detail: running states blink quickly
(`(millis()%1000)<750`), paused blinks slowly (`(millis()/400)%2==0`), idle is
steady.

On 320x240 everything scales ×2 — `pomodoro_cyd` uses
`fbFillRect(p+2, y+28, 12, 12, …)` / `fbFillRect(p+2, y+Hc-40, 12, 12, …)` and
`totalW` with the same gap it draws with (288px).

### Sizing

| Display | Wc | Hc | T | gap | colonW | notes |
|---|---|---|---|---|---|---|
| 160x128 (pomodoro) | 26 | 44 | 5 | 7 | 8 | single 7-seg cell |
| 320x240 (pomodoro_cyd) | 54 | 88 | 10 | 14 | 16 | scale everything by 2 |

Total width = `Wc*4 + 4*gap + colonW`; center with `timeX=(WIDTH-totalW)/2`.
**Use the same gap you draw with** — `pomodoro.ino`'s `sevenSegTime()` draws
with gap 7 (span 140) but its `render()` centers with gap 8 (144), leaving the
block ~2px off-center. Compute `totalW` with the draw gap.

### Compact counters (N-digit stats, not time)

Scene headers (object counts, FPS), pet stats and game scores want small
ghost-7seg numbers next to a 4x5 label. Use a leading-blank counter so the
width stays fixed as the number grows:

```cpp
// 160x128 header counters: Wc=8 Hc=11 T=2, gap=3 → 11px pitch per digit.
// Hc=11 leaves 1px breathing room inside a 14px header band.
void sevenSegNum(int x,int y,int num,int digits,uint16_t lit,uint16_t ghost){
  const int Wc=8, Hc=11, T=2, gap=3;
  if(num<0) num=0;
  int div=1;
  for(int i=1;i<digits;i++) div*=10;
  for(int i=0;i<digits;i++){
    int d=(num/div)%10;
    bool lead=(num<div)&&(i<digits-1);
    sevenSegDigit(x,y,Wc,Hc,T,lead?10:d,lit,ghost); // >9 = ghost only
    num%=div; div/=10; x+=Wc+gap;
  }
}
// N-digit width = N*Wc + (N-1)*gap (3 digits = 30px).
```

Other cells in use (measured from the sketches):

| Use | Sketch | Wc | Hc | T | gap |
|---|---|---|---|---|---|
| Header counters | `3d_cube` | 8 | 11 | 2 | 3 |
| Pet stats | `neko` | 6 | 8 | 1 | 2 |
| Pet stats (CYD) | `neko_cyd` | 12 | 16 | 2 | 4 |
| Game score | `flappy` | 8 | 12 | 2 | 3 |
| Game score (CYD) | `flappy_cyd` | 16 | 26 | 4 | 6 |

Worked header (160x128, from `3d_cube/3d_cube.ino`):

```cpp
fbFillRect(0,0,WIDTH,14,bg);
fbText57(4,4,"BALLS:",ink);
sevenSegNum(36,1,nb,3,ink,ghost);       // ends x=66
fbText57(109,4,"FPS",ink);
sevenSegNum(126,1,fpsShown,3,ink,ghost); // ends x=156 (4px right margin)
fbHLine(4,14,WIDTH-8,accentDK());
fbFillRect(4,15,WIDTH-8,3,accent());
```

Labels sit at y=4 (5px tall → rows 4–8), digits at y=1 (rows 1–11) — both
vertically centered in the 14px band with ≥1px padding.

### Header alignment rule (derive, don't hardcode)

- **Label↔digit gap is 3px** on both sides: digits start at
  `labelX + textW(label,1) + 3`; a right-side label ends at `digitX - 3`, so
  its x is `digitX - 3 - textW(label,1)`. (`3d_cube`'s `textW57("BALLS:")` = 29
  → digits at 4+29+3 = 36; `"FPS"` = 14 → label at 126−3−14 = 109. Note
  `3d_cube`'s `textW57(const char*)` takes no scale argument — see §3.)
- **Right group is anchored from the edge**: last digit block starts at
  `WIDTH - 6 - digitsW` (6px text margin; 3 digits = 30px → x=126).
- The same 3px gap applies to footer hints and SET-row captions.

## 3. The 4x5 Bitmap Font (labels + footer)

Used for all the small words (POMO, SET, state names, footer hints, scene
labels, FOOD/HAPPY/REST/CLEAN, menu buttons). The 4×5 glyph is the sweet spot
for 160×128: small enough to fit long labels like "LONG BREAK" without
truncation, still legible. 3×5 was tried and found too crude on the C3; 5×7 was
tried and found too large.

**On 320x240 (CYD), 4x5 at `scale=2` is the preferred choice** (`neko_cyd` is
the reference). Three CYD ports still use 3×5 tables
(`pomodoro_cyd`, `flappy_cyd`, `3d_cube_cyd`); do not propagate that to new
work.

Scale usage (`neko`): header/status/stats/menu/footer stay at 1x; only splash
wordmarks and transient glyphs (`Z`, `+`) go 2x. Never scale chrome labels up
for emphasis — use ink vs ghost and the red accent instead.

Row bytes, MSB = left column, 5px advance at 1x (`4*scale+scale` at 2x). Full
A-Z + 0–9 subset plus `! : . ? - +` (~170 bytes):

```cpp
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

static inline const uint8_t* glyph45(char ch){
  if(ch>='0'&&ch<='9') return gDig45[ch-'0'];
  if(ch>='A'&&ch<='Z') return gLet45[ch-'A'];
  if(ch=='!') return gExcl45;
  if(ch==':') return gColon45;
  if(ch=='.') return gDot45;
  if(ch=='?') return gQuest45;
  if(ch=='-') return gDash45;
  if(ch=='+') return gPlus45;
  return gQuest45;           // never render an unknown char as nothing
}

// Canonical API, as implemented in neko / neko_cyd:
void fbChar(int x,int y,char ch,uint16_t c,int scale){
  if(ch==' ') return;
  if(ch>='a'&&ch<='z') ch-=32;          // normalize case — labels are UPPER
  const uint8_t* g=glyph45(ch);
  for(int row=0;row<5;row++){
    uint8_t bits=g[row];
    for(int col=0;col<4;col++)
      if(bits&(0b1000>>col)) fbFillRect(x+col*scale,y+row*scale,scale,scale,c);
  }
}
void fbText(int x,int y,const char* s,uint16_t c,int scale=1){
  while(*s){ fbChar(x,y,*s,c,scale); x+=5*scale; s++; }
}
int textW57(const char* s,int scale=1){ int n=0; while(*s++)n++; return n?(n*5*scale-scale):0; }
```

**Naming drift is real.** The `57` suffix is legacy from a 5×7 experiment —
the tables are 4×5. When editing an existing sketch, keep its local names:

| Sketch | Font | Text API |
|---|---|---|
| `flappy` | 4x5 | `fbDrawChar57` / `fbText57` / `textW57(s,scale)` |
| `neko`, `neko_cyd` | 4x5 | `fbChar` / `fbText` / `textW57(s,scale)` (case-safe) |
| `pomodoro` | 4x5 | `fbDrawChar57` / `fbDrawText1x` / `fbDrawTextScaled` / `textW57(s,scale)` |
| `3d_cube` | 4x5 | `fbDrawChar57` / `fbText57` / `textW57(s)` (no scale arg) |
| `pomodoro_cyd`, `flappy_cyd` | 3x5 | `fbDrawDigitScaled` / `fbDrawCharScaled` / `fbDrawTextScaled` / `textW(s,scale)` |
| `3d_cube_cyd` | 3x5 | `fbDrawDigit` / `fbDrawNumber` / `fbDrawLetter` / `fbDrawStr` |

⚠️ **Handle every char you can receive.** `pomodoro_cyd`/`flappy_cyd` and
`3d_cube_cyd` silently draw nothing for unknown chars, and none of them
uppercase. New code: normalize case and fall back to `?` (the `glyph45` above).

## 4. Component Gallery (borderless LCD, no bezel — pick only what you need)

The panel spans the full display with no physical watch bezel. Nothing here is
mandatory except the style itself (§1–§3 plus the red accent). Each component is
opt-in:

1. **Header row** (recommended) — left label ("POMO"), centered preset
   ("25/5"), right state ("READY"/"WORK"). Scene variant: left label + ghost
   7seg counter ("BALLS: 080"), right label + counter ("FPS 060"). Carries
   identity + key numbers.
2. **Red accent strip** (signature, cheap) — a dark line + a thicker red bar.
   F-91W "ALARM" sweep. Variants in use: `pomodoro` y=13 dark + y=14 h4;
   `3d_cube` y=14 + y=15 h3; `neko` y=11 + y=12 h4; `pomodoro_cyd` 2px dark at
   `BAR_Y=26` + 8px red; `neko_cyd` y=24 dark + y=25 h8. Size it to the board —
   keep the recipe (1px dark + Npx red, no gap) and the red hue.
3. **Big MM:SS digits** (timers/clocks only) — the 7-seg time, centered.
4. **Progress bar** (only with real 0..1 progress) — thin double-border rect
   (`fbRect` edge then HI) with red fill fraction. Never decoration.
5. **SET indicator row** (only for set/checklist semantics) — literal squares +
   a `n/4` fraction; filled = done, current square blinks.
6. **Divider line** + **footer hints** (only if controls need documenting) —
   left = TAP action, right = HOLD action (in red).
7. **LCD art sprites** (`neko`) — an etched illustration system. A sprite is a
   `const char` grid in PROGMEM where `#` = solid ink, `+` = checker shade,
   `:` = sparse shade, `.` = transparent. Draw at `SPR_SC` (2 on C3, 4 on CYD)
   so each cell becomes a block/pixel pair:

   ```cpp
   // '#' solid, '+' checker, ':' sparse — engraving tones
   void drawSprite(int x,int y,const char* spr,int rows,int cols,uint16_t c){
     for(int r=0;r<rows;r++){
       const char* row=spr+r*(cols+1);
       for(int cc=0;cc<cols;cc++){
         char ch=(char)pgm_read_byte(&row[cc]);
         if(ch=='.') continue;
         int px0=x+cc*SPR_SC, py0=y+r*SPR_SC;
         if(ch=='#') fbFillRect(px0,py0,SPR_SC,SPR_SC,c);
         else if(ch=='+'){ FPIX(px0,py0,c); FPIX(px0+1,py0+1,c); }
         else if(ch==':') FPIX(px0,py0,c);
       }
     }
   }
   ```

   Variants for growth stages (`KITTEN/CHILD/ADULT/SENIOR`), props (ball,
   bowl, heart, spark, poop, ghost).

   Canvas + placement (160x128, from `neko`): `SPR_W 68 × SPR_H 29` cells at
   `SPR_SC 2` (4 on CYD), `SPR_X = (WIDTH - SPR_W*SPR_SC)/2`, `SPR_Y =
   PET_FLOOR - SPR_H*SPR_SC`; a small `CAT_DX` nudge (e.g. -4) centers the
   body + tail. Sprites are `PROGMEM` rows of `cols+1` chars (the stride
   includes the terminator). The three tones do the work — solid `#` for
   outlines/features, checker `+` for volume, sparse `:` for rim light; a flat
   one-tone silhouette reads as clip-art, not as an etched LCD.

   **Eyes are separate from the body.** Keep a per-stage anchor table in
   sprite-cell coordinates and blank the eye rect to BG before drawing the
   expression, so moods never redraw the cat:

   ```cpp
   struct CatStage { const char* spr; uint8_t ex, ey, ew, eh; };
   const CatStage CATS[4] = { /* kitten, child, adult, senior */ };
   // open = filled rects · blink = 2px line · sick = two fbDrawLines (X)
   ```

   **Animated overlays** are ordinary sprites/text drawn relative to
   `SPR_X/SPR_Y`, all driven by `millis()`: Zzz letters stepping up-right and
   growing (`scale` 1→2) while sleeping, a bouncing `|sin|` ball while playing,
   sparkles while washing, a red `+` while healing, hearts while eating/happy,
   and a bowl whose kibble pile shrinks by repainting the interior in BG.
   Poops sit on the floor line in front of the cat.

   **Backdrop** (cheap, sells the scene): a 2px dot grid in `C_GHOST` behind
   the art plus a dithered ground shadow that shrinks as the cat lifts:

   ```cpp
   void drawDotGrid(){          // every other pixel over the scene area
     for(int y=SPR_Y;y<PET_FLOOR+2;y+=2)
       for(int x=0;x<WIDTH;x+=2) fb[y*WIDTH+x]=C_GHOST;
   }
   void drawGroundShadow(int lift){      // 3 checker rows; narrower when lifted
     int hw=54-lift*3; if(hw<20) hw=20;
     int cx=80+CAT_DX, y=PET_FLOOR-1;
     for(int e=0;e<3;e++)
       for(int x=cx-(hw-e*9);x<cx+(hw-e*9);x++)
         if((x+y+e)%2==0) FPIX(x,y+e,C_GHOST);
   }
   ```

8. **Compact stat rows** (`neko`) — 4x5 label + small ghost-7seg (`statNum`)
   per attribute: FOOD / HAPPY / REST / CLEAN.
9. **Button bar** (`neko`) — five outlined cells (FEED/PLAY/WASH/SLEEP/HEAL);
   the selected cell is ink-filled with a red underline, the rest are outlines.
10. **Game HUD** (`flappy`) — a framed score module (ghost 7-seg number +
    auto-width counter), scrolling ground with dashed tread, capped pipes,
    animated bird. Ready screen and game-over modal are plain light-panel
    overlays, not inverted.

Coordinates (160x128 from `pomodoro.ino`; double them for 320x240):

```cpp
// 160x128
fbDrawText1x(6, 3, "POMO", ink);
fbDrawText1x((WIDTH - tw)/2, 3, pl, ink);                    // preset centered
fbDrawText1x(WIDTH-6-tw, 3, sn, ink);                       // state right

fbHLine(4, 13, WIDTH-8, accentDK());                         // thin dark line
fbFillRect(4, 14, WIDTH-8, 4, accent());                     // thick red bar

// digits: timeY = 24 + ((86-24) - Hc)/2
// progress bar: y=86 h=8
// SET row: y=100, squares 8x8 at x = 52 + i*16
// divider: y=114
// footer: y=119
```

On 320x240 (CYD) the named constants come from `pomodoro_cyd.ino`:
`HDR_Y 8`, `BAR_Y 26`, `DIG_Y 60`, `PROG_Y 168`, `SET_Y 196`, `DIV_Y 224`,
`FTR_Y 226`, and `FS2 2`. `neko_cyd` has its own block (`HDR_Y 8`, `ACC_Y 24`,
`PET_FLOOR 156`, `LBL_Y 170`, `DIG_Y 183`, `BAR_Y 212`). Keep layout in named
constants so it's easy to re-center.

## 5. Framebuffer Rendering (zero flicker)

Always render to a framebuffer and flush once — the LCD block updates
atomically and animated squares/colon/sprites don't tear. See the
`esp32-c3-ws2812` skill for the primitives (`fbClear`, `fbHLine`, `fbFillRect`,
`fbRect`, `fbFillCircle`, `fbDrawCircle`, `fbDrawLine`, `fbFlush`).

```cpp
void render(){
  fbClear(panelBG()); // always the light LCD — no state switching
  // header, red strip, digits, bar, SET row, div, footer, sprites
  fbFlush();
}
```

- C3: full 160x128 framebuffer (40KB), one flush.
- CYD: the 150KB frame doesn't fit DRAM — draw 64-row strips into `fb` and
  flush per band (`tft.setAddrWindow(0, fbTop, WIDTH, h)` + `writePixels`); row
  primitives clip against `fbTop`. See `pomodoro_cyd.ino`/`neko_cyd.ino`.
- Set `tft.setSPISpeed(40000000)` and, on the ST7789, `tft.invertDisplay(false)`.
- One exception: `3d_cube_cyd` is a deliberately dark cyan wireframe scene with
  its own HUD — it does **not** follow this style. Leave it alone.

## 6. State UI Patterns (all on the light panel)

- **Idle/ready**: light LCD, "READY", steady colon, footer `TAP START / HOLD PRESET`
  (right hint in red).
- **Running** (work/break): SAME light LCD — signal it with the state label
  ("WORK"/"BREAK"), a fast-blinking colon, and the filling red progress bar.
  Footer `TAP PAUSE / HOLD PRESET|RESET`.
- **Paused**: SAME light LCD, colon slow-blinks, footer `TAP RESUME / HOLD RESET`.
- **DONE flash**: full-panel blink white↔light-LCD-green with centered "DONE!" +
  subtitle. White is fine here — it always returns to light green.
- **SET squares**: filled = completed; the *current* square blinks while the
  relevant state is active; the rest are open outlines.
- **Colon**: fast blink when running, slow when paused, steady when idle.

Pet/game variants (`neko`, `flappy`):
- **Splash**: light panel, centered name/stage, hint footer; times out into idle.
- **Mood/sleep/sick**: swap the sprite + eye animation, keep the same palette;
  shade with `C_DIM`/`C_SPIRIT`, never invert.
- **Transient message**: the header center alternates between a `say()` message
  (~2.5s) and the persistent stage/state name — feedback never needs a modal.
- **Critical blink**: an urgent status label blinks `accent ↔ ink`; a critical
  stat digit blinks `accent ↔ ghost` (`blinkOn(t) = (t/380)%2==0`); values that
  are fine stay steady ink. One blink cadence for the whole screen.
- **Scene motion**: idle bob every ~650ms (`dy = -3`), sleep breathing ~1.6s,
  play jump via `|sin|`, eat bob — all `millis()`-driven, never `delay()` in
  the render path.
- **Dead/R.I.P.**: light panel with a ghost sprite + "R.I.P." text and a revive
  hint — no black background.
- **Selected menu cell**: ink-fill + red underline; blink only the active
  action while it runs. A cell's label changes with state (`SLEEP` ↔ `WAKE`).
- **Game over**: light modal over the frozen scene ("TAP TO RETRY"), score
  module unchanged.

Never invert to a black/negative panel to show state. If a state needs
emphasis, use the red accent (progress fill, active square, right footer hint).

## 7. Porting Checklist

- [ ] Single light palette, no state switching (`panelBG/Ink/Ghost/HI/Edge`,
      `accent`, `accentDK` — plain functions or constants, never a function of
      state). Delete any negative-mode branch.
- [ ] All required colors go through the sketch's `rgb()` helper
      (`tft.color565`).
- [ ] Ghost-segment pass before lit pass in every digit.
- [ ] 4x5 labels via the tables above (CYD: 4x5 at scale 2 preferred; existing
      3x5 ports are legacy).
- [ ] Every receivable char renders something: uppercase-normalize input and
      fall back to `?` — no silent blanks.
- [ ] `textW*()` used to center labels/stats instead of hand-calculated offsets.
- [ ] Layout in named constants for Y positions so centering is predictable.
- [ ] No decorative bars: every progress bar / SET row / footer earns its place.
- [ ] Framebuffer flush once per frame (strip loop on CYD); `setSPISpeed(40MHz)`
      and `invertDisplay(false)` on CYD.
- [ ] Blink timings from `millis()` (never `delay()` in the render path).
- [ ] Sprites use the 3 engraving tones (`#`/`+`/`:`) and separate eye anchors;
      scenes with "ground" get the dot-grid backdrop + dithered shadow.
- [ ] State feedback uses the shared `(t/380)%2` blink and transient header
      messages — no modals, no inversions.
- [ ] If using a NeoPixel (C3 only) for physical LED feedback, keep it
      independent of the LCD (pomodoro: red pulse=work, green pulse=break,
      blue breath=idle, amber blink=paused, white/red flash=done).

## 8. Spacing System (measured from `pomodoro.ino`, 160x128)

The watch look depends as much on spacing as on color. Tokens below are for
160x128; double them for 320x240.

- **Outer margins**: 4px top and bottom (footer text at y=119, 5px tall → 4px
  clearance). Keep top/bottom symmetric.
- **Side margins**: 6px for text/labels; 4px inset for full-bleed bars (red
  strip x=4, w=WIDTH-8). Text never touches the screen edge; bars bleed closer.
- **Header baseline varies by sketch** — `pomodoro` labels y=3, `3d_cube`
  labels y=4 with counters y=1, `neko` `HDR_Y 5`. Pick one and keep the ≥1px
  padding rule inside the header band; don't copy a y without its band height.
- **Header-to-strip gap**: 5px when the header text ends at y=8 (thin line at
  y=13). Strips sit below the header band — `3d_cube` has no gap after its 14px
  band.
- **Strip recipe**: 1px dark line + 3–8px red bar directly below, no gap. The
  bar thickness scales with the board (3–4px on 160x128, 8px on 320x240).
- **Divider-to-footer gap**: 5px (divider y=114, footer text y=119).
- **Caption gaps**: 4px between a caption and its digits/squares.
- **Horizontal centering**: labels via `(WIDTH - textW(s, scale))/2`; digit
  blocks via `(WIDTH - totalW)/2` where `totalW = Wc*4 + 4*gap + colonW`.
- **Right-align pattern**: `x = WIDTH - 6 - textW(s, scale)` (6px text margin).
- **Vertical centering in a zone**: `y = zoneTop + (zoneH - H)/2` (pomodoro
  centers the 44px digits between the strip bottom and the progress bar).
  Small labels next to taller controls get a manual offset: 5px text on 8px
  squares sits at `squareY + 2` so both centers align.
- **Breathing room**: digits and text never touch a band or screen edge — ≥1px
  padding inside chrome bands.
- **4x5 tokens** (160x128; double for 320x240): 5px advance, 5px cap height.
  Footer hint at y=119 (rows 119–123). Center with
  `(WIDTH - textW(s, scale))/2`; right-align with `x = WIDTH - 6 - textW(s, scale)`.
- **`neko` layout block** (160x128): `HDR_Y 5`, `ACC_Y 11` (dark line y=11, red
  bar 12–15), `PET_FLOOR 76` (ground line), `LBL_Y 85`, `DIG_Y 92`, `BAR_Y 110`
  (menu divider), `DIV_Y 111` / `FTR_Y 116` (splash). Stat columns:
  `cx = 20 + i*40`, centered 4x5 label at `LBL_Y`, 6×8 ghost digits at
  `cx-7, DIG_Y`. Button bar: `cellW = WIDTH/5`, cells `(x0+2, 112, cellW-4, 13)`
  with labels y=116, selected cell ink-filled + 2px red underline at y=121,
  the rest outlined in `edge`.

## Reference implementations

| Sketch | Board | Style notes |
|---|---|---|
| `pomodoro/pomodoro.ino` | C3 | Canonical timer: big ghost digits, progress bar, SET row, footer. Light-only. |
| `pomodoro_cyd/pomodoro_cyd.ino` | CYD | Same layout ×2, strip framebuffer, touch tap/hold. 3x5 labels. |
| `3d_cube/3d_cube.ino` | C3 | Scene header variant: BALLS/FPS counters + trimmed single-hint footer; 4x5 subset. |
| `neko/neko.ino` | C3 | Richest reference: halftone sprites + per-stage eye anchors, dot-grid backdrop, critical blink, stat rows, 5-button bar, splash/death. |
| `neko_cyd/neko_cyd.ino` | CYD | Neko ×2 with 4x5 labels at scale 2 and sprite scale 4. |
| `flappy/flappy.ino` | C3 | Canonical 4x5 API (`fbDrawChar57`/`fbText57`), game HUD, ready/game-over overlays. |
| `flappy_cyd/flappy_cyd.ino` | CYD | Flappy ×2, strip framebuffer, 3x5 labels. |
| `3d_cube_cyd/3d_cube_cyd.ino` | CYD | **Not an F-91W sketch** — deliberate dark cyan HUD. Do not convert. |
