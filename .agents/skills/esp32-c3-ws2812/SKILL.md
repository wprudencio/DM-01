---
name: esp32-c3-ws2812
description: Develop and flash ESP32 sketches in this repo — ESP32-C3 + ST7735 160x128 (WS2812 on GPIO 10, touch pad/BOOT button) and CYD ESP32-2432S028R + ST7789 320x240 (XPT2046 touch, CH340). Covers framebuffer/strip rendering, the shared DM-01 boot intro, WiFi reliability (TX power, retry/reset rules), serial screenshots, and Arduino CLI builds. Use when working on pomodoro, 3d_cube, neko, flappy, btc_ticker, their *_cyd ports, or flashing boards. Releases are out of scope.
---

# ESP32 Sketches — C3 / ST7735 + CYD / ST7789

Every project in this repo ships as two variants:

| Variant | Board | Display | Touch | LED | USB |
|---|---|---|---|---|---|
| **C3** | ESP32-C3 (RISC-V, 160MHz, 400KB SRAM, 4MB flash) | ST7735 160x128 | pad GPIO 0 / BOOT GPIO 9 | WS2812 GPIO 10 | native USB-Serial/JTAG |
| **CYD** | ESP32-2432S028R ("Cheap Yellow Display") | ST7789 320x240 | XPT2046 resistive | none | CH340 UART |

Projects: `pomodoro`, `3d_cube`, `neko`, `flappy`, `btc_ticker` (C3) and
`pomodoro_cyd`, `3d_cube_cyd`, `neko_cyd`, `flappy_cyd`, `btc_ticker_cyd`
(CYD). The visual style is documented separately in the `quartz-lcd-ui`
skill (note: `3d_cube_cyd` is a deliberate dark cyan scene and `btc_ticker` /
`btc_ticker_cyd` follow the VHS dark terminal palette, not the Quartz light LCD).

## Hardware

### ESP32-C3 pinout (ST7735 160x128)

| Signal | GPIO |
|---|---|
| TFT_CS | 5 |
| TFT_RST | 4 |
| TFT_DC | 3 |
| TFT_MOSI | 2 |
| TFT_SCLK | 1 |
| Touch pad / button | 0 (active LOW) |
| BOOT button | 9 (active LOW) |
| WS2812 LED | 10 |

### CYD pinout (ESP32-2432S028R, ST7789 320x240)

| Signal | GPIO |
|---|---|
| TFT_CS / DC / RST | 15 / 2 / 4 |
| TFT_MOSI / SCLK / MISO | 13 / 14 / 12 |
| TFT_BL (backlight) | 21 |
| TOUCH_CS / CLK / MOSI / MISO | 33 / 25 / 32 / 39 |
| TOUCH_IRQ | 36 (input-only — external pull-up on board) |

No NeoPixel on CYD — state feedback is on-screen + serial only.

### Toolchain

- `arduino-cli` must be on `PATH` (check `which arduino-cli`). There is **no**
  `bin/` directory in the repo, despite `build-firmware.sh` defaulting to
  `$ROOT/bin/arduino-cli`; override with `ARDUINO_CLI=$(which arduino-cli)`.
- If `arduino-cli` is missing entirely (fresh machine), bootstrap a throwaway
  copy outside the repo. The asset name carries the version and the
  `latest/download/arduino-cli_latest_*` URL can 404 — pin the real URL from
  the API first:

  ```bash
  curl -s https://api.github.com/repos/arduino/arduino-cli/releases/latest \
    | grep browser_download_url | grep macOS_ARM64        # macOS_64bit on Intel
  curl -fsSL -o /tmp/acli.tar.gz <that-url>
  mkdir -p /tmp/acli && tar -xzf /tmp/acli.tar.gz -C /tmp/acli
  /tmp/acli/arduino-cli core update-index
  /tmp/acli/arduino-cli core install esp32:esp32          # big, takes minutes
  /tmp/acli/arduino-cli lib install "Adafruit GFX Library" \
      "Adafruit ST7735 and ST7789 Library" "Adafruit NeoPixel"
  ```

  Then compile/upload with `/tmp/acli/arduino-cli` (or
  `ARDUINO_CLI=/tmp/acli/arduino-cli`). Data lives in `~/Library/Arduino15`
  (macOS) / `~/.arduino15` (Linux) — the Adafruit libraries may already be
  there, the esp32 core usually is not.
- FQBNs actually used:

| Target | FQBN |
|---|---|
| C3 (native USB serial) | `esp32:esp32:esp32c3:CDCOnBoot=cdc` |
| CYD (CH340) | `esp32:esp32:esp32:UploadSpeed=460800` |

- Ports (Linux): C3 shows up as `/dev/ttyACM*`; CYD (CH340) as `/dev/ttyUSB*`.
  macOS equivalents are `/dev/cu.usbmodem*` and `/dev/cu.usbserial*`.
  Always confirm with:

