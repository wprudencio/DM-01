# DM-01 boot intro

`Dm01Intro.h` is the shared DM-01 boot animation: a retro sunset with a rising
slatted sun, the logo flying out of the sun as colorful pixel blocks, locking
into a rainbow with a white wave sweep, then imploding back into the sun in a
burst. It runs for `DM01_INTRO_MS` (4000 ms) and is skippable with the board's
button/pad.

## Sync rule

`tools/intro/Dm01Intro.h` is the **master**. Each sketch has a copy next to its
`.ino` (same rule as `tools/screenshot/Screenshot.h`). Edit the master, then run:

```bash
tools/intro/sync.sh
```

## Usage

See the comment block at the top of `Dm01Intro.h`. Short version:

- 160x128 C3 sketch — in `setup()`: `dm01Start();`
  in `loop()`, first thing:

  ```cpp
  bool skip = (digitalRead(TOUCH_PIN) != idleLevel);
  if (dm01Frame(skip)) { fbFlush(); screenshotHandle(fb, WIDTH, HEIGHT); delay(16); return; }
  ```

  (`dm01Frame` draws one frame into `fb`; the sketch flushes it.)

- 320x240 CYD sketch — `#define DM01_SCALE 2` before the include, then use the
  strip pattern in the header comment (`dm01Tick` + `dm01Draw` per band).

Requirements: the sketch must provide `WIDTH`, `HEIGHT`, `rgb()`, and the usual
framebuffer primitives (`fbClear`, `fbHLine`, `fbFillRect`, `fbDrawLine`,
`fbFillCircle`, `FPIX`).
