// DM-01 BTC ticker — CYD (ESP32-2432S028R) version.
// Port of btc_ticker/btc_ticker.ino (ESP32-C3 + ST7735 160x128) to the Cheap
// Yellow Display: ST7789 320x240 + XPT2046 resistive touch.
//
// Live Binance ticker in the house VHS style (see the signal-vhs-ui
// skill): near-black deck, magenta accent strip, 4x5 labels at 2x, ghost
// 7-segment price, and an etched sparkline — a halftone fill under an ink
// curve over a dot grid. Selector bar at the bottom. Boots with the shared
// DM-01 intro; offline it falls back to deterministic demo data.
//
// Controls: touch screen replaces the C3 pad — tap = next pair, hold >= 1s =
// next timeframe. No NeoPixel on CYD, so state feedback is on-screen + serial.
// A LOADING screen holds after the intro until the first live fetch; offline
// the ticker falls back to deterministic demo data.
//
// WiFi reliability follows the esp32-c3-ws2812 skill §10: TX power lowered to
// 8.5dBm, sleep off, auto-reconnect off, retries hard-reset first, and begin()
// is never called on a timer. Credentials live in NVS ("dm01"), written by the
// captive portal — never hardcoded.

#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <math.h>
#include "Screenshot.h"

// ── Pins: CYD (ESP32-2432S028R) ──
#define TFT_CS    15
#define TFT_DC    2
#define TFT_RST   4
#define TFT_MOSI  13
#define TFT_SCLK  14
#define TFT_MISO  12
#define TFT_BL    21

// ── Touch (XPT2046 on separate HSPI bus) + BOOT button (GPIO 0) ──
#define TOUCH_CS   33
#define TOUCH_CLK  25
#define TOUCH_MOSI 32
#define TOUCH_MISO 39
#define TOUCH_IRQ  36
#define BOOT_PIN   0

Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);
SPIClass touchSPI(HSPI);

#define WIDTH  320
#define HEIGHT 240

// ── Framebuffer (strip-based: 64 rows; the 150 KB full frame does not fit) ──
#define FB_H 64
static uint16_t fb[WIDTH * FB_H];
static int fbTop = 0;
static ScreenshotStripSession shot;

#define FPIX(x,y,c) if((x)>=0&&(x)<WIDTH&&(y)>=fbTop&&(y)<fbTop+FB_H) fb[(y-fbTop)*WIDTH+(x)]=(c)

void fbClear(uint16_t c) {
  for (int i = 0; i < WIDTH * FB_H; i++) fb[i] = c;
}
void fbHLine(int x, int y, int w, uint16_t c) {
  if (y < fbTop || y >= fbTop + FB_H) return;
  if (x < 0) { w += x; x = 0; }
  if (x + w > WIDTH) w = WIDTH - x;
  if (w <= 0) return;
  uint16_t* p = &fb[(y - fbTop) * WIDTH + x];
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
    if (d < 0) d += 4 * x + 6;
    else { d += 4 * (x - y) + 10; y--; }
    x++;
  }
}
void fbDrawLine(int x0, int y0, int x1, int y1, uint16_t c) {
  int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int err = dx + dy, e2;
  for (;;) {
    FPIX(x0, y0, c);
    if (x0 == x1 && y0 == y1) break;
    e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}
void fbFlush() {
  int h = FB_H;
  if (fbTop + h > HEIGHT) h = HEIGHT - fbTop;
  tft.startWrite();
  tft.setAddrWindow(0, fbTop, WIDTH, h);
  tft.writePixels(fb, WIDTH * h);
  tft.endWrite();
}

uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) { return tft.color565(r, g, b); }

// ── SIGNAL palette — near-black deck, magenta + signal green (VHS theme) ──
uint16_t panelBG()    { return rgb( 36,  36,  36); } // #242424 deck
uint16_t panelInk()   { return rgb(255, 255, 255); } // white ink
uint16_t panelGhost() { return rgb( 88,  88,  88); } // grid dots / stale digits
uint16_t panelHI()    { return rgb(170, 170, 170); } // light grey rule
uint16_t panelEdge()  { return rgb(170, 170, 170); } // light grey rules + borders
uint16_t accent()     { return rgb(236,   0, 140); } // #EC008C magenta
uint16_t accentDK()   { return rgb(104,   0,  62); } // dimmed magenta
uint16_t accentGN()   { return rgb(  0, 230, 118); } // #00E676 signal green (up)

// ── VHS filter: slow chroma ripple + per-element glitch hits. Each UI band
//     tears on its own 6s slots and its rip rows are washed with cycling
//     primaries (yellow/cyan/green/magenta/red/blue); skipping the off-band
//     rows keeps the per-frame cost tiny. Only a single row is buffered. ──
static uint16_t vhsRow[WIDTH];
static uint32_t vhsT = 0;

static inline uint32_t vhsNoise(uint32_t x) {
  x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
  return x;
}

// saturated monitor-test primaries for the rip rows
static uint16_t vhsPal(uint8_t i) {
  switch (i % 6) {
    case 0: return rgb(255, 255,   0); // yellow
    case 1: return rgb(  0, 255, 255); // cyan
    case 2: return rgb(  0, 255,   0); // green
    case 3: return rgb(255,   0, 255); // magenta
    case 4: return rgb(255,   0,   0); // red
    default:return rgb(  0,   0, 255); // blue
  }
}

