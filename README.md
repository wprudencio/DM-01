# DM-01


https://github.com/user-attachments/assets/bfb8bbe7-ee6e-4292-86b1-a62717d555e8


Two boards, one project per animation: **ESP32-C3** with an ST7735 160×128 TFT (SPI), WS2812 NeoPixel (GPIO 10) and touch pad GPIO 0 / BOOT button GPIO 9; and the **CYD** (ESP32-2432S028R) with an ST7789 320×240 TFT and XPT2046 touch.

No WiFi credentials or API tokens are committed here. The four `hn_*` sketches
ship with empty `WIFI_SSID` / `WIFI_PASSWORD` defines — set your network before
flashing. The CYD `github_squares` sketches have no default GitHub token: an
empty token falls back to the public mirror API, and a token can still be
entered in the on-device setup portal.

## Development Skills

This repo includes [Agent Skills](https://agentskills.io)-compatible skills at [`.agents/skills/`](.agents/skills/) that teach AI coding agents how to work with this hardware and style:

- [`esp32-c3-ws2812`](.agents/skills/esp32-c3-ws2812/) — C3 + CYD pinouts, Arduino CLI flashing, framebuffer and 64-row strip rendering, WiFi reliability, DM-01 intro, serial screenshots.
- [`casio-f91w-lcd-ui`](.agents/skills/casio-f91w-lcd-ui/) — the Casio F-91W LCD look: light palette, ghost 7-segment digits, 4x5 font, red accent strip, sprites and component patterns.
- [`firmware-release-pipeline`](.agents/skills/firmware-release-pipeline/) — publishing to the web flasher: screenshots, merged `.bin` builds, `webui` entries. Used only when a release is requested.

The skills are auto-discovered by Agent Skills–compatible agents (opencode, Claude Code, Codex, …) working in this repo.

## Sketches

| Project | C3 | CYD | Description |
|---------|----|-----|-------------|
| [`pomodoro`](pomodoro/) | ✅ | [`pomodoro_cyd`](pomodoro_cyd/) | F-91W 25/5 focus timer: ghost 7-seg countdown, red progress sweep, 4-step SET tracker |
| [`3d_cube`](3d_cube/) | ✅ | [`3d_cube_cyd`](3d_cube_cyd/) | Wireframe cube + up to 500 liquid particles with collision physics |
| [`neko`](neko/) | ✅ | [`neko_cyd`](neko_cyd/) | F-91W LCD Tamagotchi: feed/play/wash/sleep/heal, ghost 7-seg stats, growth stages, NVS save |
| [`flappy`](flappy/) | ✅ | [`flappy_cyd`](flappy_cyd/) | Flappy Bird: tap to flap past scrolling pipes, ghost 7-seg score, NVS high score |
| [`btc_ticker`](btc_ticker/) | ✅ | [`btc_ticker_cyd`](btc_ticker_cyd/) | DM-01 Binance candlestick terminal: 10 candles, 24h change, captive WiFi portal, demo fallback |
| [`pong_clock`](pong_clock/) | ✅ | [`pong_clock_cyd`](pong_clock_cyd/) | Self-playing Pong clock: score is the time, NTP + portal; CYD runs LOCAL/NEW YORK/TOKYO lanes |
| [`github_squares`](github_squares/) | ✅ | [`github_squares_cyd`](github_squares_cyd/) | Keyless GitHub contribution calendar as green squares, totals/streaks, demo year |
| [`asteroids`](asteroids/) | ✅ | [`asteroids_cyd`](asteroids_cyd/) | Vector wireframe shooter: tap to rotate 45°, hold to thrust, auto-fire |
| [`dvd`](dvd/) | ✅ | [`dvd_cyd`](dvd_cyd/) | DVD screensaver: official wordmark bouncing, colour per wall hit, corner celebration |
| [`hn_display`](hn_display/) | ✅ | [`hn_cyd`](hn_cyd/) | Hacker News top stories from the keyless Firebase API: rank/score/title list, hold to marquee |

## Web Flasher

[`webui/`](webui/) is a Next.js gallery that previews each sketch's screenshot and flashes
it over WebSerial with [esptool-js](https://github.com/espressif/esptool-js). Projects ship
C3, CYD, or both while a port is being prepared:

- **C3** — ESP32-C3 + ST7735 160×128 (`pomodoro`, `3d_cube`, `neko`, `flappy`, `btc_ticker`,
  `pong_clock`, `github_squares`, `asteroids`, `dvd`, `hn_display`)
- **CYD** — ESP32-2432S028R + ST7789 320×240 (`pomodoro_cyd`, `3d_cube_cyd`, `neko_cyd`,
  `flappy_cyd`, `btc_ticker_cyd`, `pong_clock_cyd`, `github_squares_cyd`, `asteroids_cyd`,
  `dvd_cyd`, `hn_cyd`)

```bash
cd webui
npm install
ARDUINO_CLI=$(which arduino-cli) npm run firmware   # build all 40 merged images (20 sketches × C3/CYD) into public/firmware/
npm run dev        # open http://localhost:3000 in Chrome/Edge/Opera
npm run deploy     # static export + wrangler deploy to Cloudflare Workers
```

Each variant is a merged 4 MB binary (bootloader + partition table + OTA selector + app)
written at `0x0`. C3 builds use `CDCOnBoot=cdc`; CYD builds flash over the CH340 at 460800 baud.

The flasher deploys to Cloudflare Workers as an assets-only Worker: `npm run deploy`
builds the static export (`output: "export"`) and uploads it with wrangler — see
[`webui/README.md`](webui/README.md) for details. The published `hn_*` images are
built from the credential-free sources, so they boot without a network until you
rebuild them with your own WiFi defines.
