/*
 * ESP8266 LED Strip Clock
 * 60-LED NeoPixel time display with NTP sync and timezone auto-detection
 * Features: WiFi captive portal, firmware OTA upload, time sync, color blending
 */

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESPAsyncWebServer.h>
#include <ESPAsyncTCP.h>
#include <DNSServer.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <ESP8266mDNS.h>
#include <Adafruit_NeoPixel.h>
#include <EEPROM.h>
#include <Updater.h>
#include <ESP8266httpUpdate.h>
#include <time.h>
#include <sys/time.h>

#ifndef NUM_LEDS
#define NUM_LEDS 60
#endif
#ifndef LED_PIN
#define LED_PIN D4
#endif

// Version is injected by scripts/auto_version.py pre-build script.
// MAJOR.MINOR.PATCH+BUILD  where minor=git commit count, build=compile counter.
// To bump major/patch: edit version.json manually.
#ifndef FW_VERSION_STR
#  define FW_VERSION_STR "0.0.0+0"   // fallback if script didn't run
#endif
#ifndef FW_GIT_HASH
#  define FW_GIT_HASH "unknown"
#endif
const char* FW_VERSION_BASE = FW_VERSION_STR;
const char* FW_BUILD_TIME   = __DATE__ " " __TIME__;
// AP_SSID is built at runtime with last 3 MAC bytes, e.g. "LED-Clock-A1B2C3"
String AP_SSID_STR;
const char* AP_SSID = nullptr; // set in setup() after WiFi.macAddress() is available
const char* AP_PASS = "";
const char* OTA_PASS = "admin123";
const char* NTP_SERVER = "pool.ntp.org";
const byte DNS_PORT = 53;
const int MAX_SCAN_CACHE = 30;

// Display modes
enum DisplayMode {
  DISPLAY_SOLID = 0,        // Rainbow orbit with HMS markers
  DISPLAY_SIMPLE = 1,       // Clean 3-LED HMS (red/green/blue)
  DISPLAY_PULSE = 2,        // Subtle pulse with HMS
  DISPLAY_BINARY = 3,       // Binary clock stretched to 60 LEDs
  DISPLAY_HOUR_MARKER = 4,  // Minute progress with hour beacon
  DISPLAY_FLAME = 5,        // Optimized flame effect with HMS
  DISPLAY_PASTEL = 6,       // Soft pastel HMS colors
  DISPLAY_NEON = 7,         // Bright neon HMS
  DISPLAY_COMET = 8,        // Animated HMS comet trails
  DISPLAY_MAX = 9
};

// EEPROM Layout
const int EEPROM_SIZE = 512;
const int EEPROM_MAGIC_ADDR = 0;
const int EEPROM_SSID_ADDR = 1;
const int EEPROM_PASS_ADDR = 65;
const int EEPROM_BRIGHTNESS_ADDR = 129;
const int EEPROM_TZ_OFFSET_ADDR = 130;
const int EEPROM_DISPLAY_MODE_ADDR = 134;
const int EEPROM_MODE_CFG_MAGIC_ADDR = 135;
const int EEPROM_RGBW_ADDR = 136;          // 1 byte: 1=RGBW, 0=RGB
const int EEPROM_REVERSED_ADDR = 137;      // 1 byte: 1=reversed, 0=normal
const int EEPROM_MODE_CFG_BASE_ADDR = 160; // 9 modes x 13 bytes = 117 bytes -> uses 160-276
// Debug remote logging (addr 279-298, after mode configs)
const int EEPROM_DBG_MAGIC_ADDR   = 279;   // 1 byte: magic 0xD8
const int EEPROM_DBG_ENABLED_ADDR = 280;   // 1 byte: 1=enabled
const int EEPROM_DBG_IP_ADDR      = 281;   // 16 bytes: null-terminated IP string
const int EEPROM_DBG_PORT_ADDR    = 297;   // 2 bytes: port (big-endian)
const int EEPROM_FADE_MS_ADDR     = 299;   // 2 bytes: Simple mode fade duration (big-endian, 0=off)
const int EEPROM_AUTO_BRIGHT_ADDR = 301;   // 6 bytes: magic(1), enabled(1), dim_val(1), peak_val(1), dim_hour(1), peak_hour(1)
const uint8_t EEPROM_AUTO_BRIGHT_MAGIC = 0xAB;
const uint8_t EEPROM_DBG_MAGIC    = 0xD8;
const uint8_t EEPROM_MODE_CFG_MAGIC = 0x5C;
const uint8_t EEPROM_MAGIC = 0xA7;
const int MAX_SSID_LEN = 32;
const int MAX_PASS_LEN = 64;

// Global state - LEDs
Adafruit_NeoPixel *ledStrip = nullptr;
uint8_t ledBrightness = 76;  // 30%
bool    autoBrightEnabled  = false;
uint8_t autoBrightDimVal   = 26;   // 10% of 255  (night minimum)
uint8_t autoBrightPeakVal  = 255;  // 100%        (day maximum)
uint8_t autoBrightDimHour  = 2;    // 2 am
uint8_t autoBrightPeakHour = 14;   // 2 pm
bool ledRgbw = false;        // false=RGB, true=RGBW
bool ledReversed = false;    // false=normal (0â†'59), true=reversed (59â†'0)
DisplayMode displayMode = DISPLAY_SOLID;
struct CRGB { uint8_t r, g, b; } leds[NUM_LEDS];

struct ModeDisplayConfig {
  uint8_t hourR, hourG, hourB;
  uint8_t minuteR, minuteG, minuteB;
  uint8_t secondR, secondG, secondB;
  uint8_t hourWidth;
  uint8_t minuteWidth;
  uint8_t secondWidth;
  uint8_t spectrum;
};

ModeDisplayConfig modeConfigs[DISPLAY_MAX];

// Web server
AsyncWebServer server(80);
DNSServer dnsServer;
bool mdnsStarted = false;

// WiFi state
struct ScanCacheEntry {
  String ssid;
  int32_t rssi = 0;
  int channel = -1;
  int enc = -1;
  bool hasBssid = false;
};
ScanCacheEntry scanCache[MAX_SCAN_CACHE];
int scanCacheCount = 0;
unsigned long scanCacheUpdatedAt = 0;

struct {
  bool active = false;
  bool connecting = false;
  bool success = false;
  String attemptedSsid;
  String error = "";
  unsigned long startedAt = 0;
  wl_status_t lastStatus = WL_DISCONNECTED;
} wifiConnect;

bool wifiConnected = false;
bool forceClockDisplay = false;   // show clock even before NTP
bool forceStatusDisplay = false;  // show boot animation even after NTP synced
String savedSsid;   // credentials loaded from EEPROM, used after AP is up
String savedPass;
unsigned long lastNtpSync = 0, lastTzCheck = 0;
struct { int32_t utcOffset; String name; bool autoDetected; } tz = {0, "UTC", true};
struct {
  String source = "ip-api.com";
  String status = "idle";
  String message = "not started";
  String responseSample = "";
  int httpCode = 0;
  unsigned long lastAttemptMs = 0;
  unsigned long lastSuccessMs = 0;
} tzDiag;

// Direct self-update (device fetches firmware URL itself)
String pendingDirectUpdateUrl  = "";
String lastDirectUpdateError   = "";

// OTA state
struct {
  bool inProgress = false;
  bool approved = false;
  bool lastSuccess = false;
  uint8_t lastErrorCode = 0;
  String lastErrorText = "";
  String lastFileName = "";
  uint32_t expectedBytes = 0;
  uint32_t writtenBytes = 0;
  unsigned long startedAt = 0;
  unsigned long finishedAt = 0;
} otaStatus;

// RTC memory boot record -- survives WDT/exception resets, cleared on power-cycle
struct BootRecord {
  uint32_t magic;       // RTC_BOOT_MAGIC when valid
  uint32_t uptime_s;    // last saved uptime in seconds (updated every 30s)
  uint32_t boot_count;  // incremented each reboot
};
#define RTC_BOOT_MAGIC 0xB007C10CUL
#define RTC_BOOT_SLOT  0  // offset 0 in user RTC memory (SDK reserves lower words)

// Remote UDP debug logging
bool debugRemoteEnabled = false;
String debugServerIp = "";
uint16_t debugServerPort = 7878;
WiFiUDP debugUdp;
String cachedBootInfo = "";      // captured in setup(), sent after WiFi connects
bool bootInfoSent = false;

// Forward declarations for display functions
void displayMode_Simple();
void displayMode_Pulse();
void displayMode_Pastel();
void displayMode_Binary();
void displayMode_HourMarker();
void displayMode_Neon();
void displayMode_Comet();
void displayMode_Flame();
void displayClock_Solid();
void setDefaultModeConfigs();
void loadModeConfigsFromEEPROM();
void saveModeConfigToEEPROM(uint8_t mode);
void saveDisplayModeToEEPROM();

// ============================================================================
// Remote Debug Logging
// ============================================================================

void debugLogf(const char* level, const char* tag, const char* fmt, ...) {
  char msg[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(msg, sizeof(msg), fmt, args);
  va_end(args);
  Serial.printf("[%lu][%s][%s] %s\n", millis(), level, tag, msg);
  if (!debugRemoteEnabled || !wifiConnected || debugServerIp.length() == 0) return;
  char packet[320];
  int plen = snprintf(packet, sizeof(packet), "[%lu][%s][%s] %s\n", millis(), level, tag, msg);
  debugUdp.beginPacket(debugServerIp.c_str(), debugServerPort);
  debugUdp.write((const uint8_t*)packet, (size_t)plen);
  debugUdp.endPacket();
}

#define DLOGI(tag, ...) debugLogf("INF", tag, __VA_ARGS__)
#define DLOGE(tag, ...) debugLogf("ERR", tag, __VA_ARGS__)
#define DLOGW(tag, ...) debugLogf("WRN", tag, __VA_ARGS__)

void captureBootInfo() {
  rst_info *ri = ESP.getResetInfoPtr();

  // Read RTC boot record (valid across WDT/exception resets, not power cycles)
  BootRecord br = {};
  bool rtcValid = ESP.rtcUserMemoryRead(RTC_BOOT_SLOT, (uint32_t*)&br, sizeof(br))
                  && (br.magic == RTC_BOOT_MAGIC);
  uint32_t lastUptime_s  = rtcValid ? br.uptime_s  : 0;
  uint32_t bootCount     = rtcValid ? br.boot_count + 1 : 1;

  // Update record immediately so loop() can increment uptime from zero
  br.magic      = RTC_BOOT_MAGIC;
  br.uptime_s   = 0;
  br.boot_count = bootCount;
  ESP.rtcUserMemoryWrite(RTC_BOOT_SLOT, (uint32_t*)&br, sizeof(br));

  // Format last uptime as h:mm:ss
  char uptimeBuf[24];
  if (rtcValid) {
    snprintf(uptimeBuf, sizeof(uptimeBuf), "%luh%02lum%02lus",
             lastUptime_s / 3600, (lastUptime_s % 3600) / 60, lastUptime_s % 60);
  } else {
    snprintf(uptimeBuf, sizeof(uptimeBuf), "unknown (power-on)");
  }

  // Build reset-reason string
  char reason[128];
  snprintf(reason, sizeof(reason), "reason=%d (%s)", ri->reason, ESP.getResetReason().c_str());
  // Crash reasons: 1=HW WDT, 2=Exception, 3=Soft WDT  (4=ESP.restart() is intentional)
  bool isCrash = (ri->reason == 1 || ri->reason == 2 || ri->reason == 3);
  if (isCrash) {
    char crash[128];
    snprintf(crash, sizeof(crash), " CRASH! exccause=%d epc1=0x%08X excvaddr=0x%08X",
             ri->exccause, ri->epc1, ri->excvaddr);
    cachedBootInfo = String(reason) + String(crash);
  } else {
    cachedBootInfo = String(reason);
  }

  Serial.printf("[BOOT] #%lu  last_uptime=%s  %s\n",
                bootCount, uptimeBuf, cachedBootInfo.c_str());
}

// ============================================================================
// LED Functions
// ============================================================================

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
      ledStrip->setPixelColor(phys, ledStrip->Color(leds[i].r, leds[i].g, leds[i].b, 0));
    else
      ledStrip->setPixelColor(phys, ledStrip->Color(leds[i].r, leds[i].g, leds[i].b));
  }
  ledStrip->show();
}

// ============================================================================
// Display Mode Functions
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
static void drawWeightedMarker(int pos, int pixelCount, CRGB color,
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

// Transition duration in ms — cross-fade when any hand moves to a new LED.
// 0 = disabled (instant step). Configurable via /api/simple/fade, stored in EEPROM.
uint32_t simpleFadeMs = 400;

// Per-hand fade state: tracks old/new positions independently of RTC/millis alignment.
struct HandFade {
  int    fromPos  = -1;
  int    toPos    = -1;
  uint32_t startMs = 0;
};
static HandFade fadeS, fadeM, fadeH;

// Update fade state when a hand moves to a new position.
static void updateFade(HandFade &f, int newPos) {
  if (f.toPos == newPos) return;           // same position, nothing to do
  f.fromPos = (f.toPos < 0) ? newPos : f.toPos;  // on first call, no fade
  f.toPos   = newPos;
  f.startMs = millis();
}

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
    leds[i] = {base / 4, base / 3, base / 2};
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
    leds[i] = {base / 4, base, base / 2};
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

  // Optimize: update flame only every 50ms (20 times/sec instead of 60)
  static uint32_t lastUpdate = 0;
  uint32_t now = millis();
  
  if (now - lastUpdate > 50) {  // Update max 20 times/sec instead of 60
    lastUpdate = now;
    uint32_t phase = now / 40;  // Slower phase calculation
    
    for (int i = 0; i < NUM_LEDS; i++) {
      // Simplified pseudo-random without heavy multiplication
      uint32_t x = (uint32_t)(i * 41 + phase * 7);
      uint8_t flicker = (uint8_t)((x ^ (x >> 4)) & 0x3F);  // Reduced bit ops
      uint8_t r = 80 + flicker;
      uint8_t g = 15 + flicker / 3;
      uint8_t b = 2;
      leds[i] = {r, g, b};
    }
  }

  overlayTimeMarkers(hour12, minute, second, cfg, 9);
  applyBrightnessAndShow();
}

// Boot progress stages --" advances externally as system state changes
enum BootStage {
  BOOT_STAGE_INIT     = 0,  // just started          --" red,    LEDs 0-14
  BOOT_STAGE_AP_UP    = 1,  // AP is up              --" orange, LEDs 0-29
  BOOT_STAGE_SCANNING = 2,  // scanning networks     --" yellow, LEDs 0-44
  BOOT_STAGE_STA_CONN = 3,  // STA connecting        --" blue,   LEDs 0-59
  BOOT_STAGE_WIFI_OK  = 4,  // WiFi connected        --" green,  fill 60
  BOOT_STAGE_NTP_WAIT = 5,  // waiting for NTP       --" cyan,   full bounce
  BOOT_STAGE_RUNNING  = 6,  // fully operational     --" clock displayed
};
BootStage bootStage = BOOT_STAGE_INIT;