static bool elemHit(int i, uint32_t t, int &dx, uint8_t &pal) {
  const uint32_t SLOT = 6000;
  uint32_t phased = t + (uint32_t)(i * 1013u);
  uint32_t s = vhsNoise(phased / SLOT * 2654435761u ^ ((uint32_t)(i + 3) * 97u));
  if ((s % 5u) != 0) return false;
  uint32_t start = (s >> 8) % (SLOT - 500u);
  uint32_t dur   = 200u + ((s >> 20) % 240u);
  uint32_t local = phased % SLOT;
  if (local < start || local >= start + dur) return false;
  dx  = (int)((s >> 12) % 11u) - 5;   // ±5px tears
  pal = (uint8_t)(s >> 24);
  return true;
}

void vhsApply(int h) {
  uint32_t t = vhsT;
  float ph = (float)t * 0.0018f;

  // per-element bands: header, accent strip, chart, caption+price, pair bar
  static const int bandY0[5] = {   8, 24,  40, 156, 220 };
  static const int bandY1[5] = {  18, 33, 151, 218, 238 };
  int bDx[5], bBoost[5]; float bChroma[5]; uint8_t bPal[5]; bool bHit[5];
  for (int i = 0; i < 5; i++) {
    bHit[i]    = elemHit(i, t, bDx[i], bPal[i]);
    bDx[i]     = bHit[i] ? bDx[i] : 0;
    bChroma[i] = bHit[i] ? 7.0f : 0.0f;
    bBoost[i]  = bHit[i] ? 4 : 0;
  }

  int flick = 246 + (int)(vhsNoise(t / 110u) % 11);

  for (int row = 0; row < h; row++) {
    int y = fbTop + row;
    int bi = -1;
    for (int i = 0; i < 5; i++) if (y >= bandY0[i] && y < bandY1[i]) { bi = i; break; }
    if (bi < 0) continue;
    bool hit = bHit[bi];
    float lsh = 1.4f + 1.3f * sinf(y * 0.085f + ph) + bChroma[bi];
    float rsh = 1.8f + 2.1f * sinf(y * 0.061f + 1.7f + ph * 1.3f) + bChroma[bi];
    int boost = bBoost[bi], xOff = bDx[bi];
    // rip colour: shifts every 60ms and every 8 rows inside the hit
    uint16_t pc = vhsPal((uint8_t)(bPal[bi] + t / 60u + (y >> 3)));
    int pr = (pc >> 11) & 0x1F, pg = (pc >> 5) & 0x3F, pb = pc & 0x1F;
    if (lsh < 0) lsh = 0; if (rsh < 0) rsh = 0;
    int li0 = (int)lsh, lfr = (int)((lsh - li0) * 256.0f);
    int ri0 = (int)rsh, rfr = (int)((rsh - ri0) * 256.0f);
    memcpy(vhsRow, &fb[row * WIDTH], WIDTH * sizeof(uint16_t));
    const uint16_t* s = vhsRow;
    uint16_t* d = &fb[row * WIDTH];
    for (int x = 0; x < WIDTH; x++) {
      int cx = x + xOff;
      if (cx < 0) cx = 0; if (cx > WIDTH - 1) cx = WIDTH - 1;
      uint16_t cC = s[cx];
      int lp = cx - li0, lq = lp - 1; if (lp < 0) lp = 0; if (lq < 0) lq = 0;
      int rp = cx + ri0, rq = rp + 1; if (rp > WIDTH - 1) rp = WIDTH - 1; if (rq > WIDTH - 1) rq = WIDTH - 1;
      uint16_t la = s[lp], lb = s[lq], ra = s[rp], rb = s[rq];
      int rSrc = (int)(((((la >> 11) & 0x1F) * (256 - lfr)) + (((lb >> 11) & 0x1F) * lfr)) >> 8);
      int bSrc = (int)((((ra & 0x1F) * (256 - rfr)) + ((rb & 0x1F) * rfr)) >> 8);
      int r = (3 * rSrc + ((cC >> 11) & 0x1F)) >> 2;   // 75% shift = stronger fringe
      int g = (cC >> 5) & 0x3F;
      int b = (3 * bSrc + (cC & 0x1F)) >> 2;
      r = (r * flick) >> 8; g = (g * flick) >> 8; b = (b * flick) >> 8;
      r += boost; g += boost; b += boost;
      if (hit) {                                       // wash the band with the palette
        int w = (g > 22) ? 3 : 2;                      // 75% on ink, 50% on the deck
        r = (r * (4 - w) + pr * w) >> 2;
        g = (g * (4 - w) + pg * w) >> 2;
        b = (b * (4 - w) + pb * w) >> 2;
      }
      if (r < 0) r = 0; if (r > 31) r = 31;
      if (g < 0) g = 0; if (g > 63) g = 63;
      if (b < 0) b = 0; if (b > 31) b = 31;
      d[x] = (uint16_t)((r << 11) | (g << 5) | b);
    }
  }
}

// ── Departure Mono (rasterized, 11px). y is the cap/digit top. ──
#include "DepartureMono.h"

static inline const uint8_t* dmGlyph(char ch) {
  if (ch < 32 || ch > 90) return dmFont[0];
  return dmFont[ch - 32];
}
void fbChar(int x, int y, char ch, uint16_t c, int scale) {
  if (ch == ' ') return;
  if (ch >= 'a' && ch <= 'z') ch -= 32; // normalize — labels are UPPER
  const uint8_t* g = dmGlyph(ch);
  int cy = y - DM_TOP * scale;
  for (int row = 0; row < DM_H; row++) {
    uint8_t bits = g[row];
    if (!bits) continue;
    for (int col = 0; col < DM_W; col++)
      if (bits & (0x40 >> col)) fbFillRect(x + col * scale, cy + row * scale, scale, scale, c);
  }
}
void fbText(int x, int y, const char* s, uint16_t c, int scale = 1) {
  while (*s) { fbChar(x, y, *s, c, scale); x += DM_ADV * scale; s++; }
}
int textW57(const char* s, int scale = 1) { int n = 0; while (*s++) n++; return n ? (n * DM_ADV * scale - scale) : 0; }

