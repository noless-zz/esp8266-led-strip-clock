#include "led.h"
#include "globals.h"
#include "debug.h"
#include <time.h>

void setupLEDs() {
  if (ledStrip) delete ledStrip;
  uint16_t ledType = ledRgbw ? (NEO_GRBW + NEO_KHZ800) : (NEO_GRB + NEO_KHZ800);
  ledStrip = new Adafruit_NeoPixel(NUM_LEDS, LED_PIN, ledType);
  ledStrip->begin();
  ledStrip->setBrightness(ledBrightness);
  ledStrip->clear();
  ledStrip->show();
  Serial.printf("[LED] 60-LED %s clock initialized\n", ledRgbw ? "RGBW" : "RGB");
}

void showLEDs() {
  if (!ledStrip) return;
  for (int i = 0; i < NUM_LEDS; i++) {
    int phys = ledReversed ? (NUM_LEDS - 1 - i) : i;
    if (ledRgbw)
      ledStrip->setPixelColor(phys, ledStrip->Color(leds[i].r, leds[i].g, leds[i].b, W_BRIGHT_LEVELS[wBrightLevel]));
    else
      ledStrip->setPixelColor(phys, ledStrip->Color(leds[i].r, leds[i].g, leds[i].b));
  }
  ledStrip->show();
}

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

void hsvToRgb(uint16_t hue, uint8_t sat, uint8_t val, uint8_t &r, uint8_t &g, uint8_t &b) {
  hue %= 360;
  uint8_t region = hue / 60;
  uint16_t rem = (hue % 60) * 255 / 60;
  uint8_t p = (uint16_t)val * (255 - sat) / 255;
  uint8_t q = (uint16_t)val * (255 - ((uint16_t)sat * rem / 255)) / 255;
  uint8_t t = (uint16_t)val * (255 - ((uint16_t)sat * (255 - rem) / 255)) / 255;
  switch (region) {
    case 0: r = val; g = t; b = p; break;
    case 1: r = q; g = val; b = p; break;
    case 2: r = p; g = val; b = t; break;
    case 3: r = p; g = q; b = val; break;
    case 4: r = t; g = p; b = val; break;
    default: r = val; g = p; b = q; break;
  }
}

// Returns 0-255 brightness for the current hour using a smooth cosine curve
// between autoBrightDimHour (minimum) and autoBrightPeakHour (maximum).
uint8_t computeAutoBrightness() {
  time_t now = time(nullptr);
  if (now < 86400) return autoBrightDimVal;  // no valid time yet, stay dim
  struct tm* t = localtime(&now);
  int h = t->tm_hour;
  int halfPeriod = ((int)autoBrightPeakHour - (int)autoBrightDimHour + 24) % 24;
  if (halfPeriod == 0) halfPeriod = 12;
  int otherHalf = 24 - halfPeriod;
  int tHours = (h - (int)autoBrightDimHour + 24) % 24;
  float theta;
  if (tHours <= halfPeriod) {
    theta = M_PI * (float)tHours / (float)halfPeriod;
  } else {
    float rem = (float)(tHours - halfPeriod);
    theta = M_PI + M_PI * rem / (float)otherHalf;
  }
  float f = 0.5f - 0.5f * cosf(theta);
  float br = (float)autoBrightDimVal + ((float)autoBrightPeakVal - (float)autoBrightDimVal) * f;
  return (uint8_t)constrain((int)br, 0, 255);
}

void applyBrightnessAndShow() {
  uint8_t effectiveBr = autoBrightEnabled ? computeAutoBrightness() : ledBrightness;
  for (int i = 0; i < NUM_LEDS; i++) {
    leds[i].r = (uint16_t)leds[i].r * effectiveBr / 255;
    leds[i].g = (uint16_t)leds[i].g * effectiveBr / 255;
    leds[i].b = (uint16_t)leds[i].b * effectiveBr / 255;
  }
  showLEDs();
}

