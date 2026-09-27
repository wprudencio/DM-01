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
ARDUINO_CLI=$(which arduino-cli) npm run firmware   # build all 40 merged images -> public/firmware/<project>/<c3|cyd>.bin
npm run dev        # http://localhost:3000
npm run deploy     # static export + wrangler deploy to Cloudflare Workers
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

## Deploy to Cloudflare Workers

The site builds as a static export (`output: "export"` in `next.config.ts`) and
ships as an assets-only Worker — `wrangler.jsonc` points wrangler at `out/` and
there is no Worker script, so asset requests are served straight from the edge.

```bash
npm run deploy     # next build && wrangler deploy
npm run preview    # next build && wrangler dev (local Workers runtime)
```

- One-time setup: `npx wrangler login` (or export `CLOUDFLARE_API_TOKEN`).
- Deploy credentials never live in the repo: `wrangler login` keeps its OAuth
  token under `$HOME`, CI should pass `CLOUDFLARE_API_TOKEN` as a secret, and
  local Worker secrets belong in `.dev.vars` (git-ignored).
- First deploy creates the `dm-01` Worker and serves it at
  `https://dm-01.<your-subdomain>.workers.dev`.
- Routing is handled by `assets.html_handling: "auto-trailing-slash"` (so `/vhs`
  serves `vhs.html`) and `assets.not_found_handling: "404-page"`.
- The `hn_*` firmware images carry no WiFi credentials (the sketches ship with
  empty defines) — set your SSID/password and run `npm run firmware` before
  flashing if you need them to join a network.