// 5x5 trend arrow sprite, scalable
void drawArrow(int x, int y, int sc, bool up, uint16_t c) {
  static const uint8_t A[5] = {0b00100,0b01110,0b11111,0b00100,0b00100};
  for (int r = 0; r < 5; r++) {
    uint8_t bits = up ? A[r] : A[4 - r];
    for (int col = 0; col < 5; col++)
      if (bits & (0b10000 >> col)) fbFillRect(x + col * sc, y + r * sc, sc, sc, c);
  }
}

// ── Layout (320x240, 2x the C3 layout) ──
#define FS2 1
#define HDR_Y       8
#define ACC_Y       24   // dark line; red bar 25..32
#define CHART_X0    12
#define CHART_X1    308
#define CHART_TOP   40
#define CHART_BASE  150
#define LBL_Y       158  // caption row ("LAST" / "HOLD TF")
#define PRICE_Y     174
#define PRICE_WC    22
#define PRICE_HC    44
#define PRICE_T     6
#define PRICE_GAP   4
#define PRICE_DOT   12
#define BAR_Y       220  // selector divider
#define MENU_Y      222  // cells 222..237
#define MENU_LBL_Y  224

// ── Market data ──
#define NUM_CANDLES 32
struct Candle { float o, h, l, c; };

struct Pair { const char* symbol; const char* label; const char* cell; float demoBase; };
static const Pair PAIRS[] = {
  {"BTCUSDT", "BTC/USDT", "BTC", 87500.0f},
  {"ETHUSDT", "ETH/USDT", "ETH", 3200.0f},
  {"SOLUSDT", "SOL/USDT", "SOL", 150.0f},
  {"USDTBRL", "USD/BRL", "BRL", 5.47f},
};
#define PAIR_COUNT 4

static const char* TF_INTERVALS[] = {"5m", "1h", "1d", "1w"};
static const char* TF_LABELS[]    = {"5M", "1H", "1D", "1W"};
#define TF_COUNT 4

Candle candles[NUM_CANDLES];
float currentPrice = 0.0f;
float dailyPercentChange = 0.0f;
bool dataIsLive = false;
bool gettingData = true;    // LOADING placeholder until the first fetch resolves
int currentPair = 0;
int currentTimeFrame = 2; // 1D

unsigned long tickFlashUntil = 0;
unsigned long pairFlashUntil = 0; // selector cell blink after a coin change
unsigned long tfFlashUntil = 0;   // red strip blink after a timeframe change
char msgText[16] = "";
bool chartStale = false;      // switch in progress: show LOADING, not the old curve
unsigned long staleSince = 0;

static inline bool blinkOn(unsigned long t) { return (t / 380) % 2 == 0; }

// ── Network state ──
enum NetMode : uint8_t { NET_BOOT, NET_CONNECTING, NET_UP, NET_DEMO, NET_PORTAL };
NetMode netMode = NET_BOOT;
bool bootDone = false;
bool fetching = false;
bool fetchQueued = false;
int fetchPhase = 0;
int fetchPair = -1;
int fetchTF = -1;
unsigned long lastDataMs = 0;
unsigned long lastAttemptMs = 0;
unsigned long lastReconnectMs = 0;
#define REFRESH_MS   10000UL
#define RETRY_MS     20000UL
#define RECONNECT_MS 15000UL

const char* AP_SSID = "DM-01-Setup";
WebServer server(80);
DNSServer dnsServer;
#define DNS_PORT 53
Preferences preferences;
String wifiSSID = "";
String wifiPassword = "";

// ── Touch ──
bool wasPressed = false;
bool touchDown = false;
bool introMode = false;
const char* bootLine = "CONNECTING";
unsigned long pressStartMs = 0;
bool holdFired = false;
#define HOLD_MS 1000

// ── Helpers ──
void formatPrice(float p, char* out, size_t n) {
  if (p >= 10000.0f)   snprintf(out, n, "%.0f", p);
  else if (p >= 1.0f)  snprintf(out, n, "%.2f", p);
  else                 snprintf(out, n, "%.4f", p);
}

// ── Demo data (deterministic, used until the first live fetch succeeds) ──
void loadDemoData() {
  float base = PAIRS[currentPair].demoBase;
  uint32_t seed = 0x9E3779B9u * (uint32_t)(currentPair + 1) + 0x85EBCA6Bu * (uint32_t)(currentTimeFrame + 1);
  float px = base * (1.0f + 0.004f * sinf((float)seed * 1e-7f));
  float first = 0, last = 0;
  for (int i = 0; i < NUM_CANDLES; i++) {
    uint32_t h = seed + i * 2654435761u;
    h ^= h >> 16; h *= 0x7feb352dU; h ^= h >> 15; h *= 0x846ca68bU; h ^= h >> 16;
    float drift = base * (((int)(h % 2001) - 1000) / 1000000.0f) + base * 0.00035f * sinf(i * 0.6f);
    float open = px;
    float close = open + drift;
    float hi = (open > close ? open : close) + base * ((h >> 8) % 500) / 2000000.0f;
    float lo = (open < close ? open : close) - base * ((h >> 16) % 500) / 2000000.0f;
    candles[i] = {open, hi, lo, close};
    px = close;
    if (i == 0) first = open;
    last = close;
  }
  currentPrice = last;
  dailyPercentChange = first > 0 ? (last - first) / first * 100.0f : 0.0f;
  dataIsLive = false;
}

