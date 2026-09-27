export type Variant = {
  id: "c3" | "cyd";
  label: string;
  board: string;
  chip: string;
  baud: number;
  bin: string;
  screenshot: string;
};

export type Firmware = {
  id: string;
  name: string;
  tagline: string;
  description: string;
  variants: Variant[];
};

export const REPO_URL = "https://github.com/wprudencio/esp32-ST7735-animations";

export const FIRMWARES: Firmware[] = [
  {
    id: "pomodoro",
    name: "POMODORO",
    tagline: "F-91W TIMER",
    description:
      "25/5 focus timer with ghost 7-seg countdown, red progress sweep and a 4-step SET tracker. Tap to start or pause, hold to switch preset.",
    variants: [
      {
        id: "c3",
        label: "C3",
        board: "ST7735 160×128",
        chip: "ESP32-C3",
        baud: 921600,
        bin: "/firmware/pomodoro/c3.bin",
        screenshot: "/screenshots/pomodoro.png",
      },
      {
        id: "cyd",
        label: "CYD",
        board: "ST7789 320×240",
        chip: "ESP32",
        baud: 460800,
        bin: "/firmware/pomodoro/cyd.bin",
        screenshot: "/screenshots/pomodoro_cyd.png",
      },
    ],
  },
  {
    id: "3d_cube",
    name: "3D CUBE",
    tagline: "WIREFRAME CUBE",
    description:
      "A rotating wireframe cube with up to 500 liquid particles that collide inside the glass. Tap the screen to drop another ball.",
    variants: [
      {
        id: "c3",
        label: "C3",
        board: "ST7735 160×128",
        chip: "ESP32-C3",
        baud: 921600,
        bin: "/firmware/3d_cube/c3.bin",
        screenshot: "/screenshots/cube.png",
      },
      {
        id: "cyd",
        label: "CYD",
        board: "ST7789 320×240",
        chip: "ESP32",
        baud: 460800,
        bin: "/firmware/3d_cube/cyd.bin",
        screenshot: "/screenshots/cube_cyd.png",
      },
    ],
  },
  {
    id: "flappy",
    name: "FLAPPY",
    tagline: "PIPE DODGER",
    description:
      "F-91W flavoured Flappy Bird: tap to flap, dodge scrolling pipes and rack up a ghost 7-seg score. High score saved to NVS.",
    variants: [
      {
        id: "c3",
        label: "C3",
        board: "ST7735 160×128",
        chip: "ESP32-C3",
        baud: 921600,
        bin: "/firmware/flappy/c3.bin",
        screenshot: "/screenshots/flappy.png",
      },
      {
        id: "cyd",
        label: "CYD",
        board: "ST7789 320×240",
        chip: "ESP32",
        baud: 460800,
        bin: "/firmware/flappy/cyd.bin",
        screenshot: "/screenshots/flappy_cyd.png",
      },
    ],
  },
  {
    id: "neko",
    name: "NEKO",
    tagline: "DIGITAL PET",
    description:
      "Feed, play, wash, sleep and heal a virtual cat. Ghost 7-seg stats, four growth stages and progress saved to NVS.",
    variants: [
      {
        id: "c3",
        label: "C3",
        board: "ST7735 160×128",
        chip: "ESP32-C3",
        baud: 921600,
        bin: "/firmware/neko/c3.bin",
        screenshot: "/screenshots/neko.png",
      },
      {
        id: "cyd",
        label: "CYD",
        board: "ST7789 320×240",
        chip: "ESP32",
        baud: 460800,
        bin: "/firmware/neko/cyd.bin",
        screenshot: "/screenshots/neko_cyd.png",
      },
    ],
  },
  {
    id: "btc_ticker",
    name: "BTC TICKER",
    tagline: "BINANCE CANDLES",
    description:
      "Live Binance candlestick terminal: last 10 candles, 24h change and a demo chart while offline. Boots a captive portal for WiFi setup — tap for the next pair, hold for the next timeframe.",
    variants: [
      {
        id: "c3",
        label: "C3",
        board: "ST7735 160×128",
        chip: "ESP32-C3",
        baud: 921600,
        bin: "/firmware/btc_ticker/c3.bin",
        screenshot: "/screenshots/btc_ticker.png",
      },
      {
        id: "cyd",
        label: "CYD",
        board: "ST7789 320×240",
        chip: "ESP32",
        baud: 460800,
        bin: "/firmware/btc_ticker/cyd.bin",
        screenshot: "/screenshots/btc_ticker_cyd.png",
      },
    ],
  },
  {
    id: "pong_clock",
    name: "PONG CLOCK",
    tagline: "NTP WATCH",
    description:
      "A Pong match plays itself and the score IS the time — hours left, minutes right — kept honest by NTP with a captive portal for WiFi and timezone. Tap for the WS2812 nightlight, hold to re-provision.",
    variants: [
      {
        id: "c3",
        label: "C3",
        board: "ST7735 160×128",
        chip: "ESP32-C3",
        baud: 921600,
        bin: "/firmware/pong_clock/c3.bin",
        screenshot: "/screenshots/pong_clock.png",
      },
      {
        id: "cyd",
        label: "CYD",
        board: "ST7789 320×240",
        chip: "ESP32",
        baud: 460800,
        bin: "/firmware/pong_clock/cyd.bin",
        screenshot: "/screenshots/pong_clock_cyd.png",
      },
    ],
  },
  {
    id: "github_squares",
    name: "GITHUB SQUARES",
    tagline: "CONTRIBUTION GRAPH",
    description:
      "The public GitHub contribution calendar as green squares: 30 weeks, yearly total and streaks from a keyless API, with a deterministic demo year offline. Portal stores WiFi and the username.",
    variants: [
      {
        id: "c3",
        label: "C3",
        board: "ST7735 160×128",
        chip: "ESP32-C3",
        baud: 921600,
        bin: "/firmware/github_squares/c3.bin",
        screenshot: "/screenshots/github_squares.png",
      },
      {
        id: "cyd",
        label: "CYD",
        board: "ST7789 320×240",
        chip: "ESP32",
        baud: 460800,
        bin: "/firmware/github_squares/cyd.bin",
        screenshot: "/screenshots/github_squares_cyd.png",
      },
    ],
  },
  {
    id: "asteroids",
    name: "ASTEROIDS",
    tagline: "VECTOR SHOOTER",
    description:
      "Vector Asteroids on the 3d_cube wireframe renderer: rocks drift and split, firing is automatic. Tap rotates the ship 45°, hold thrusts. Three lives, growing waves.",
    variants: [
      {
        id: "c3",
        label: "C3",
        board: "ST7735 160×128",
        chip: "ESP32-C3",
        baud: 921600,
        bin: "/firmware/asteroids/c3.bin",
        screenshot: "/screenshots/asteroids.png",
      },
      {
        id: "cyd",
        label: "CYD",
        board: "ST7789 320×240",
        chip: "ESP32",
        baud: 460800,
        bin: "/firmware/asteroids/cyd.bin",
        screenshot: "/screenshots/asteroids_cyd.png",
      },
    ],
  },
  {
    id: "dvd",
    name: "DVD BOUNCE",
    tagline: "SCREENSAVER",
    description:
      "The classic DVD screensaver: the official wordmark drifts across a dark deck, changes colour on every wall hit and — rarely — nails a corner. Tap cycles 1x/2x/3x speed, hold resets the counters.",
    variants: [
      {
        id: "c3",
        label: "C3",
        board: "ST7735 160×128",
        chip: "ESP32-C3",
        baud: 921600,
        bin: "/firmware/dvd/c3.bin",
        screenshot: "/screenshots/dvd.png",
      },
      {
        id: "cyd",
        label: "CYD",
        board: "ST7789 320×240",
        chip: "ESP32",
        baud: 460800,
        bin: "/firmware/dvd/cyd.bin",
        screenshot: "/screenshots/dvd_cyd.png",
      },
    ],
  },
  {
    id: "hn",
    name: "HACKER NEWS",
    tagline: "TOP STORIES",
    description:
      "Top Hacker News stories from the keyless Firebase API — rank, score and title under the SIGNAL/VHS glitch. Hold to marquee-scroll long titles; the list auto-refreshes hourly.",
    variants: [
      {
        id: "c3",
        label: "C3",
        board: "ST7735 160×128",
        chip: "ESP32-C3",
        baud: 921600,
        bin: "/firmware/hn/c3.bin",
        screenshot: "/screenshots/hn_display.png",
      },
      {
        id: "cyd",
        label: "CYD",
        board: "ST7789 320×240",
        chip: "ESP32",
        baud: 460800,
        bin: "/firmware/hn/cyd.bin",
        screenshot: "/screenshots/hn_cyd.png",
      },
    ],
  },
];
