---
name: signal-vhs-ui
description: Recreate the "SIGNAL" breaking-news theme with an animated VHS glitch for ESP32 TFT sketches (ST7735 160x128 C3 and ST7789 320x240 CYD) — near-black deck, white ink, magenta + signal-green accents, light-grey rules, Departure Mono pixel type (11px raster + hand-drawn 5x6 small set), a huge block countdown, and per-element glitch hits whose rip rows cycle the six monitor primaries (yellow/cyan/green/magenta/red/blue). Use when restyling or building SIGNAL sketches (pomodoro, 3d_cube, neko, flappy, btc_ticker, pong_clock and their _cyd ports) or when the user mentions the signal theme, VHS/chromatic glitch, colourful rips, magenta/green palette, or Departure Mono.
---

# SIGNAL / VHS UI Style

A flat, high-contrast "breaking news" theme for the sketches, with a colourful
VHS filter that glitches one UI element at a time.

Reference implementations: `btc_ticker` / `btc_ticker_cyd` (ticker + portals),
`pong_clock` / `pong_clock_cyd` (per-lane pong clock). Hardware, framebuffer,
strip-rendering and touch details live in the `esp32-c3-ws2812` skill.

The look: **#101010 / #242424 deck**, **white ink**, a **magenta accent strip**
and filled pips, **signal-green** progress + live pip + state text, **light-grey
rules**, a **giant block countdown**, and a static-tuned **VHS filter** that
tears individual elements sideways at random and washes the rip rows with the
six monitor primaries.

## 1. Palette

```cpp
uint16_t cBlack(){   return rgb( 16,  16,  16); } // #101010 (C3) / rgb(36,36,36) #242424 (CYD)
uint16_t cWhite(){   return rgb(255, 255, 255); } // ink, digits
uint16_t cMagenta(){ return rgb(236,   0, 140); } // #EC008C accent strip, filled pips
uint16_t cGreen(){   return rgb(  0, 230, 118); } // #00E676 progress, live pip, state, tap feedback
uint16_t cLine(){    return rgb(170, 170, 170); } // light grey rules + frames
uint16_t cMagentaD(){return rgb(104,   0,  62); } // dimmed magenta under the strip
```

Rip primaries (only the glitch uses these — saturated monitor-test colours):

```cpp
uint16_t vhsPal(uint8_t i) {
  switch (i % 6) {
    case 0: return rgb(255, 255,   0); // yellow
    case 1: return rgb(  0, 255, 255); // cyan
    case 2: return rgb(  0, 255,   0); // green
    case 3: return rgb(255,   0, 255); // magenta
    case 4: return rgb(255,   0,   0); // red
    default:return rgb(  0,   0, 255); // blue
  }
}
```

Role rules learned by iterating:
- **Rules/outlines are light grey, never black.** On a `#242424` deck the old
  `rgb(45,45,45)` "rule" colour was invisible/dirty; `cLine()` is the fix.
- Magenta owns the **accent strip**, the **filled SET pips** and the DONE
  strobe. Green owns the **progress fill**, the **live pip**, the **state
  label** and the **tap-press feedback**. White is everything else.
- The deck colour differs on purpose: C3 `#101010`, CYD `#242424`. Don't
  "unify" them without being asked.
- `pomodoro_cyd` still declares legacy `cInk/cMute/cRule/cGhost` helpers from
  earlier iterations — unused; don't build on them.

## 2. Type — Departure Mono, raster + hand-drawn small

Departure Mono is a pixel font (OFL 1.1, © Helena Zhang), pixel-perfect at
multiples of 11px. The 11px set is rasterized from the OTF; the *small* set is
**hand-drawn 5x6 pixels in the same style** because outline rasterization below
11px lands off the grid and thresholding knocks thin strokes out — at 9px the
first X/W/A/4 came out with holes and jitter (FreeType mono mode only helped a
little). Don't regenerate the small set from outlines:

```bash
python3 tools/font/make_departure_font.py --size 11 --prefix dm  --out btc_ticker/DepartureMono.h
python3 tools/font/make_departure_font.py --pixel --prefix dms --out btc_ticker/DepartureMonoSmall.h
```