// ── HTTP ──
bool httpGet(const String& url, String& out, int* code) {
  HTTPClient http;
  http.setTimeout(10000);
  http.setConnectTimeout(6000);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  if (!http.begin(url)) { if (code) *code = 0; return false; }
  int c = http.GET();
  if (code) *code = c;
  bool ok = false;
  if (c == 200) { out = http.getString(); ok = true; }
  else if (c > 0) { http.getString(); }
  http.end();
  return ok;
}

bool parseKlines(const String& json) {
  Candle parsed[NUM_CANDLES];
  int n = 0;
  int pos = 0;
  while (n < NUM_CANDLES) {
    int lb = json.indexOf('[', pos);
    if (lb < 0) break;
    int p = json.indexOf(',', lb);
    if (p < 0) break;
    float v[4];
    bool ok = true;
    for (int k = 0; k < 4; k++) {
      int q1 = json.indexOf('"', p);
      int q2 = q1 < 0 ? -1 : json.indexOf('"', q1 + 1);
      if (q1 < 0 || q2 < 0) { ok = false; break; }
      v[k] = strtof(json.substring(q1 + 1, q2).c_str(), nullptr);
      p = q2 + 1;
      if (k < 3) {
        int c = json.indexOf(',', p);
        if (c < 0) { ok = false; break; }
        p = c + 1;
      }
    }
    if (!ok) break;
    parsed[n++] = {v[0], v[1], v[2], v[3]};
    pos = p;
  }
  if (n < 2) return false;
  for (int i = 0; i < n; i++) candles[i] = parsed[i];
  currentPrice = candles[n - 1].c;
  dataIsLive = true;
  gettingData = false;
  chartStale = false;
  lastDataMs = millis();
  tickFlashUntil = millis() + 700;
  Serial.printf("[btc] klines ok n=%d last=%.2f\n", n, currentPrice);
  return true;
}

bool parseTicker(const String& json) {
  const char* key = "\"priceChangePercent\":\"";
  int k = json.indexOf(key);
  if (k < 0) return false;
  k += strlen(key);
  int end = json.indexOf('"', k);
  if (end < 0) return false;
  dailyPercentChange = strtof(json.substring(k, end).c_str(), nullptr);
  Serial.printf("[btc] 24h change=%.2f%%\n", dailyPercentChange);
  return true;
}

void startFetch() {
  if (fetching) { fetchQueued = true; return; }
  fetching = true;
  fetchPhase = 0;
  fetchPair = currentPair;
  fetchTF = currentTimeFrame;
  lastAttemptMs = millis();
  Serial.printf("[btc] fetch %s %s\n", PAIRS[currentPair].symbol, TF_INTERVALS[currentTimeFrame]);
}

// One HTTP request per call so the UI keeps rendering between phases.
bool continueFetch() {
  String payload;
  int code = 0;

  if (fetchPhase == 0) {
    String url = String("https://api.binance.com/api/v3/klines?symbol=") +
                 PAIRS[fetchPair].symbol + "&interval=" + TF_INTERVALS[fetchTF] + "&limit=" + String(NUM_CANDLES);
    bool got = httpGet(url, payload, &code);
    if (got && (fetchPair != currentPair || fetchTF != currentTimeFrame)) {
      fetching = false;      // response raced a switch: drop it, fetch the new one
      startFetch();
      return true;
    }
    if (got && parseKlines(payload)) {
      fetchPhase = 1;
      return false;
    }
    Serial.printf("[btc] klines failed http=%d\n", code);
    fetching = false;
    if (chartStale || gettingData) { loadDemoData(); chartStale = false; gettingData = false; }
    return true;
  }

  if (fetchPhase == 1) {
    String url = String("https://api.binance.com/api/v3/ticker/24hr?symbol=") + PAIRS[fetchPair].symbol;
    if (httpGet(url, payload, &code) && parseTicker(payload)) {
      lastDataMs = millis();
      Serial.println("[btc] data live");
      fetchPhase = 2;
      fetching = false;
      if (fetchQueued || fetchPair != currentPair || fetchTF != currentTimeFrame) {
        fetchQueued = false;
        startFetch();
      }
      return true;
    }
    Serial.printf("[btc] ticker failed http=%d\n", code);
    fetching = false;
    return true;
  }

  fetching = false;
  return true;
}