bool extractClock(int &hour12, int &hour24, int &minute, int &second, int &daySeconds) {
  time_t now = time(nullptr);
  if (now < 86400) {
    memset(leds, 0, sizeof(leds));
    leds[0] = {255, 0, 0};
    leds[NUM_LEDS - 1] = {255, 0, 0};
    showLEDs();
    return false;
  }
  struct tm* t = localtime(&now);
  hour24 = t->tm_hour;
  hour12 = t->tm_hour % 12;
  minute = t->tm_min;
  second = t->tm_sec;
  daySeconds = hour24 * 3600 + minute * 60 + second;
  return true;
}

CRGB blendSpectrumColor(const CRGB &base, uint8_t spectrum, uint8_t level, uint16_t phaseSeed) {
  if (spectrum == 0) return base;

  uint8_t rr = base.r;
  uint8_t gg = base.g;
  uint8_t bb = base.b;

  if (spectrum == 1) {
    uint8_t hueR, hueG, hueB;
    uint16_t hue = (uint16_t)((phaseSeed + millis() / 18) % 360);
    hsvToRgb(hue, 255, level, hueR, hueG, hueB);
    rr = addSat(base.r / 2, hueR / 2);
    gg = addSat(base.g / 2, hueG / 2);
    bb = addSat(base.b / 2, hueB / 2);
  } else if (spectrum == 2) {
    uint8_t tw = (uint8_t)((phaseSeed + (millis() / 24)) % 120);
    uint8_t swing = tw < 60 ? tw : (119 - tw);
    uint8_t boost = (uint8_t)(level / 4 + swing);
    rr = addSat(base.r, boost / 2);
    gg = addSat(base.g, boost / 3);
    bb = addSat(base.b, boost / 2);
  }

  return CRGB{rr, gg, bb};
}

ModeDisplayConfig defaultModeConfigFor(uint8_t mode) {
  ModeDisplayConfig cfg = {
    255, 0, 0,
    0, 255, 0,
    0, 0, 255,
    5,
    3,
    3,
    0
  };

  if (mode == DISPLAY_SIMPLE) {
    cfg.hourWidth = 3;
    cfg.minuteWidth = 3;
    cfg.secondWidth = 3;
  } else if (mode == DISPLAY_COMET) {
    cfg.hourWidth = 7;
    cfg.minuteWidth = 5;
    cfg.secondWidth = 10;
    cfg.spectrum = 1;
  } else if (mode == DISPLAY_PASTEL) {
    cfg = {
      190, 95, 150,
      110, 210, 170,
      110, 170, 220,
      3,
      3,
      3,
      0
    };
  } else if (mode == DISPLAY_NEON) {
    cfg = {
      255, 0, 220,
      0, 255, 220,
      255, 255, 0,
      3,
      3,
      3,
      2
    };
  }

  return cfg;
}

void setDefaultModeConfigs() {
  for (int m = 0; m < DISPLAY_MAX; m++) {
    modeConfigs[m] = defaultModeConfigFor((uint8_t)m);
  }
}

bool isModeConfigValid(const ModeDisplayConfig &cfg) {
  if (cfg.hourWidth < 1 || cfg.hourWidth > 21) return false;
  if (cfg.minuteWidth < 1 || cfg.minuteWidth > 21) return false;
  if (cfg.secondWidth < 1 || cfg.secondWidth > 30) return false;
  if (cfg.spectrum > 2) return false;
  return true;
}

void applyModeConfigDefaultsIfInvalid(uint8_t mode) {
  if (mode >= DISPLAY_MAX) return;
  if (!isModeConfigValid(modeConfigs[mode])) {
    setDefaultModeConfigs();
    return;
  }
}