| Board | Big (countdown / price / headings) | Labels / body |
|---|---|---|
| C3 | `DepartureMono.h` 11px (cap 8, adv 7) at scale ≥ 2 | `DepartureMonoSmall.h` 5x6 (cap 6, adv 6) at scale 1 |
| CYD | `DepartureMono.h` 11px at scale ≥ 2 | `DepartureMono.h` 11px at scale 1 |

Draw semantics: **`y` is the cap/digit top**; the cell is drawn at
`y - <PFX>_TOP*scale`, so headers/footers keep their original y maths.

`textW57()` / `textW()` must dispatch on the same rule as the drawer — on the
C3, `scale == 1` uses the small metrics and everything else the 11px metrics.

Sizing learned from user feedback:
- The C3 has three usable sizes: 5x6 (small), 11px@2 (16px ink) and up. A
  "medium" 11px@1 needs a small helper on the C3 (`fbText11()`/`textW11()`:
  the 11px glyph path without the scale multiply) because scale 1 dispatches to
  the small set.
- **Setup/portal screens use 11px at 1x** (title, SSID, URL included). At 2x
  (C3) or 3x (CYD) users report the WiFi setup text as "too big" — and a
  12-char SSID overflows a 160px screen at 2x. Keep body lines in the small set
  on the C3, and fall back to it when a string (e.g. an SSID) won't fit 11px.

**Straggler check** (this bit the repo twice — pomodoro and btc_ticker shipped
it for days): open the sketch's `DepartureMonoSmall.h` and read its header
comment. If it says `--size 9` / "rasterized", the file is the broken outline
raster and must be regenerated in place:

```bash
python3 tools/font/make_departure_font.py --pixel --prefix dms --out <sketch>/DepartureMonoSmall.h
```

The defines are identical (`DMS_W/H/ADV/TOP`, 59 glyphs, 14-row cells), so the
renderer and layout need no changes; digits end up 1px lower than the old
raster (uniform baseline with the caps) which has never needed a layout tweak.
Fixed so far: `btc_ticker`, `pomodoro`. `3d_cube` (C3) is the last known
straggler. CYD ports have no small font at all — they use the 11px raster at
every scale.

## 3. Layout

| Row | C3 (160x128) | CYD (320x240) |
|---|---|---|
| header text (cap top) | y 3 | `HDR_Y` 9 |
| live pip | 4x4 @ (34, 5) | 6x6 @ (46, HDR_Y+1) |
| accent strip | line y 13, bar y 14 h 4 | lines 26/27, bar 28 h 8 |
| countdown | scale 4 @ y 40 | scale 8 @ `DIG_Y` 64 |
| progress bar | x 6, y 86, w 148, h 8 | x 12, y 168, w 296, h 16 |
| SET pips | 8x8 @ x 52+i*16, y 100 | 16x16 @ x 104+i*32, `SET_Y` 192 |
| footer | divider 114, text 119 | divider 216, text 224 |

Header order is POMO + pip on the left, preset **centred on the screen** (CYD)
or in the gap between POMO and the state (C3 — a 160px screen can't fit a
screen-centred preset plus a long state label), state right-aligned.
Vertically centre each label row inside its band once the font is small.

## 4. Components

- **Live pip** blinks only while a phase runs (`(millis()/500)%2==0`),
  otherwise solid.
- **Accent strip breathes** while running: `scale565(cMagenta(), 205..255)`
  driven by `sinf(millis()*0.004f)`.
- **Countdown** is the block font, centred, with the colon replaced by a space
  when the blink is off — never drawn as a ghost.
- **Progress bar** is a light-grey double frame with a green fill plus a
  moving highlight (`scale565(cGreen(), 340)`, 20px wide on CYD / 10px on C3).
- **SET row**: filled magenta pips, light-grey outline for the rest, the
  current pip blinks.
- **DONE card**: magenta/black strobe (`(millis()-doneFlashStart)/200 % 2`),
  big `DONE!`, message and "TAP TO CONTINUE".