void displayBootAnimation() {
  if (!ledStrip) return;

  // Each stage: {r, g, b, lastLED (inclusive), stepMs}
  struct StageStyle { uint8_t r, g, b; uint8_t lastLed; uint8_t stepMs; };
  static const StageStyle styles[] = {
    {80,  0,  0, 14, 60},   // INIT     --" red,    slow
    {80, 40,  0, 29, 50},   // AP_UP    --" orange
    {70, 70,  0, 44, 40},   // SCANNING --" yellow
    { 0, 30, 90, 59, 30},   // STA_CONN --" blue,   fast
    { 0, 80,  0, 59, 20},   // WIFI_OK  --" green,  fast fill
    { 0, 70, 70, 59, 25},   // NTP_WAIT --" cyan
  };

  const StageStyle& s = styles[(int)bootStage];

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
    // Green creeping fill --" LED 0..fillCount lit, then a bright head
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

  // forceStatusDisplay beats everything --" always show animation
  // otherwise: show animation until NTP synced, unless forceClockDisplay
  if (forceStatusDisplay || (!ntpSynced && !forceClockDisplay)) {
    displayBootAnimation();
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

// ============================================================================
// EEPROM Functions
// ============================================================================

void saveDisplayModeToEEPROM() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.write(EEPROM_DISPLAY_MODE_ADDR, (uint8_t)displayMode);
  EEPROM.commit();
}

void saveModeConfigToEEPROM(uint8_t mode) {
  if (mode >= DISPLAY_MAX) return;
  int addr = EEPROM_MODE_CFG_BASE_ADDR + (int)mode * (int)sizeof(ModeDisplayConfig);
  EEPROM.begin(EEPROM_SIZE);
  const uint8_t *raw = reinterpret_cast<const uint8_t*>(&modeConfigs[mode]);
  for (unsigned int i = 0; i < sizeof(ModeDisplayConfig); i++) {
    EEPROM.write(addr + (int)i, raw[i]);
  }
  EEPROM.write(EEPROM_MODE_CFG_MAGIC_ADDR, EEPROM_MODE_CFG_MAGIC);
  EEPROM.commit();
}

void loadModeConfigsFromEEPROM() {
  setDefaultModeConfigs();
  EEPROM.begin(EEPROM_SIZE);

  if (EEPROM.read(EEPROM_MODE_CFG_MAGIC_ADDR) != EEPROM_MODE_CFG_MAGIC) {
    for (uint8_t m = 0; m < DISPLAY_MAX; m++) {
      saveModeConfigToEEPROM(m);
    }
    return;
  }

  for (uint8_t m = 0; m < DISPLAY_MAX; m++) {
    int addr = EEPROM_MODE_CFG_BASE_ADDR + (int)m * (int)sizeof(ModeDisplayConfig);
    ModeDisplayConfig loaded;
    uint8_t *raw = reinterpret_cast<uint8_t*>(&loaded);
    for (unsigned int i = 0; i < sizeof(ModeDisplayConfig); i++) {
      raw[i] = EEPROM.read(addr + (int)i);
    }
    if (isModeConfigValid(loaded)) {
      modeConfigs[m] = loaded;
    }
  }
}

void loadEEPROMSettings() {
  EEPROM.begin(EEPROM_SIZE);
  uint8_t magic = EEPROM.read(EEPROM_MAGIC_ADDR);
  if (magic != EEPROM_MAGIC) {
    EEPROM.write(EEPROM_MAGIC_ADDR, EEPROM_MAGIC);
    EEPROM.write(EEPROM_DISPLAY_MODE_ADDR, (uint8_t)displayMode);
    EEPROM.commit();
    loadModeConfigsFromEEPROM();
    Serial.println("[EEPROM] Initialized new settings");
    return;
  }
  
  // Load WiFi SSID --" reject if any byte is non-printable (garbage/uninitialized EEPROM)
  String ssid = "";
  bool ssidValid = true;
  for (int i = 0; i < MAX_SSID_LEN; i++) {
    char c = EEPROM.read(EEPROM_SSID_ADDR + i);
    if (c == 0) break;
    if (c < 0x20 || c > 0x7E) { ssidValid = false; break; }
    ssid += c;
  }
  if (!ssidValid || ssid.length() == 0) {
    Serial.println("[EEPROM] No valid WiFi credentials stored");
  } else {
    String pass = "";
    for (int i = 0; i < MAX_PASS_LEN; i++) {
      char c = EEPROM.read(EEPROM_PASS_ADDR + i);
      if (c == 0) break;
      pass += c;
    }
    Serial.println("[EEPROM] Loaded WiFi: " + ssid);
    // DEBUG: dump password bytes to verify EEPROM integrity
    Serial.printf("[EEPROM] Pass len=%d chars: [", pass.length());
    for (unsigned int i = 0; i < pass.length(); i++) {
      Serial.printf("%c(0x%02X)", pass[i], (uint8_t)pass[i]);
      if (i < pass.length() - 1) Serial.print(' ');
    }
    Serial.println("]");
    savedSsid = ssid;
    savedPass = pass;  // will be used in setupWiFi() after AP is up
  }
  
  // Load brightness
  uint8_t br = EEPROM.read(EEPROM_BRIGHTNESS_ADDR);
  if (br > 0 && br <= 255) ledBrightness = br;

  // Load LED type
  uint8_t rgbwFlag = EEPROM.read(EEPROM_RGBW_ADDR);
  if (rgbwFlag == 0x01 || rgbwFlag == 0x00) ledRgbw = (rgbwFlag == 0x01);

  // Load LED direction
  uint8_t revFlag = EEPROM.read(EEPROM_REVERSED_ADDR);
  if (revFlag == 0x01 || revFlag == 0x00) ledReversed = (revFlag == 0x01);
  
  // Load timezone offset
  int32_t tzOff = (EEPROM.read(EEPROM_TZ_OFFSET_ADDR + 0) << 24) |
                  (EEPROM.read(EEPROM_TZ_OFFSET_ADDR + 1) << 16) |
                  (EEPROM.read(EEPROM_TZ_OFFSET_ADDR + 2) << 8) |
                  (EEPROM.read(EEPROM_TZ_OFFSET_ADDR + 3));
  if (tzOff != 0 && tzOff >= -43200 && tzOff <= 43200) tz.utcOffset = tzOff;
  
  // Load display mode
  uint8_t mode = EEPROM.read(EEPROM_DISPLAY_MODE_ADDR);
  if (mode < DISPLAY_MAX) displayMode = (DisplayMode)mode;

  loadModeConfigsFromEEPROM();

  // Load debug remote logging settings
  if (EEPROM.read(EEPROM_DBG_MAGIC_ADDR) == EEPROM_DBG_MAGIC) {
    debugRemoteEnabled = (EEPROM.read(EEPROM_DBG_ENABLED_ADDR) == 0x01);
    char ipBuf[17] = {};
    for (int i = 0; i < 16; i++) ipBuf[i] = (char)EEPROM.read(EEPROM_DBG_IP_ADDR + i);
    ipBuf[16] = 0;
    debugServerIp = String(ipBuf);
    uint16_t pHi = EEPROM.read(EEPROM_DBG_PORT_ADDR);
    uint16_t pLo = EEPROM.read(EEPROM_DBG_PORT_ADDR + 1);
    uint16_t p = (pHi << 8) | pLo;
    if (p > 0 && p < 65535) debugServerPort = p;
  }

  // Load Simple fade duration (0=off, 50–2000ms valid range)
  {
    uint16_t fHi = EEPROM.read(EEPROM_FADE_MS_ADDR);
    uint16_t fLo = EEPROM.read(EEPROM_FADE_MS_ADDR + 1);
    uint16_t fms = (fHi << 8) | fLo;
    if (fms <= 2000) simpleFadeMs = fms;  // 0 = disabled, up to 2000ms
  }

  // Load adaptive brightness settings
  if (EEPROM.read(EEPROM_AUTO_BRIGHT_ADDR) == EEPROM_AUTO_BRIGHT_MAGIC) {
    autoBrightEnabled  = (EEPROM.read(EEPROM_AUTO_BRIGHT_ADDR + 1) != 0);
    uint8_t dv = EEPROM.read(EEPROM_AUTO_BRIGHT_ADDR + 2);
    uint8_t pv = EEPROM.read(EEPROM_AUTO_BRIGHT_ADDR + 3);
    uint8_t dh = EEPROM.read(EEPROM_AUTO_BRIGHT_ADDR + 4);
    uint8_t ph = EEPROM.read(EEPROM_AUTO_BRIGHT_ADDR + 5);
    if (dh < 24 && ph < 24 && dh != ph) {
      autoBrightDimVal   = dv;
      autoBrightPeakVal  = pv;
      autoBrightDimHour  = dh;
      autoBrightPeakHour = ph;
    }
  }
}

void saveFadeMs() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.write(EEPROM_FADE_MS_ADDR,     (simpleFadeMs >> 8) & 0xFF);
  EEPROM.write(EEPROM_FADE_MS_ADDR + 1, simpleFadeMs & 0xFF);
  EEPROM.commit();
}

void saveAutoBrightSettings() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.write(EEPROM_AUTO_BRIGHT_ADDR,     EEPROM_AUTO_BRIGHT_MAGIC);
  EEPROM.write(EEPROM_AUTO_BRIGHT_ADDR + 1, autoBrightEnabled ? 1 : 0);
  EEPROM.write(EEPROM_AUTO_BRIGHT_ADDR + 2, autoBrightDimVal);
  EEPROM.write(EEPROM_AUTO_BRIGHT_ADDR + 3, autoBrightPeakVal);
  EEPROM.write(EEPROM_AUTO_BRIGHT_ADDR + 4, autoBrightDimHour);
  EEPROM.write(EEPROM_AUTO_BRIGHT_ADDR + 5, autoBrightPeakHour);
  EEPROM.commit();
}

void saveDebugConfig() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.write(EEPROM_DBG_MAGIC_ADDR, EEPROM_DBG_MAGIC);
  EEPROM.write(EEPROM_DBG_ENABLED_ADDR, debugRemoteEnabled ? 0x01 : 0x00);
  for (int i = 0; i < 16; i++) {
    EEPROM.write(EEPROM_DBG_IP_ADDR + i, (uint8_t)(i < (int)debugServerIp.length() ? debugServerIp[i] : 0));
  }
  EEPROM.write(EEPROM_DBG_PORT_ADDR,     (debugServerPort >> 8) & 0xFF);
  EEPROM.write(EEPROM_DBG_PORT_ADDR + 1, debugServerPort & 0xFF);
  EEPROM.commit();
}

void saveEEPROMSettings(const String& ssid, const String& pass) {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.write(EEPROM_MAGIC_ADDR, EEPROM_MAGIC);
  
  for (int i = 0; i < MAX_SSID_LEN; i++) {
    EEPROM.write(EEPROM_SSID_ADDR + i, i < ssid.length() ? ssid[i] : 0);
  }
  
  for (int i = 0; i < MAX_PASS_LEN; i++) {
    EEPROM.write(EEPROM_PASS_ADDR + i, i < pass.length() ? pass[i] : 0);
  }
  
  EEPROM.write(EEPROM_BRIGHTNESS_ADDR, ledBrightness);
  EEPROM.write(EEPROM_TZ_OFFSET_ADDR + 0, (tz.utcOffset >> 24) & 0xFF);
  EEPROM.write(EEPROM_TZ_OFFSET_ADDR + 1, (tz.utcOffset >> 16) & 0xFF);
  EEPROM.write(EEPROM_TZ_OFFSET_ADDR + 2, (tz.utcOffset >> 8) & 0xFF);
  EEPROM.write(EEPROM_TZ_OFFSET_ADDR + 3, tz.utcOffset & 0xFF);
  EEPROM.write(EEPROM_DISPLAY_MODE_ADDR, (uint8_t)displayMode);
  
  EEPROM.commit();
  Serial.println("[EEPROM] Saved settings for " + ssid);
  DLOGI("EEPROM", "Settings saved  ssid=%s  heap=%u", ssid.c_str(), ESP.getFreeHeap());
}

// ============================================================================
// Time & Timezone Functions
// ============================================================================

void syncTimeNTP() {
  if (!wifiConnected) return;
  // Only show NTP_WAIT animation during initial boot (not on hourly refreshes)
  if (bootStage < BOOT_STAGE_RUNNING) bootStage = BOOT_STAGE_NTP_WAIT;
  Serial.println("[NTP] Syncing with " + String(NTP_SERVER));
  configTime(tz.utcOffset, 0, NTP_SERVER);
  time_t now = time(nullptr);
  int attempts = 50;
  while (now < 86400 && attempts-- > 0) { delay(100); now = time(nullptr); }
  if (now > 86400) {
    bootStage = BOOT_STAGE_RUNNING;
    Serial.println("[NTP] Time synced");
    DLOGI("NTP", "Synced  epoch=%lu  tz=%s  heap=%u", (unsigned long)now, tz.name.c_str(), ESP.getFreeHeap());
  } else {
    DLOGW("NTP", "Sync failed after %d attempts  heap=%u", 50, ESP.getFreeHeap());
  }
  lastNtpSync = millis();
}

void detectTimezone() {
  tzDiag.source = "ip-api.com";
  tzDiag.lastAttemptMs = millis();
  tzDiag.status = "running";
  tzDiag.message = "starting detection";
  tzDiag.httpCode = 0;
  tzDiag.responseSample = "";

  if (!wifiConnected) {
    tzDiag.status = "error";
    tzDiag.message = "wifi not connected";
    Serial.println("[GEO] Not connected to WiFi, skipping timezone detection");
    return;
  }
  DLOGI("TZ", "Auto-detect start  heap=%u", ESP.getFreeHeap());

  struct TzProvider { const char* host; const char* path; const char* label; };
  TzProvider providers[] = {
    {"ip-api.com", "/json/?fields=status,message,timezone,offset,country,city", "ip-api"},
    {"ipwho.is", "/", "ipwho.is"}
  };

  bool detected = false;
  for (size_t p = 0; p < (sizeof(providers) / sizeof(providers[0])); p++) {
    WiFiClient client;
    tzDiag.source = providers[p].label;

    DLOGI("TZ", "Trying %s (heap=%u)", providers[p].label, ESP.getFreeHeap());
    if (!client.connect(providers[p].host, 80)) {
      tzDiag.status = "error";
      tzDiag.message = String("connect failed to ") + providers[p].host;
      DLOGW("TZ", "%s connect failed", providers[p].label);
      continue;
    }
    DLOGI("TZ", "%s connected", providers[p].label);

    client.print("GET ");
    client.print(providers[p].path);
    client.print(" HTTP/1.0\r\nHost: ");
    client.print(providers[p].host);
    client.print("\r\nUser-Agent: LED-Clock/2.0\r\nConnection: close\r\n\r\n");

    String line;
    while (client.connected()) {
      line = client.readStringUntil('\n');
      if (line.startsWith("HTTP/")) {
        int sp = line.indexOf(' ');
        if (sp > 0 && sp + 4 <= line.length()) {
          tzDiag.httpCode = line.substring(sp + 1, sp + 4).toInt();
        }
      }
      if (line == "\r") break;
    }

    String body;
    unsigned long readStart = millis();
    while (millis() - readStart < 5000) {
      while (client.available()) {
        body += (char)client.read();
        readStart = millis();
      }
      if (!client.connected() && !client.available()) break;
      delay(5);
    }
    client.stop();
    body.trim();

    if (body.length() == 0) {
      tzDiag.status = "error";
      tzDiag.message = "empty response body";
      tzDiag.responseSample = "<empty>";
      continue;
    }

    int startObj = body.indexOf('{');
    int endObj = body.lastIndexOf('}');
    if (startObj >= 0 && endObj > startObj) {
      body = body.substring(startObj, endObj + 1);
    }

    tzDiag.responseSample = body.substring(0, 160);

    DynamicJsonDocument doc(1024);
    DeserializationError error = deserializeJson(doc, body);
    if (error) {
      tzDiag.status = "error";
      tzDiag.message = (body[0] != '{' && body[0] != '[')
        ? ("non-json response (http " + String(tzDiag.httpCode) + ")")
        : ("json parse failed: " + String(error.c_str()));
      continue;
    }

    if (doc["status"] && doc["status"].as<String>() != "success") {
      tzDiag.status = "error";
      tzDiag.message = "api status=" + doc["status"].as<String>() + (doc["message"] ? (", " + doc["message"].as<String>()) : "");
      continue;
    }
    if (doc["success"] && !doc["success"].as<bool>()) {
      tzDiag.status = "error";
      tzDiag.message = "api success=false" + (doc["message"] ? (", " + doc["message"].as<String>()) : "");
      continue;
    }

    String tzName = "";
    int32_t tzOffset = 0;
    bool hasOffset = false;

    if (doc["timezone"] && doc["timezone"].is<const char*>()) {
      tzName = doc["timezone"].as<String>();
    } else if (doc["timezone"] && doc["timezone"].is<JsonObject>() && doc["timezone"]["id"]) {
      tzName = doc["timezone"]["id"].as<String>();
    }

    if (doc["offset"] && !doc["offset"].isNull()) {
      tzOffset = doc["offset"].as<int32_t>();
      hasOffset = true;
    } else if (doc["timezone"] && doc["timezone"].is<JsonObject>() && doc["timezone"]["offset"] && !doc["timezone"]["offset"].isNull()) {
      tzOffset = doc["timezone"]["offset"].as<int32_t>();
      hasOffset = true;
    }

    if (tzName.length() == 0 || !hasOffset) {
      tzDiag.status = "error";
      tzDiag.message = "missing timezone/offset fields";
      continue;
    }

    tz.name = tzName;
    tz.utcOffset = tzOffset;
    tz.autoDetected = true;
    tzDiag.status = "ok";
    tzDiag.message = "timezone=" + tz.name + ", offset=" + String(tz.utcOffset);
    tzDiag.lastSuccessMs = millis();
    detected = true;
    DLOGI("TZ", "OK  tz=%s  offset=%ld  via=%s  heap=%u",
          tz.name.c_str(), (long)tz.utcOffset, providers[p].label, ESP.getFreeHeap());
    break;
  }

  if (!detected) {
    DLOGE("TZ", "All providers failed: %s", tzDiag.message.c_str());
    Serial.println("[GEO] All timezone providers failed: " + tzDiag.message);
    return;
  }
  
  syncTimeNTP();
  lastTzCheck = millis();
}

