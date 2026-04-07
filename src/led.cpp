#include "led.h"
#include "globals.h"
#include <time.h>

// ============================================================================
// LED hardware
// ============================================================================

void setupLEDs() {
  if (ledStrip) delete ledStrip;
  ledStrip = new Adafruit_NeoPixel(NUM_LEDS, LED_PIN, NEO_GRBW + NEO_KHZ800);
  ledStrip->begin();
  ledStrip->setBrightness(ledBrightness);
  ledStrip->clear();
  ledStrip->show();
  Serial.println("[LED] 60-LED RGBW clock initialized");
}

void showLEDs() {
  if (!ledStrip) return;
  for (int i = 0; i < NUM_LEDS; i++) {
    ledStrip->setPixelColor(i, ledStrip->Color(leds[i].r, leds[i].g, leds[i].b, 0));
  }
  ledStrip->show();
}

// ============================================================================
// Display helpers
// ============================================================================

uint8_t addSat(uint8_t base, uint8_t add) {
  uint16_t v = (uint16_t)base + add;
  return (v > 255) ? 255 : (uint8_t)v;
}

void addPixelWrap(int index, uint8_t r, uint8_t g, uint8_t b) {
  int pos = index % NUM_LEDS;
  if (pos < 0) pos += NUM_LEDS;
  leds[pos].r = addSat(leds[pos].r, r);
  leds[pos].g = addSat(leds[pos].g, g);
  leds[pos].b = addSat(leds[pos].b, b);
}

void hsvToRgb(uint16_t hue, uint8_t sat, uint8_t val,
              uint8_t& r, uint8_t& g, uint8_t& b) {
  hue %= 360;
  uint8_t  region = hue / 60;
  uint16_t rem    = (hue % 60) * 255 / 60;
  uint8_t  p = (uint16_t)val * (255 - sat) / 255;
  uint8_t  q = (uint16_t)val * (255 - ((uint16_t)sat * rem / 255)) / 255;
  uint8_t  t = (uint16_t)val * (255 - ((uint16_t)sat * (255 - rem) / 255)) / 255;
  switch (region) {
    case 0: r = val; g = t;   b = p;   break;
    case 1: r = q;   g = val; b = p;   break;
    case 2: r = p;   g = val; b = t;   break;
    case 3: r = p;   g = q;   b = val; break;
    case 4: r = t;   g = p;   b = val; break;
    default:r = val; g = p;   b = q;   break;
  }
}

void applyBrightnessAndShow() {
  for (int i = 0; i < NUM_LEDS; i++) {
    leds[i].r = (uint16_t)leds[i].r * ledBrightness / 255;
    leds[i].g = (uint16_t)leds[i].g * ledBrightness / 255;
    leds[i].b = (uint16_t)leds[i].b * ledBrightness / 255;
  }
  showLEDs();
}

bool extractClock(int& hour12, int& hour24,
                  int& minute, int& second, int& daySeconds) {
  time_t now = time(nullptr);
  if (now < 86400) {
    memset(leds, 0, sizeof(leds));
    leds[0]            = {255, 0, 0};
    leds[NUM_LEDS - 1] = {255, 0, 0};
    showLEDs();
    return false;
  }
  struct tm* t = localtime(&now);
  hour24     = t->tm_hour;
  hour12     = t->tm_hour % 12;
  minute     = t->tm_min;
  second     = t->tm_sec;
  daySeconds = hour24 * 3600 + minute * 60 + second;
  return true;
}

void overlayTimeMarkers(int hour12, int minute, int second, int secTrailLen) {
  int hourPos   = (hour12 * 5 + minute / 12) % NUM_LEDS;
  int minutePos = minute % NUM_LEDS;
  int secondPos = second % NUM_LEDS;

  for (int i = -2; i <= 2; i++) {
    uint8_t level = (i == 0) ? 255 : ((abs(i) == 1) ? 170 : 90);
    addPixelWrap(hourPos + i, level, 30, 30);
  }

  for (int i = -1; i <= 1; i++) {
    uint8_t level = (i == 0) ? 255 : 120;
    addPixelWrap(minutePos + i, 30, level, 30);
  }

  for (int i = 0; i < secTrailLen; i++) {
    uint8_t level = 255 - (i * 210 / secTrailLen);
    addPixelWrap(secondPos - i, 30, 60, level);
  }
}

// ============================================================================
// Display modes
// ============================================================================

