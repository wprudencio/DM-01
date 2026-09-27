# webui

Next.js gallery (DM-01 dark synthwave theme, Silkscreen + Geist Mono) that previews the
`pomodoro`, `3d_cube`, `neko`, `flappy`, `btc_ticker`, `pong_clock`, `github_squares`,
`asteroids`, `dvd` and `hn_display` sketches and flashes them over
WebSerial with [esptool-js](https://github.com/espressif/esptool-js):

- **C3** — ESP32-C3 + ST7735 160×128 (`pomodoro`, `3d_cube`, `neko`, `flappy`, `btc_ticker`,
  `pong_clock`, `github_squares`, `asteroids`, `dvd`, `hn_display`)
- **CYD** — ESP32-2432S028R + ST7789 320×240 (`pomodoro_cyd`, `3d_cube_cyd`, `neko_cyd`,
  `flappy_cyd`, `btc_ticker_cyd`, `pong_clock_cyd`, `github_squares_cyd`, `asteroids_cyd`,
  `dvd_cyd`, `hn_cyd`)

```bash
npm install
ARDUINO_CLI=$(which arduino-cli) npm run firmware   # build all 20 merged images -> public/firmware/<project>/<c3|cyd>.bin
npm run dev        # http://localhost:3000
```

- `npm run firmware` shells out to `scripts/build-firmware.sh`; it defaults to
  `../bin/arduino-cli`, so point `ARDUINO_CLI` at your binary if there is no
  local `bin/` copy.
- Images are merged 4 MB binaries (bootloader + partition table + OTA selector +
  app) written at `0x0`. C3 builds use `CDCOnBoot=cdc`; CYD builds talk over the
  CH340 at 460800 baud.
- Flashing requires Chrome/Edge/Opera on `localhost` or HTTPS.
- Close any serial monitor or `tools/screenshot` watcher before flashing —
  a second process holding the port corrupts the esptool session.
- Adding a sketch? See the `firmware-release-pipeline` skill for the full
  checklist (`build-firmware.sh` targets, `lib/firmwares.ts` entry,
  screenshots) — release only, on request.

For static hosting add `output: "export"` to `next.config.ts` and serve the
generated `out/` directory.