void overlayTimeMarkers(int hour12, int minute, int second, const ModeDisplayConfig &cfg, int secTrailLen) {
  int hourPos = (hour12 * 5 + minute / 12) % NUM_LEDS;
  int minutePos = minute % NUM_LEDS;
  int secondPos = second % NUM_LEDS;

  int hCount = max(1, (int)cfg.hourWidth);
  int mCount = max(1, (int)cfg.minuteWidth);
  int sCount = max(1, (int)cfg.secondWidth);

  auto drawCenteredMarker = [&](int centerPos, int pixelCount, CRGB base, uint16_t seedBase) {
    int centerIdx = pixelCount / 2;
    int maxDist = max(centerIdx, pixelCount - 1 - centerIdx);
    for (int j = 0; j < pixelCount; j++) {
      int offset = j - centerIdx;
      int dist = abs(offset);
      uint8_t level = 255;
      if (maxDist > 0) {
        level = (uint8_t)(255 - (dist * 190 / maxDist));
      }
      CRGB col = blendSpectrumColor(base, cfg.spectrum, level, (uint16_t)(seedBase + j * 19));
      addPixelWrap(centerPos + offset, col.r, col.g, col.b);
    }
  };

  drawCenteredMarker(hourPos, hCount, CRGB{cfg.hourR, cfg.hourG, cfg.hourB}, 40);
  drawCenteredMarker(minutePos, mCount, CRGB{cfg.minuteR, cfg.minuteG, cfg.minuteB}, 120);
  drawCenteredMarker(secondPos, sCount, CRGB{cfg.secondR, cfg.secondG, cfg.secondB}, 220);

  int tail = max(0, secTrailLen);
  int tailStart = (sCount / 2) + 1;
  for (int i = 0; i < tail; i++) {
    int dist = tailStart + i;
    uint8_t level = (uint8_t)(220 - ((i + 1) * 200 / (tail + 1)));
    CRGB sc = blendSpectrumColor(CRGB{cfg.secondR, cfg.secondG, cfg.secondB}, cfg.spectrum, level, (uint16_t)(260 + (i + 1) * 17));
    addPixelWrap(secondPos - dist, sc.r, sc.g, sc.b);
  }
}

// Draw a marker centered at `pos`, optionally cross-fading to `nextPos`.
// wCur/wNext are 0..255 brightness weights — caller controls the blend.
// Marker size stays at pixelCount throughout; no spatial extension.
void drawWeightedMarker(int pos, int pixelCount, CRGB color,
                                uint8_t weight, uint16_t seedBase, uint8_t spectrum) {
  if (weight == 0) return;
  int half = pixelCount / 2;
  int maxDist = max(half, pixelCount - 1 - half);
  for (int j = 0; j < pixelCount; j++) {
    int offset = j - half;
    int dist = abs(offset);
    uint8_t level = maxDist > 0 ? (uint8_t)(255 - dist * 190 / maxDist) : 255;
    uint8_t scaled = (uint8_t)((uint16_t)level * weight / 255);
    CRGB col = blendSpectrumColor(color, spectrum, scaled, (uint16_t)(seedBase + j * 19));
    addPixelWrap(pos + offset, col.r, col.g, col.b);
  }
}

// Update fade state when a hand moves to a new position.
void updateFade(HandFade &f, int newPos) {
  if (f.toPos == newPos) return;           // same position, nothing to do
  f.fromPos = (f.toPos < 0) ? newPos : f.toPos;  // on first call, no fade
  f.toPos   = newPos;
  f.startMs = millis();
}

// Per-hand fade state (file-scoped)
static HandFade fadeS, fadeM, fadeH;