void displayMode_Simple() {
  int hour12, hour24, minute, second, daySeconds;
  if (!extractClock(hour12, hour24, minute, second, daySeconds)) return;

  memset(leds, 0, sizeof(leds));

  int hourPos   = (hour12 * 5 + minute / 12) % NUM_LEDS;
  int minutePos = minute % NUM_LEDS;
  int secondPos = second % NUM_LEDS;

  // Red hour marker (3 LEDs)
  for (int i = -1; i <= 1; i++) {
    int idx = (hourPos + i + NUM_LEDS) % NUM_LEDS;
    leds[idx] = {255, 0, 0};
  }

  // Green minute marker (3 LEDs)
  for (int i = -1; i <= 1; i++) {
    int idx = (minutePos + i + NUM_LEDS) % NUM_LEDS;
    leds[idx] = {0, 255, 0};
  }

  // Blue second marker (3 LEDs)
  for (int i = -1; i <= 1; i++) {
    int idx = (secondPos + i + NUM_LEDS) % NUM_LEDS;
    leds[idx] = {0, 0, 255};
  }

  applyBrightnessAndShow();
}

void displayMode_Pulse() {
  int hour12, hour24, minute, second, daySeconds;
  if (!extractClock(hour12, hour24, minute, second, daySeconds)) return;

  memset(leds, 0, sizeof(leds));
  uint32_t ms   = millis();
  uint8_t  beat = (uint8_t)((ms % 1000) < 500
                    ? ((ms % 500) * 80 / 500)
                    : ((1000 - (ms % 1000)) * 80 / 500));

  // Very dim background pulse
  for (int i = 0; i < NUM_LEDS; i++) {
    uint8_t base = 5 + beat / 8;
    leds[i] = {base / 4, base / 3, base / 2};
  }

  // Clear HMS markers with more contrast
  overlayTimeMarkers(hour12, minute, second, 7);
  applyBrightnessAndShow();
}

void displayMode_Pastel() {
  int hour12, hour24, minute, second, daySeconds;
  if (!extractClock(hour12, hour24, minute, second, daySeconds)) return;

  memset(leds, 0, sizeof(leds));

  int hourPos   = (hour12 * 5 + minute / 12) % NUM_LEDS;
  int minutePos = minute % NUM_LEDS;
  int secondPos = second % NUM_LEDS;

  // Soft pastel hour (pink)
  for (int i = -1; i <= 1; i++) {
    uint8_t brightness = (i == 0) ? 180 : 100;
    addPixelWrap(hourPos + i, brightness, brightness / 3, brightness / 2);
  }

  // Soft pastel minute (mint green)
  for (int i = -1; i <= 1; i++) {
    uint8_t brightness = (i == 0) ? 160 : 90;
    addPixelWrap(minutePos + i, brightness / 4, brightness, brightness / 2);
  }

  // Soft pastel second (sky blue)
  for (int i = -1; i <= 1; i++) {
    uint8_t brightness = (i == 0) ? 180 : 100;
    addPixelWrap(secondPos + i, brightness / 4, brightness / 2, brightness);
  }

  applyBrightnessAndShow();
}

void displayMode_Binary() {
  int hour12, hour24, minute, second, daySeconds;
  if (!extractClock(hour12, hour24, minute, second, daySeconds)) return;

  memset(leds, 0, sizeof(leds));

  uint8_t bits[20];
  for (int i = 0; i < 20; i++) bits[i] = 0;

  for (int i = 0; i < 6; i++) bits[i]      = (hour24 >> i) & 1;
  bits[6] = 2;
  for (int i = 0; i < 6; i++) bits[7 + i]  = (minute >> i) & 1;
  bits[13] = 2;
  for (int i = 0; i < 6; i++) bits[14 + i] = (second >> i) & 1;

  for (int slot = 0; slot < 20; slot++) {
    for (int j = 0; j < 3; j++) {
      int idx = slot * 3 + j;
      if (bits[slot] == 2) {
        leds[idx] = {15, 15, 15};
      } else if (slot < 7) {
        leds[idx] = bits[slot] ? CRGB{220, 60, 60} : CRGB{20, 6, 6};
      } else if (slot < 14) {
        leds[idx] = bits[slot] ? CRGB{60, 220, 60} : CRGB{6, 20, 6};
      } else {
        leds[idx] = bits[slot] ? CRGB{60, 120, 255} : CRGB{6, 10, 20};
      }
    }
  }

  applyBrightnessAndShow();
}

void displayMode_HourMarker() {
  int hour12, hour24, minute, second, daySeconds;
  if (!extractClock(hour12, hour24, minute, second, daySeconds)) return;

  memset(leds, 0, sizeof(leds));

  for (int i = 0; i < NUM_LEDS; i++) {
    uint8_t base = (i <= minute) ? 40 : 12;
    leds[i] = {base / 4, base, base / 2};
  }

  overlayTimeMarkers(hour12, minute, second, 9);
  applyBrightnessAndShow();
}