```bash
arduino-cli board list
```

### Which board is connected? (detect — don't make the user tell you)

Before compiling, flashing or screenshotting, identify the target yourself:

1. **Port + VID:PID** via `arduino-cli board list --format json` (or pyserial's
   `list_ports`). In this repo: `303a:1001` on `/dev/ttyACM*` = **C3** (native
   USB-Serial/JTAG; the esp32 core reports it as the generic "ESP32 Family
   Device"); `1a86:7523` (CH340) or `1a86:55d4` (CH9102) on `/dev/ttyUSB*` =
   **CYD**. A bare CH340 does not prove CYD — any ESP32 dev board can carry one.
2. **Definitive**: ask the chip through the esptool bundled with the core. This
   is read-only and briefly resets the board:

   ```bash
   ESPTOOL=$(ls -d "$(arduino-cli config get directories.data)"/packages/esp32/tools/esptool_py/*/esptool | sort -V | tail -1)
   "$ESPTOOL" --port /dev/ttyUSB0 flash-id
   # Chip type: ESP32-C3         -> C3 firmware (esp32c3 FQBN)
   # Chip type: ESP32-D0WD-V3    -> CYD firmware (plain esp32 FQBN)
   ```

3. Only if there is **no serial port at all**, or detection is genuinely
   ambiguous, ask the user which board is connected. Otherwise never assume and
   never ask.

## Quick Start — Create, Compile & Flash

```bash
# Compile
arduino-cli compile --fqbn esp32:esp32:esp32c3:CDCOnBoot=cdc my_sketch

# Flash C3 (port changes on every reconnect — check board list)
arduino-cli upload -p /dev/ttyACM0 --fqbn esp32:esp32:esp32c3:CDCOnBoot=cdc my_sketch

# Flash CYD
arduino-cli upload -p /dev/ttyUSB0 --fqbn esp32:esp32:esp32:UploadSpeed=460800 pomodoro_cyd
```

On the C3, `CDCOnBoot=cdc` is required for `Serial` to reach the USB port
(without it, output goes to UART0 and the screenshot tool sees nothing).

Minimal C3 sketch with a NeoPixel:

```cpp
#include <Adafruit_NeoPixel.h>

#define PIN        10
#define NUMPIXELS  1

Adafruit_NeoPixel pixels(NUMPIXELS, PIN, NEO_GRB + NEO_KHZ800);

void setup() {
  pixels.begin();
  pixels.setBrightness(50);
}

void loop() {
  pixels.setPixelColor(0, pixels.Color(255, 0, 0));
  pixels.show();
  delay(500);
  pixels.clear();
  pixels.show();
  delay(500);
}
```

## Monitoring Serial Output

```bash
arduino-cli monitor -p /dev/ttyACM0 -c baudrate=115200
```

---

# Display Guide — ST7735 (C3) and ST7789 (CYD)

## 1. Hardware SPI Initialization (Critical!)

The ST7735/ST7789 communicate over SPI. You **must** call `SPI.begin()` with
the correct pins before `tft.init*()`, or the display gets no data → white
screen.

### C3 — ST7735 160x128

```cpp
#define TFT_CS    5
#define TFT_RST   4
#define TFT_DC    3
#define TFT_MOSI  2
#define TFT_SCLK  1

Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_RST);

void setup() {
  // ⚠️ MANDATORY — tells the ESP32-C3 which pins are MOSI/SCLK (no MISO)
  SPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);

  tft.initR(INITR_BLACKTAB);
  tft.setSPISpeed(40000000);
  tft.setRotation(1);     // 0/2 = portrait, 1/3 = landscape
  tft.fillScreen(ST7735_BLACK);
}
```

### CYD — ST7789 320x240

```cpp
#define TFT_CS    15
#define TFT_DC    2
#define TFT_RST   4
#define TFT_MOSI  13
#define TFT_SCLK  14
#define TFT_MISO  12
#define TFT_BL    21

Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);

void setup() {
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);              // backlight on
  SPI.begin(TFT_SCLK, TFT_MISO, TFT_MOSI, TFT_CS);

  tft.init(240, 320);
  tft.setSPISpeed(40000000);
  tft.setRotation(1);                      // landscape 320x240
  // The library ST7789 init sends INVON; the CYD panel needs it OFF.
  tft.invertDisplay(false);
  tft.fillScreen(0x0000);
}
```

`invertDisplay(false)` is essential on the CYD — colors come out inverted
without it. All four `*_cyd` sketches set it.

## 2. The Rendering Pipeline — How the Display Works

Every `fillRect()`, `drawPixel()`, etc. goes through this chain:

```
CPU → SPI.transfer(data) → MOSI pin → Display controller → Frame buffer → LCD
```

Each `fillRect()` sends a **command** to set the drawing window plus a **data
burst** of RGB565 pixels (2 bytes each). Minimizing SPI writes is critical for
smooth animation.

## 3. Animation Strategy — Incremental Updates

Erase-old/draw-new works for simple scenes but flickers on complex ones: the
display updates as data arrives, so intermediate states are visible.

```cpp
if (newTop < prevTop) {
  tft.fillRect(bx, newTop, bw, prevTop - newTop, barColor);   // grew
} else if (newTop > prevTop) {
  tft.fillRect(bx, prevTop, bw, newTop - prevTop, BLACK);     // shrank
}
// Same height = zero SPI writes
```

Handle the first frame separately with a full draw:

```cpp
bool firstFrame = true;
void loop() {
  if (firstFrame) { tft.fillScreen(ST7735_BLACK); drawEverything(); firstFrame = false; }
  else { incrementalUpdates(); }
}
```

## 4. Full-Frame Framebuffer (C3, recommended for 3D/games)

The ST7735/ST7789 has no double buffer — draw the whole frame in RAM, then
blast it in one SPI transaction. Zero flicker.

### Memory budget

```cpp
// 160x128 × 2 bytes (RGB565) = 40,960 bytes ≈ 40KB
static uint16_t fb[WIDTH * HEIGHT];   // global scope — never on the stack!
```

ESP32-C3 has **400KB on-chip SRAM (16KB reserved as cache)**. 40KB framebuffer
+ ~15KB globals is easily fine.

### Canonical primitives (used across the C3 sketches — trim what you don't need)

```cpp
#define WIDTH  160
#define HEIGHT 128
static uint16_t fb[WIDTH * HEIGHT];
#define FPIX(x,y,c) if((x)>=0&&(x)<WIDTH&&(y)>=0&&(y)<HEIGHT) fb[(y)*WIDTH+(x)]=(c)

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
    if (d < 0) d += 4 * x + 6; else { d += 4 * (x - y) + 10; y--; }
    x++;
  }
}
void fbDrawCircle(int cx, int cy, int r, uint16_t c) {
  int x = 0, y = r, d = 3 - 2 * r;
  while (x <= y) {
    FPIX(cx+x,cy+y,c); FPIX(cx-x,cy+y,c);
    FPIX(cx+x,cy-y,c); FPIX(cx-x,cy-y,c);
    FPIX(cx+y,cy+x,c); FPIX(cx-y,cy+x,c);
    FPIX(cx+y,cy-x,c); FPIX(cx-y,cy-x,c);
    if (d < 0) d += 4 * x + 6; else { d += 4 * (x - y) + 10; y--; }
    x++;
  }
}
void fbDrawLine(int x0, int y0, int x1, int y1, uint16_t c) {
  int dx = abs(x1-x0), sx = x0 < x1 ? 1 : -1;
  int dy = -abs(y1-y0), sy = y0 < y1 ? 1 : -1;
  int err = dx + dy, e2;
  for (;;) {
    FPIX(x0,y0,c);
    if (x0 == x1 && y0 == y1) break;
    e2 = 2*err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}
void fbFlush() {
  tft.startWrite();
  tft.setAddrWindow(0, 0, WIDTH, HEIGHT);   // one address window
  tft.writePixels(fb, WIDTH * HEIGHT);      // one 40KB SPI burst
  tft.endWrite();
}
```

`writePixels()` exists in the installed Adafruit_SPITFT (≥1.12) — no fallback
needed.

### Render loop pattern

```cpp
void loop() {
  updatePhysics();
  fbClear(bg);
  drawScene();       // all in-memory, zero SPI
  fbFlush();         // one atomic burst
  delay(8);          // ~100 FPS target
}
```

### When to use framebuffer vs incremental

| Scenario | Method |
|---|---|
| Bar charts, simple meters, static text | Incremental |
| 3D wireframes, games, particle systems | **Framebuffer** |

| Operation | Direct SPI | Framebuffer |
|---|---|---|
| `fillCircle(r=6)` | ~1ms | ~10μs |
| `drawLine(40px)` | ~0.2ms | ~5μs |
| Full frame erase | ~11ms | ~1ms |
| Frame flush | N/A | ~8ms (40KB @ 40MHz) |

### Thick lines and layered blobs (cheap in RAM)

```cpp
// 3px edge: three offset lines
fbDrawLine(sx[a], sy[a], sx[b], sy[b], edgeColor);
fbDrawLine(sx[a]+1, sy[a], sx[b]+1, sy[b], edgeColor);
fbDrawLine(sx[a]-1, sy[a], sx[b]-1, sy[b], dimColor);

// 4-layer ball: halo → body → core → specular
fbDrawCircle(px, py, vr+1, darkHalo);
fbFillCircle(px, py, vr,   dimBody);
fbFillCircle(px, py, vr-1, brightCore);
fbFillCircle(px-1, py-1, vr-3, white);
```

## 5. CYD: Strip Framebuffer + XPT2046 Touch

A full 320x240 RGB565 frame is 150KB — it does not fit in ESP32 DRAM. CYD
sketches render in **64-row strips** (8 bands for 240 rows).

### Strip framebuffer

```cpp
#define FB_H 64
static uint16_t fb[WIDTH * FB_H];   // WIDTH=320
static int fbTop = 0;

// Every row primitive clips against the current band
#define FPIX(x,y,c) if((x)>=0&&(x)<WIDTH&&(y)>=fbTop&&(y)<fbTop+FB_H) \
  fb[(y-fbTop)*WIDTH+(x)]=(c)

void fbHLine(int x, int y, int w, uint16_t c) {
  if (y < fbTop || y >= fbTop + FB_H) return;
  if (x < 0) { w += x; x = 0; }
  if (x + w > WIDTH) w = WIDTH - x;
  if (w <= 0) return;
  uint16_t* p = &fb[(y - fbTop) * WIDTH + x];
  while (w--) *p++ = c;
}

void fbFlush() {
  int h = FB_H;
  if (fbTop + h > HEIGHT) h = HEIGHT - fbTop;
  tft.startWrite();
  tft.setAddrWindow(0, fbTop, WIDTH, h);
  tft.writePixels(fb, WIDTH * h);
  tft.endWrite();
}

void render() {
  for (fbTop = 0; fbTop < HEIGHT; fbTop += FB_H) {
    drawFrame();       // fbFillRect/… all clip to this band
    fbFlush();
  }
}
```

Everything else (fbClear, fbFillRect, fbRect, circles, lines) is the C3 code
from §4 with the `FPIX`/`fbHLine` bodies above. Scale factors on CYD sketches
differ per sketch (2x/4x/6x literals), unlike the C3 `FS2` convention.

### Touch (XPT2046 on a separate HSPI bus)

```cpp
#define TOUCH_CS   33
#define TOUCH_CLK  25
#define TOUCH_MOSI 32
#define TOUCH_MISO 39
#define TOUCH_IRQ  36
SPIClass touchSPI(HSPI);

void touchInit() {
  touchSPI.begin(TOUCH_CLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
  pinMode(TOUCH_CS, OUTPUT);
  digitalWrite(TOUCH_CS, HIGH);
  pinMode(TOUCH_IRQ, INPUT);   // GPIO36 is input-only: external pull-up on CYD
}
bool touchPressed() {
  // Debounce: a glitch must persist across 3 loop passes, so line noise can
  // neither tap the backlight nor fire the setup portal (there is no exit path
  // out of portal mode — a spurious hold parks the sketch there).
  static uint8_t lowN = 0;
  if (digitalRead(TOUCH_IRQ) != LOW) { lowN = 0; return false; }
  if (lowN < 3) { lowN++; return false; }
  return true;
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
  // rotation 1 swaps the axes: screen X comes from the Y channel
  tx = map(yRaw, 200, 3900, 0, 320);
  ty = map(xRaw, 3900, 200, 0, 240);
  return valid;   // untouched panels read near the rails (xRaw high / yRaw low)
}
```

Only start a press when `touchRead()` reports a valid reading — checking it on
the press edge is enough and costs nothing per frame.

Tap/hold conventions across ports: `pomodoro_cyd` reads coordinates and
distinguishes tap vs hold (1500ms); `neko_cyd` same with 700ms; `flappy_cyd`
tap-only with 80ms debounce; `3d_cube_cyd` polls IRQ only, no coordinates.

## 6. 3D Rendering Patterns

### Pre-compute rotation matrices

```cpp
// ❌ Per-particle trig:
void rotateParticle(Vec3 *p, float ax, float ay, float az) {
  float cx=cosf(ax), sx=sinf(ax); // called for every particle
}
// ✅ 6 trig calls per frame, reused for all particles:
float rot[9];
void compRot(float ax, float ay, float az) { /* ... */ }
void applyRot(Vec3 *p) { /* 9 mults + 6 adds, no trig */ }
```

### Local-space physics for 3D containers

```cpp
// Physics in LOCAL space = easy bounds checking
float bound = 0.85;
pos[i].y -= 0.002f;          // gravity
if (pos[i].y < -bound) {     // trivial collision
  pos[i].y = -bound;
  vel[i].y *= -0.1f;         // liquid: no bounce
}
// Then rotate to world space ONCE for rendering
Vec3 world = pos[i];
applyRot(&world);
project(world, &sx, &sy, &depth);
```

## 7. Performance Optimization

- `drawPixel` = 1 SPI command + 2 bytes (~5μs); `fillCircle(r=3)` = ~28 pixels
  (~140μs). Many particles → `drawPixel`; few elements → `fillCircle`.
- Pre-compute constant colors instead of calling `tft.color565()` per frame.
- Keep `delay()` ≤ 16ms for 60 FPS, ≤ 8ms for 100+ FPS.
- Use `fbHLine` inside `fillCircle` — never a per-pixel `FPIX` loop.
- **Measure before tuning**: wrap `drawFrame` / post-processing / flush in
  `micros()` and print averages every 5 s (plus `ESP.getFreeHeap()` /
  `getMinFreeHeap()` — a falling free heap is how the TZ leak was found).
  On the CYD a full 320x240 frame costs roughly: draw 5 ms, colour filter
  25 ms, SPI flush 29 ms at 80 MHz (45 ms at 40 MHz) → ~14 FPS.
- **CYD panels take `tft.setSPISpeed(80000000)`** — halves the strip flush
  (ST7789 specs 62.5 MHz; the module runs 80 MHz fine). The Adafruit
  `writePixels()` ESP32 path already uses the core's endian-swapping fast
  transfer, so a full-framebuffer `fbFlush()` is close to bus-bound.
- A per-pixel colour filter over 76.8k px is the CYD's biggest CPU item: use
  nearest-tap chroma (skip sub-pixel interpolation), hoist per-row constants,
  and keep the filter to the elements/bands that need it. The same filter on the
  C3 (20k px) is ~4x cheaper, which is why C3 effects can afford more.

## 8. Serial Screenshots (framebuffer capture)

> Capture on explicit request only — never automatically after sketch changes.

The `tools/screenshot/` tool dumps the framebuffer over USB serial to a PNG.

- **Master file**: `tools/screenshot/Screenshot.h`. Every sketch has a copy
  next to its `.ino` — edit the master, then re-copy to all sketches. Do not let
  copies drift (`flappy` carries one intentional local patch: `Serial.peek()`
  so the screenshot request doesn't swallow its other serial commands).
- Host script: `tools/screenshot/screenshot.py` (no Pillow dependency).
- Compile C3 with `CDCOnBoot=cdc` or no bytes reach the host.

**C3 (full-frame)** — all five C3 sketches are wired:

```cpp
#include "Screenshot.h"
void loop() {
  ...
  render();
  screenshotHandle(fb, WIDTH, HEIGHT);   // blocks only when a shot is requested
}
```

**CYD (strip)** — poll before rendering, send instead of flushing per band:

```cpp
#include "Screenshot.h"
static ScreenshotStripSession shot;
void loop() { screenshotStripPoll(shot, WIDTH, HEIGHT); ... }

void render() {
  for (fbTop = 0; fbTop < HEIGHT; fbTop += FB_H) {
    drawFrame();
    int h = FB_H; if (fbTop + h > HEIGHT) h = HEIGHT - fbTop;
    if (!screenshotStripSend(shot, fb, fbTop, h)) fbFlush();
  }
}
```

Host usage:

```bash
pip install -r tools/screenshot/requirements.txt

python3 tools/screenshot/screenshot.py -o neko.png              # first board found
python3 tools/screenshot/screenshot.py -p /dev/ttyACM0 -s 4 -o neko.png
python3 tools/screenshot/screenshot.py -n 8 -i 0.6 -o shot.png # 8 frames
python3 tools/screenshot/screenshot.py -w -i 1 -o shot.png     # watch until Ctrl-C
```

Flags: `-p/--port`, `-o/--out`, `-n/--count`, `-i/--interval`, `-w/--watch`,
`-s/--scale`, `-b/--baud`, `-t/--timeout`, `--settle`. Opening the port can
reset the board (the script waits `--settle`, default 1.5s); `--watch` keeps
the port open so there are no resets between shots. On USB CDC drops the script
re-detects and reconnects (5 attempts). Close any serial monitor or
`screenshot.py` watcher before WebSerial flashing — two processes on the port
corrupt the esptool session.

Opening the port **does** reset the ESP32-C3 (native USB). On sketches that
boot with the DM-01 intro and a blocking WiFi connect, the default 1.5s settle
only ever captures intro/connect frames — pass `--settle 12` or more (intro
3.45s + connect up to 15s). For live-motion checks, open the port once, sleep
past boot, then ping `'S'` manually; with `-n/--count`, a mid-run USB drop
reconnects and resets the board, which can make two "animation" frames
identical.

## 9. WiFi Reliability (C3 + CYD)

WiFi sketches in this repo: `btc_ticker`, `btc_ticker_cyd`, `hn_display`,
`hn_cyd`, `nyc_events_cyd`.

The C3 boards can be flaky at the default 19.5dBm TX power. Real case:
`hn_display` failed association on every cold boot with repeated
`AUTH_EXPIRE` (reason 2), progressed to `ASSOC_EXPIRE` / `4WAY_HANDSHAKE_TIMEOUT`
after stack resets, yet the same SSID + password worked from a laptop a metre
away. The fault was the board's radio, not the AP. **Fix: lower TX power.**

```cpp
WiFi.mode(WIFI_STA);
WiFi.setSleep(false);                    // power-save hurts reliability
WiFi.setTxPower(WIFI_POWER_8_5dBm);      // default 19.5dBm saturates marginal C3 antennas
WiFi.setAutoReconnect(false);            // retries are managed below
```

After that: 8/8 cold boots connected in 2.5–6s at −25dBm RSSI.

Rules learned the hard way:

- **Never call `WiFi.begin()` on a timer.** Doing it while the stack is still
  connecting logs `E wifi:sta is connecting, cannot set config` and wedges the
  attempt. Wait for a real failure, then disconnect/reset before retrying.
- **Hard-reset before each retry** (this reproduces the manual reboot that used
  to "eventually work"):

  ```cpp
  WiFi.persistent(false);
  WiFi.disconnect(true, true);   // erase stored AP config
  WiFi.mode(WIFI_OFF);
  delay(200);
  WiFi.mode(WIFI_STA);
  ```

- Boot: one direct `WiFi.begin(ssid, pass)` with a bounded wait (15s is plenty).
  Only the retry path pays for a hard reset + scan/visibility log.
- `loop()` reconnect attempts should back off (~15s) and kick a fetch after
  they succeed.
- Diagnostics: register `WiFi.onEvent()` and log the reason code.

  ```cpp
  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED)
      Serial.printf("WiFi disconnected reason=%d\n", (int)info.wifi_sta_disconnected.reason);
  });
  ```

  | Reason | Meaning | Action |
  |---|---|---|
  | 2 `AUTH_EXPIRE` | AP never answers auth frames | Check TX power/antenna first — not the password |
  | 4 `ASSOC_EXPIRE` | Association stalled | Hard reset + retry |
  | 15 `4WAY_HANDSHAKE_TIMEOUT` | Auth OK, key handshake failed | Wrong password, PMF mismatch, or heavy packet loss |
  | 3 `AUTH_LEAVE` / 36 `STA_LEAVING` | Our own `disconnect()` | Expected — ignore |
  | 203 `ASSOC_FAIL` | Association rejected | Retry with hard reset; check AP client limits |

- To separate board from AP: join the same 2.4GHz SSID from a laptop with the
  same password (`nmcli dev wifi connect ...`). If the laptop joins, the
  credentials and AP are fine — fix the ESP side (TX power, power supply,
  antenna).
- Do not hardcode WiFi credentials in new sketches — they leak into git
  history. Prefer a setup portal (`btc_ticker` has one) or NVS.

### WiFi failure UX (mandatory for every WiFi sketch)

An offline sketch that only retries forever is broken UX: the user cannot
change networks without a computer. Every WiFi sketch ships all of this:

- **Status screen names the failed SSID** (and the retry state) with an
  actionable hint line: `TAP RETRY / HOLD 2S: WIFI SETUP`, plus
  `BOOT 3S: WIFI SETUP` on a second line. Never a silent retry loop.
- **Tap = retry now** — set `lastReconnectMs = 0` so the watchdog redials on
  the next pass (online, tap keeps the sketch's own action, e.g. refresh).
- **Hold 2s while offline/connecting opens the `DM-01-Setup` portal** so new
  credentials can be entered; saving overwrites the failed network.
- **Hold BOOT 3s opens the portal from any state** — including while
  connected, to switch networks deliberately. Track the press origin
  (`bootOrigin`) at press start so a 3s BOOT hold cannot also fire the
  sketch's 1s touch hold.
- **The portal is never a trap**: serve `GET /reboot` (a "reboot without
  saving" link) and make on-device `HOLD BOOT 3S: EXIT` work — arm that exit
  only after BOOT is released, or the hold that opened the portal reboots
  straight back out of it.
- **Show the network it was trying** on the portal page
  (`Trying: <ssid>`, HTML-escaped) so the user knows what is being replaced.
- **Serial escape hatches**: `W` = portal, `R` = reboot. Read with `peek()`
  and only consume `W`/`R`, so a screenshot `S` stays queued for
  `Screenshot.h`.

Reference implementation: `nyc_events_cyd` (status screen, tap/hold/BOOT
gestures, `/reboot` link, `W`/`R` console).

### NTP clocks (`pong_clock` pattern)

- `configTzTime("UTC0", "pool.ntp.org", "time.cloudflare.com", "time.google.com")`
  starts SNTP and is safe to call again — the core `sntp_stop()`s first, so a
  60s retry while `time(nullptr) <= 1700000000` recovers from late DNS/DHCP.
- Multi-zone display: keep SNTP on UTC, then per zone
  `setenv("TZ", posix_tz, 1); tzset(); localtime_r(&t, &tm);` — newlib applies
  DST, no `tm_gmtoff` math needed. Zone strings in use: `UTC0`, `<-03>3`
  (São Paulo), `EST5EDT,M3.2.0,M11.1.0`, `GMT0BST,M3.5.0/1,M10.5.0`,
  `CET-1CEST,M3.5.0,M10.5.0/3`, `JST-9`, `AEST-10AEDT,M10.1.0,M4.1.0/3`,
  `IST-5:30`.
- **Never switch `TZ` inside the render loop.** newlib's `setenv`/`tzset` leak
  ~24 bytes on every TZ *change* (same-zone repeats are clean, which is why the
  single-zone C3 clock looked fine). `pong_clock_cyd` rotated three zones twice
  a second: −79 B/s, heap gone after ~20 minutes, allocations failing, frame
  frozen — the "stuck after a few minutes" bug. Cache offsets instead and
  derive zone time with integer math:

  ```cpp
  int zoneOff[ZONE_COUNT];  // seconds east of UTC
  int zoneOffDay = -1;
  static long daysFromCivil(long y, int m, int d) {   // Hinnant's algorithm
    y -= m <= 2;
    const long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long)doe - 719468;
  }
  void refreshZoneOffsets(bool force) {          // once a day: DST-safe
    time_t t = clockNow(); struct tm gt; gmtime_r(&t, &gt);
    if (!force && gt.tm_yday == zoneOffDay) return;
    for (int i = 0; i < ZONE_COUNT; i++) {
      setenv("TZ", ZONES[i].tz, 1); tzset();
      struct tm lt; localtime_r(&t, &lt);
      long asUtc = daysFromCivil(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday) * 86400L
                 + lt.tm_hour * 3600L + lt.tm_min * 60L + lt.tm_sec;
      zoneOff[i] = (int)(asUtc - (long)t);
    }
    zoneOffDay = gt.tm_yday;
  }
  void zoneHM(int zi, int& h, int& m) {
    long zt = (long)clockNow() + zoneOff[zi];
    zt = ((zt % 86400L) + 86400L) % 86400L;
    h = (int)(zt / 3600); m = (int)((zt / 60) % 60);
  }
  ```

  Do **not** compute the offset with the `mktime(&lt) - t` round trip: on this
  newlib it returned 0 for every zone (all the world clocks showed UTC). A
  one-shot probe printing the offsets caught it — verify offsets on hardware
  (`UTC 0`, `NEW YORK -14400`, `TOKYO 32400`, `KOLKATA 19800`).

  Dates come from `gmtime_r(now + offset)` plus a month string table — no
  `strftime`/locale/TZ interaction at all.
- Before sync `time()` counts from boot, so `time() + 43200` is a plausible
  fake epoch (~12:00 UTC): an offline clock keeps playing and snaps on sync.
- WiFi creds + a `tz` index share the NVS namespace `dm01`; the portal select
  is just `preferences.getInt("tz", 0)`.

## 10. DM-01 Boot Intro (shared header)

Every project can play the DM-01 boot animation: a retro sunset gradient with
a rising slatted sun, the logo flying out of the sun as pixel blocks, a rainbow
lock-in with a white wave sweep, then an implosion back into the sun and a
white burst. 3450 ms, skippable via the board's button/pad.

Code lives in `tools/intro/Dm01Intro.h` (master) plus a copy next to every
`.ino` — same sync rule as `Screenshot.h`:

```bash
tools/intro/sync.sh    # re-copy the master into all sketch folders
```

**Full-frame sketch (C3, 160x128)** — include the header after the framebuffer
primitives, then:

```cpp
// setup()
dm01Start();

// loop(), first thing
bool introSkip = (digitalRead(TOUCH_PIN) != idleLevel);  // sketch's own idle test
if (dm01Frame(introSkip)) {
  fbFlush();
  screenshotHandle(fb, WIDTH, HEIGHT);
  delay(16);
  return;
}
```

`dm01Frame()` draws one intro frame into `fb`; the sketch flushes it. Skip
input is debounced inside the header (5 consecutive pressed frames). C3
sketches can pulse the NeoPixel with `dm01Pal((int)(millis()/150))` while the
intro is on.

**Strip-framebuffer sketch (CYD, 320x240)** — `#define DM01_SCALE 2` before the
include, then either set an `introMode` flag and branch inside `render()` (see
the `*_cyd` sketches), or inline:

```cpp
if (dm01Tick(touchPressed())) {
  unsigned long t = dm01Elapsed();
  for (fbTop = 0; fbTop < HEIGHT; fbTop += FB_H) { dm01Draw(t); /* band flush */ }
  delay(16);
  return;
}
```

The header requires `WIDTH`, `HEIGHT`, `rgb()`, and the usual primitives
(`fbClear`, `fbHLine`, `fbFillRect`, `fbDrawLine`, `fbFillCircle`, `FPIX`).
The CYD integration is new — verify it on hardware the first time you flash a
CYD build.

## 11. Common Pitfalls

| Problem | Cause | Fix |
|---|---|---|
| **White screen** | `SPI.begin()` not called | Call it with correct pins before `tft.init*()` |
| **C3 serial silent** | Missing `CDCOnBoot=cdc` | Add `esp32:esp32:esp32c3:CDCOnBoot=cdc` |
| **CYD colors inverted** | ST7789 init sends INVON | `tft.invertDisplay(false)` after `tft.init(240,320)` |
| **CYD touch never fires** | IRQ pin 36 needs an external pull-up; wrong axis map | `pinMode(36, INPUT)`, swap axes: `tx=map(yRaw,200,3900,0,320)` |
| **CYD draw corrupts / crashes** | Full 150KB framebuffer attempted | Use the 64-row strip framebuffer (§5) |
| **C3 WiFi flaky / `AUTH_EXPIRE`** | Default 19.5dBm TX saturates marginal antenna | `WiFi.setTxPower(WIFI_POWER_8_5dBm)` (§9) |
| **`sta is connecting, cannot set config`** | `WiFi.begin()` called while connecting | Disconnect + back off; never begin on a timer (§9) |
| **"How do I change WiFi?" unreachable** | Portal only opens with empty NVS; the offline loop retries forever | Status screen + tap retry; hold 2s offline (or BOOT 3s anywhere) opens the portal; portal has a reboot-without-saving exit (§9) |
| **Tearing / flicker** | Full-screen clear every frame, or erase-then-draw | Framebuffer (one flush) or diff-only updates |
| **Slow FPS** | Long `delay()`, too many SPI writes | ≤16ms delay; render to RAM |
| **Display won't init** | Wrong `initR` parameter | Try `INITR_BLACKTAB`, `INITR_GREENTAB`, `INITR_144GREENTAB` |
| **Ghosting** | Drawing outside visible area | Verify `setRotation()` |
| **Port not found** | Reconnect changed the device name | `arduino-cli board list` (`/dev/ttyACM*`/`/dev/ttyUSB*`) |
| **Freeze after minutes (multi-zone clock)** | `setenv`/`tzset` leak ~24 B per TZ *change*; heap dies, allocations fail | Cache UTC offsets, no TZ switching in the loop (§9) |
| **Spurious taps / stuck on portal screen** | Raw XPT2046 IRQ on GPIO36 reads line noise as a press | Debounce 3 samples + validate the coordinates (§5) |
| **CYD FPS ~10** | 40 MHz flush + interpolated per-pixel filter | `setSPISpeed(80000000)`, nearest-tap filter (§7) |
| **C3 flash fails** | Boot mode issue | Hold **BOOT** (GPIO 9) while connecting |
| **NeoPixel too hot/bright** | Full brightness at 3.3V | `pixels.setBrightness(50)` |

## 12. Performance Checklist

- [ ] `SPI.begin()` called with correct pins (no MISO on C3: `-1`)
- [ ] `tft.setSPISpeed(40000000)` set (CYD: 80 MHz is fine and 2x the flush)
- [ ] CYD only: `invertDisplay(false)` + backlight GPIO 21 HIGH
- [ ] Multi-zone clocks: no `setenv`/`tzset` in the loop — cached offsets (§9)
- [ ] CYD touch: IRQ debounced + coordinate read validated (§5)
- [ ] 3D/games use a framebuffer; C3 full-frame, CYD 64-row strips
- [ ] Framebuffer at global scope (not stack); one flush per frame/band
- [ ] `delay()` ≤ 16ms for 60 FPS
- [ ] Incremental (simple scenes): only changed pixels written, first frame full-draw
- [ ] Rotation matrices pre-computed; colors pre-computed
- [ ] Physics in local space for container simulations
- [ ] Screenshot hook wired and `Screenshot.h` copy matches the master
- [ ] DM-01 intro wired (`dm01Start()` + `dm01Frame()`/`dm01Tick()`); `Dm01Intro.h` copy matches the master (§10)
- [ ] WiFi sketches: `setTxPower(WIFI_POWER_8_5dBm)` + `setSleep(false)`; retries
      disconnect first and never `begin()` on a timer (§9)
- [ ] WiFi sketches: failure UX mandatory — failed SSID on screen, `TAP RETRY /
      HOLD 2S` / `BOOT 3S` hints, portal with a reboot-without-saving exit (§9)

---

## Important Notes

- The ESP32-C3's built-in USB-Serial/JTAG handles flashing and serial — no
  external UART. The CYD needs its CH340 USB port.
- GPIO 10 is safe for WS2812 (no strapping conflicts on C3). The C3 BOOT button
  is GPIO 9; GPIO 0 is the touch pad used by the sketches.
- Always run `arduino-cli board list` before flashing — the port changes on
  reconnect.
- Keep `Screenshot.h` copies in sync with `tools/screenshot/Screenshot.h`; all
  sketches currently ship identical copies.
- The visual style (palette, 7-seg digits, 4x5 font, CYD 3x5 caveat) lives in
  the `quartz-lcd-ui` skill.