// ── WiFi (esp32-c3-ws2812 skill §10) ──
bool wifiConnect(uint32_t timeoutMs, bool hardReset) {
  static bool eventsRegistered = false;
  if (!eventsRegistered) {
    eventsRegistered = true;
    WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
      if (event == ARDUINO_EVENT_WIFI_STA_CONNECTED) {
        Serial.println("WiFi associated");
      } else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
        Serial.printf("WiFi disconnected reason=%d\n", (int)info.wifi_sta_disconnected.reason);
      } else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
        Serial.println("WiFi got IP");
      }
    });
  }

  if (hardReset) {
    WiFi.persistent(false);
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);
    delay(200);
  }

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  WiFi.setAutoReconnect(false);

  if (hardReset) {
    int16_t best = -1;
    int32_t bestRssi = -127;
    int16_t n = WiFi.scanNetworks(false, false, false, 120);
    for (int16_t i = 0; i < n; i++) {
      if (WiFi.SSID(i) == wifiSSID && (int32_t)WiFi.RSSI(i) > bestRssi) { best = i; bestRssi = WiFi.RSSI(i); }
    }
    if (best >= 0) Serial.printf("WiFi visible ch=%d rssi=%d\n", (int)WiFi.channel(best), (int)bestRssi);
    else Serial.println("WiFi SSID not visible");
    WiFi.scanDelete();
  }

  Serial.printf("WiFi begin -> %d\n", (int)WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str()));

  unsigned long t0 = millis();
  wl_status_t last = WL_IDLE_STATUS;
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) {
    wl_status_t st = WiFi.status();
    if (st != last) {
      Serial.printf("WiFi status %d -> %d at %lums\n", (int)last, (int)st, millis() - t0);
      last = st;
    }
    delay(250);
  }
  Serial.printf("WiFi result=%d after %lums\n", (int)WiFi.status(), millis() - t0);
  return WiFi.status() == WL_CONNECTED;
}

void loadCredentials() {
  preferences.begin("dm01", true);
  wifiSSID = preferences.getString("ssid", "");
  wifiPassword = preferences.getString("password", "");
  preferences.end();

  if (wifiSSID.length() == 0) { // migrate the pre-DM-01 namespace
    preferences.begin("mydock", true);
    String s = preferences.getString("ssid", "");
    String p = preferences.getString("password", "");
    preferences.end();
    if (s.length() > 0) {
      preferences.begin("dm01", false);
      preferences.putString("ssid", s);
      preferences.putString("password", p);
      preferences.end();
      wifiSSID = s;
      wifiPassword = p;
      Serial.println("migrated WiFi credentials from mydock");
    }
  }
}