static const char* wlStatusName(wl_status_t s) {
  switch (s) {
    case WL_IDLE_STATUS:     return "IDLE";
    case WL_NO_SSID_AVAIL:  return "NO_SSID_AVAIL";
    case WL_SCAN_COMPLETED:  return "SCAN_COMPLETED";
    case WL_CONNECTED:       return "CONNECTED";
    case WL_CONNECT_FAILED:  return "CONNECT_FAILED";
    case WL_CONNECTION_LOST: return "CONNECTION_LOST";
    case WL_DISCONNECTED:    return "DISCONNECTED";
    default:                 return "UNKNOWN";
  }
}

void checkWiFi() {
  static unsigned long lastCheck = 0;
  static wl_status_t lastKnownStatus = WL_IDLE_STATUS;
  if (millis() - lastCheck < 5000) return;
  lastCheck = millis();

  wl_status_t status = WiFi.status();

  // Log every status change (even outside an active connect attempt)
  if (status != lastKnownStatus) {
    Serial.printf("[WiFi] checkWiFi: status %s -> %s\n",
                  wlStatusName(lastKnownStatus), wlStatusName(status));
    lastKnownStatus = status;
  }

  bool connected = (status == WL_CONNECTED);
  if (connected != wifiConnected) {
    wifiConnected = connected;
    if (connected) {
      Serial.println("[WiFi] Connected! IP: " + WiFi.localIP().toString() + "  SSID: " + WiFi.SSID());
      if (lastTzCheck == 0) {
        detectTimezone();
      } else {
        DLOGI("WiFi", "Reconnected -- skipping TZ detect (already done)  uptime=%lus", millis()/1000);
      }
    } else {
      Serial.printf("[WiFi] Disconnected! Last status: %d (%s)\n", (int)status, wlStatusName(status));
      DLOGW("WiFi", "Disconnected  status=%s  uptime=%lus  heap=%u",
            wlStatusName(status), millis()/1000, ESP.getFreeHeap());
    }
  }
}

// ============================================================================
// WiFi Scanning & Connection
// ============================================================================

String getWifiScanJson() {
  DynamicJsonDocument doc(2048);
  JsonArray arr = doc["networks"].to<JsonArray>();
  
  int scanState = WiFi.scanComplete();
  if (scanState == WIFI_SCAN_RUNNING) {
    doc["scanning"] = true;
  } else if (scanState == WIFI_SCAN_FAILED || scanState < 0) {
    WiFi.scanDelete();
    WiFi.scanNetworks(true, true);
    doc["scanning"] = true;
  } else {
    doc["scanning"] = false;
    scanCacheCount = 0;
    for (int i = 0; i < scanState; i++) {
      String ssid = WiFi.SSID(i);
      if (ssid.length() == 0) continue;
      JsonObject obj = arr.add<JsonObject>();
      obj["ssid"] = ssid;
      obj["rssi"] = WiFi.RSSI(i);
      obj["enc"] = (WiFi.encryptionType(i) != ENC_TYPE_NONE);
      
      if (scanCacheCount < MAX_SCAN_CACHE) {
        scanCache[scanCacheCount].ssid = ssid;
        scanCache[scanCacheCount].rssi = WiFi.RSSI(i);
        scanCache[scanCacheCount].channel = WiFi.channel(i);
        scanCache[scanCacheCount].enc = WiFi.encryptionType(i);
        scanCacheCount++;
      }
    }
    scanCacheUpdatedAt = millis();
    WiFi.scanDelete();
    if (scanState <= 0) {
      WiFi.scanNetworks(true, true);
      doc["scanning"] = true;
    }
  }
  
  String out;
  serializeJson(doc, out);
  return out;
}

bool startWiFiConnect(const String& ssid, const String& pass, bool saveToEeprom = false) {
  if (ssid.length() == 0) {
    Serial.println("[WiFi] startWiFiConnect: SSID is empty, aborting");
    return false;
  }
  if (wifiConnect.active) {
    Serial.println("[WiFi] startWiFiConnect: already connecting, aborting");
    return false;
  }

  Serial.println("[WiFi] --- Connection attempt ---");
  Serial.println("[WiFi] Target SSID : " + ssid);
  // DEBUG: print password bytes to catch any EEPROM corruption
  Serial.printf("[WiFi] Pass (%d): [", pass.length());
  for (unsigned int i = 0; i < pass.length(); i++) Serial.printf("%c", pass[i]);
  Serial.println("]");
  Serial.println("[WiFi] Current status: " + String(WiFi.status()));
  Serial.println("[WiFi] MAC address  : " + WiFi.macAddress());

  wifiConnect.active = true;
  wifiConnect.connecting = true;
  wifiConnect.attemptedSsid = ssid;
  wifiConnect.startedAt = millis();
  wifiConnect.lastStatus = WiFi.status();

  if (saveToEeprom) saveEEPROMSettings(ssid, pass);

  // Check scan cache for channel
  int targetChannel = 0;
  for (int i = 0; i < scanCacheCount; i++) {
    if (scanCache[i].ssid == ssid) {
      targetChannel = scanCache[i].channel;
      Serial.printf("[WiFi] Found in scan cache: ch=%d  rssi=%d  enc=%d\n",
                    scanCache[i].channel, scanCache[i].rssi, scanCache[i].enc);
      break;
    }
  }
  if (targetChannel == 0) {
    Serial.println("[WiFi] Not in scan cache -- connecting without channel hint");
  }

  // ESP8266 AP+STA constraint: both AP and STA must share the same radio channel.
  // Restart AP on the router's channel before connecting, or STA stays DISCONNECTED.
  if (targetChannel > 0 && targetChannel != (int)WiFi.channel()) {
    Serial.printf("[WiFi] Restarting AP on ch=%d to match router\n", targetChannel);
    WiFi.softAP(AP_SSID, AP_PASS, targetChannel);
  }

  Serial.printf("[WiFi] Calling WiFi.begin(\"%s\", <pass>)\n", ssid.c_str());
  WiFi.begin(ssid.c_str(), pass.c_str());
  Serial.println("[WiFi] WiFi.begin() called, polling for result...");
  DLOGI("WiFi", "Connecting  ssid=%s  ch=%d  heap=%u",
        ssid.c_str(), targetChannel, ESP.getFreeHeap());

  return true;
}

void updateWiFiConnect() {
  if (!wifiConnect.active) return;

  wl_status_t status = WiFi.status();

  // Log every status change during connection attempt
  if (status != wifiConnect.lastStatus) {
    unsigned long elapsed = millis() - wifiConnect.startedAt;
    Serial.printf("[WiFi] Status changed: %s -> %s  (+%lums)\n",
                  wlStatusName(wifiConnect.lastStatus), wlStatusName(status), elapsed);
    wifiConnect.lastStatus = status;
  }

  if (status == WL_CONNECTED) {
    wifiConnect.active = false;
    wifiConnect.connecting = false;
    wifiConnect.success = true;
    wifiConnected = true;
    bootStage = BOOT_STAGE_WIFI_OK;
    Serial.print("[WiFi] Connected! IP: ");
    Serial.println(WiFi.localIP());
    DLOGI("WiFi", "Connected  ssid=%s  ip=%s  rssi=%d  ch=%d  heap=%u",
          WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(),
          WiFi.RSSI(), (int)WiFi.channel(), ESP.getFreeHeap());
    if (!bootInfoSent) {
      bootInfoSent = true;
      DLOGI("BOOT", "fw=%s git:%s built:%s  %s", FW_VERSION_BASE, FW_GIT_HASH, FW_BUILD_TIME, cachedBootInfo.c_str());
    }
    if (lastTzCheck == 0) detectTimezone();  // skip on reconnects — already detected
  } else if (status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL) {
    wifiConnect.active = false;
    wifiConnect.connecting = false;
    wifiConnect.success = false;
    wifiConnect.error = "Connection failed";
    Serial.printf("[WiFi] Failed with status %d (%s) after %lums\n",
                  (int)status, wlStatusName(status), millis() - wifiConnect.startedAt);
    DLOGE("WiFi", "FAILED  status=%s  ssid=%s  after=%lums  heap=%u",
          wlStatusName(status), wifiConnect.attemptedSsid.c_str(),
          millis() - wifiConnect.startedAt, ESP.getFreeHeap());
  } else if (millis() - wifiConnect.startedAt > 20000) {
    // Timeout -- clear state, trigger a fresh scan, retry after scan completes
    wifiConnect.active = false;
    wifiConnect.connecting = false;
    wifiConnect.success = false;
    wifiConnect.error = "Connection timeout";
    Serial.printf("[WiFi] Timeout! Last status: %d (%s) -- rescanning and will retry\n",
                  (int)status, wlStatusName(status));
    DLOGW("WiFi", "TIMEOUT  status=%s  ssid=%s  heap=%u",
          wlStatusName(status), wifiConnect.attemptedSsid.c_str(), ESP.getFreeHeap());
    WiFi.disconnect(false);
    WiFi.scanNetworks(true, true);  // async rescan; boot auto-connect logic will retry
  } else {
    // Heartbeat every 5s so we can see it's still trying
    static unsigned long lastHeartbeat = 0;
    if (millis() - lastHeartbeat > 5000) {
      lastHeartbeat = millis();
      Serial.printf("[WiFi] Still connecting... status=%s  elapsed=%lus\n",
                    wlStatusName(status), (millis() - wifiConnect.startedAt) / 1000);
    }
  }
}

// ============================================================================
// OTA & Firmware Update
// ============================================================================

