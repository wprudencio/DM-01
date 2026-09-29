# ESP32 framebuffer screenshots

Dump the on-screen framebuffer from a sketch over USB serial and save it as a PNG.
Works for any project that draws into a `uint16_t fb[WIDTH * HEIGHT]` framebuffer.

## How it works

1. The sketch includes `Screenshot.h` and calls it once per `loop()`:

   ```cpp
   #include "Screenshot.h"          // copy Screenshot.h next to your .ino
   ...
   void loop() {
     ...
     render(t);
     screenshotHandle(fb, WIDTH, HEIGHT);   // blocks only when a shot is requested
   }
   ```

2. The host script sends `S`, the sketch replies with:

   ```
   "SHOT" | uint16 width (MSB) | uint16 height (MSB) | w*h RGB565 pixels (little-endian)
   ```

3. The script converts the pixels and writes a PNG (no Pillow needed).

## Usage

The sketch must be built with **USB CDC On Boot enabled**, otherwise `Serial`
goes to UART0 and nothing reaches the USB port:

```bash
arduino-cli compile --fqbn esp32:esp32:esp32c3:CDCOnBoot=cdc neko
arduino-cli upload  -p /dev/ttyACM0 \
                    --fqbn esp32:esp32:esp32c3:CDCOnBoot=cdc neko
```

(On macOS the C3 port is `/dev/cu.usbmodem*`; run `arduino-cli board list`.)

Then run the script:

```bash
pip install -r tools/screenshot/requirements.txt

# one frame from the first board found, 3x upscale
python3 tools/screenshot/screenshot.py -o neko.png

# explicit port and bigger image
python3 tools/screenshot/screenshot.py -p /dev/ttyACM0 -s 4 -o neko.png

# animation: 8 frames, 0.6 s apart -> shot_000.png ... shot_007.png
python3 tools/screenshot/screenshot.py -n 8 -i 0.6 -o shot.png

# keep capturing every second until Ctrl-C; the port stays open,
# so the board is not reset between shots -> shot_000.png, shot_001.png, ...
python3 tools/screenshot/screenshot.py -w -i 1 -o shot.png
```

Options: `-p/--port`, `-o/--out`, `-n/--count`, `-i/--interval`,
`-w/--watch`, `-s/--scale`, `-b/--baud`, `-t/--timeout`, `--settle`.

## Notes

- The board pauses its render loop while a frame is transmitted
  (well under a second for 160x128 over USB CDC).
- Opening the port can reset the ESP32-C3; the script waits `--settle`
  (1.5 s) before requesting a frame, and re-syncs on the `SHOT` magic
  so boot logs are ignored. With `--watch` the port is opened only once,
  so there are no resets between shots.
- If the USB CDC link drops mid-capture (common on the C3), the script
  closes, re-detects the port and reconnects automatically (5 attempts).
- `Screenshot.h` is kept in sync across all 21 sketches (eleven C3 + ten CYD).
  `flappy` carries one local patch: `Serial.peek()` instead of `read()` so the
  screenshot request doesn't swallow the sketch's other serial commands.
  Edit the master here, then re-copy it — don't let the copies drift.
- C3 sketches call `screenshotHandle(fb, WIDTH, HEIGHT)` once per loop.
- CYD sketches use the strip API (`ScreenshotStripSession`,
  `screenshotStripPoll` in `loop()`, `screenshotStripSend` per 64-row band
  inside `render()`), since the full 320x240 frame doesn't fit in one buffer.
- The `esp32-c3-ws2812` skill documents the wider build/flash/screenshot
  workflow; publishing to the web flasher (screenshots included) is release
  work — do it only on request.