void displayMode_Simple() {
  int hour12, hour24, minute, second, daySeconds;
  if (!extractClock(hour12, hour24, minute, second, daySeconds)) return;
  const ModeDisplayConfig &cfg = modeConfigs[DISPLAY_SIMPLE];

  int secPos  = second % NUM_LEDS;
  int minPos  = minute % NUM_LEDS;
  int hourPos = (hour12 * 5 + minute / 12) % NUM_LEDS;

  updateFade(fadeS, secPos);
  updateFade(fadeM, minPos);
  updateFade(fadeH, hourPos);

  memset(leds, 0, sizeof(leds));

  // For each hand: compute fade weights from the independent per-hand timer
  auto renderHand = [&](HandFade &f, int pixelCount, CRGB color, uint16_t seedBase) {
    uint32_t elapsed = millis() - f.startMs;
    uint8_t wTo, wFrom;
    if (simpleFadeMs == 0 || elapsed >= simpleFadeMs || f.fromPos == f.toPos) {
      wTo = 255; wFrom = 0;
    } else {
      wTo   = (uint8_t)(elapsed * 255 / simpleFadeMs);
      wFrom = 255 - wTo;
    }
    drawWeightedMarker(f.toPos,   pixelCount, color, wTo,   seedBase, cfg.spectrum);
    drawWeightedMarker(f.fromPos, pixelCount, color, wFrom, seedBase, cfg.spectrum);
  };

  renderHand(fadeS, cfg.secondWidth, CRGB{cfg.secondR, cfg.secondG, cfg.secondB}, 220);
  renderHand(fadeM, cfg.minuteWidth, CRGB{cfg.minuteR, cfg.minuteG, cfg.minuteB}, 120);
  renderHand(fadeH, cfg.hourWidth,   CRGB{cfg.hourR,   cfg.hourG,   cfg.hourB},   40);

  applyBrightnessAndShow();
}

void displayMode_Pulse() {
  int hour12, hour24, minute, second, daySeconds;
  if (!extractClock(hour12, hour24, minute, second, daySeconds)) return;
  const ModeDisplayConfig &cfg = modeConfigs[DISPLAY_PULSE];

  memset(leds, 0, sizeof(leds));
  uint32_t ms = millis();
  uint8_t beat = (uint8_t)((ms % 1000) < 500 ? ((ms % 500) * 80 / 500) : ((1000 - (ms % 1000)) * 80 / 500));

  // Very dim background pulse (reduced from previous)
  for (int i = 0; i < NUM_LEDS; i++) {
    uint8_t base = 5 + beat / 8;
    leds[i] = {(uint8_t)(base / 4), (uint8_t)(base / 3), (uint8_t)(base / 2)};
  }

  // Clear HMS markers with more contrast
  overlayTimeMarkers(hour12, minute, second, cfg, 7);
  applyBrightnessAndShow();
}

void displayMode_Pastel() {
  int hour12, hour24, minute, second, daySeconds;
  if (!extractClock(hour12, hour24, minute, second, daySeconds)) return;
  const ModeDisplayConfig &cfg = modeConfigs[DISPLAY_PASTEL];

  memset(leds, 0, sizeof(leds));
  overlayTimeMarkers(hour12, minute, second, cfg, 2);
  applyBrightnessAndShow();
}

void displayMode_Binary() {
  int hour12, hour24, minute, second, daySeconds;
  if (!extractClock(hour12, hour24, minute, second, daySeconds)) return;
  const ModeDisplayConfig &cfg = modeConfigs[DISPLAY_BINARY];

  memset(leds, 0, sizeof(leds));

  uint8_t bits[20];
  for (int i = 0; i < 20; i++) bits[i] = 0;

  for (int i = 0; i < 6; i++) bits[i] = (hour24 >> i) & 1;
  bits[6] = 2;
  for (int i = 0; i < 6; i++) bits[7 + i] = (minute >> i) & 1;
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

  overlayTimeMarkers(hour12, minute, second, cfg, 1);
  applyBrightnessAndShow();
}