Pong clock variant (`pong_clock`, `pong_clock_cyd`):
- The score is the time, so the "countdown" slot holds a **block clock**:
  Departure Mono `HH:MM` at scale 3 (C3) / scale 4 (CYD, one per lane), hours
  and minutes drawn separately so each can flash magenta when it scores.
- Paddles are **magenta on the left, signal green on the right**, the ball is
  white, the net is `cLine()` dashes behind the clock. **No ball trail** — a
  ghost trail was tried and the user asked for just the ball; don't add one back.
- CYD shows three world-clock lanes (one pong match each), a label above every
  lane, and the lane bands double as the glitch elements.

## 5. The VHS glitch (the signature animation)

Colour-only post-process on the finished frame: `vhsSrc` holds a copy, then
each row reads shifted red/blue taps.

```cpp
float ph = (float)t * 0.0018f;                       // slow chroma ripple
float lsh = 1.4f + 1.3f * sinf(y*0.085f + ph);       // red pulled left
float rsh = 1.8f + 2.1f * sinf(y*0.061f + 1.7f + ph*1.3f); // blue pulled right
// sub-pixel interpolation of the taps, then:  r = (3*rSrc + base) >> 2
int flick = 246 + (vhsNoise(t / 110u) % 11);         // 246..256 tape flicker
```