// ── Captive portal (non-blocking: the ticker keeps rendering behind it) ──
void handleRoot() {
  String html = R"HTML(<!DOCTYPE html><html><head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>DM-01 Setup</title>
<style>
  body{background:#07050f;color:#f4eeff;font-family:ui-monospace,SFMono-Regular,Menlo,monospace;margin:0;padding:24px}
  main{max-width:360px;margin:0 auto}
  h1{color:#ff8c1e;letter-spacing:.15em;margin:0 0 4px}
  p{color:#746a9b;margin:0 0 24px;font-size:14px}
  label{display:block;font-size:12px;color:#b3a8cf;margin:14px 0 6px;text-transform:uppercase;letter-spacing:.1em}
  input{width:100%;box-sizing:border-box;padding:12px;background:#150e28;border:1px solid #3a2a5e;color:#f4eeff;border-radius:4px}
  button{width:100%;margin-top:22px;padding:13px;border:0;border-radius:4px;font-weight:bold;letter-spacing:.1em;color:#07050f;background:linear-gradient(90deg,#ff8c1e,#dc3cc8);cursor:pointer}
</style></head><body><main>
<h1>DM-01</h1>
<p>WiFi setup &mdash; saved to NVS, device reboots.</p>
<form action="/save" method="POST">
  <label for="ssid">WiFi name</label>
  <input type="text" id="ssid" name="ssid" placeholder="SSID" required>
  <label for="password">Password</label>
  <input type="password" id="password" name="password" placeholder="password">
  <button type="submit">Connect</button>
</form>
</main></body></html>)HTML";
  server.send(200, "text/html", html);
}

void handleSave() {
  String ssid = server.arg("ssid");
  String password = server.arg("password");
  if (ssid.length() == 0) {
    server.send(200, "text/html", "SSID required. <a href='/'>Back</a>");
    return;
  }
  preferences.begin("dm01", false);
  preferences.putString("ssid", ssid);
  preferences.putString("password", password);
  preferences.end();
  server.send(200, "text/html",
    "<html><head><meta name='viewport' content='width=device-width,initial-scale=1'></head>"
    "<body style='background:#07050f;color:#f4eeff;font-family:monospace;text-align:center;padding:60px'>"
    "<h2 style='color:#ff8c1e'>SAVED</h2><p>Rebooting...</p></body></html>");
  delay(900);
  ESP.restart();
}

void startPortal() {
  netMode = NET_PORTAL;
  WiFi.persistent(false);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID);
  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.onNotFound(handleRoot);
  server.begin();
  Serial.print("portal AP=");
  Serial.print(AP_SSID);
  Serial.print(" ip=");
  Serial.println(WiFi.softAPIP());
}

void onWifiUp() {
  netMode = NET_UP;
  lastReconnectMs = millis();
  Serial.printf("WiFi OK IP=%s RSSI=%d\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
  startFetch();
}

// ── Copy / transient messages ──
void say(const char* s) {
  snprintf(msgText, sizeof(msgText), "%s", s);
}

void switchPair() {
  currentPair = (currentPair + 1) % PAIR_COUNT;
  char buf[18];
  snprintf(buf, sizeof(buf), "COIN %s", PAIRS[currentPair].label);
  say(buf);
  pairFlashUntil = millis() + 900;
  lastDataMs = 0;
  if (netMode == NET_UP) { chartStale = true; staleSince = millis(); startFetch(); }
  else loadDemoData();
  Serial.printf("[btc] pair -> %s\n", PAIRS[currentPair].symbol);
}

void switchTimeFrame() {
  currentTimeFrame = (currentTimeFrame + 1) % TF_COUNT;
  char buf[18];
  snprintf(buf, sizeof(buf), "TIME %s", TF_LABELS[currentTimeFrame]);
  say(buf);
  tfFlashUntil = millis() + 900;
  lastDataMs = 0;
  if (netMode == NET_UP) { chartStale = true; staleSince = millis(); startFetch(); }
  else loadDemoData();
  Serial.printf("[btc] timeframe -> %s\n", TF_LABELS[currentTimeFrame]);
}

// ── Touch (XPT2046): the screen replaces the C3 pad ──
void touchInit() {
  touchSPI.begin(TOUCH_CLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
  pinMode(TOUCH_CS, OUTPUT);
  digitalWrite(TOUCH_CS, HIGH);
  pinMode(TOUCH_IRQ, INPUT);  // GPIO36 input-only: external pull-up on the CYD
  pinMode(BOOT_PIN, INPUT_PULLUP);
}
bool touchPressed() {
  return digitalRead(TOUCH_IRQ) == LOW;
}

// BOOT button (GPIO 0) or the touch panel
static inline bool inputPressed() {
  if (digitalRead(BOOT_PIN) == LOW) return true;
  return touchPressed();
}
void touchRead(int &tx, int &ty) {
  touchSPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  digitalWrite(TOUCH_CS, LOW);
  touchSPI.transfer(0xD0);
  uint16_t xRaw = ((touchSPI.transfer(0) << 8) | touchSPI.transfer(0)) >> 3;
  touchSPI.transfer(0x90);
  uint16_t yRaw = ((touchSPI.transfer(0) << 8) | touchSPI.transfer(0)) >> 3;
  digitalWrite(TOUCH_CS, HIGH);
  touchSPI.endTransaction();
  // CYD landscape swaps the axes: raw Y -> screen X, raw X -> screen Y
  tx = map(yRaw, 200, 3900, 0, 320);
  ty = map(xRaw, 3900, 200, 0, 240);
  if (tx < 0) tx = 0; if (tx > 319) tx = 319;
  if (ty < 0) ty = 0; if (ty > 239) ty = 239;
}

void handleTouch() {
  bool pressed = inputPressed();
  unsigned long now = millis();
  touchDown = pressed;

  if (pressed && !wasPressed) {
    pressStartMs = now;
    wasPressed = true;
    holdFired = false;
    int tx, ty; touchRead(tx, ty);
    Serial.printf("[touch] press @ %d,%d\n", tx, ty);
  } else if (!pressed && wasPressed) {
    unsigned long dur = now - pressStartMs;
    if (!holdFired && dur >= 60 && dur < HOLD_MS) switchPair();
    wasPressed = false;
    holdFired = false;
    delay(20);
  } else if (pressed && wasPressed && !holdFired && (now - pressStartMs >= HOLD_MS)) {
    holdFired = true;
    switchTimeFrame();
  }
}

// ── DM-01 boot intro (2x for 320x240) ──
#define DM01_SCALE 2
#include "Dm01Intro.h"

// ── Rendering: VHS deck, scaled 2x ──
void drawHeader() {
  fbText(12, HDR_Y, PAIRS[currentPair].label, panelInk(), FS2);
  fbText((WIDTH - textW57(TF_LABELS[currentTimeFrame], FS2)) / 2, HDR_Y, TF_LABELS[currentTimeFrame], panelInk(), FS2);

  int x = WIDTH - 12;
  char chg[12];
  snprintf(chg, sizeof(chg), "%+.2f%%", dailyPercentChange);
  uint16_t chgColor = dailyPercentChange >= 0 ? accentGN() : accent();
  int w = textW57(chg, FS2);
  fbText(x - w, HDR_Y, chg, chgColor, FS2);
  x -= w + 6;
  drawArrow(x - 10, HDR_Y + 3, 2, dailyPercentChange >= 0, chgColor);
  x -= 20;

  const char* st = nullptr;
  uint16_t stc = panelGhost();
  if (fetching) { st = "SYNC"; }
  else if (!dataIsLive) { st = "DEMO"; stc = blinkOn(millis()) ? accent() : panelInk(); }
  if (st) fbText(x - textW57(st, FS2), HDR_Y, st, stc, FS2);
}

void drawSparkline() {
  const int x0 = CHART_X0, x1 = CHART_X1;
  const int w = x1 - x0;

  float mn = candles[0].c, mx = candles[0].c;
  for (int i = 0; i < NUM_CANDLES; i++) {
    if (candles[i].c < mn) mn = candles[i].c;
    if (candles[i].c > mx) mx = candles[i].c;
  }
  float pad = (mx - mn) * 0.10f + 0.000001f;
  mn -= pad; mx += pad;

  int ys[WIDTH];
  for (int x = x0; x <= x1; x++) {
    float t = (float)(x - x0) / (float)w * (NUM_CANDLES - 1);
    int i = (int)t;
    float f = t - i;
    if (i >= NUM_CANDLES - 1) { i = NUM_CANDLES - 2; f = 1.0f; }
    float c = candles[i].c + (candles[i + 1].c - candles[i].c) * f;
    int y = CHART_BASE - (int)((c - mn) / (mx - mn) * (CHART_BASE - CHART_TOP));
    if (y < CHART_TOP) y = CHART_TOP;
    if (y > CHART_BASE) y = CHART_BASE;
    ys[x] = y;
  }

  // 4px dot-grid backdrop (neko_cyd uses step 4 on the bigger panel)
  for (int y = CHART_TOP; y <= CHART_BASE; y += 4)
    for (int x = x0 + ((y / 4) & 1) * 2; x <= x1; x += 4) FPIX(x, y, panelGhost());

  // halftone fill under the curve
  for (int x = x0; x <= x1; x++)
    for (int y = ys[x] + 1; y <= CHART_BASE; y++)
      if (((x + y) & 1) == 0) FPIX(x, y, panelGhost());

  // dashed accent level at the current price
  int ly = ys[x1];
  for (int x = x0; x <= x1; x += 12) fbHLine(x, ly, 4, accent());

  // ink curve + last-point marker
  for (int x = x0; x < x1; x++) fbDrawLine(x, ys[x], x + 1, ys[x + 1], panelInk());
  fbFillRect(x1 - 2, ly - 2, 5, 5, accent());

  // ground line
  fbHLine(x0, CHART_BASE + 1, w + 1, panelEdge());
}

void drawLoadingChart() {
  // same dot-grid backdrop as the sparkline, plus action + LOADING
  for (int y = CHART_TOP; y <= CHART_BASE; y += 4)
    for (int x = CHART_X0 + ((y / 4) & 1) * 2; x <= CHART_X1; x += 4) FPIX(x, y, panelGhost());
  if (msgText[0]) fbText((WIDTH - textW57(msgText, FS2)) / 2, CHART_TOP + 22, msgText, panelEdge(), FS2);
  const char* ld = "LOADING";
  int w = textW57(ld, 2);
  int dots = (millis() / 300) % 4;
  int x = (WIDTH - (w + dots * 14)) / 2;
  fbText(x, CHART_TOP + 56, ld, panelInk(), 2);
  for (int i = 0; i < dots; i++) fbFillRect(x + w + 6 + i * 14, CHART_TOP + 64, 6, 6, accent());
}

void drawPrice() {
  char price[16];
  formatPrice(currentPrice, price, sizeof(price));

  uint16_t lit = chartStale ? panelGhost()
                 : ((millis() < tickFlashUntil) ? accent() : panelInk());
  int w = textW57(price, 5);
  fbText((WIDTH - w) / 2, PRICE_Y, price, lit, 5);
}

void drawPairBar() {
  fbHLine(0, BAR_Y, WIDTH, panelEdge());
  const int cellW = WIDTH / PAIR_COUNT;
  bool cellFlash = (millis() < pairFlashUntil) && ((millis() / 120) % 2 == 0);
  for (int i = 0; i < PAIR_COUNT; i++) {
    int x0 = i * cellW;
    int cx = x0 + cellW / 2;
    const char* lab = PAIRS[i].cell;
    int w = textW57(lab, FS2);
    int x = cx - w / 2;
    if (i == currentPair) {
      fbFillRect(x0 + 4, MENU_Y, cellW - 8, 16, cellFlash ? accent() : panelInk());
      fbText(x, MENU_LBL_Y, lab, panelBG(), FS2);
      fbFillRect(x0 + 12, MENU_LBL_Y + 11, cellW - 24, 2, accent());
    } else {
      fbRect(x0 + 4, MENU_Y, cellW - 8, 16, panelEdge());
      fbText(x, MENU_LBL_Y, lab, panelEdge(), FS2);
    }
  }
}

void drawPortal() {
  fbText(12, HDR_Y, "DM-01", panelInk(), FS2);
  fbText(WIDTH - 12 - textW57("SETUP", FS2), HDR_Y, "SETUP", accent(), FS2);
  fbHLine(8, ACC_Y, WIDTH - 16, accentDK());
  fbHLine(8, ACC_Y + 1, WIDTH - 16, accentDK());
  fbFillRect(8, ACC_Y + 2, WIDTH - 16, 8, accent());

  fbText((WIDTH - textW57("WIFI SETUP")) / 2, 56, "WIFI SETUP", panelInk());
  fbText(24, 96, "1 JOIN WIFI", panelInk());
  fbText(24, 116, AP_SSID, panelInk());
  fbText(24, 148, "2 OPEN BROWSER", panelInk());
  fbText(24, 168, "192.168.4.1", accent());
  fbText((WIDTH - textW57("SAVES + REBOOTS")) / 2, 208, "SAVES + REBOOTS", accentDK());
}

void drawBootScreen() {
  fbText(12, HDR_Y, "DM-01", panelInk(), FS2);
  fbText(WIDTH - 12 - textW57("SYNC", FS2), HDR_Y, "SYNC", panelEdge(), FS2);
  fbHLine(8, ACC_Y, WIDTH - 16, accentDK());
  fbHLine(8, ACC_Y + 1, WIDTH - 16, accentDK());
  fbFillRect(8, ACC_Y + 2, WIDTH - 16, 8, accent());
  for (int y = 44; y <= 200; y += 4)
    for (int x = ((y / 4) & 1) * 2; x < WIDTH; x += 4) FPIX(x, y, panelGhost());
  fbText((WIDTH - textW57(bootLine, 2)) / 2, 84, bootLine, panelInk(), 2);
  fbText((WIDTH - textW57(wifiSSID.c_str(), FS2)) / 2, 128, wifiSSID.c_str(), panelInk(), FS2);
  fbText(12, HEIGHT - 24, "TAP PAIR", panelInk(), FS2);
  fbText(WIDTH - 12 - textW57("HOLD TF", FS2), HEIGHT - 24, "HOLD TF", accentDK(), FS2);
}

void drawFetchScreen() {
  fbText(12, HDR_Y, "DM-01", panelInk(), FS2);
  fbText(WIDTH - 12 - textW57("SYNC", FS2), HDR_Y, "SYNC", accent(), FS2);
  fbHLine(8, ACC_Y, WIDTH - 16, accentDK());
  fbHLine(8, ACC_Y + 1, WIDTH - 16, accentDK());
  fbFillRect(8, ACC_Y + 2, WIDTH - 16, 8, accent());
  for (int y = 44; y <= 200; y += 4)
    for (int x = ((y / 4) & 1) * 2; x < WIDTH; x += 4) FPIX(x, y, panelGhost());
  const char* ld = "LOADING";
  int w = textW57(ld, 2);
  int dots = (millis() / 300) % 4;
  int x = (WIDTH - (w + dots * 14)) / 2;
  fbText(x, 80, ld, panelInk(), 2);
  for (int i = 0; i < dots; i++) fbFillRect(x + w + 6 + i * 14, 88, 6, 6, accent());
  fbText((WIDTH - textW57(PAIRS[currentPair].label, FS2)) / 2, 128, PAIRS[currentPair].label, panelInk(), FS2);
  fbText(12, HEIGHT - 24, "TAP PAIR", panelInk(), FS2);
  fbText(WIDTH - 12 - textW57("HOLD TF", FS2), HEIGHT - 24, "HOLD TF", accentDK(), FS2);
}

void drawFrame() {
  fbClear(panelBG()); // always the light LCD — no state switching
  if (netMode == NET_PORTAL) { drawPortal(); return; }
  if (netMode == NET_CONNECTING) { drawBootScreen(); return; }
  if (gettingData) { drawFetchScreen(); return; }

  drawHeader();

  // red strip blinks after a timeframe change, steady otherwise
  bool stripOn = (millis() >= tfFlashUntil) || ((millis() / 120) % 2 == 0);
  uint16_t stripLine = stripOn ? accentDK() : panelGhost();
  uint16_t stripBar = stripOn ? accent() : panelGhost();
  fbHLine(8, ACC_Y, WIDTH - 16, stripLine);
  fbHLine(8, ACC_Y + 1, WIDTH - 16, stripLine);
  fbFillRect(8, ACC_Y + 2, WIDTH - 16, 8, stripBar);

  if (chartStale) drawLoadingChart(); else drawSparkline();
  fbText(12, LBL_Y, "LAST", panelInk(), FS2);
  fbText(WIDTH - 12 - textW57("HOLD TF", FS2), LBL_Y, "HOLD TF", accentDK(), FS2);
  drawPrice();
  drawPairBar();
}

void render() {
  vhsT = millis();
  for (fbTop = 0; fbTop < HEIGHT; fbTop += FB_H) {
    if (introMode) dm01Draw(dm01Elapsed()); else drawFrame();
    int h = FB_H;
    if (fbTop + h > HEIGHT) h = HEIGHT - fbTop;
    if (!introMode) vhsApply(h);
    if (!screenshotStripSend(shot, fb, fbTop, h)) fbFlush();
  }
}

// ── Fetch/watchdog scheduling ──
void netWatchdog() {
  if (netMode == NET_UP) {
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi lost -> demo");
      netMode = NET_DEMO;
      dataIsLive = false;
      lastReconnectMs = millis();
    }
  } else if (netMode == NET_DEMO) {
    if (millis() - lastReconnectMs > RECONNECT_MS) {
      netMode = NET_CONNECTING;
      bootLine = "RETRY";
      render();
      if (wifiConnect(8000, true)) onWifiUp();
      else { netMode = NET_DEMO; lastReconnectMs = millis(); }
    }
  }
}

void updateFetch() {
  if (netMode == NET_PORTAL) return;
  if (chartStale && millis() - staleSince > 4000) { loadDemoData(); chartStale = false; }
  if (fetching) return; // the request runs after the frame is flushed
  if (netMode != NET_UP && netMode != NET_DEMO) return;
  unsigned long base = lastAttemptMs > lastDataMs ? lastAttemptMs : lastDataMs;
  unsigned long wait = lastDataMs ? REFRESH_MS : RETRY_MS;
  if (millis() - base >= wait) startFetch();
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== DM-01 BTC TICKER CYD ===");

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  SPI.begin(TFT_SCLK, TFT_MISO, TFT_MOSI, TFT_CS);
  tft.init(240, 320);
  tft.setSPISpeed(40000000);
  tft.setRotation(1);
  // The library ST7789 init sends INVON; the CYD panel needs it off.
  tft.invertDisplay(false);
  tft.fillScreen(0x0000);

  touchInit();
  loadCredentials();
  dm01Start();
}

void bootNetwork() {
  if (wifiSSID.length() == 0) { startPortal(); return; }
  netMode = NET_CONNECTING;
  bootLine = "CONNECTING";
  render();
  if (wifiConnect(15000, false)) onWifiUp();
  else startPortal();
}

void loop() {
  screenshotStripPoll(shot, WIDTH, HEIGHT);

  if (dm01Tick(inputPressed())) {
    introMode = true;
    render();
    delay(16);
    return;
  }
  if (introMode) introMode = false;

  if (!bootDone) { bootDone = true; bootNetwork(); }

  if (netMode == NET_PORTAL) {
    dnsServer.processNextRequest();
    server.handleClient();
    render();
    delay(10);
    return;
  }

  handleTouch();
  netWatchdog();
  updateFetch();
  render();
  if (fetching) continueFetch(); // blocks, but the LOADING frame is already on screen
  delay(30);
}