void displayMode_HourMarker() {
  int hour12, hour24, minute, second, daySeconds;
  if (!extractClock(hour12, hour24, minute, second, daySeconds)) return;
  const ModeDisplayConfig &cfg = modeConfigs[DISPLAY_HOUR_MARKER];

  memset(leds, 0, sizeof(leds));

  for (int i = 0; i < NUM_LEDS; i++) {
    uint8_t base = (i <= minute) ? 40 : 12;
    leds[i] = {(uint8_t)(base / 4), base, (uint8_t)(base / 2)};
  }

  overlayTimeMarkers(hour12, minute, second, cfg, 9);
  applyBrightnessAndShow();
}

void displayMode_Neon() {
  int hour12, hour24, minute, second, daySeconds;
  if (!extractClock(hour12, hour24, minute, second, daySeconds)) return;
  const ModeDisplayConfig &cfg = modeConfigs[DISPLAY_NEON];

  memset(leds, 0, sizeof(leds));
  overlayTimeMarkers(hour12, minute, second, cfg, 2);
  applyBrightnessAndShow();
}

void displayMode_Comet() {
  int hour12, hour24, minute, second, daySeconds;
  if (!extractClock(hour12, hour24, minute, second, daySeconds)) return;
  const ModeDisplayConfig &cfg = modeConfigs[DISPLAY_COMET];

  memset(leds, 0, sizeof(leds));
  int cometTail = 4 + (int)cfg.secondWidth * 2;
  for (int i = 0; i < NUM_LEDS; i++) {
    uint8_t dim = (uint8_t)(2 + (i % 7));
    leds[i] = {dim, dim, dim};
  }
  overlayTimeMarkers(hour12, minute, second, cfg, cometTail);
  applyBrightnessAndShow();
}

void displayMode_Flame() {
  int hour12, hour24, minute, second, daySeconds;
  if (!extractClock(hour12, hour24, minute, second, daySeconds)) return;
  const ModeDisplayConfig &cfg = modeConfigs[DISPLAY_FLAME];

  // Update the flame phase at most 20 times/sec, but always rewrite leds[]
  // from the saved phase so applyBrightnessAndShow() never re-scales stale values.
  static uint32_t lastUpdate = 0;
  static uint32_t phase = 0;
  uint32_t now = millis();

  if (now - lastUpdate > 50) {  // Advance phase max 20 times/sec
    lastUpdate = now;
    phase = now / 40;
  }

  for (int i = 0; i < NUM_LEDS; i++) {
    uint32_t x = (uint32_t)(i * 41 + phase * 7);
    uint8_t flicker = (uint8_t)((x ^ (x >> 4)) & 0x3F);
    uint8_t r = 80 + flicker;
    uint8_t g = 15 + flicker / 3;
    uint8_t b = 2;
    leds[i] = {r, g, b};
  }

  overlayTimeMarkers(hour12, minute, second, cfg, 9);
  applyBrightnessAndShow();
}