Per-element hits keep it from feeling like a strobe (and are what the user
approved) — tuned up after the first pass ("more colours, a little more
intense"):

```cpp
static bool elemHit(int i, uint32_t t, int &dx, uint8_t &pal){
  const uint32_t SLOT = 6000;                        // 6s slots, per element
  uint32_t phased = t + (uint32_t)(i * 1013u);       // stagger the elements
  uint32_t s = vhsNoise(phased / SLOT * 2654435761u ^ ((uint32_t)(i + 3) * 97u));
  if((s % 5u) != 0) return false;                    // ~1 event / 5s overall
  uint32_t start = (s >> 8) % (SLOT - 500u);
  uint32_t dur   = 200u + ((s >> 20) % 240u);        // 200..440 ms burst
  uint32_t local = phased % SLOT;
  if(local < start || local >= start + dur) return false;
  dx  = (int)((s >> 12) % 11u) - 5;                  // ±5px sideways tear
  pal = (uint8_t)(s >> 24);                          // starting rip colour
  return true;
}
```

A hit adds `+7` to both chroma shifts, `+4` brightness and the `dx` offset for
that band, and **washes the band's rows with the primary palette**: the colour
index steps every 60 ms and every 8 rows inside the hit
(`vhsPal(pal + t/60 + (y>>3))`), mixed 75 % into ink pixels and 50 % into the
deck (branch on the green channel: `g > 22` = ink). That keeps text legible
while whole scanline stripes go saturated — the look the user asked for.
Bands (y0..y1, exclusive) map to the components:

| # | element | C3 pomodoro | CYD pomodoro | pong C3 | pong CYD |
|---|---|---|---|---|---|
| 0 | header | 2..12 | 8..18 | 2..13 | 2..20 |
| 1 | strip | 12..19 | 25..36 | 13..18 | 20..30 |
| 2 | countdown / lane 0 | 38..74 | 62..129 | court above 22..50 | 30..100 |
| 3 | progress / clock | 85..95 | 166..186 | clock 50..86 | 100..170 |
| 4 | SET / lane 2 | 99..109 | 190..210 | court below 86..126 | 170..240 |
| 5 | footer | 113..128 | 222..234 | — | — |

Hard-won rules:
- **No full-width horizontal bars.** Rolling tracking bands and random tear
  bands were rejected as too busy/noisy. Only per-element hits. (The webui is
  the exception — its drifting rainbow lines were also rejected, see §8.)
- **No quiet zones.** The countdown glitches with everything else now; the
  earlier quiet zone was a workaround for the full-width bands.
- Apply the filter to the timer frame only (`if (!introMode)`), so the DM-01
  boot intro stays clean.
- `elemHit` is evaluated **once per frame** (C3 full-frame) or **once per
  strip** from the shared `vhsT` (CYD), otherwise bands shear across strips.
- **Watch the cost on the CYD.** A per-pixel filter over 320x240 costs ~38 ms
  with sub-pixel chroma taps; nearest taps (`s[cx - li0]` instead of the
  interpolated pair) plus `setSPISpeed(80000000)` took pong_clock_cyd from
  10 to 14 FPS (draw 5 ms + filter 25 ms + flush 29 ms). The C3 filters a
  quarter of the pixels and can afford the interpolation.

## 6. Gotchas

- **Always `fbClear(bg)` in the non-DONE path.** When the signal theme was
  first ported the clear was dropped: frames accumulated, colours drifted and
  text turned to mush — it looked like "the glitch is too strong" for days.
- CYD blends must clip to the strip: `if(y < fbTop || y >= fbTop+FB_H) return;`
  and index with `(y-fbTop)*WIDTH+x`.
- Scratch buffers cost RAM: C3 `WIDTH*HEIGHT*2` (40KB), CYD `WIDTH*FB_H*2`
  (40KB) on top of the framebuffer — CYD sits around 32% DRAM.
- `sinf` per row (64 rows/strip) is cheap; the per-pixel tap maths is the cost.
- Wire the screenshot hook **after** the filter so captures show the glitch.
- **Multi-zone clocks must not switch `TZ` in the loop** (newlib leaks per
  change — a 3-lane clock wedges in ~20 minutes). Cache UTC offsets; see the
  `esp32-c3-ws2812` skill §10.
- **Measure with `micros()` + `ESP.getFreeHeap()`** before tuning: the CYD's
  numbers were draw 5 ms / filter 25 ms / flush 29 ms at 14 FPS, and the heap
  trace is what exposed the TZ leak.
- The generator downloads the font on first run (only for the raster sizes);
  `--pixel` needs no Pillow. Cache: `/tmp/departure-mono/DepartureMono.otf`.

## 7. Checklist

- [ ] `fbClear(bg)` on the normal path
- [ ] Light-grey rules (`cLine`), never black outlines on the deck
- [ ] Labels vertically centred in their rows; header re-centred for the font
- [ ] Countdown/price use the 11px raster, labels the small set (C3: hand-drawn 5x6)
- [ ] C3: `DepartureMonoSmall.h` is the `--pixel` hand-drawn set, not the 9px
      outline raster (check the header comment)
- [ ] `textW` dispatch matches the drawer
- [ ] Setup/portal screens draw at 11px 1x, never 2x/3x
- [ ] Glitch: ripple + flicker + per-element hits with the primary palette,
      no full-width bands
- [ ] Bands cover every element
- [ ] Filter skipped during the boot intro
- [ ] CYD: `setSPISpeed(80000000)`, nearest-tap filter, touch debounced
- [ ] Multi-zone clocks: cached TZ offsets, no `setenv` in the loop
- [ ] Both boards still compile (`esp32c3:CDCOnBoot=cdc`, `esp32:UploadSpeed=460800`)

## 8. Webui (Next.js deck) — same theme, cheap animations

`webui/` mirrors the SIGNAL deck (globals.css theme tokens + Gallery.tsx) and
experiments with the same glitch language. It lives on branches until the user
merges; `webui-colour-rips` holds the primaries variant.

The CPU traps found there — worth remembering for any web version of the theme:
- **Animate only `transform`/`opacity`.** `background-position` keyframes
  (scanline drift) repaint the whole viewport every frame: at 60 fps the deck
  idles at high CPU. Drift the scanline layer with `transform: translateY()` on
  an element that is 3px taller instead.
- **A continuous `filter: hue-rotate()` on a large masked element re-renders
  that layer every frame.** Use `steps(8, end)` so it only repaints on the
  jump, or leave it static.
- Text glitch effects (`text-shadow` + `clip-path` steps) and `opacity` pips
  are cheap — keep those, they carry the look.
- Full-width rainbow rip lines over the page were tried and **rejected**
  ("can you remove the lines") — keep rips to element accents (a 2px card rule,
  the hero underline) and the wordmark fringes.
- `npm run lint` and `npm run build` before pushing a theme branch.