uint32_t getMaxUpdateSize() {
  return (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
}

const char* updateErrorToString(uint8_t error) {
  switch (error) {
    case UPDATE_ERROR_OK: return "OK";
    case UPDATE_ERROR_WRITE: return "Flash write failed";
    case UPDATE_ERROR_ERASE: return "Flash erase failed";
    case UPDATE_ERROR_READ: return "Flash read failed";
    case UPDATE_ERROR_SPACE: return "Not enough flash space";
    case UPDATE_ERROR_SIZE: return "Binary size mismatch";
    case UPDATE_ERROR_STREAM: return "Upload stream timeout/error";
    case UPDATE_ERROR_MD5: return "MD5 validation failed";
    case UPDATE_ERROR_MAGIC_BYTE: return "Invalid firmware magic byte";
    case UPDATE_ERROR_FLASH_CONFIG: return "Flash config mismatch";
    case UPDATE_ERROR_NEW_FLASH_CONFIG: return "New flash config invalid";
    case UPDATE_ERROR_BOOTSTRAP: return "Bootstrap validation failed";
    case UPDATE_ERROR_OOM: return "Out of memory during update";
    case UPDATE_ERROR_NO_DATA: return "No OTA data received";
    default: return "Unknown OTA error";
  }
}

// ============================================================================
// Web Pages (HTML)
// ============================================================================

const char INDEX_HTML[] PROGMEM = R"html(
<!DOCTYPE html><html><head><meta charset='utf-8'/><meta name='viewport' content='width=device-width, initial-scale=1'/>
<title>LED Clock</title><style>
*{margin:0;padding:0;box-sizing:border-box}body{font-family:system-ui;background:linear-gradient(135deg,#667eea,#764ba2);min-height:100vh;padding:20px;color:#333}
.container{max-width:600px;margin:0 auto}.clock-card{background:#fff;border-radius:20px;padding:40px 20px;margin-bottom:20px;box-shadow:0 10px 40px rgba(0,0,0,0.3);text-align:center}
.clock-time{font-size:72px;font-weight:300;letter-spacing:4px;color:#667eea;margin-bottom:10px;font-variant-numeric:tabular-nums}
.card{background:#fff;border-radius:16px;padding:20px;margin-bottom:15px;box-shadow:0 5px 20px rgba(0,0,0,0.2)}
h3{font-size:14px;letter-spacing:2px;text-transform:uppercase;color:#999;margin-bottom:15px}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:15px}.item{padding:12px;background:#f5f5f5;border-radius:8px;text-align:center}
.label{font-size:11px;color:#999;text-transform:uppercase}.value{font-size:16px;font-weight:600;color:#333;margin-top:5px}
.btn{width:100%;padding:12px;border:none;border-radius:6px;font-weight:bold;cursor:pointer;text-transform:uppercase;letter-spacing:1px;background:linear-gradient(135deg,#667eea,#764ba2);color:#fff;margin-bottom:10px}
.btn:active{opacity:.9}.btn-sm{padding:8px 16px;border:none;border-radius:6px;font-weight:bold;cursor:pointer;font-size:12px;letter-spacing:1px}
.btn-clock{background:#667eea;color:#fff}.btn-status{background:#eee;color:#555}
.stage-bar{display:flex;gap:4px;margin:12px 0}.stage-pip{flex:1;height:8px;border-radius:4px;background:#eee;transition:.4s}
.stage-pip.done{background:#4CAF50}.stage-pip.active{background:#667eea}.ring-badge{display:inline-block;padding:4px 12px;border-radius:20px;font-size:12px;font-weight:bold;letter-spacing:1px}
.badge-clock{background:#e8f5e9;color:#2e7d32}.badge-status{background:#e3f2fd;color:#1565c0}
</style></head><body><div class='container'>
<div class='clock-card'><div class='clock-time' id='time'>--:--:--</div><div style='font-size:16px;color:#999;margin-top:10px' id='date'>Loading</div></div>

<div class='card'><h3>Ring Display</h3>
<div style='display:flex;align-items:center;justify-content:space-between;margin-bottom:10px'>
<div><div class='label'>Showing</div><span class='ring-badge' id='ringBadge'>--</span></div>
<div><div class='label'>Boot Stage</div><div class='value' id='stageName'>--</div></div>
</div>
<div class='stage-bar'>
<div class='stage-pip' id='pip0' title='Booting'></div>
<div class='stage-pip' id='pip1' title='AP Ready'></div>
<div class='stage-pip' id='pip2' title='Scanning'></div>
<div class='stage-pip' id='pip3' title='Connecting'></div>
<div class='stage-pip' id='pip4' title='WiFi OK'></div>
<div class='stage-pip' id='pip5' title='NTP Sync'></div>
<div class='stage-pip' id='pip6' title='Running'></div>
</div>
<div style='display:flex;gap:8px;margin-top:12px'>
<button class='btn-sm btn-clock' style='flex:1' onclick='setRingMode(1)'>Show Clock</button>
<button class='btn-sm btn-status' style='flex:1' onclick='setRingMode(0)'>Show Status</button>
</div></div>

<div class='card'><h3>System</h3><div class='grid'>
<div class='item'><div class='label'>WiFi</div><div class='value' id='wifi'>--</div></div>
<div class='item'><div class='label'>Signal</div><div class='value' id='signal'>--</div></div>
<div class='item'><div class='label'>Timezone</div><div class='value' id='tz'>UTC</div></div>
<div class='item'><div class='label'>NTP</div><div class='value' id='ntp'>--</div></div>
</div></div>
<div class='card'><h3>Device</h3><div class='grid'>
<div class='item'><div class='label'>Firmware</div><div class='value' id='fw' title='Build timestamp' style='cursor:help;font-size:11px'>-</div></div>
<div class='item'><div class='label'>TZ Debug</div><div class='value' id='tz_debug'>manual UTC</div></div>
<div class='item'><div class='label'>Brightness</div><div class='value' id='bright'>-</div></div>
<div class='item'><div class='label'>IP</div><div class='value' style='font-size:12px' id='ip'>-</div></div>
<div class='item'><div class='label'>Heap</div><div class='value' id='heap'>-</div></div>
<div class='item'><div class='label'>Mode</div><div class='value' id='modeName'>-</div></div>
</div></div>
<button class='btn' onclick='location.href="/settings.html"'>Settings</button>
</div>
<script>
const STAGE_NAMES=['Booting','AP Ready','Scanning','Connecting','WiFi OK','NTP Wait','Running'];
const MODE_NAMES=['Solid','Simple','Pulse','Binary','HourMark','Flame','Pastel','Neon','Comet'];
function setRingMode(v){fetch('/api/ring?force_clock='+v).then(r=>r.json()).then(d=>{updateRingUI(d);}).catch(e=>console.warn(e));}
function updateRingUI(d){
  const isClock=d.ring_mode==='clock';
  const badge=document.getElementById('ringBadge');
  badge.textContent=isClock?'CLOCK':'STATUS';badge.className='ring-badge '+(isClock?'badge-clock':'badge-status');
  document.getElementById('stageName').textContent=d.boot_stage_name||'--';
  const stage=d.boot_stage||0;
  for(let i=0;i<7;i++){const p=document.getElementById('pip'+i);if(p)p.className='stage-pip'+(i<stage?' done':i===stage?' active':'');}
}
function updateStatus(){fetch('/api/status').then(r=>r.json()).then(d=>{
  const now=new Date();
  document.getElementById('time').textContent=now.toLocaleTimeString('en-US',{hour:'2-digit',minute:'2-digit',second:'2-digit'});
  document.getElementById('date').textContent=now.toLocaleDateString('en-US',{weekday:'short',month:'short',day:'numeric'});
  document.getElementById('wifi').textContent=d.wifi_connected?'\u2713 '+d.wifi_ssid:'\u2717 Offline';
  document.getElementById('signal').textContent=d.wifi_rssi?d.wifi_rssi+' dBm':'--';
  document.getElementById('tz').textContent=d.timezone||'UTC';
  document.getElementById('tz_debug').textContent=d.timezone_auto_detected?'Auto '+d.timezone_utc_offset_hours+'h':'Manual '+d.timezone_utc_offset_hours+'h';
  document.getElementById('ntp').textContent=d.ntp_synced?'\u2713 Synced':'\u23f1 Wait';
  document.getElementById('fw').textContent=d.fw_version_base||'-';document.getElementById('fw').title='Build: '+(d.fw_build_time||'unknown');
  document.getElementById('bright').textContent=d.brightness+'%';
  document.getElementById('ip').textContent=d.ip||'-';
  document.getElementById('heap').textContent=Math.round(d.heap/1024)+' KB';
  document.getElementById('modeName').textContent=MODE_NAMES[d.display_mode]||d.display_mode;
  updateRingUI({ring_mode:d.ring_mode,boot_stage:d.boot_stage,boot_stage_name:d.boot_stage_name});
}).catch(e=>console.warn(e));}
setInterval(updateStatus,3000);updateStatus();
</script></body></html>
)html";

const char SETTINGS_HTML[] PROGMEM = R"html(
<!DOCTYPE html><html><head><meta charset='utf-8'/><meta name='viewport' content='width=device-width, initial-scale=1'/>
<title>Settings</title><style>
*{margin:0;padding:0;box-sizing:border-box}body{font-family:system-ui;background:linear-gradient(135deg,#667eea,#764ba2);
min-height:100vh;padding:20px;color:#333}.container{max-width:700px;margin:0 auto}.header{color:#fff;margin-bottom:20px;display:flex;align-items:center;gap:15px}
.back{background:rgba(255,255,255,0.2);border:none;color:#fff;padding:10px 15px;border-radius:6px;cursor:pointer;font-size:14px;font-weight:bold}
.card{background:#fff;border-radius:12px;padding:20px;margin-bottom:20px;box-shadow:0 10px 30px rgba(0,0,0,0.2)}
.card h2{font-size:14px;text-transform:uppercase;letter-spacing:1px;color:#333;margin-bottom:15px;border-bottom:2px solid #667eea;padding-bottom:10px}
.form-group{margin-bottom:15px}.form-group label{display:block;font-size:12px;color:#666;text-transform:uppercase;letter-spacing:0.5px;margin-bottom:6px;font-weight:500}
.form-group select,.form-group input[type=text],.form-group input[type=password],.form-group input[type=number]{width:100%;padding:10px;border:1px solid #ddd;border-radius:6px;font-size:14px;font-family:inherit}
.form-group input:focus,.form-group select:focus{outline:none;border-color:#667eea;box-shadow:0 0 0 3px rgba(102,126,234,0.1)}
.wifi-row{display:flex;gap:8px;align-items:center;margin-bottom:10px}.wifi-row select{flex:1}.wifi-row .mini-btn{white-space:nowrap}
.btn{width:100%;padding:12px;border:none;border-radius:6px;font-size:14px;font-weight:bold;cursor:pointer;text-transform:uppercase;letter-spacing:1px;background:linear-gradient(135deg,#667eea,#764ba2);color:#fff;margin-bottom:10px}
.btn:hover{opacity:0.9}.btn-secondary{background:#666}
.mini-btn{display:inline-block;padding:8px 12px;font-size:12px;background:#f0f0f0;color:#333;border:1px solid #ddd;border-radius:4px;cursor:pointer;font-weight:bold}
.mini-btn:hover{background:#e0e0e0}.mini-btn:disabled{opacity:0.5;cursor:not-allowed}
.status-msg{font-size:12px;margin-top:8px;padding:8px;border-radius:4px;text-align:center;min-height:1em}
.status-ok{background:#d4edda;color:#155724;border:1px solid #c3e6cb}.status-err{background:#f8d7da;color:#721c24;border:1px solid #f5c6cb}.status-info{background:#d1ecf1;color:#0c5460;border:1px solid #bee5eb}
.upload-area{border:2px dashed #667eea;border-radius:8px;padding:20px;text-align:center;cursor:pointer;transition:all 0.3s;background:#f9f9f9}
.upload-area:hover{border-color:#764ba2;background:#f0f0f0}.upload-area p{font-size:12px;color:#666;margin:0}
.upload-btn{display:block;width:100%;margin-top:10px;padding:10px;font-family:inherit;font-size:12px;background:#667eea;color:#fff;border:none;border-radius:6px;cursor:pointer;font-weight:bold}
.upload-btn:disabled{opacity:0.4;cursor:not-allowed}
.progress-bar{width:100%;height:4px;background:#e0e0e0;border-radius:2px;margin-top:10px;display:none;overflow:hidden}
.progress-fill{height:100%;background:linear-gradient(90deg,#667eea,#764ba2);width:0%;transition:width 0.3s}
.note{font-size:11px;color:#999;margin-top:8px;font-style:italic}
</style></head><body><div class='container'><div class='header'><button class='back' onclick='history.back()'>&lt; Back</button><h1>Settings</h1></div>

<div class='card'><h2>WiFi Configuration</h2>
<div class='form-group'><label>Available Networks <span id='scanStatus' style='font-size:11px;color:#999'></span></label><div class='wifi-row'>
<select id='ssidList' size='6' onchange='networkSelected()'><option value=''>Scanning...</option></select>
<button class='mini-btn' id='scanBtn' onclick='scanWifi()' style='height:36px'>SCAN</button>
</div></div>
<div class='form-group'><label>SSID <span style='font-size:11px;color:#999'>(or type manually)</span></label>
<input type='text' id='wifiSsid' placeholder='Network name' style='font-family:monospace'/></div>
<div class='form-group'><label>Password <span id='openLabel' style='font-size:11px;color:#4a4'></span></label>
<input type='password' id='wifiPass' placeholder='Leave empty for open networks'/></div>
<button class='btn' onclick='connectWifi()'>Connect WiFi</button>
<div class='status-msg' id='wifiMsg'></div>
</div>

<div class='card'><h2>Time & Timezone</h2>
<div class='form-group'><label>NTP Server</label><input type='text' id='ntpServer' value='pool.ntp.org' placeholder='pool.ntp.org'/></div>
<button class='btn btn-secondary' onclick='syncNTP()'>Sync Now</button>

<div class='form-group' style='margin-top:15px'><label>Timezone Mode</label>
<label style='display:flex;align-items:center;margin-top:8px'><input type='radio' name='tzmode' value='auto' checked onchange='toggleTzMode()' style='margin-right:8px'/>Auto-detect</label>
<label style='display:flex;align-items:center;margin-top:8px'><input type='radio' name='tzmode' value='manual' onchange='toggleTzMode()' style='margin-right:8px'/>Manual offset</label></div>

<div id='manualTz' style='display:none'><div class='form-group'><label>UTC Offset (hours, -12 to +14)</label>
<input type='number' id='tzOffset' min='-12' max='14' value='0' placeholder='e.g., -5 for EST'/></div></div>
<button class='btn btn-secondary' onclick='saveTimezone()'>Save Timezone</button>
<div class='status-msg' id='tzMsg'></div>
</div>

<div class='card'><h2>Display Mode</h2>
<div class='form-group'><label>LED Display Style</label>
<select id='displayMode' onchange='saveDisplayMode()' style='width:100%;padding:8px;border:1px solid #ddd;border-radius:4px'>
<option value='0'>Marker Ring (rainbow orbit + HMS)</option>
<option value='1'>Simple HMS (clean 3-LED red/green/blue)</option>
<option value='2'>Pulse (subtle heartbeat + HMS)</option>
<option value='3'>Binary Clock (60 LEDs stretched bits)</option>
<option value='4'>Hour Beacon (minute progress + markers)</option>
<option value='5'>Flame HMS (warm flicker + markers)</option>
<option value='6'>Pastel HMS (soft pink/mint/sky)</option>
<option value='7'>Neon HMS (bright magenta/cyan/yellow)</option>
<option value='8'>Comet Trails (animated HMS tails)</option>
</select></div>
<div style='font-size:11px;color:#888;margin-top:5px' id='modeDesc'>Choose a display mode</div>
<button class='btn btn-secondary' onclick='updateModeDescription()'>Refresh Display</button>
<div class='form-group' style='margin-top:12px'>
<label>Hour Color</label><input type='color' id='hourColor' value='#ff0000' style='width:100%;height:36px;border:1px solid #ddd;border-radius:4px'/>
</div>
<div class='form-group'>
<label>Minute Color</label><input type='color' id='minuteColor' value='#00ff00' style='width:100%;height:36px;border:1px solid #ddd;border-radius:4px'/>
</div>
<div class='form-group'>
<label>Second Color</label><input type='color' id='secondColor' value='#0000ff' style='width:100%;height:36px;border:1px solid #ddd;border-radius:4px'/>
</div>
<div class='form-group'>
<label>Hour Width (pixels)</label><input type='range' id='hourWidth' min='1' max='21' value='5' style='width:100%'/>
<div style='font-size:10px;color:#777'>Pixels: <span id='hourWidthLabel'>5</span></div>
</div>
<div class='form-group'>
<label>Minute Width (pixels)</label><input type='range' id='minuteWidth' min='1' max='21' value='3' style='width:100%'/>
<div style='font-size:10px;color:#777'>Pixels: <span id='minuteWidthLabel'>3</span></div>
</div>
<div class='form-group'>
<label>Second Width (pixels)</label><input type='range' id='secondWidth' min='1' max='30' value='3' style='width:100%'/>
<div style='font-size:10px;color:#777'>Pixels: <span id='secondWidthLabel'>3</span></div>
</div>
<div class='form-group'>
<label>Color Spectrum</label>
<select id='spectrum' style='width:100%;padding:8px;border:1px solid #ddd;border-radius:4px'>
<option value='0'>Static</option>
<option value='1'>Rainbow blend</option>
<option value='2'>Pulse glow</option>
</select>
</div>
<div class='form-group' id='fadeGroup'>
<label>Simple HMS Transition Speed</label>
<input type='range' id='fadeMsSlider' min='0' max='2000' step='50' value='400' oninput='updateFadeMsLabel()' style='width:100%'/>
<div style='font-size:10px;color:#777'><span id='fadeMsLabel'>400</span> ms &nbsp;(0 = instant / no fade)</div>
</div>
<button class='btn btn-secondary' onclick='saveModeConfig()'>Save Mode Visuals</button>
<button class='btn btn-secondary' onclick='resetModeConfig()' style='margin-top:8px;background:#f8f8f8;border:1px solid #ddd;color:#555'>Reset Current Mode to Default</button>
<div class='status-msg' id='modeCfgMsg'></div>
</div>

<div class='card'><h2>Brightness</h2>
<div id='manualBrightGroup'>
<div class='form-group' style='margin-bottom:5px'><label>LED Brightness</label>
<input type='range' id='brightness' min='10' max='255' value='76' oninput='updateBrightnessLabel()' style='width:100%'/></div>
<div style='text-align:center;font-size:12px;color:#666'>
<span id='brightLabel'>30%</span> (<span id='brightValue'>76</span>/255)</div>
<button class='btn btn-secondary' onclick='saveBrightness()'>Save Brightness</button>
</div>
<div id='autoBrightNote' style='display:none;font-size:11px;color:#888;margin-top:6px'>Manual slider disabled &mdash; adaptive brightness is active. Current: <span id='effectiveBrPct'>-</span>%</div>
</div>

<div class='card'><h2>Adaptive Brightness</h2>
<div class='form-group' style='display:flex;align-items:center;gap:12px'>
<label style='margin:0'>Off</label>
<label style='position:relative;display:inline-block;width:48px;height:26px;margin:0'>
<input type='checkbox' id='autoBrightToggle' style='opacity:0;width:0;height:0' onchange='saveAutoBright()'>
<span id='autoBrightSlider' style='position:absolute;cursor:pointer;inset:0;background:#ccc;border-radius:26px;transition:.3s'></span>
</label>
<label style='margin:0'>On</label>
</div>
<div style='font-size:11px;color:#888;margin-top:4px;margin-bottom:12px'>Smoothly dims at night, brightens during day using a cosine curve between dim and peak hours.</div>
<div id='autoBrightControls'>
<div class='form-group'>
<label>Night Brightness (dim) &mdash; <span id='abDimHourLabel'>2</span>:00</label>
<input type='range' id='abDimHour' min='0' max='23' value='2' oninput='updateAutoBrightLabels()' style='width:100%'/>
</div>
<div class='form-group'>
<label>Day Brightness (peak) &mdash; <span id='abPeakHourLabel'>14</span>:00</label>
<input type='range' id='abPeakHour' min='0' max='23' value='14' oninput='updateAutoBrightLabels()' style='width:100%'/>
</div>
<div class='form-group'>
<label>Dim value: <span id='abDimPctLabel'>10</span>%</label>
<input type='range' id='abDimPct' min='0' max='100' value='10' oninput='updateAutoBrightLabels()' style='width:100%'/>
</div>
<div class='form-group'>
<label>Peak value: <span id='abPeakPctLabel'>100</span>%</label>
<input type='range' id='abPeakPct' min='0' max='100' value='100' oninput='updateAutoBrightLabels()' style='width:100%'/>
</div>
<button class='btn btn-secondary' onclick='saveAutoBright()'>Save Adaptive Brightness</button>
<div style='font-size:11px;color:#888;margin-top:8px'>Effective now: <span id='abEffectivePct'>-</span>%</div>
</div>
</div>

<div class='card'><h2>LED Strip Type</h2>
<div class='form-group' style='display:flex;align-items:center;gap:12px'>
<label style='margin:0'>RGB</label>
<label style='position:relative;display:inline-block;width:48px;height:26px;margin:0'>
<input type='checkbox' id='rgbwToggle' style='opacity:0;width:0;height:0' onchange='saveLedType()'>
<span id='rgbwSlider' style='position:absolute;cursor:pointer;inset:0;background:#ccc;border-radius:26px;transition:.3s'></span>
</label>
<label style='margin:0'>RGBW</label>
</div>
<div style='font-size:11px;color:#888;margin-top:6px'>Current: <span id='ledTypeLabel'>RGB</span> &mdash; change restarts the LED driver</div>
</div>

<div class='card'><h2>LED Direction</h2>
<div class='form-group' style='display:flex;align-items:center;gap:12px'>
<label style='margin:0'>Normal</label>
<label style='position:relative;display:inline-block;width:48px;height:26px;margin:0'>
<input type='checkbox' id='revToggle' style='opacity:0;width:0;height:0' onchange='saveLedDirection()'>
<span id='revSlider' style='position:absolute;cursor:pointer;inset:0;background:#ccc;border-radius:26px;transition:.3s'></span>
</label>
<label style='margin:0'>Reversed</label>
</div>
<div style='font-size:11px;color:#888;margin-top:6px'>Current: <span id='ledDirLabel'>Normal</span> &mdash; flips LED 0&harr;59 for opposite mounting orientation</div>
</div>

<div class='card'><h2>Debug Logging</h2>
<div class='form-group'><label>Debug Server IP</label>
<input type='text' id='dbgIp' placeholder='192.168.x.x' style='width:100%;padding:8px;box-sizing:border-box;border:1px solid #ddd;border-radius:4px;font-size:14px'></div>
<div class='form-group'><label>UDP Port (default 7878)</label>
<input type='number' id='dbgPort' value='7878' min='1' max='65535' style='width:100%;padding:8px;box-sizing:border-box;border:1px solid #ddd;border-radius:4px;font-size:14px'></div>
<div class='form-group' style='display:flex;align-items:center;gap:12px'>
<label style='margin:0'>Remote UDP Log</label>
<label style='position:relative;display:inline-block;width:48px;height:26px;margin:0'>
<input type='checkbox' id='dbgToggle' style='opacity:0;width:0;height:0' onchange='saveDebugConfig()'>
<span id='dbgSlider' style='position:absolute;cursor:pointer;inset:0;background:#ccc;border-radius:26px;transition:.3s'></span>
</label>
<span id='dbgEnabledLabel'>Disabled</span>
</div>
<div style='display:flex;gap:8px;margin-top:10px'>
<button onclick='saveDebugConfig()' class='btn btn-primary' style='padding:8px 16px'>Save</button>
<button onclick='sendDebugTest()' class='btn btn-secondary' style='padding:8px 16px'>Send Test</button>
</div>
<div id='dbgMsg' class='status-msg' style='margin-top:8px'></div>
<div style='font-size:11px;color:#888;margin-top:8px'>UDP log packets sent to server IP:port.<br>Run <code>node scripts/debug-server.js</code> in VSCode terminal &mdash; then open <code>http://localhost:7879</code> to view live logs.</div>
</div>

<div class='card'><h2>Firmware Update</h2>
<div style='margin-bottom:10px;display:flex;align-items:center;gap:10px;flex-wrap:wrap'>
<button class='btn btn-secondary' onclick='checkForUpdate()' id='checkUpdateBtn' style='padding:6px 14px;font-size:13px'>Check for Update</button>
<span id='updateStatus' style='font-size:12px;color:#888'></span>
</div>
<div id='updateInfo' style='display:none;margin-bottom:10px;padding:8px 10px;background:#f0f8ff;border:1px solid #b3d9ff;border-radius:6px;font-size:12px'>
<div>Latest: <strong id='latestTag'>-</strong> <a id='releaseLink' href='#' target='_blank' style='color:#2779bd;font-size:11px'>(view release)</a></div>
<div style='margin-top:4px'>Diff from <span id='currentHashSpan' style='font-family:monospace'></span>: <a id='diffLink' href='#' target='_blank' style='color:#2779bd'>compare on GitHub</a></div>
<div style='margin-top:8px;display:flex;gap:10px;flex-wrap:wrap;align-items:center'>
<a id='downloadLink' href='#' target='_blank' class='btn btn-secondary' style='font-size:12px;padding:6px 14px;text-decoration:none'>Download firmware.bin</a>
<button id='directFlashBtn' onclick='directFlash()' style='display:none;padding:6px 14px;background:#d35400;color:#fff;border:none;border-radius:4px;cursor:pointer;font-size:12px;font-weight:bold'>Flash directly to device</button>
</div>
<div id='directFlashStatus' style='display:none;margin-top:6px;font-size:11px'></div>
</div>
<div class='upload-area' onclick='document.getElementById("fwFile").click()' id='uploadArea'>
<p id='fileName'>Click to select .bin firmware file</p>
<input type='file' id='fwFile' accept='.bin' style='display:none' onchange='fileSelected(this)'>
</div>
<button class='upload-btn' id='uploadBtn' onclick='uploadFirmware()' disabled>Upload Firmware</button>
<div class='progress-bar' id='progBar'><div class='progress-fill' id='progFill'></div></div>
<div class='status-msg' id='statusMsg'></div>
<div class='note'>Max size: <span id='maxSize'>-</span> bytes</div>
</div>

<div class='card'><h2>Device Information</h2>
<div style='font-size:12px;line-height:1.8;color:#666'>
<div>Firmware: <span id='fwVersion'>-</span> <span id='fwBuildTime' style='font-size:10px;color:#999'></span> <span id='fwGitHash' style='font-size:10px;color:#666'></span></div>
<div>IP Address: <span id='deviceIp'>-</span></div>
<div>WiFi Mode: <span id='wifiMode'>AP</span></div>
<div>Signal: <span id='signal'>-</span></div>
<div>Timezone: <span id='tzDisplay'>UTC</span> <span id='tzMode' style='font-size:10px;color:#999'></span></div>
<div id='tzDebug' style='margin-top:8px;padding:4px;background:#f5f5f5;border-radius:3px;color:#666;font-size:10px;display:none'>
Offset: <span id='tzOffset'>0</span>h (<span id='tzOffsetSec'>0</span>s) | Auto-detected: <span id='tzAuto'>no</span>
</div>
<div style='margin-top:6px;font-size:10px;color:#666'>Detect status: <span id='tzDetectStatus'>-</span></div>
<div style='margin-top:2px;font-size:10px;color:#666'>Detect message: <span id='tzDetectMsg'>-</span></div>
</div></div>

</div>

<script>
let fwFile=null,uploading=false,_directUrl='';function updateBrightnessLabel(){const v=document.getElementById('brightness').value;
document.getElementById('brightValue').textContent=v;document.getElementById('brightLabel').textContent=(Math.round(v/255*100))+'%';}
let modeCfgSaveTimer=null,modeCfgPersistTimer=null;
function toggleTzMode(){document.getElementById('manualTz').style.display=document.querySelector('input[name="tzmode"]:checked').value==='manual'?'block':'none';}
function fileSelected(input){const sb=document.getElementById('statusMsg');const ub=document.getElementById('uploadBtn');fwFile=null;ub.disabled=true;
if(input.files.length===0)return;fwFile=input.files[0];document.getElementById('fileName').textContent='ðŸ"„ '+fwFile.name;sb.textContent='Checking firmware...';sb.className='status-msg status-info';
const r=new FileReader();r.onload=()=>{const b=new Uint8Array(r.result);const m=b.length>0?b[0]:0;
fetch('/api/update/precheck?name='+encodeURIComponent(fwFile.name)+'&size='+fwFile.size+'&magic='+m).then(r=>r.json()).then(d=>{
if(d.ok){ub.disabled=false;sb.textContent='\u2713 '+d.summary;sb.className='status-msg status-ok';}else{ub.disabled=true;sb.textContent='\u2717 '+d.error;sb.className='status-msg status-err';}}).catch(e=>{ub.disabled=true;sb.textContent='Check failed: '+e;sb.className='status-msg status-err';});};
r.onerror=()=>{ub.disabled=true;sb.textContent='Failed to read file';sb.className='status-msg status-err';};r.readAsArrayBuffer(fwFile.slice(0,1));}
function uploadFirmware(){if(!fwFile||uploading)return;if(!confirm('Upload '+fwFile.name+'?'))return;uploading=true;document.getElementById('uploadBtn').disabled=true;
const sb=document.getElementById('statusMsg');const pb=document.getElementById('progBar');const pf=document.getElementById('progFill');pb.style.display='block';sb.textContent='';
const fd=new FormData();fd.append('firmware',fwFile);const x=new XMLHttpRequest();
x.upload.addEventListener('progress',(e)=>{if(e.lengthComputable)pf.style.width=(e.loaded/e.total*100)+'%';});
x.addEventListener('load',()=>{uploading=false;try{const p=JSON.parse(x.responseText);if(x.status===200&&p.ok){sb.textContent='\u2713 Update OK ('+p.written+' bytes). Rebooting...';sb.className='status-msg status-ok';setTimeout(()=>location.reload(),2000);}else{const e=p?p.error:x.responseText;sb.textContent='\u2717 '+e;sb.className='status-msg status-err';document.getElementById('uploadBtn').disabled=false;}}catch(e){sb.textContent='\u2717 Upload failed';sb.className='status-msg status-err';document.getElementById('uploadBtn').disabled=false;}});
x.addEventListener('error',()=>{uploading=false;sb.textContent='\u2717 Connection error';sb.className='status-msg status-err';document.getElementById('uploadBtn').disabled=false;});
x.open('POST','/api/update?approve=1');x.send(fd);}
function checkForUpdate(){const btn=document.getElementById('checkUpdateBtn');const st=document.getElementById('updateStatus');const info=document.getElementById('updateInfo');
btn.disabled=true;st.textContent='Checking...';st.style.color='#888';
fetch('/api/status').then(r=>r.json()).then(d=>{
const currentVer=d.fw_version_base||'';const currentHash=d.fw_git_hash||'';
document.getElementById('currentHashSpan').textContent=currentHash;
fetch('https://api.github.com/repos/noless-zz/esp8266-led-strip-clock/releases/latest',{headers:{'Accept':'application/vnd.github+json'}})
.then(r=>r.json()).then(rel=>{
const tag=rel.tag_name||'';const releaseUrl=rel.html_url||'#';
document.getElementById('latestTag').textContent=tag+' ('+currentVer+' on device)';
document.getElementById('releaseLink').href=releaseUrl;
if(currentHash){document.getElementById('diffLink').href='https://github.com/noless-zz/esp8266-led-strip-clock/compare/'+currentHash+'...'+tag;}
const asset=(rel.assets||[]).find(a=>a.name==='firmware.bin');
if(asset){document.getElementById('downloadLink').href=asset.browser_download_url;_directUrl=asset.browser_download_url;document.getElementById('directFlashBtn').style.display='inline-block';}
else{document.getElementById('downloadLink').style.display='none';document.getElementById('directFlashBtn').style.display='none';}
info.style.display='block';
const upToDate=tag==='v'+currentVer.split('+')[0];
if(upToDate){st.textContent='\u2713 Up to date ('+tag+')';st.style.color='#3a3';}
else{st.textContent='\u25b2 Update available: '+tag;st.style.color='#c80';}
btn.disabled=false;
}).catch(e=>{st.textContent='GitHub error: '+e;st.style.color='#c33';btn.disabled=false;});
}).catch(e=>{st.textContent='Device error: '+e;st.style.color='#c33';btn.disabled=false;});}
function directFlash(){
if(!_directUrl){alert('No firmware URL — run Check for Update first.');return;}
if(!confirm('Flash firmware directly to device?\nThe device will download and install it, then reboot.'))return;
const btn=document.getElementById('directFlashBtn');
const st=document.getElementById('directFlashStatus');
btn.disabled=true;st.style.display='block';st.style.color='#888';st.textContent='Sending URL to device...';
fetch('/api/update/direct?url='+encodeURIComponent(_directUrl))
.then(r=>r.json()).then(d=>{
if(!d.ok){st.textContent='\u2717 '+d.error;st.style.color='#c33';btn.disabled=false;return;}
st.textContent='Downloading firmware \u2026 device will reboot automatically.';
let polls=0,gone=false;
const iv=setInterval(()=>{
polls++;
fetch('/api/status',{signal:AbortSignal.timeout?AbortSignal.timeout(2500):undefined}).then(r=>r.json()).then(s=>{
if(s.direct_update_error){clearInterval(iv);st.textContent='\u2717 Update failed: '+s.direct_update_error;st.style.color='#c33';btn.disabled=false;}
else if(polls>50){clearInterval(iv);st.textContent='Timeout \u2014 check device manually.';st.style.color='#c33';btn.disabled=false;}
}).catch(()=>{
if(!gone){gone=true;st.textContent='Device rebooting\u2026';clearInterval(iv);
setTimeout(()=>{st.textContent='Waiting for device to come back\u2026';
let wait=0;const iv2=setInterval(()=>{wait++;
fetch('/api/status').then(r=>r.json()).then(()=>{clearInterval(iv2);st.textContent='\u2713 Update complete!';st.style.color='#3a3';setTimeout(()=>location.reload(),1500);})
.catch(()=>{if(wait>30){clearInterval(iv2);st.textContent='Device not responding \u2014 check manually.';st.style.color='#c33';btn.disabled=false;}});
},2000);},3000);}
});
},2000);
}).catch(e=>{st.textContent='\u2717 '+e;st.style.color='#c33';btn.disabled=false;});}
var _scanData=[];
function networkSelected(){const list=document.getElementById('ssidList');const ssid=list.value;if(!ssid)return;document.getElementById('wifiSsid').value=ssid;const net=_scanData.find(n=>n.ssid===ssid);const ol=document.getElementById('openLabel');if(net&&!net.enc){document.getElementById('wifiPass').value='';ol.textContent='open network';ol.style.color='#4a4';}else{ol.textContent='';}}
function scanWifi(attempt=0){const list=document.getElementById('ssidList');const sb=document.getElementById('scanBtn');const ss=document.getElementById('scanStatus');
if(attempt===0){ss.textContent='scanning...';sb.disabled=true;list.innerHTML='<option>Scanning...</option>';}
fetch('/api/wifi/scan').then(r=>r.json()).then(d=>{if(d.scanning){if(attempt>25){ss.textContent='timeout';sb.disabled=false;return;}setTimeout(()=>scanWifi(attempt+1),350);return;}
_scanData=d.networks||[];list.innerHTML='';if(!_scanData.length){list.innerHTML='<option>No networks found</option>';ss.textContent='none';sb.disabled=false;return;}
_scanData.forEach(n=>{const o=document.createElement('option');o.value=n.ssid;o.textContent=(n.enc?'\uD83D\uDD12 ':'\uD83D\uDD13 ')+n.ssid+' ('+n.rssi+' dBm)';list.appendChild(o);});
ss.textContent='found '+_scanData.length;sb.disabled=false;}).catch(e=>{list.innerHTML='<option>Scan failed</option>';ss.textContent='error';sb.disabled=false;});}
function pollConnect(s){const msg=document.getElementById('wifiMsg');fetch('/api/wifi/connect').then(r=>r.json()).then(d=>{
if(d.connected){msg.textContent='\u2713 Connected to "'+s+'"! IP: '+d.ip;msg.className='status-msg status-ok';}
else if(d.connecting){msg.textContent='Connecting to "'+s+'"...';setTimeout(()=>pollConnect(s),800);}
else{msg.textContent='\u2717 '+(d.error||'Connection failed');msg.className='status-msg status-err';}}).catch(e=>{msg.textContent='Error: '+e;msg.className='status-msg status-err';});}
function connectWifi(){const s=document.getElementById('wifiSsid').value.trim();const p=document.getElementById('wifiPass').value;const msg=document.getElementById('wifiMsg');
if(!s){msg.textContent='Enter or select a network name';msg.className='status-msg status-err';return;}
msg.textContent='Connecting to "'+s+'"'+(p?'':' (open)');msg.className='status-msg status-info';
const sp=new URLSearchParams({ssid:s,pass:p});fetch('/api/wifi/connect?'+sp.toString()).then(r=>r.json()).then(d=>{
if(d.connected){msg.textContent='\u2713 Connected! IP: '+d.ip;msg.className='status-msg status-ok';}
else if(d.connecting){setTimeout(()=>pollConnect(s),800);}
else{msg.textContent='\u2717 '+(d.error||'Connection failed');msg.className='status-msg status-err';}}).catch(e=>{msg.textContent='Error: '+e;msg.className='status-msg status-err';});}
function syncNTP(){const msg=document.getElementById('tzMsg');msg.textContent='Syncing...';msg.className='status-msg status-info';
fetch('/api/ntp').then(r=>r.text()).then(t=>{msg.textContent='\u2713 Syncing with NTP server...';msg.className='status-msg status-ok';}).catch(e=>{msg.textContent='\u2717 Error: '+e;msg.className='status-msg status-err';});}
function saveTimezone(){const m=document.querySelector('input[name="tzmode"]:checked').value;const o=m==='manual'?document.getElementById('tzOffset').value:'0';
const b=document.getElementById('tzMsg');b.textContent='Saving...';b.className='status-msg status-info';
fetch('/api/timezone?mode='+m+'&offset='+o).then(r=>r.text()).then(t=>{b.textContent='\u2713 Saved!';b.className='status-msg status-ok';}).catch(e=>{b.textContent='\u2717 Error: '+e;b.className='status-msg status-err';});}
function saveBrightness(){const v=document.getElementById('brightness').value;fetch('/api/brightness?value='+Math.round(v/255*100)).catch(e=>console.warn(e));}
function updateAutoBrightLabels(){
  document.getElementById('abDimHourLabel').textContent=document.getElementById('abDimHour').value;
  document.getElementById('abPeakHourLabel').textContent=document.getElementById('abPeakHour').value;
  document.getElementById('abDimPctLabel').textContent=document.getElementById('abDimPct').value;
  document.getElementById('abPeakPctLabel').textContent=document.getElementById('abPeakPct').value;
}
function saveAutoBright(){
  const en=document.getElementById('autoBrightToggle').checked?1:0;
  const dp=document.getElementById('abDimPct').value;
  const pp=document.getElementById('abPeakPct').value;
  const dh=document.getElementById('abDimHour').value;
  const ph=document.getElementById('abPeakHour').value;
  fetch('/api/autobright?enabled='+en+'&dim_pct='+dp+'&peak_pct='+pp+'&dim_hour='+dh+'&peak_hour='+ph)
    .then(r=>r.json()).then(d=>{
      document.getElementById('abEffectivePct').textContent=d.effective_pct;
      applyAutoBrightUI(en==1);
    }).catch(e=>console.warn(e));
}
function applyAutoBrightUI(enabled){
  document.getElementById('autoBrightSlider').style.background=enabled?'#4CAF50':'#ccc';
  document.getElementById('manualBrightGroup').style.opacity=enabled?'0.4':'1';
  document.getElementById('manualBrightGroup').style.pointerEvents=enabled?'none':'auto';
  document.getElementById('autoBrightNote').style.display=enabled?'block':'none';
}
function saveLedType(){const v=document.getElementById('rgbwToggle').checked?1:0;fetch('/api/ledtype?rgbw='+v).then(r=>r.json()).then(d=>{document.getElementById('rgbwSlider').style.background=d.rgbw?'#4CAF50':'#ccc';document.getElementById('ledTypeLabel').textContent=d.rgbw?'RGBW':'RGB';}).catch(e=>console.warn(e));}
function saveLedDirection(){const v=document.getElementById('revToggle').checked?1:0;fetch('/api/leddirection?reversed='+v).then(r=>r.json()).then(d=>{document.getElementById('revSlider').style.background=d.reversed?'#4CAF50':'#ccc';document.getElementById('ledDirLabel').textContent=d.reversed?'Reversed':'Normal';}).catch(e=>console.warn(e));}
function saveDebugConfig(){const ip=document.getElementById('dbgIp').value.trim();const port=document.getElementById('dbgPort').value||'7878';const en=document.getElementById('dbgToggle').checked?1:0;const msg=document.getElementById('dbgMsg');msg.textContent='Saving...';msg.className='status-msg status-info';fetch('/api/debug?enabled='+en+'&ip='+encodeURIComponent(ip)+'&port='+port).then(r=>r.json()).then(d=>{document.getElementById('dbgSlider').style.background=d.enabled?'#4CAF50':'#ccc';document.getElementById('dbgEnabledLabel').textContent=d.enabled?'Enabled':'Disabled';msg.textContent='\u2713 Saved';msg.className='status-msg status-ok';}).catch(e=>{msg.textContent='\u2717 '+e;msg.className='status-msg status-err';});}
function sendDebugTest(){const msg=document.getElementById('dbgMsg');msg.textContent='Sending...';msg.className='status-msg status-info';fetch('/api/debug?test=1').then(r=>r.json()).then(d=>{msg.textContent='\u2713 Test packet sent';msg.className='status-msg status-ok';}).catch(e=>{msg.textContent='\u2717 '+e;msg.className='status-msg status-err';});}
function rgbToHex(r,g,b){return'#'+[r,g,b].map(v=>{const h=Number(v).toString(16);return h.length===1?'0'+h:h;}).join('');}
function hexToRgb(hex){const v=(hex||'#000000').replace('#','');if(v.length!==6)return{r:0,g:0,b:0};return{r:parseInt(v.substring(0,2),16),g:parseInt(v.substring(2,4),16),b:parseInt(v.substring(4,6),16)};}
function updateWidthLabels(){document.getElementById('hourWidthLabel').textContent=document.getElementById('hourWidth').value;
document.getElementById('minuteWidthLabel').textContent=document.getElementById('minuteWidth').value;
document.getElementById('secondWidthLabel').textContent=document.getElementById('secondWidth').value;}
function applyModeCfgToControls(c){if(!c)return;
document.getElementById('hourColor').value=rgbToHex(c.hour.r,c.hour.g,c.hour.b);
document.getElementById('minuteColor').value=rgbToHex(c.minute.r,c.minute.g,c.minute.b);
document.getElementById('secondColor').value=rgbToHex(c.second.r,c.second.g,c.second.b);
document.getElementById('hourWidth').value=c.width.hour;
document.getElementById('minuteWidth').value=c.width.minute;
document.getElementById('secondWidth').value=c.width.second;
document.getElementById('spectrum').value=c.spectrum;updateWidthLabels();}
function loadModeConfig(){const m=document.getElementById('displayMode').value;
fetch('/api/mode/config?mode='+m).then(r=>r.json()).then(d=>{if(d&&d.ok)applyModeCfgToControls(d);}).catch(e=>console.warn(e));}
function buildModeCfgQuery(persist){const m=document.getElementById('displayMode').value;
const h=hexToRgb(document.getElementById('hourColor').value);
const mn=hexToRgb(document.getElementById('minuteColor').value);
const s=hexToRgb(document.getElementById('secondColor').value);
const hw=document.getElementById('hourWidth').value;
const mw=document.getElementById('minuteWidth').value;
const sw=document.getElementById('secondWidth').value;
const sp=document.getElementById('spectrum').value;
return `/api/mode/config?set=1&persist=${persist?1:0}&mode=${m}&hr=${h.r}&hg=${h.g}&hb=${h.b}&mr=${mn.r}&mg=${mn.g}&mb=${mn.b}&sr=${s.r}&sg=${s.g}&sb=${s.b}&hw=${hw}&mw=${mw}&sw=${sw}&sp=${sp}`;}
function queueModeConfigSave(){updateWidthLabels();
if(modeCfgSaveTimer)clearTimeout(modeCfgSaveTimer);
modeCfgSaveTimer=setTimeout(()=>saveModeConfig(true,false),100);
if(modeCfgPersistTimer)clearTimeout(modeCfgPersistTimer);
modeCfgPersistTimer=setTimeout(()=>saveModeConfig(true,true),1200);
}
function saveModeConfig(silent=false,persist=true){
const msg=document.getElementById('modeCfgMsg');
if(!silent){msg.textContent=persist?'Saving mode visuals...':'Applying mode visuals...';msg.className='status-msg status-info';}
const q=buildModeCfgQuery(persist);
fetch(q).then(r=>r.json()).then(d=>{if(d&&d.ok){if(!silent){msg.textContent='\u2713 Mode visuals saved';msg.className='status-msg status-ok';}applyModeCfgToControls(d);}else{msg.textContent='\u2717 '+((d&&d.error)||'Failed');msg.className='status-msg status-err';}})
.catch(e=>{msg.textContent='\u2717 '+e;msg.className='status-msg status-err';});}
function resetModeConfig(){const m=document.getElementById('displayMode').value;
const msg=document.getElementById('modeCfgMsg');
msg.textContent='Resetting mode visuals...';msg.className='status-msg status-info';
fetch('/api/mode/config?reset=1&persist=1&mode='+m).then(r=>r.json()).then(d=>{
if(d&&d.ok){msg.textContent='\u2713 Mode visuals reset to defaults';msg.className='status-msg status-ok';applyModeCfgToControls(d);}else{msg.textContent='\u2717 '+((d&&d.error)||'Failed');msg.className='status-msg status-err';}
}).catch(e=>{msg.textContent='\u2717 '+e;msg.className='status-msg status-err';});}
function updateFadeMsLabel(){const v=document.getElementById('fadeMsSlider').value;document.getElementById('fadeMsLabel').textContent=v;}
function saveFadeMs(){const ms=document.getElementById('fadeMsSlider').value;fetch('/api/simple/fade?ms='+ms).catch(e=>console.warn(e));}
function saveDisplayMode(){const m=document.getElementById('displayMode').value;fetch('/api/display?mode='+m).catch(e=>console.warn(e));updateModeDescription();loadModeConfig();}
function updateModeDescription(){const m=parseInt(document.getElementById('displayMode').value);
const desc={0:'Rainbow orbit background with clear red hour, green minute, and blue second markers (5-3-7 LED spread).',
1:'Minimal clean mode: exactly 3 LEDs each for red hour, green minute, blue second. No background.',
2:'Very subtle background pulse (reduced from before) with strong HMS markers for easy readability.',
3:'Binary clock stretched across all 60 LEDs: 20 groups Ã-- 3 LEDs showing hour/minute/second bits in color.',
4:'Minute fills like a progress bar, hour shown as bright beacon, second has moving trail.',
5:'Optimized warm flame effect (20fps update) with clear HMS markers. Performance improved.',
6:'Soft pastel colors: pink hour, mint green minute, sky blue second. Gentle and easy on eyes.',
7:'Bright neon colors: magenta hour, cyan minute, yellow second. Vivid and energetic.',
8:'Animated comet trails: red hour (7 LED), green minute (5 LED), blue second (10 LED fast tail).'};
document.getElementById('modeDesc').textContent=desc[m]||'Mode '+m;}
function pollStatus(){fetch('/api/status').then(r=>r.json()).then(d=>{
document.getElementById('fwVersion').textContent=d.fw_version_base||'-';
document.getElementById('fwBuildTime').textContent='('+d.fw_build_time+')';
if(d.fw_git_hash){var h=document.getElementById('fwGitHash');if(h)h.textContent='git:'+d.fw_git_hash;}
document.getElementById('deviceIp').textContent=d.ip||'-';
document.getElementById('wifiMode').textContent=d.wifi_connected?'STA (Connected)':'AP (Hotspot)';
document.getElementById('signal').textContent=d.wifi_connected?(d.wifi_rssi+' dBm'):'(AP mode)';
document.getElementById('tzDisplay').textContent=d.timezone||'UTC';
document.getElementById('tzMode').textContent=(d.timezone_auto_detected?'auto':'manual');
document.getElementById('tzOffset').textContent=d.timezone_utc_offset_hours;
document.getElementById('tzOffsetSec').textContent=d.timezone_utc_offset_seconds;
document.getElementById('tzAuto').textContent=d.timezone_auto_detected?'yes':'no';
document.getElementById('tzDetectStatus').textContent=d.tz_detect_status||'-';
document.getElementById('tzDetectMsg').textContent=d.tz_detect_message||'-';
document.getElementById('tzDebug').style.display='block';
if(d.display_mode!==undefined){
const current=document.getElementById('displayMode').value;
document.getElementById('displayMode').value=d.display_mode;
updateModeDescription();
if(String(current)!==String(d.display_mode)){loadModeConfig();}
}
if(d.display_cfg){applyModeCfgToControls(d.display_cfg);}
if(d.led_rgbw!==undefined){const isRgbw=d.led_rgbw===true;document.getElementById('rgbwToggle').checked=isRgbw;document.getElementById('rgbwSlider').style.background=isRgbw?'#4CAF50':'#ccc';document.getElementById('ledTypeLabel').textContent=isRgbw?'RGBW':'RGB';}
if(d.led_reversed!==undefined){const isRev=d.led_reversed===true;document.getElementById('revToggle').checked=isRev;document.getElementById('revSlider').style.background=isRev?'#4CAF50':'#ccc';document.getElementById('ledDirLabel').textContent=isRev?'Reversed':'Normal';}
if(d.debug_enabled!==undefined){const en=d.debug_enabled===true;document.getElementById('dbgToggle').checked=en;document.getElementById('dbgSlider').style.background=en?'#4CAF50':'#ccc';document.getElementById('dbgEnabledLabel').textContent=en?'Enabled':'Disabled';}
if(d.debug_ip){document.getElementById('dbgIp').value=d.debug_ip;}
if(d.debug_port){document.getElementById('dbgPort').value=d.debug_port;}
if(d.simple_fade_ms!==undefined){document.getElementById('fadeMsSlider').value=d.simple_fade_ms;updateFadeMsLabel();}
if(d.auto_bright_enabled!==undefined){
  const en=d.auto_bright_enabled===true;
  document.getElementById('autoBrightToggle').checked=en;
  applyAutoBrightUI(en);
}
if(d.auto_bright_dim_pct!==undefined){document.getElementById('abDimPct').value=d.auto_bright_dim_pct;document.getElementById('abDimPctLabel').textContent=d.auto_bright_dim_pct;}
if(d.auto_bright_peak_pct!==undefined){document.getElementById('abPeakPct').value=d.auto_bright_peak_pct;document.getElementById('abPeakPctLabel').textContent=d.auto_bright_peak_pct;}
if(d.auto_bright_dim_hour!==undefined){document.getElementById('abDimHour').value=d.auto_bright_dim_hour;document.getElementById('abDimHourLabel').textContent=d.auto_bright_dim_hour;}
if(d.auto_bright_peak_hour!==undefined){document.getElementById('abPeakHour').value=d.auto_bright_peak_hour;document.getElementById('abPeakHourLabel').textContent=d.auto_bright_peak_hour;}
if(d.effective_brightness!==undefined){document.getElementById('abEffectivePct').textContent=d.effective_brightness;document.getElementById('effectiveBrPct').textContent=d.effective_brightness;}
}).catch(e=>console.warn(e));}
function getMaxSize(){fetch('/api/status').then(r=>r.json()).then(d=>{document.getElementById('maxSize').textContent=(d.heap||262144).toString();}).catch(e=>console.warn(e));}
document.getElementById('hourWidth').addEventListener('input',updateWidthLabels);
document.getElementById('minuteWidth').addEventListener('input',updateWidthLabels);
document.getElementById('secondWidth').addEventListener('input',updateWidthLabels);
document.getElementById('hourColor').addEventListener('input',queueModeConfigSave);
document.getElementById('minuteColor').addEventListener('input',queueModeConfigSave);
document.getElementById('secondColor').addEventListener('input',queueModeConfigSave);
document.getElementById('hourWidth').addEventListener('input',queueModeConfigSave);
document.getElementById('minuteWidth').addEventListener('input',queueModeConfigSave);
document.getElementById('secondWidth').addEventListener('input',queueModeConfigSave);
document.getElementById('spectrum').addEventListener('change',queueModeConfigSave);
document.getElementById('fadeMsSlider').addEventListener('input',updateFadeMsLabel);
document.getElementById('fadeMsSlider').addEventListener('change',saveFadeMs);
['abDimHour','abPeakHour','abDimPct','abPeakPct'].forEach(id=>{
  document.getElementById(id).addEventListener('input',updateAutoBrightLabels);
  document.getElementById(id).addEventListener('change',saveAutoBright);
});
document.getElementById('hourColor').addEventListener('change',()=>saveModeConfig(true,true));
document.getElementById('minuteColor').addEventListener('change',()=>saveModeConfig(true,true));
document.getElementById('secondColor').addEventListener('change',()=>saveModeConfig(true,true));
document.getElementById('hourWidth').addEventListener('change',()=>saveModeConfig(true,true));
document.getElementById('minuteWidth').addEventListener('change',()=>saveModeConfig(true,true));
document.getElementById('secondWidth').addEventListener('change',()=>saveModeConfig(true,true));
document.getElementById('spectrum').addEventListener('change',()=>saveModeConfig(true,true));
pollStatus();getMaxSize();scanWifi();setInterval(pollStatus,5000);updateModeDescription();loadModeConfig();updateWidthLabels();
</script></body></html>
)html";

// ============================================================================
// Web Server Setup
// ============================================================================

void setupWebServer() {
  // Main page - just clock & status
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *req) { 
    req->send_P(200, "text/html", INDEX_HTML); 
  });
  
  // Settings page
  server.on("/settings.html", HTTP_GET, [](AsyncWebServerRequest *req) { 
    req->send_P(200, "text/html", SETTINGS_HTML); 
  });
  
  // API: Status
  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *req) {
    DynamicJsonDocument doc(1408);
    time_t now = time(nullptr);
    struct tm* t = localtime(&now);
    String fullVersion = String(FW_VERSION_BASE) + " (" + FW_BUILD_TIME + ")";
    doc["fw_version"] = fullVersion;
    doc["fw_version_base"] = FW_VERSION_BASE;
    doc["fw_build_time"] = FW_BUILD_TIME;
    doc["fw_git_hash"] = FW_GIT_HASH;
    doc["time_hour"] = t->tm_hour;
    doc["time_minute"] = t->tm_min;
    doc["time_second"] = t->tm_sec;
    doc["ntp_synced"] = (now > 86400);
    doc["wifi_connected"] = wifiConnected;
    doc["wifi_ssid"] = WiFi.SSID();
    doc["wifi_rssi"] = wifiConnected ? WiFi.RSSI() : 0;
    doc["timezone"] = tz.name;
    doc["timezone_auto_detected"] = tz.autoDetected;
    doc["timezone_utc_offset_hours"] = tz.utcOffset / 3600;
    doc["timezone_utc_offset_seconds"] = tz.utcOffset;
    doc["tz_detect_source"] = tzDiag.source;
    doc["tz_detect_status"] = tzDiag.status;
    doc["tz_detect_message"] = tzDiag.message;
    doc["tz_detect_http_code"] = tzDiag.httpCode;
    doc["tz_detect_last_attempt_ms"] = tzDiag.lastAttemptMs;
    doc["tz_detect_last_success_ms"] = tzDiag.lastSuccessMs;
    doc["tz_detect_response_sample"] = tzDiag.responseSample;
    doc["brightness"] = (ledBrightness * 100) / 255;
    doc["auto_bright_enabled"]  = autoBrightEnabled;
    doc["auto_bright_dim_pct"]  = (autoBrightDimVal  * 100) / 255;
    doc["auto_bright_peak_pct"] = (autoBrightPeakVal * 100) / 255;
    doc["auto_bright_dim_hour"]  = autoBrightDimHour;
    doc["auto_bright_peak_hour"] = autoBrightPeakHour;
    doc["effective_brightness"]  = (autoBrightEnabled ? computeAutoBrightness() : ledBrightness) * 100 / 255;
    doc["led_rgbw"] = ledRgbw;
    doc["led_reversed"] = ledReversed;
    doc["display_mode"] = (int)displayMode;
    doc["boot_stage"] = (int)bootStage;
    bool _ntpSynced = (now >= 86400);
    unsigned long _ntpInterval = _ntpSynced ? 3600UL : 20UL;
    unsigned long _ntpAge = min((millis() - lastNtpSync) / 1000, _ntpInterval);
    int _ntpPct = (int)((_ntpAge * 100) / _ntpInterval);
    const char* stageNames[] = {"Booting","AP Ready","Scanning","Connecting","WiFi OK","NTP Wait","Running"};
    if (bootStage == BOOT_STAGE_RUNNING) {
      char stageBuf[48];
      snprintf(stageBuf, sizeof(stageBuf), "Running (NTP %s, refresh %d%%)",
               _ntpSynced ? "OK" : "fail", _ntpPct);
      doc["boot_stage_name"] = stageBuf;
    } else {
      doc["boot_stage_name"] = bootStage <= BOOT_STAGE_RUNNING ? stageNames[(int)bootStage] : "Unknown";
    }
    bool clockShowing = ((now >= 86400) || forceClockDisplay) && !forceStatusDisplay;
    doc["ring_mode"] = clockShowing ? "clock" : "status";
    doc["ring_force_clock"] = forceClockDisplay;
    doc["ring_force_status"] = forceStatusDisplay;
    const ModeDisplayConfig &cfg = modeConfigs[(int)displayMode];
    doc["display_cfg"]["hour"]["r"] = cfg.hourR;
    doc["display_cfg"]["hour"]["g"] = cfg.hourG;
    doc["display_cfg"]["hour"]["b"] = cfg.hourB;
    doc["display_cfg"]["minute"]["r"] = cfg.minuteR;
    doc["display_cfg"]["minute"]["g"] = cfg.minuteG;
    doc["display_cfg"]["minute"]["b"] = cfg.minuteB;
    doc["display_cfg"]["second"]["r"] = cfg.secondR;
    doc["display_cfg"]["second"]["g"] = cfg.secondG;
    doc["display_cfg"]["second"]["b"] = cfg.secondB;
    doc["display_cfg"]["width"]["hour"] = cfg.hourWidth;
    doc["display_cfg"]["width"]["minute"] = cfg.minuteWidth;
    doc["display_cfg"]["width"]["second"] = cfg.secondWidth;
    doc["display_cfg"]["spectrum"] = cfg.spectrum;
    doc["ip"] = wifiConnected ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
    doc["heap"] = ESP.getFreeHeap();
    doc["heap_frag"] = ESP.getHeapFragmentation();
    doc["debug_enabled"] = debugRemoteEnabled;
    doc["debug_ip"] = debugServerIp;
    doc["debug_port"] = debugServerPort;
    doc["simple_fade_ms"] = simpleFadeMs;
    doc["direct_update_pending"] = (pendingDirectUpdateUrl.length() > 0);
    if (lastDirectUpdateError.length() > 0) doc["direct_update_error"] = lastDirectUpdateError;
    String json;
    serializeJson(doc, json);
    req->send(200, "application/json", json);
  });
  
  // API: WiFi Scan
  server.on("/api/wifi/scan", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send(200, "application/json", getWifiScanJson());
  });
  
  // API: WiFi Connect
  server.on("/api/wifi/connect", HTTP_GET, [](AsyncWebServerRequest *req) {
    DynamicJsonDocument doc(256);
    
    if (req->hasParam("ssid")) {
      String ssid = req->getParam("ssid")->value();
      String pass = req->hasParam("pass") ? req->getParam("pass")->value() : "";
      
      if (startWiFiConnect(ssid, pass, true)) {
        doc["connecting"] = true;
      } else {
        doc["connecting"] = false;
        doc["error"] = "Connection already in progress or invalid SSID";
      }
    } else {
      updateWiFiConnect();
      if (wifiConnect.active) {
        doc["connecting"] = true;
      } else if (wifiConnect.success) {
        doc["connecting"] = false;
        doc["connected"] = true;
        doc["ip"] = WiFi.localIP().toString();
        wifiConnect.success = false;
      } else {
        doc["connecting"] = false;
        doc["connected"] = wifiConnected;
        if (wifiConnect.error.length() > 0) {
          doc["error"] = wifiConnect.error;
          wifiConnect.error = "";
        }
      }
    }
    
    String json;
    serializeJson(doc, json);
    req->send(200, "application/json", json);
  });
  
  // API: Brightness
  server.on("/api/brightness", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (req->hasParam("value")) {
      int val = atoi(req->getParam("value")->value().c_str());
      ledBrightness = (val * 255) / 100;
      if (ledStrip) ledStrip->setBrightness(ledBrightness);
    }
    req->send(200, "text/plain", "OK");
  });

  // API: Adaptive brightness settings
  server.on("/api/autobright", HTTP_GET, [](AsyncWebServerRequest *req) {
    bool anyParam = false;
    if (req->hasParam("enabled"))   { autoBrightEnabled  = req->getParam("enabled")->value() == "1"; anyParam = true; }
    if (req->hasParam("dim_pct"))   { int v = atoi(req->getParam("dim_pct")->value().c_str());  if (v >= 0 && v <= 100) { autoBrightDimVal  = (uint8_t)((v * 255) / 100); anyParam = true; } }
    if (req->hasParam("peak_pct"))  { int v = atoi(req->getParam("peak_pct")->value().c_str()); if (v >= 0 && v <= 100) { autoBrightPeakVal = (uint8_t)((v * 255) / 100); anyParam = true; } }
    if (req->hasParam("dim_hour"))  { int v = atoi(req->getParam("dim_hour")->value().c_str());  if (v >= 0 && v < 24 && v != (int)autoBrightPeakHour) { autoBrightDimHour  = (uint8_t)v; anyParam = true; } }
    if (req->hasParam("peak_hour")) { int v = atoi(req->getParam("peak_hour")->value().c_str()); if (v >= 0 && v < 24 && v != (int)autoBrightDimHour)  { autoBrightPeakHour = (uint8_t)v; anyParam = true; } }
    if (anyParam) saveAutoBrightSettings();
    uint8_t effectiveBr = autoBrightEnabled ? computeAutoBrightness() : ledBrightness;
    String json = String("{\"enabled\":") + (autoBrightEnabled ? "true" : "false") +
      ",\"dim_pct\":"   + (int)(autoBrightDimVal  * 100 / 255) +
      ",\"peak_pct\":"  + (int)(autoBrightPeakVal * 100 / 255) +
      ",\"dim_hour\":"  + autoBrightDimHour +
      ",\"peak_hour\":" + autoBrightPeakHour +
      ",\"effective_pct\":" + (int)(effectiveBr * 100 / 255) + "}";
    req->send(200, "application/json", json);
  });

  // API: LED type (RGB / RGBW)
  server.on("/api/ledtype", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (req->hasParam("rgbw")) {
      bool newRgbw = (req->getParam("rgbw")->value() == "1");
      if (newRgbw != ledRgbw) {
        ledRgbw = newRgbw;
        EEPROM.begin(EEPROM_SIZE);
        EEPROM.write(EEPROM_RGBW_ADDR, ledRgbw ? 0x01 : 0x00);
        EEPROM.commit();
        setupLEDs();  // reinit strip with correct pixel type
        Serial.printf("[LED] Switched to %s mode\n", ledRgbw ? "RGBW" : "RGB");
      }
    }
    String json = String("{\"rgbw\":") + (ledRgbw ? "true" : "false") + "}";
    req->send(200, "application/json", json);
  });
  
  // API: LED direction
  server.on("/api/leddirection", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (req->hasParam("reversed")) {
      bool newReversed = (req->getParam("reversed")->value() == "1");
      if (newReversed != ledReversed) {
        ledReversed = newReversed;
        EEPROM.begin(EEPROM_SIZE);
        EEPROM.write(EEPROM_REVERSED_ADDR, ledReversed ? 0x01 : 0x00);
        EEPROM.commit();
        Serial.printf("[LED] Direction: %s\n", ledReversed ? "reversed" : "normal");
      }
    }
    String json = String("{\"reversed\":") + (ledReversed ? "true" : "false") + "}";
    req->send(200, "application/json", json);
  });


  // API: Remote debug logging config
  server.on("/api/debug", HTTP_GET, [](AsyncWebServerRequest *req) {
    bool changed = false;
    if (req->hasParam("enabled")) {
      debugRemoteEnabled = (req->getParam("enabled")->value() == "1");
      changed = true;
    }
    if (req->hasParam("ip")) {
      String newIp = req->getParam("ip")->value();
      newIp.trim();
      if (newIp.length() <= 15) { debugServerIp = newIp; changed = true; }
    }
    if (req->hasParam("port")) {
      int p = atoi(req->getParam("port")->value().c_str());
      if (p > 0 && p < 65536) { debugServerPort = (uint16_t)p; changed = true; }
    }
    if (changed) saveDebugConfig();
    if (req->hasParam("test")) {
      DLOGI("TEST", "Debug test from web UI  heap=%u  uptime=%lus", ESP.getFreeHeap(), millis()/1000);
    }
    String json = String("{\"enabled\":") + (debugRemoteEnabled ? "true" : "false") +
                  ",\"ip\":\"" + debugServerIp + "\",\"port\":" + debugServerPort + "}";
    req->send(200, "application/json", json);
  });

  // API: Simple mode fade duration  (?ms=400 to set, no params to read)
  server.on("/api/simple/fade", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (req->hasParam("ms")) {
      int ms = atoi(req->getParam("ms")->value().c_str());
      if (ms >= 0 && ms <= 2000) {
        simpleFadeMs = (uint32_t)ms;
        saveFadeMs();
      }
    }
    String json = String("{\"fade_ms\":") + simpleFadeMs + "}";
    req->send(200, "application/json", json);
  });

  // API: Ring control --" force_clock=1 â†' show clock, force_clock=0 â†' show status animation
  server.on("/api/ring", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (req->hasParam("force_clock")) {
      bool wantClock = (req->getParam("force_clock")->value() == "1");
      forceClockDisplay  = wantClock;
      forceStatusDisplay = !wantClock;
    }
    bool ntpSynced = (time(nullptr) >= 86400);
    bool clockShowing = (ntpSynced || forceClockDisplay) && !forceStatusDisplay;
    const char* stageNames[] = {"Booting","AP Ready","Scanning","Connecting","WiFi OK","NTP Wait","Running"};
    const char* stageName = bootStage <= BOOT_STAGE_RUNNING ? stageNames[(int)bootStage] : "Unknown";
    String json = String("{\"ring_mode\":\"") + (clockShowing ? "clock" : "status") +
                  "\",\"boot_stage\":" + (int)bootStage +
                  ",\"boot_stage_name\":\"" + stageName +
                  "\",\"force_clock\":" + (forceClockDisplay ? "true" : "false") +
                  ",\"force_status\":" + (forceStatusDisplay ? "true" : "false") + "}";
    req->send(200, "application/json", json);
  });

  // API: Display Mode
  server.on("/api/display", HTTP_GET, [](AsyncWebServerRequest *req) {
    DynamicJsonDocument doc(512);
    doc["current_mode"] = displayMode;
    doc["available_modes"]["SOLID"] = DISPLAY_SOLID;
    doc["available_modes"]["SIMPLE"] = DISPLAY_SIMPLE;
    doc["available_modes"]["PULSE"] = DISPLAY_PULSE;
    doc["available_modes"]["BINARY"] = DISPLAY_BINARY;
    doc["available_modes"]["HOUR_MARKER"] = DISPLAY_HOUR_MARKER;
    doc["available_modes"]["FLAME"] = DISPLAY_FLAME;
    doc["available_modes"]["PASTEL"] = DISPLAY_PASTEL;
    doc["available_modes"]["NEON"] = DISPLAY_NEON;
    doc["available_modes"]["COMET"] = DISPLAY_COMET;
    
    if (req->hasParam("mode")) {
      int newMode = atoi(req->getParam("mode")->value().c_str());
      if (newMode >= 0 && newMode < DISPLAY_MAX) {
        displayMode = (DisplayMode)newMode;
        saveDisplayModeToEEPROM();
        doc["changed"] = true;
        doc["new_mode"] = displayMode;
      } else {
        doc["error"] = "Invalid mode";
      }
    }

    const ModeDisplayConfig &cfg = modeConfigs[(int)displayMode];
    doc["config"]["hour"]["r"] = cfg.hourR;
    doc["config"]["hour"]["g"] = cfg.hourG;
    doc["config"]["hour"]["b"] = cfg.hourB;
    doc["config"]["minute"]["r"] = cfg.minuteR;
    doc["config"]["minute"]["g"] = cfg.minuteG;
    doc["config"]["minute"]["b"] = cfg.minuteB;
    doc["config"]["second"]["r"] = cfg.secondR;
    doc["config"]["second"]["g"] = cfg.secondG;
    doc["config"]["second"]["b"] = cfg.secondB;
    doc["config"]["width"]["hour"] = cfg.hourWidth;
    doc["config"]["width"]["minute"] = cfg.minuteWidth;
    doc["config"]["width"]["second"] = cfg.secondWidth;
    doc["config"]["spectrum"] = cfg.spectrum;
    
    String response;
    serializeJson(doc, response);
    req->send(200, "application/json", response);
  });

  // API: Display Mode Configuration (per mode, persistent)
  server.on("/api/mode/config", HTTP_GET, [](AsyncWebServerRequest *req) {
    DynamicJsonDocument doc(512);

    int mode = (int)displayMode;
    if (req->hasParam("mode")) {
      mode = atoi(req->getParam("mode")->value().c_str());
    }

    if (mode < 0 || mode >= DISPLAY_MAX) {
      doc["ok"] = false;
      doc["error"] = "Invalid mode";
      String out;
      serializeJson(doc, out);
      req->send(400, "application/json", out);
      return;
    }

    bool shouldPersist = false;

    if (req->hasParam("reset")) {
      modeConfigs[mode] = defaultModeConfigFor((uint8_t)mode);
      shouldPersist = true;
      doc["reset"] = true;
    }

    if (req->hasParam("set")) {
      ModeDisplayConfig cfg = modeConfigs[mode];
      auto parseByte = [&](const char* key, uint8_t currentValue) -> uint8_t {
        if (!req->hasParam(key)) return currentValue;
        int v = atoi(req->getParam(key)->value().c_str());
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        return (uint8_t)v;
      };

      auto parseWidth = [&](const char* key, uint8_t currentValue) -> uint8_t {
        if (!req->hasParam(key)) return currentValue;
        int v = atoi(req->getParam(key)->value().c_str());
        if (v < 1) v = 1;
        if (v > 30) v = 30;
        return (uint8_t)v;
      };

      cfg.hourR = parseByte("hr", cfg.hourR);
      cfg.hourG = parseByte("hg", cfg.hourG);
      cfg.hourB = parseByte("hb", cfg.hourB);
      cfg.minuteR = parseByte("mr", cfg.minuteR);
      cfg.minuteG = parseByte("mg", cfg.minuteG);
      cfg.minuteB = parseByte("mb", cfg.minuteB);
      cfg.secondR = parseByte("sr", cfg.secondR);
      cfg.secondG = parseByte("sg", cfg.secondG);
      cfg.secondB = parseByte("sb", cfg.secondB);
      cfg.hourWidth = parseWidth("hw", cfg.hourWidth);
      cfg.minuteWidth = parseWidth("mw", cfg.minuteWidth);
      cfg.secondWidth = parseWidth("sw", cfg.secondWidth);
      cfg.spectrum = (uint8_t)min(2, max(0, req->hasParam("sp") ? atoi(req->getParam("sp")->value().c_str()) : (int)cfg.spectrum));

      if (!isModeConfigValid(cfg)) {
        doc["ok"] = false;
        doc["error"] = "Invalid config values";
        String out;
        serializeJson(doc, out);
        req->send(400, "application/json", out);
        return;
      }

      modeConfigs[mode] = cfg;
      int persistFlag = req->hasParam("persist") ? atoi(req->getParam("persist")->value().c_str()) : 1;
      if (persistFlag != 0) shouldPersist = true;
      doc["saved"] = true;
      doc["persisted"] = (persistFlag != 0);
    }

    if (shouldPersist) {
      saveModeConfigToEEPROM((uint8_t)mode);
      doc["persisted"] = true;
    }

    const ModeDisplayConfig &cfg = modeConfigs[mode];
    doc["ok"] = true;
    doc["mode"] = mode;
    doc["hour"]["r"] = cfg.hourR;
    doc["hour"]["g"] = cfg.hourG;
    doc["hour"]["b"] = cfg.hourB;
    doc["minute"]["r"] = cfg.minuteR;
    doc["minute"]["g"] = cfg.minuteG;
    doc["minute"]["b"] = cfg.minuteB;
    doc["second"]["r"] = cfg.secondR;
    doc["second"]["g"] = cfg.secondG;
    doc["second"]["b"] = cfg.secondB;
    doc["width"]["hour"] = cfg.hourWidth;
    doc["width"]["minute"] = cfg.minuteWidth;
    doc["width"]["second"] = cfg.secondWidth;
    doc["spectrum"] = cfg.spectrum;

    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
  });
  
  // API: Timezone
  server.on("/api/timezone", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (req->hasParam("mode")) {
      if (req->getParam("mode")->value() == "auto") {
        tz.autoDetected = true;
        tzDiag.status = "running";
        tzDiag.message = "auto mode requested";
        detectTimezone();
        req->send(200, "text/plain", "Auto-detecting...");
      } else if (req->hasParam("offset")) {
        tz.utcOffset = (int32_t)(atof(req->getParam("offset")->value().c_str()) * 3600);
        tz.autoDetected = false;
        tz.name = "Manual";
        syncTimeNTP();
        req->send(200, "text/plain", "Manual timezone set");
      } else {
        req->send(400, "text/plain", "Missing offset param");
      }
    } else {
      req->send(400, "text/plain", "Missing mode param");
    }
  });
  
  // API: NTP Sync
  server.on("/api/ntp", HTTP_GET, [](AsyncWebServerRequest *req) {
    syncTimeNTP();
    req->send(200, "text/plain", "Syncing...");
  });
  
  // API: OTA Precheck
  server.on("/api/update/precheck", HTTP_GET, [](AsyncWebServerRequest *req) {
    DynamicJsonDocument doc(256);
    if (!req->hasParam("name") || !req->hasParam("size") || !req->hasParam("magic")) {
      doc["ok"] = false;
      doc["error"] = "Missing parameters";
    } else {
      String name = req->getParam("name")->value();
      size_t size = (size_t) req->getParam("size")->value().toInt();
      int magic = req->getParam("magic")->value().toInt();
      uint32_t maxSize = getMaxUpdateSize();
      
      bool extOk = name.endsWith(".bin") || name.endsWith(".BIN");
      bool sizeOk = size > 0 && size <= maxSize;
      bool magicOk = (magic == 0xE9);
      
      doc["ok"] = extOk && sizeOk && magicOk;
      if (doc["ok"].as<bool>()) {
        doc["summary"] = String(name + " (" + String(size) + " bytes)");
      } else {
        String err;
        if (!extOk) err += "Invalid file type (must be .bin). ";
        if (!sizeOk) err += "File too large (max " + String(maxSize) + " bytes). ";
        if (!magicOk) err += "Invalid firmware magic. ";
        doc["error"] = err;
      }
    }
    String json;
    serializeJson(doc, json);
    req->send(200, "application/json", json);
  });
  
  // API: OTA Upload
  server.on("/api/update", HTTP_POST,
    [](AsyncWebServerRequest *req) {
      bool success = !Update.hasError() && Update.isFinished();
      req->send(success ? 200 : 500, "application/json", 
        String("{\"ok\":") + (success ? "true" : "false") + ",\"written\":" + String(Update.progress()) + "}");
      if (success) {
        delay(500);
        ESP.restart();
      }
    },
    [](AsyncWebServerRequest *req, String filename, size_t index, uint8_t *data, size_t len, bool final) {
      if (index == 0) {
        Update.runAsync(true);
        uint32_t maxSize = getMaxUpdateSize();
        if (!Update.begin(maxSize, U_FLASH)) {
          Serial.println("[OTA] Update begin failed");
          DLOGE("OTA", "Web upload begin FAILED  heap=%u", ESP.getFreeHeap());
          return;
        }
        Serial.print("[OTA] Updating: ");
        Serial.println(filename);
        DLOGI("OTA", "Web upload start  file=%s  heap=%u", filename.c_str(), ESP.getFreeHeap());
      }
      
      if (Update.write(data, len) == len) {} else {
        Update.printError(Serial);
      }
      
      if (final) {
        if (Update.end(true)) {
          Serial.println("\n[OTA] Update Success!");
          DLOGI("OTA", "Web upload SUCCESS  written=%u", (unsigned)Update.progress());
        } else {
          Serial.println("\n[OTA] Update Failed!");
          Update.printError(Serial);
          DLOGE("OTA", "Web upload FAILED  err=%u", (unsigned)Update.getError());
        }
      }
    }
  );
  
  // API: Direct self-update — device fetches firmware binary from a URL and flashes itself
  server.on("/api/update/direct", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (!wifiConnected) {
      req->send(200, "application/json", "{\"ok\":false,\"error\":\"No WiFi connection\"}");
      return;
    }
    if (!req->hasParam("url")) {
      req->send(200, "application/json", "{\"ok\":false,\"error\":\"url param required\"}");
      return;
    }
    String url = req->getParam("url")->value();
    if (!url.startsWith("http://") && !url.startsWith("https://")) {
      req->send(200, "application/json", "{\"ok\":false,\"error\":\"invalid url\"}");
      return;
    }
    pendingDirectUpdateUrl = url;
    lastDirectUpdateError  = "";
    req->send(200, "application/json", "{\"ok\":true,\"msg\":\"Download started\"}");
  });

  // Catchall
  server.onNotFound([](AsyncWebServerRequest *req) {
    req->redirect("http://" + WiFi.softAPIP().toString() + "/");
  });
  
  server.begin();
  Serial.println("[Web] Server started @ http://192.168.4.1");
}

void setupWiFi() {
  Serial.println("[WiFi] Starting AP+STA mode...");
  WiFi.mode(WIFI_AP_STA);

  // Log when a device connects/disconnects from our AP
  WiFi.onSoftAPModeStationConnected([](const WiFiEventSoftAPModeStationConnected& e) {
    char mac[18];
    snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
             e.mac[0], e.mac[1], e.mac[2], e.mac[3], e.mac[4], e.mac[5]);
    Serial.printf("[AP] Client connected:    MAC=%s  aid=%d  clients=%d\n",
                  mac, e.aid, WiFi.softAPgetStationNum());
  });
  WiFi.onSoftAPModeStationDisconnected([](const WiFiEventSoftAPModeStationDisconnected& e) {
    char mac[18];
    snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
             e.mac[0], e.mac[1], e.mac[2], e.mac[3], e.mac[4], e.mac[5]);
    Serial.printf("[AP] Client disconnected: MAC=%s  aid=%d  clients=%d\n",
                  mac, e.aid, WiFi.softAPgetStationNum());
  });

  WiFi.softAP(AP_SSID, AP_PASS);
  bootStage = BOOT_STAGE_AP_UP;
  Serial.println("[WiFi] AP: " + String(AP_SSID));
  Serial.println("[WiFi] AP IP: " + WiFi.softAPIP().toString());

}

void setupDNS() {
  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
  Serial.println("[DNS] Captive portal ready");
}

void setupOTA() {
  ArduinoOTA.setPassword(OTA_PASS);
  ArduinoOTA.onStart([]() {
    DLOGI("OTA", "ArduinoOTA start  heap=%u", ESP.getFreeHeap());
  });
  ArduinoOTA.onEnd([]() {
    DLOGI("OTA", "ArduinoOTA end -- rebooting");
  });
  ArduinoOTA.onError([](ota_error_t err) {
    DLOGE("OTA", "ArduinoOTA error %u", (unsigned)err);
  });
  ArduinoOTA.begin();
  Serial.println("[OTA] Ready");
}

// ============================================================================
// Main Setup & Loop
// ============================================================================

void setup() {
  Serial.begin(115200);
  delay(100);

  // Build unique AP SSID from last 3 MAC bytes, e.g. "LED-Clock-A1B2C3"
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char macSuffix[7];
  snprintf(macSuffix, sizeof(macSuffix), "%02X%02X%02X", mac[3], mac[4], mac[5]);
  AP_SSID_STR = String("LED-Clock-") + macSuffix;
  AP_SSID = AP_SSID_STR.c_str();

  Serial.println("\n\n");
  Serial.println("  ESP8266 LED Strip Clock v" + String(FW_VERSION_BASE) + " (" + FW_BUILD_TIME + ")");
  Serial.println("  60-LED NeoPixel Clock with NTP Sync");
  Serial.println("\n");
  
  captureBootInfo();
  setupLEDs();
  loadEEPROMSettings();
  setupWiFi();
  setupDNS();
  setupOTA();
  setupWebServer();
  
  WiFi.scanNetworks(true, true);  // Start first scan
  
  Serial.println("[READY] Access point: " + String(AP_SSID));
  Serial.println("[READY] Open browser: http://192.168.4.1\n");
}

void loop() {
  // Auto-connect: wait for a scan to finish so we have a channel hint, then connect.
  // Also handles retries after timeout (updateWiFiConnect triggers a rescan on timeout).
  if (!wifiConnected && !wifiConnect.active && savedSsid.length() > 0) {
    int scanState = WiFi.scanComplete();
    if (scanState == WIFI_SCAN_RUNNING) {
      if (bootStage < BOOT_STAGE_SCANNING) bootStage = BOOT_STAGE_SCANNING;
    } else if (scanState >= 0) {
      Serial.printf("[WiFi] Scan done (%d networks). Connecting to: %s\n",
                    scanState, savedSsid.c_str());
      getWifiScanJson();  // populate scan cache

      bootStage = BOOT_STAGE_STA_CONN;
      startWiFiConnect(savedSsid, savedPass);  // AP channel switch now done inside startWiFiConnect
    } else if (scanState == WIFI_SCAN_FAILED) {
      Serial.println("[WiFi] Scan failed, connecting without channel hint");
      bootStage = BOOT_STAGE_STA_CONN;
      startWiFiConnect(savedSsid, savedPass);
    }
    // WIFI_SCAN_RUNNING: keep waiting
  }

  dnsServer.processNextRequest();
  if (mdnsStarted) {
    MDNS.update();
  } else if (WiFi.status() == WL_CONNECTED && !mdnsStarted) {
    MDNS.begin("ledclock");
    mdnsStarted = true;
    Serial.println("[mDNS] Available at http://ledclock.local");
  }
  
  // Serial command interface
  // Commands:  wifi <SSID> [password]   --" connect (omit password for open networks)
  //            scan                     --" trigger scan and print results
  //            status                   --" print current WiFi/time status
  //            disconnect               --" disconnect STA
  if (Serial.available()) {
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (line.length() > 0) {
      Serial.println("[CMD] > " + line);
      if (line.equalsIgnoreCase("status")) {
        Serial.printf("[CMD] WiFi: %s  SSID: %s  IP: %s\n",
          wifiConnected ? "connected" : "disconnected",
          WiFi.SSID().c_str(),
          WiFi.localIP().toString().c_str());
        Serial.printf("[CMD] AP: %s  ch=%d\n", AP_SSID, WiFi.channel());
        time_t now = time(nullptr);
        Serial.printf("[CMD] Time: %s  NTP: %s\n",
          ctime(&now), now > 86400 ? "synced" : "waiting");
        const char* modeNames[] = {"SOLID","SIMPLE","PULSE","BINARY","HOUR_MARKER","FLAME","PASTEL","NEON","COMET"};
        Serial.printf("[CMD] Display mode: %d (%s)  bootStage: %d  brightness: %d\n",
          (int)displayMode, displayMode < DISPLAY_MAX ? modeNames[displayMode] : "?",
          (int)bootStage, ledBrightness);
      } else if (line.equalsIgnoreCase("scan")) {
        Serial.println("[CMD] Starting scan...");
        WiFi.scanNetworks(true, true);
      } else if (line.equalsIgnoreCase("disconnect")) {
        WiFi.disconnect(false);
        wifiConnected = false;
        wifiConnect.active = false;
        savedSsid = "";
        savedPass = "";
        Serial.println("[CMD] Disconnected and cleared saved credentials");
      } else if (line.startsWith("wifi ") || line.startsWith("wifi\t")) {
        String args = line.substring(5);
        args.trim();
        int space = args.indexOf(' ');
        String ssid, pass;
        if (space < 0) {
          ssid = args;
          pass = "";
        } else {
          ssid = args.substring(0, space);
          pass = args.substring(space + 1);
          pass.trim();
        }
        if (ssid.length() == 0) {
          Serial.println("[CMD] Usage: wifi <SSID> [password]");
        } else {
          savedSsid = ssid;
          savedPass = pass;
          Serial.printf("[CMD] Connecting to \"%s\" %s\n",
            ssid.c_str(), pass.length() ? "with password" : "(open network)");
          wifiConnect.active = false;  // reset so startWiFiConnect accepts it
          startWiFiConnect(ssid, pass, true);
        }
      } else if (line.startsWith("mode ")) {
        int m = line.substring(5).toInt();
        if (m >= 0 && m < DISPLAY_MAX) {
          displayMode = (DisplayMode)m;
          saveDisplayModeToEEPROM();
          const char* modeNames[] = {"SOLID","SIMPLE","PULSE","BINARY","HOUR_MARKER","FLAME","PASTEL","NEON","COMET"};
          Serial.printf("[CMD] Display mode set to %d (%s)\n", m, modeNames[m]);
        } else {
          Serial.printf("[CMD] Mode must be 0-%d  (0=SOLID 1=SIMPLE 2=PULSE 3=BINARY 4=HOUR_MARKER 5=FLAME 6=PASTEL 7=NEON 8=COMET)\n", DISPLAY_MAX-1);
        }
      } else {
        Serial.println("[CMD] Commands: wifi <SSID> [pass]  |  scan  |  status  |  disconnect  |  mode <0-8>");
      }
    }
  }

  checkWiFi();
  updateWiFiConnect();

  // Self-update: device fetches firmware from URL and flashes itself
  if (pendingDirectUpdateUrl.length() > 0 && wifiConnected) {
    String url = pendingDirectUpdateUrl;
    pendingDirectUpdateUrl = "";
    DLOGI("OTA", "Direct self-update start  url=%s", url.c_str());
    Serial.println("[OTA] Direct update from: " + url);
    BearSSL::WiFiClientSecure client;
    client.setInsecure();  // skip cert check — URL came from our own UI / GitHub release
    ESPhttpUpdate.setLedPin(LED_BUILTIN, LOW);
    ESPhttpUpdate.rebootOnUpdate(true);
    t_httpUpdate_return ret = ESPhttpUpdate.update(client, url);
    if (ret != HTTP_UPDATE_OK) {
      lastDirectUpdateError = ESPhttpUpdate.getLastErrorString();
      DLOGE("OTA", "Direct update FAILED: %s", lastDirectUpdateError.c_str());
      Serial.println("[OTA] Direct update failed: " + lastDirectUpdateError);
    }
    // HTTP_UPDATE_OK never reaches here — board reboots automatically
  }

  ArduinoOTA.handle();
  
  // Trigger NTP sync after WiFi connects
  // Retry NTP every 20s until synced, then every hour
  bool ntpSynced = (time(nullptr) > 86400);
  unsigned long ntpInterval = ntpSynced ? 3600000UL : 20000UL;
  if (wifiConnected && millis() - lastNtpSync > ntpInterval) {
    syncTimeNTP();
  }
  
  // Refresh timezone daily if auto-detected
  if (wifiConnected && tz.autoDetected && millis() - lastTzCheck > 86400000) {
    detectTimezone();
  }
  
  // Periodic RTC uptime save + debug heartbeat (every 30s)
  {
    static unsigned long lastHb = 0;
    if (millis() - lastHb > 30000) {
      lastHb = millis();
      // Save current uptime to RTC so next boot can report it
      BootRecord br = {};
      ESP.rtcUserMemoryRead(RTC_BOOT_SLOT, (uint32_t*)&br, sizeof(br));
      br.magic    = RTC_BOOT_MAGIC;
      br.uptime_s = millis() / 1000;
      ESP.rtcUserMemoryWrite(RTC_BOOT_SLOT, (uint32_t*)&br, sizeof(br));
      // UDP heartbeat -- boot info resent on first HB in case early packets were ARP-dropped
      if (wifiConnected && debugRemoteEnabled) {
        static bool bootMsgConfirmed = false;
        if (!bootMsgConfirmed) {
          bootMsgConfirmed = true;
          DLOGI("BOOT", "fw=%s git:%s built:%s  %s", FW_VERSION_BASE, FW_GIT_HASH, FW_BUILD_TIME, cachedBootInfo.c_str());
        }
        DLOGI("HB", "heap=%u frag=%u uptime=%lus mode=%d ntp=%d rssi=%d",
              ESP.getFreeHeap(), ESP.getHeapFragmentation(),
              millis() / 1000, (int)displayMode, ntpSynced ? 1 : 0,
              wifiConnected ? WiFi.RSSI() : 0);
      }
    }
  }

  // Update LED clock display
  displayClock();

  delay(100);
}
