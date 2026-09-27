---
name: firmware-release-pipeline
description: Publish ESP32 sketches to this repo's web flasher — capture serial screenshots, build merged 4 MB firmware images, and update webui (build-firmware.sh TARGETS, lib/firmwares.ts, screenshot assets, doc counts). Use ONLY when the user explicitly asks to release, publish, or update the web flasher; never during normal sketch development.
---

# Firmware Release Pipeline (web flasher)

> **Only on explicit request.** Never capture screenshots, rebuild firmware
> binaries, or touch `webui/` as a side effect of developing a sketch. Wait for
> the user to ask to release, publish, or update the web flasher. While
> developing, work only in the sketch folders.

`webui/` is a Next.js gallery (DM-01 dark synthwave theme) that previews each
sketch's screenshot and flashes it over WebSerial with esptool-js. Every
project ships two variants:

| Variant | Board | FQBN | Flash baud | Bin | Screenshot |
|---|---|---|---|---|---|
| C3 | ESP32-C3 + ST7735 160×128 | `esp32:esp32:esp32c3:CDCOnBoot=cdc` | 921600 | `/firmware/<project>/c3.bin` | `/screenshots/<project>.png` |
| CYD | ESP32-2432S028R + ST7789 320×240 | `esp32:esp32:esp32:UploadSpeed=460800` | 460800 | `/firmware/<project>/cyd.bin` | `/screenshots/<project>_cyd.png` |

`<project>` is the sketch dir minus `_cyd`. `3d_cube` is the exception: its
screenshots are `cube.png` / `cube_cyd.png`.

## Release checklist

When the user asks to release a new or renamed sketch, update **all** of these:

1. **Sketch dirs**: `<name>/` (C3) and `<name>_cyd/` (CYD), each with a copy of
   `Screenshot.h` and the screenshot hook wired in (see the `esp32-c3-ws2812`
   skill §8).
2. **`webui/scripts/build-firmware.sh`** — add to `TARGETS`:
   `"c3  <name>      $C3_FQBN"` and `"cyd <name>_cyd  $CYD_FQBN"`.
3. **`webui/lib/firmwares.ts`** — add a `FIRMWARES` entry (id, name, tagline,
   description) with both variants per the table above.
4. **Screenshots** — capture on the real boards and place each PNG in **both**
   `screenshots/` and `webui/public/screenshots/` (same filename).
5. **Build**: `cd webui && ARDUINO_CLI=$(which arduino-cli) npm run firmware` —
   writes `webui/public/firmware/<project>/{c3,cyd}.bin` (committed even though
   `*.bin` is ignored — `.gitignore` negates `!webui/public/firmware/**/*.bin`).
   Images are `-O` merged 4 MB binaries written at `0x0`.
6. **Verify**: `npm run lint && npm run build` in `webui/`; `npm run dev` →
   Chrome/Edge/Opera on localhost. Before editing Next.js code, read
   `webui/AGENTS.md` (this Next.js version has breaking changes; check
   `node_modules/next/dist/docs/`). Close any serial monitor or screenshot
   watcher before flashing — two processes on the port corrupt esptool.

## Capturing the screenshots

`tools/screenshot/screenshot.py` dumps the framebuffer over USB serial:

```bash
python3 tools/screenshot/screenshot.py -p /dev/ttyACM0 -s 3 -o shot.png   # C3
python3 tools/screenshot/screenshot.py -p /dev/ttyUSB0 -s 2 -o shot.png   # CYD
```

- Ports: C3 = `/dev/ttyACM*` (native USB-Serial/JTAG), CYD = `/dev/ttyUSB*`
  (CH340). Confirm with `arduino-cli board list`.
- **For screenshots, flash the app partition only** (`firmware-build/<sketch>/<sketch>.ino.bin`
  at `0x10000`). Writing the merged 4 MB image at `0x0` wipes the NVS partition,
  so every WiFi sketch (btc_ticker, pong_clock, github_squares)
  boots into the captive-portal screen and the capture is useless. If a board's
  NVS was already wiped by a merged flash, copy it back from the other board
  (same partition table): `esptool read-flash 0x9000 0x5000 nvs.bin` on the
  board that still connects, then `write-flash 0x9000 nvs.bin` on the wiped one.
- Sizes used by the gallery: C3 480×384 (`-s 3`), CYD 640×480 (`-s 2`).
- C3 needs the `CDCOnBoot=cdc` FQBN or nothing reaches USB.
- Opening the port resets the board: use `--settle` to skip the DM-01 intro and
  pick a frame that shows the app (`-n`/`-i` to sample several, `--watch` to
  keep the port open).
- C3 sketches call `screenshotHandle(fb, WIDTH, HEIGHT)` per loop; CYD sketches
  use the strip API (`screenshotStripPoll`/`screenshotStripSend`). If a sketch
  isn't wired yet, do that first (dev work, not release work).

## Counts to keep in sync

- `README.md` — sketches table, C3/CYD board lists, "build all N merged images".
- `webui/README.md` — supported sketch list and build count.
- `tools/screenshot/README.md` — "kept in sync across all N sketches".

The `esp32-c3-ws2812` skill covers everything that happens before this: pins,
Arduino CLI builds, framebuffer/strip rendering, WiFi, the DM-01 intro and the
screenshot hook itself.