void displayMode_Neon() {
  int hour12, hour24, minute, second, daySeconds;
  if (!extractClock(hour12, hour24, minute, second, daySeconds)) return;

  memset(leds, 0, sizeof(leds));

  int hourPos   = (hour12 * 5 + minute / 12) % NUM_LEDS;
  int minutePos = minute % NUM_LEDS;
  int secondPos = second % NUM_LEDS;

  // Bright neon magenta hour
  for (int i = -1; i <= 1; i++) {
    uint8_t brightness = (i == 0) ? 255 : 160;
    addPixelWrap(hourPos + i, brightness, 0, brightness);
  }

  // Bright neon cyan minute
  for (int i = -1; i <= 1; i++) {
    uint8_t brightness = (i == 0) ? 255 : 160;
    addPixelWrap(minutePos + i, 0, brightness, brightness);
  }

  // Bright neon yellow second
  for (int i = -1; i <= 1; i++) {
    uint8_t brightness = (i == 0) ? 255 : 160;
    addPixelWrap(secondPos + i, brightness, brightness, 0);
  }

  applyBrightnessAndShow();
}

void displayMode_Comet() {
  int hour12, hour24, minute, second, daySeconds;
  if (!extractClock(hour12, hour24, minute, second, daySeconds)) return;

  memset(leds, 0, sizeof(leds));

  int hourPos   = (hour12 * 5 + minute / 12) % NUM_LEDS;
  int minutePos = minute % NUM_LEDS;
  int secondPos = second % NUM_LEDS;

  // Hour comet trail (red, 7 LEDs long)
  for (int i = 0; i < 7; i++) {
    uint8_t fade = 255 - (i * 35);
    addPixelWrap(hourPos - i, fade, 0, 0);
  }

  // Minute comet trail (green, 5 LEDs long)
  for (int i = 0; i < 5; i++) {
    uint8_t fade = 255 - (i * 50);
    addPixelWrap(minutePos - i, 0, fade, 0);
  }

  // Second comet trail (blue, 10 LEDs long with faster fade)
  for (int i = 0; i < 10; i++) {
    uint8_t fade = 255 - (i * 25);
    addPixelWrap(secondPos - i, 0, 0, fade);
  }

  applyBrightnessAndShow();
}

void displayMode_Flame() {
  int hour12, hour24, minute, second, daySeconds;
  if (!extractClock(hour12, hour24, minute, second, daySeconds)) return;

  // Update flame only every 50ms (20 times/sec instead of 60)
  static uint32_t lastUpdate = 0;
  uint32_t now = millis();

  if (now - lastUpdate > 50) {
    lastUpdate = now;
    uint32_t phase = now / 40;

    for (int i = 0; i < NUM_LEDS; i++) {
      uint32_t x      = (uint32_t)(i * 41 + phase * 7);
      uint8_t flicker = (uint8_t)((x ^ (x >> 4)) & 0x3F);
      uint8_t r = 80 + flicker;
      uint8_t g = 15 + flicker / 3;
      uint8_t b = 2;
      leds[i] = {r, g, b};
    }
  }

  overlayTimeMarkers(hour12, minute, second, 9);
  applyBrightnessAndShow();
}

void displayClock_Solid() {
  int hour12, hour24, minute, second, daySeconds;
  if (!extractClock(hour12, hour24, minute, second, daySeconds)) return;

  memset(leds, 0, sizeof(leds));
  for (int i = 0; i < NUM_LEDS; i++) {
    uint8_t r, g, b;
    hsvToRgb((i * 6 + second * 6) % 360, 180, 30, r, g, b);
    leds[i] = {r, g, b};
  }

  overlayTimeMarkers(hour12, minute, second, 9);
  applyBrightnessAndShow();
}

void displayClock() {
  switch (displayMode) {
    case DISPLAY_SIMPLE:      displayMode_Simple();     break;
    case DISPLAY_PULSE:       displayMode_Pulse();      break;
    case DISPLAY_BINARY:      displayMode_Binary();     break;
    case DISPLAY_HOUR_MARKER: displayMode_HourMarker(); break;
    case DISPLAY_FLAME:       displayMode_Flame();      break;
    case DISPLAY_PASTEL:      displayMode_Pastel();     break;
    case DISPLAY_NEON:        displayMode_Neon();       break;
    case DISPLAY_COMET:       displayMode_Comet();      break;
    case DISPLAY_SOLID:
    default:                  displayClock_Solid();     break;
  }
}