void displayBootAnimation() {
  if (!ledStrip) return;

  // Each stage: {r, g, b, lastLED (inclusive), stepMs}
  struct StageStyle { uint8_t r, g, b; uint8_t lastLed; uint8_t stepMs; };
  static const StageStyle styles[] = {
    {80,  0,  0, 14, 60},   // INIT     -- red,    slow
    {80, 40,  0, 29, 50},   // AP_UP    -- orange
    {70, 70,  0, 44, 40},   // SCANNING -- yellow
    { 0, 30, 90, 59, 30},   // STA_CONN -- blue,   fast
    { 0, 80,  0, 59, 20},   // WIFI_OK  -- green,  fast fill
    { 0, 70, 70, 59, 25},   // NTP_WAIT -- cyan
  };

  int stageIdx = (int)bootStage;
  if (stageIdx >= (int)(sizeof(styles) / sizeof(styles[0])))
    stageIdx = (int)(sizeof(styles) / sizeof(styles[0])) - 1;
  const StageStyle& s = styles[stageIdx];

  static int  pos       = 0;
  static int  dir       = 1;
  static int  fillCount = 0;
  static BootStage lastStage = BOOT_STAGE_INIT;
  static unsigned long lastStep = 0;

  // Reset bounce position when stage changes
  if (bootStage != lastStage) {
    pos = 0; dir = 1; fillCount = 0;
    lastStage = bootStage;
  }

  if (millis() - lastStep < s.stepMs) return;
  lastStep = millis();

  ledStrip->setBrightness(28);
  ledStrip->clear();

  const uint8_t TAIL = 7;

  if (bootStage == BOOT_STAGE_WIFI_OK) {
    // Green creeping fill -- LED 0..fillCount lit, then a bright head
    if (fillCount < NUM_LEDS) fillCount++;
    for (int i = 0; i < fillCount; i++)
      ledStrip->setPixelColor(i, ledStrip->Color(0, 25, 0));
    if (fillCount < NUM_LEDS)
      ledStrip->setPixelColor(fillCount, ledStrip->Color(0, 80, 0));
  } else {
    // Bouncing comet within 0..lastLed
    int span = s.lastLed + 1;

    // Draw tail
    for (int t = 1; t <= TAIL; t++) {
      int idx = pos - dir * t;
      if (idx >= 0 && idx < span) {
        uint8_t fade = (TAIL - t + 1) * 255 / ((TAIL + 1) * 8);
        ledStrip->setPixelColor(idx, ledStrip->Color(
          s.r * fade / 32, s.g * fade / 32, s.b * fade / 32));
      }
    }
    // Draw head
    ledStrip->setPixelColor(pos, ledStrip->Color(s.r, s.g, s.b));

    // Bounce
    pos += dir;
    if (pos >= span) { pos = span - 2; dir = -1; }
    if (pos < 0)     { pos = 1;        dir =  1; }
  }

  ledStrip->show();
}

void displayClock() {
  time_t now = time(nullptr);
  bool ntpSynced = (now >= 86400);

  // forceStatusDisplay beats everything -- always show animation
  // otherwise: show animation until NTP synced, unless forceClockDisplay
  if (forceStatusDisplay || (!ntpSynced && !forceClockDisplay)) {
    displayBootAnimation();
    return;
  }

  // LEDs turned off via button 1 long press -- blank the strip
  if (ledsOff) {
    if (ledStrip) { ledStrip->clear(); ledStrip->show(); }
    return;
  }

  // Restore user brightness when transitioning to clock for the first time
  static bool brightnessRestored = false;
  if (!brightnessRestored) {
    if (ledStrip) ledStrip->setBrightness(ledBrightness);
    brightnessRestored = true;
  }

  // Dispatcher: choose display mode
  switch (displayMode) {
    case DISPLAY_SIMPLE:
      displayMode_Simple();
      break;
    case DISPLAY_PULSE:
      displayMode_Pulse();
      break;
    case DISPLAY_BINARY:
      displayMode_Binary();
      break;
    case DISPLAY_HOUR_MARKER:
      displayMode_HourMarker();
      break;
    case DISPLAY_FLAME:
      displayMode_Flame();
      break;
    case DISPLAY_PASTEL:
      displayMode_Pastel();
      break;
    case DISPLAY_NEON:
      displayMode_Neon();
      break;
    case DISPLAY_COMET:
      displayMode_Comet();
      break;
    case DISPLAY_SOLID:
    default:
      displayClock_Solid();
      break;
  }
}

void displayClock_Solid() {
  int hour12, hour24, minute, second, daySeconds;
  if (!extractClock(hour12, hour24, minute, second, daySeconds)) return;
  const ModeDisplayConfig &cfg = modeConfigs[DISPLAY_SOLID];

  memset(leds, 0, sizeof(leds));
  for (int i = 0; i < NUM_LEDS; i++) {
    uint8_t r, g, b;
    hsvToRgb((i * 6 + second * 6) % 360, 180, 30, r, g, b);
    leds[i] = {r, g, b};
  }

  overlayTimeMarkers(hour12, minute, second, cfg, 9);
  applyBrightnessAndShow();
}
