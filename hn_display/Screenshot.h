#pragma once
#include <Arduino.h>

// Build with "USB CDC On Boot: Enabled" (FQBN option CDCOnBoot=cdc) when the
// board talks over its native USB (ESP32-C3/S3). UART bridges (CYD/CH340) work
// as-is — ignore the warning below for those builds.
#if !ARDUINO_USB_CDC_ON_BOOT && (defined(ARDUINO_ARCH_ESP32) && !defined(CONFIG_IDF_TARGET_ESP32))
#warning "Screenshot hook needs USB CDC On Boot = Enabled (CDCOnBoot=cdc)"
#endif

// Serial screenshot hook (full framebuffer).
//
// Send the byte 'S' over serial and the sketch answers with:
//   "SHOT" | uint16 width (MSB first) | uint16 height (MSB first) | w*h RGB565 pixels
// Pixels are little-endian RGB565, matching the ESP32 framebuffer layout.
//
// Call this once per loop() with the global framebuffer:
//   screenshotHandle(fb, WIDTH, HEIGHT);
// The loop blocks while the frame is transmitted (~4 s for 160x128 @ 115200).
inline void screenshotHandle(const uint16_t* fb, uint16_t w, uint16_t h) {
  if (!Serial.available()) return;
  if (Serial.peek() != 'S') return;  // leave other serial commands for loop()
  Serial.read();
  // C3 sketches may run with a 0ms TX timeout so debug logs can never stall
  // the frame loop — a capture must block until the host drains it, though
#if defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT
  Serial.setTxTimeoutMs(1000);
#endif
  uint8_t hdr[8] = { 'S', 'H', 'O', 'T',
                     (uint8_t)(w >> 8), (uint8_t)w,
                     (uint8_t)(h >> 8), (uint8_t)h };
  Serial.write(hdr, sizeof(hdr));
  Serial.write((const uint8_t*)fb, (size_t)w * h * sizeof(uint16_t));
  Serial.flush();
#if defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT
  Serial.setTxTimeoutMs(0);
#endif
}

// ── Strip framebuffers (e.g. CYD 320x240 drawn in 64-row bands) ──
// The sketch only keeps one band in RAM, so stream the frame band by band:
//
//   static ScreenshotStripSession shot;
//   void loop() { screenshotStripPoll(shot, WIDTH, HEIGHT); render(); ... }
//   void render() {
//     for (fbTop = 0; fbTop < HEIGHT; fbTop += FB_H) {
//       drawFrame();                       // draws into fb for this band
//       int h = min(FB_H, HEIGHT - fbTop);
//       if (!screenshotStripSend(shot, fb, fbTop, h)) fbFlush();
//     }
//   }
struct ScreenshotStripSession {
  uint16_t w = 0, h = 0;
  bool pending = false;
  bool active = false;
};

inline void screenshotStripPoll(ScreenshotStripSession& s, uint16_t w, uint16_t h) {
  s.w = w;
  s.h = h;
  if (!s.active && !s.pending && Serial.available() && Serial.peek() == 'S') {
    Serial.read();
    s.pending = true;
  }
}

// Send one band if a capture is pending/active. Returns true while capturing
// (the caller should skip the SPI flush for this band).
inline bool screenshotStripSend(ScreenshotStripSession& s, const uint16_t* buf, int fbTop, int bandH) {
  if (s.pending && fbTop == 0) {
    uint8_t hdr[8] = { 'S', 'H', 'O', 'T',
                       (uint8_t)(s.w >> 8), (uint8_t)s.w,
                       (uint8_t)(s.h >> 8), (uint8_t)s.h };
    Serial.write(hdr, sizeof(hdr));
    s.pending = false;
    s.active = true;
  }
  if (!s.active) return false;
  Serial.write((const uint8_t*)buf, (size_t)s.w * bandH * sizeof(uint16_t));
  if (fbTop + bandH >= (int)s.h) {
    s.active = false;
    Serial.flush();
  }
  return true;
}
