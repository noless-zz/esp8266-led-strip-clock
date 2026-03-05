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
#include <time.h>
#include <sys/time.h>

#ifndef NUM_LEDS
#define NUM_LEDS 60
#endif
#ifndef LED_PIN
#define LED_PIN D4
#endif

// Build timestamp - compiler generates this automatically
const char* FW_VERSION_BASE = "2.0.0";
const char* FW_BUILD_TIME = __DATE__ " " __TIME__;
const char* AP_SSID = "LED-Clock";
const char* AP_PASS = "";
const char* OTA_PASS = "admin123";
const char* NTP_SERVER = "pool.ntp.org";
const byte DNS_PORT = 53;
const int MAX_SCAN_CACHE = 30;

// Display modes
enum DisplayMode {
  DISPLAY_SOLID = 0,        // Hour/min/sec overlay
  DISPLAY_PROGRESS_BAR = 1, // Time as % fill (0-100% throughout day)
  DISPLAY_PULSE_DOTS = 2,   // Dots pulse to seconds rhythm
  DISPLAY_GRADIENT = 3,     // Color shifts throughout day
  DISPLAY_BINARY = 4,       // Binary clock (hour/min/sec both)
  DISPLAY_HOUR_MARKER = 5,  // Bright LED marks hour, dimmer fills minutes
  DISPLAY_STROBE = 6,       // Flashes per second (1-60)
  DISPLAY_RAINBOW = 7,      // Scrolling rainbow animation
  DISPLAY_FLAME = 8,        // Flickering fire effect
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
const uint8_t EEPROM_MAGIC = 0xA7;
const int MAX_SSID_LEN = 32;
const int MAX_PASS_LEN = 64;

// Global state - LEDs
Adafruit_NeoPixel *ledStrip = nullptr;
uint8_t ledBrightness = 76;  // 30%
DisplayMode displayMode = DISPLAY_SOLID;
struct CRGB { uint8_t r, g, b; } leds[NUM_LEDS];

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

// Forward declarations for display functions
void displayMode_ProgressBar();
void displayMode_PulseDots();
void displayMode_Gradient();
void displayMode_Binary();
void displayMode_HourMarker();
void displayMode_Strobe();
void displayMode_Rainbow();
void displayMode_Flame();
void displayClock_Solid();

// ============================================================================
// LED Functions
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

void applyBrightnessAndShow() {
  for (int i = 0; i < NUM_LEDS; i++) {
    leds[i].r = (uint16_t)leds[i].r * ledBrightness / 255;
    leds[i].g = (uint16_t)leds[i].g * ledBrightness / 255;
    leds[i].b = (uint16_t)leds[i].b * ledBrightness / 255;
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

void overlayTimeMarkers(int hour12, int minute, int second, int secTrailLen) {
  int hourPos = (hour12 * 5 + minute / 12) % NUM_LEDS;
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

void displayMode_ProgressBar() {
  int hour12, hour24, minute, second, daySeconds;
  if (!extractClock(hour12, hour24, minute, second, daySeconds)) return;

  memset(leds, 0, sizeof(leds));
  int fill = (int)((uint32_t)daySeconds * NUM_LEDS / 86400);
  uint32_t ms = millis();
  uint8_t pulse = 90 + (uint8_t)((ms % 1000) * 120 / 1000);

  for (int i = 0; i < NUM_LEDS; i++) {
    uint8_t r, g, b;
    if (i <= fill) {
      hsvToRgb((i * 240 / NUM_LEDS) + (second * 2), 255, pulse, r, g, b);
      leds[i] = {r, g, b};
    } else {
      leds[i] = {8, 8, 16};
    }
  }

  overlayTimeMarkers(hour12, minute, second, 8);
  applyBrightnessAndShow();
}

void displayMode_PulseDots() {
  int hour12, hour24, minute, second, daySeconds;
  if (!extractClock(hour12, hour24, minute, second, daySeconds)) return;

  memset(leds, 0, sizeof(leds));
  uint32_t ms = millis();
  uint8_t beat = (uint8_t)((ms % 1000) < 500 ? ((ms % 500) * 255 / 500) : ((1000 - (ms % 1000)) * 255 / 500));

  for (int i = 0; i < NUM_LEDS; i++) {
    int ring = i % 5;
    uint8_t base = 20 + (ring * 8);
    leds[i] = {base / 2, base, base + beat / 6};
  }

  int secPos = second;
  for (int i = 0; i < NUM_LEDS; i++) {
    int d = abs(i - secPos);
    d = min(d, NUM_LEDS - d);
    if (d < 10) {
      uint8_t boost = (uint8_t)((10 - d) * beat / 10);
      addPixelWrap(i, boost / 4, boost / 3, boost);
    }
  }

  overlayTimeMarkers(hour12, minute, second, 10);
  applyBrightnessAndShow();
}

void displayMode_Gradient() {
  int hour12, hour24, minute, second, daySeconds;
  if (!extractClock(hour12, hour24, minute, second, daySeconds)) return;

  memset(leds, 0, sizeof(leds));
  uint16_t dayHue = (uint32_t)daySeconds * 360 / 86400;
  uint16_t shift = (millis() / 40) % 360;

  for (int i = 0; i < NUM_LEDS; i++) {
    uint8_t r, g, b;
    hsvToRgb((dayHue + shift + i * 5) % 360, 220, 90, r, g, b);
    leds[i] = {r, g, b};
  }

  overlayTimeMarkers(hour12, minute, second, 12);
  applyBrightnessAndShow();
}

void displayMode_Binary() {
  int hour12, hour24, minute, second, daySeconds;
  if (!extractClock(hour12, hour24, minute, second, daySeconds)) return;

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

void displayMode_Strobe() {
  int hour12, hour24, minute, second, daySeconds;
  if (!extractClock(hour12, hour24, minute, second, daySeconds)) return;

  memset(leds, 0, sizeof(leds));
  uint32_t ms = millis();
  int flashesPerSec = second + 1;
  int cycle = max(16, 1000 / flashesPerSec);
  bool on = (ms % cycle) < (cycle / 3);

  for (int i = 0; i < NUM_LEDS; i++) {
    leds[i] = on ? CRGB{55, 10, 10} : CRGB{12, 4, 4};
  }

  overlayTimeMarkers(hour12, minute, second, 6);
  applyBrightnessAndShow();
}

void displayMode_Rainbow() {
  int hour12, hour24, minute, second, daySeconds;
  if (!extractClock(hour12, hour24, minute, second, daySeconds)) return;

  memset(leds, 0, sizeof(leds));
  uint16_t drift = (millis() / 30) % 360;

  for (int i = 0; i < NUM_LEDS; i++) {
    uint8_t r, g, b;
    hsvToRgb((i * 360 / NUM_LEDS + drift) % 360, 255, 80, r, g, b);
    leds[i] = {r, g, b};
  }

  overlayTimeMarkers(hour12, minute, second, 10);
  applyBrightnessAndShow();
}

void displayMode_Flame() {
  int hour12, hour24, minute, second, daySeconds;
  if (!extractClock(hour12, hour24, minute, second, daySeconds)) return;

  memset(leds, 0, sizeof(leds));
  uint32_t phase = millis() / 20;

  for (int i = 0; i < NUM_LEDS; i++) {
    uint32_t x = (uint32_t)(i * 37 + phase * 13 + minute * 17);
    uint8_t flicker = (uint8_t)((x ^ (x >> 3) ^ (x >> 5)) & 0x7F);
    uint8_t r = 100 + flicker;
    uint8_t g = 20 + flicker / 2;
    uint8_t b = 2 + (flicker / 16);
    leds[i] = {r, g, b};
  }

  overlayTimeMarkers(hour12, minute, second, 11);
  applyBrightnessAndShow();
}

void displayClock() {
  // Dispatcher: choose display mode
  switch (displayMode) {
    case DISPLAY_PROGRESS_BAR:
      displayMode_ProgressBar();
      break;
    case DISPLAY_PULSE_DOTS:
      displayMode_PulseDots();
      break;
    case DISPLAY_GRADIENT:
      displayMode_Gradient();
      break;
    case DISPLAY_BINARY:
      displayMode_Binary();
      break;
    case DISPLAY_HOUR_MARKER:
      displayMode_HourMarker();
      break;
    case DISPLAY_STROBE:
      displayMode_Strobe();
      break;
    case DISPLAY_RAINBOW:
      displayMode_Rainbow();
      break;
    case DISPLAY_FLAME:
      displayMode_Flame();
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

  memset(leds, 0, sizeof(leds));
  for (int i = 0; i < NUM_LEDS; i++) {
    uint8_t r, g, b;
    hsvToRgb((i * 6 + second * 6) % 360, 180, 30, r, g, b);
    leds[i] = {r, g, b};
  }

  overlayTimeMarkers(hour12, minute, second, 9);
  applyBrightnessAndShow();
}

// ============================================================================
// EEPROM Functions
// ============================================================================

void loadEEPROMSettings() {
  EEPROM.begin(EEPROM_SIZE);
  uint8_t magic = EEPROM.read(EEPROM_MAGIC_ADDR);
  if (magic != EEPROM_MAGIC) {
    EEPROM.write(EEPROM_MAGIC_ADDR, EEPROM_MAGIC);
    EEPROM.commit();
    Serial.println("[EEPROM] Initialized new settings");
    return;
  }
  
  // Load WiFi SSID
  String ssid = "";
  for (int i = 0; i < MAX_SSID_LEN; i++) {
    char c = EEPROM.read(EEPROM_SSID_ADDR + i);
    if (c == 0) break;
    ssid += c;
  }
  if (ssid.length() > 0) {
    String pass = "";
    for (int i = 0; i < MAX_PASS_LEN; i++) {
      char c = EEPROM.read(EEPROM_PASS_ADDR + i);
      if (c == 0) break;
      pass += c;
    }
    Serial.println("[EEPROM] Loaded WiFi: " + ssid);
    WiFi.begin(ssid.c_str(), pass.c_str());
  }
  
  // Load brightness
  uint8_t br = EEPROM.read(EEPROM_BRIGHTNESS_ADDR);
  if (br > 0 && br <= 255) ledBrightness = br;
  
  // Load timezone offset
  int32_t tzOff = (EEPROM.read(EEPROM_TZ_OFFSET_ADDR + 0) << 24) |
                  (EEPROM.read(EEPROM_TZ_OFFSET_ADDR + 1) << 16) |
                  (EEPROM.read(EEPROM_TZ_OFFSET_ADDR + 2) << 8) |
                  (EEPROM.read(EEPROM_TZ_OFFSET_ADDR + 3));
  if (tzOff != 0 && tzOff >= -43200 && tzOff <= 43200) tz.utcOffset = tzOff;
  
  // Load display mode
  uint8_t mode = EEPROM.read(EEPROM_DISPLAY_MODE_ADDR);
  if (mode < DISPLAY_MAX) displayMode = (DisplayMode)mode;
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
}

// ============================================================================
// Time & Timezone Functions
// ============================================================================

void syncTimeNTP() {
  if (!wifiConnected) return;
  Serial.println("[NTP] Syncing with " + String(NTP_SERVER));
  configTime(tz.utcOffset, 0, NTP_SERVER);
  time_t now = time(nullptr);
  int attempts = 50;
  while (now < 86400 && attempts-- > 0) { delay(100); now = time(nullptr); }
  if (now > 86400) {
    Serial.println("[NTP] Time synced");
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
  Serial.println("[GEO] Detecting timezone from IP geolocation...");

  struct TzProvider { const char* host; const char* path; const char* label; };
  TzProvider providers[] = {
    {"ip-api.com", "/json/?fields=status,message,timezone,offset,country,city", "ip-api"},
    {"ipwho.is", "/", "ipwho.is"}
  };

  bool detected = false;
  for (size_t p = 0; p < (sizeof(providers) / sizeof(providers[0])); p++) {
    WiFiClient client;
    tzDiag.source = providers[p].label;

    if (!client.connect(providers[p].host, 80)) {
      tzDiag.status = "error";
      tzDiag.message = String("connect failed to ") + providers[p].host;
      continue;
    }

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
    break;
  }

  if (!detected) {
    Serial.println("[GEO] All timezone providers failed: " + tzDiag.message);
    return;
  }
  
  syncTimeNTP();
  lastTzCheck = millis();
}

void checkWiFi() {
  static unsigned long lastCheck = 0;
  if (millis() - lastCheck < 5000) return;
  lastCheck = millis();
  
  bool connected = (WiFi.status() == WL_CONNECTED);
  if (connected != wifiConnected) {
    wifiConnected = connected;
    Serial.println("[WiFi] " + String(connected ? "Connected" : "Disconnected"));
    if (connected) detectTimezone();
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

bool startWiFiConnect(const String& ssid, const String& pass) {
  if (ssid.length() == 0) return false;
  if (wifiConnect.active) return false;
  
  wifiConnect.active = true;
  wifiConnect.connecting = true;
  wifiConnect.attemptedSsid = ssid;
  wifiConnect.startedAt = millis();
  wifiConnect.lastStatus = WiFi.status();
  
  saveEEPROMSettings(ssid, pass);
  
  // Check scan cache for channel
  int targetChannel = 0;
  for (int i = 0; i < scanCacheCount; i++) {
    if (scanCache[i].ssid == ssid) {
      targetChannel = scanCache[i].channel;
      break;
    }
  }
  
  if (targetChannel > 0) {
    Serial.printf("[WiFi] Connecting to %s on channel %d\n", ssid.c_str(), targetChannel);
    WiFi.begin(ssid.c_str(), pass.c_str(), targetChannel);
  } else {
    WiFi.begin(ssid.c_str(), pass.c_str());
  }
  
  return true;
}

void updateWiFiConnect() {
  if (!wifiConnect.active) return;
  
  wl_status_t status = WiFi.status();
  if (status == WL_CONNECTED) {
    wifiConnect.active = false;
    wifiConnect.connecting = false;
    wifiConnect.success = true;
    wifiConnected = true;
    Serial.print("[WiFi] Connected! IP: ");
    Serial.println(WiFi.localIP());
    detectTimezone();
  } else if (status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL) {
    wifiConnect.active = false;
    wifiConnect.connecting = false;
    wifiConnect.success = false;
    wifiConnect.error = "Connection failed";
  } else if (millis() - wifiConnect.startedAt > 30000) {
    wifiConnect.active = false;
    wifiConnect.connecting = false;
    wifiConnect.success = false;
    wifiConnect.error = "Connection timeout";
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
*{margin:0;padding:0;box-sizing:border-box}body{font-family:system-ui;background:linear-gradient(135deg,#667eea,#764ba2);
min-height:100vh;padding:20px;color:#333}.container{max-width:600px;margin:0 auto}.clock-card{background:#fff;
border-radius:20px;padding:40px 20px;margin-bottom:20px;box-shadow:0 10px 40px rgba(0,0,0,0.3);text-align:center}
.clock-time{font-size:72px;font-weight:300;letter-spacing:4px;color:#667eea;margin-bottom:10px;font-variant-numeric:tabular-nums}
.card{background:#fff;border-radius:16px;padding:20px;margin-bottom:15px;box-shadow:0 5px 20px rgba(0,0,0,0.2)}
h3{font-size:14px;letter-spacing:2px;text-transform:uppercase;color:#999;margin-bottom:15px}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:15px}.item{padding:12px;background:#f5f5f5;border-radius:8px;text-align:center}
.label{font-size:11px;color:#999;text-transform:uppercase;}
.value{font-size:16px;font-weight:600;color:#333;margin-top:5px}
.btn{width:100%;padding:12px;border:none;border-radius:6px;font-weight:bold;cursor:pointer;text-transform:uppercase;letter-spacing:1px;
background:linear-gradient(135deg,#667eea,#764ba2);color:#fff;margin-bottom:10px}
.btn:active{opacity:0.9}
</style></head><body><div class='container'>
<div class='clock-card'><div class='clock-time' id='time'>--:--:--</div><div style='font-size:16px;color:#999;margin-top:10px;' id='date'>Loading</div></div>
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
</div></div>
<button class='btn' onclick='location.href="/settings.html"'>Settings</button>
</div>
<script>
function updateStatus(){fetch('/api/status').then(r=>r.json()).then(d=>{
const now=new Date();document.getElementById('time').textContent=now.toLocaleTimeString('en-US',{hour:'2-digit',minute:'2-digit',second:'2-digit'});
document.getElementById('date').textContent=now.toLocaleDateString('en-US',{weekday:'short',month:'short',day:'numeric'});
document.getElementById('wifi').textContent=d.wifi_connected?'✓ '+d.wifi_ssid:'✗ Offline';
document.getElementById('signal').textContent=d.wifi_rssi?d.wifi_rssi+' dBm':'--';
const tzDebug=d.timezone_auto_detected?'Auto '+d.timezone_utc_offset_hours+'h':'Manual '+d.timezone_utc_offset_hours+'h';
document.getElementById('tz').textContent=d.timezone||'UTC';
document.getElementById('tz_debug').textContent=tzDebug;
document.getElementById('ntp').textContent=d.ntp_synced?'✓ Synced':'⏱ Wait';
document.getElementById('fw').textContent=d.fw_version_base||'-';document.getElementById('fw').title='Build: '+(d.fw_build_time||'unknown');
document.getElementById('bright').textContent=d.brightness+'%';
document.getElementById('ip').textContent=d.ip||'-';
document.getElementById('heap').textContent=Math.round(d.heap/1024)+' KB';
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
<div class='form-group'><label>Available Networks</label><div class='wifi-row'>
<select id='ssidList' size='6'><option value=''>Scanning...</option></select>
<button class='mini-btn' id='scanBtn' onclick='scanWifi()' style='height:36px'>SCAN</button>
</div></div>
<div class='form-group'><label>Password</label><input type='password' id='wifiPass' placeholder='Network password'/></div>
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
<option value='0'>Solid Orbit (H/M/S comet)</option>
<option value='1'>Day Progress (full-strip timeline)</option>
<option value='2'>Pulse Wave (second heartbeat)</option>
<option value='3'>Day Gradient (time-colored sweep)</option>
<option value='4'>Binary 60 (stretched H/M/S bits)</option>
<option value='5'>Marker Ring (hour+minute+second)</option>
<option value='6'>Strobe Clock (tempo by seconds)</option>
<option value='7'>Rainbow Clock (moving spectrum)</option>
<option value='8'>Flame Clock (warm dynamic clock)</option>
</select></div>
<div style='font-size:11px;color:#888;margin-top:5px' id='modeDesc'>Choose a display mode</div>
<button class='btn btn-secondary' onclick='updateModeDescription()'>Refresh Display</button>
</div>

<div class='card'><h2>Brightness</h2>
<div class='form-group' style='margin-bottom:5px'><label>LED Brightness</label>
<input type='range' id='brightness' min='10' max='255' value='76' oninput='updateBrightnessLabel()' style='width:100%'/></div>
<div style='text-align:center;font-size:12px;color:#666'>
<span id='brightLabel'>30%</span> (<span id='brightValue'>76</span>/255)</div>
<button class='btn btn-secondary' onclick='saveBrightness()'>Save Brightness</button>
</div>

<div class='card'><h2>Firmware Update</h2>
<div class='upload-area' onclick='document.getElementById("fwFile").click()' id='uploadArea'>
<p id='fileName'>📁 Click to select .bin firmware file</p>
<input type='file' id='fwFile' accept='.bin' style='display:none' onchange='fileSelected(this)'>
</div>
<button class='upload-btn' id='uploadBtn' onclick='uploadFirmware()' disabled>Upload Firmware</button>
<div class='progress-bar' id='progBar'><div class='progress-fill' id='progFill'></div></div>
<div class='status-msg' id='statusMsg'></div>
<div class='note'>Max size: <span id='maxSize'>-</span> bytes</div>
</div>

<div class='card'><h2>Device Information</h2>
<div style='font-size:12px;line-height:1.8;color:#666'>
<div>Firmware: <span id='fwVersion'>-</span> <span id='fwBuildTime' style='font-size:10px;color:#999'></span></div>
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
let fwFile=null,uploading=false;function updateBrightnessLabel(){const v=document.getElementById('brightness').value;
document.getElementById('brightValue').textContent=v;document.getElementById('brightLabel').textContent=(Math.round(v/255*100))+'%';}
function toggleTzMode(){document.getElementById('manualTz').style.display=document.querySelector('input[name="tzmode"]:checked').value==='manual'?'block':'none';}
function fileSelected(input){const sb=document.getElementById('statusMsg');const ub=document.getElementById('uploadBtn');fwFile=null;ub.disabled=true;
if(input.files.length===0)return;fwFile=input.files[0];document.getElementById('fileName').textContent='📄 '+fwFile.name;sb.textContent='Checking firmware...';sb.className='status-msg status-info';
const r=new FileReader();r.onload=()=>{const b=new Uint8Array(r.result);const m=b.length>0?b[0]:0;
fetch('/api/update/precheck?name='+encodeURIComponent(fwFile.name)+'&size='+fwFile.size+'&magic='+m).then(r=>r.json()).then(d=>{
if(d.ok){ub.disabled=false;sb.textContent='✓ '+d.summary;sb.className='status-msg status-ok';}else{ub.disabled=true;sb.textContent='✗ '+d.error;sb.className='status-msg status-err';}}).catch(e=>{ub.disabled=true;sb.textContent='Check failed: '+e;sb.className='status-msg status-err';});};
r.onerror=()=>{ub.disabled=true;sb.textContent='Failed to read file';sb.className='status-msg status-err';};r.readAsArrayBuffer(fwFile.slice(0,1));}
function uploadFirmware(){if(!fwFile||uploading)return;if(!confirm('Upload '+fwFile.name+'?'))return;uploading=true;document.getElementById('uploadBtn').disabled=true;
const sb=document.getElementById('statusMsg');const pb=document.getElementById('progBar');const pf=document.getElementById('progFill');pb.style.display='block';sb.textContent='';
const fd=new FormData();fd.append('firmware',fwFile);const x=new XMLHttpRequest();
x.upload.addEventListener('progress',(e)=>{if(e.lengthComputable)pf.style.width=(e.loaded/e.total*100)+'%';});
x.addEventListener('load',()=>{uploading=false;try{const p=JSON.parse(x.responseText);if(x.status===200&&p.ok){sb.textContent='✓ Update OK ('+p.written+' bytes). Rebooting...';sb.className='status-msg status-ok';setTimeout(()=>location.reload(),2000);}else{const e=p?p.error:x.responseText;sb.textContent='✗ '+e;sb.className='status-msg status-err';document.getElementById('uploadBtn').disabled=false;}}catch(e){sb.textContent='✗ Upload failed';sb.className='status-msg status-err';document.getElementById('uploadBtn').disabled=false;}});
x.addEventListener('error',()=>{uploading=false;sb.textContent='✗ Connection error';sb.className='status-msg status-err';document.getElementById('uploadBtn').disabled=false;});
x.open('POST','/api/update?approve=1');x.send(fd);}
function scanWifi(attempt=0){const list=document.getElementById('ssidList');const sb=document.getElementById('scanBtn');const msg=document.getElementById('wifiMsg');
if(attempt===0){msg.textContent='Scanning...';msg.className='status-msg status-info';sb.disabled=true;}
fetch('/api/wifi/scan').then(r=>r.json()).then(d=>{if(d.scanning){if(attempt>25){msg.textContent='Scan timeout';msg.className='status-msg status-err';sb.disabled=false;return;}
setTimeout(()=>scanWifi(attempt+1),350);return;}
list.innerHTML='';if(!d.networks||d.networks.length===0){list.innerHTML='<option>No networks found</option>';msg.textContent='';msg.className='status-msg';sb.disabled=false;return;}
d.networks.forEach(n=>{const o=document.createElement('option');o.value=n.ssid;o.textContent=n.ssid+' ('+n.rssi+' dBm)';list.appendChild(o);});
msg.textContent='Found '+d.networks.length+' networks';msg.className='status-msg status-ok';sb.disabled=false;}).catch(e=>{list.innerHTML='<option>Scan failed</option>';msg.textContent='Error: '+e;msg.className='status-msg status-err';sb.disabled=false;});}
function connectWifi(){const s=document.getElementById('ssidList').value;const p=document.getElementById('wifiPass').value;const msg=document.getElementById('wifiMsg');
if(!s){msg.textContent='Select a network first';msg.className='status-msg status-err';return;}msg.textContent='Connecting...';msg.className='status-msg status-info';
const sp=new URLSearchParams({ssid:s,pass:p});fetch('/api/wifi/connect?'+sp.toString()).then(r=>r.json()).then(d=>{
if(d.connected){msg.textContent='✓ Connected! IP: '+d.ip;msg.className='status-msg status-ok';}else if(d.connecting){msg.textContent='Connecting, please wait...';msg.className='status-msg status-info';setTimeout(()=>connectWifi(),500);}else{msg.textContent='✗ '+(d.error||'Connection failed');msg.className='status-msg status-err';}}).catch(e=>{msg.textContent='Connection failed: '+e;msg.className='status-msg status-err';});}
function syncNTP(){const msg=document.getElementById('tzMsg');msg.textContent='Syncing...';msg.className='status-msg status-info';
fetch('/api/ntp').then(r=>r.text()).then(t=>{msg.textContent='✓ Syncing with NTP server...';msg.className='status-msg status-ok';}).catch(e=>{msg.textContent='✗ Error: '+e;msg.className='status-msg status-err';});}
function saveTimezone(){const m=document.querySelector('input[name="tzmode"]:checked').value;const o=m==='manual'?document.getElementById('tzOffset').value:'0';
const b=document.getElementById('tzMsg');b.textContent='Saving...';b.className='status-msg status-info';
fetch('/api/timezone?mode='+m+'&offset='+o).then(r=>r.text()).then(t=>{b.textContent='✓ Saved!';b.className='status-msg status-ok';}).catch(e=>{b.textContent='✗ Error: '+e;b.className='status-msg status-err';});}
function saveBrightness(){const v=document.getElementById('brightness').value;fetch('/api/brightness?value='+Math.round(v/255*100)).catch(e=>console.warn(e));}
function saveDisplayMode(){const m=document.getElementById('displayMode').value;fetch('/api/display?mode='+m).catch(e=>console.warn(e));updateModeDescription();}
function updateModeDescription(){const m=parseInt(document.getElementById('displayMode').value);
const desc={0:'All 60 LEDs are active. Red=hour marker, green=minute marker, blue=second comet with trail.',
1:'All 60 LEDs form a day timeline. Filled section shows progress through 24h; H/M/S markers stay visible.',
2:'All 60 LEDs pulse in waves. Pulse follows seconds while distinct red/green/blue time markers remain.',
3:'All 60 LEDs show a moving day-color gradient; H/M/S overlays keep exact time readable.',
4:'All 60 LEDs used as 20 groups × 3 LEDs: hour bits, minute bits, second bits stretched across strip.',
5:'Ring map mode: minute progress fills the strip, hour is a bold beacon, second is a moving tail.',
6:'Global strobe tempo follows seconds, but H/M/S markers stay visible on top for readability.',
7:'Moving rainbow over all 60 LEDs with clear H/M/S markers layered above for instant read.',
8:'Animated warm flame texture on all 60 LEDs with distinct H/M/S pointers for real clock behavior.'};
document.getElementById('modeDesc').textContent=desc[m]||'Mode '+m;}
function pollStatus(){fetch('/api/status').then(r=>r.json()).then(d=>{
document.getElementById('fwVersion').textContent=d.fw_version_base||'-';
document.getElementById('fwBuildTime').textContent='('+d.fw_build_time+')';
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
if(d.display_mode!==undefined){document.getElementById('displayMode').value=d.display_mode;updateModeDescription();}
}).catch(e=>console.warn(e));}
function getMaxSize(){fetch('/api/status').then(r=>r.json()).then(d=>{document.getElementById('maxSize').textContent=(d.heap||262144).toString();}).catch(e=>console.warn(e));}
pollStatus();getMaxSize();scanWifi();setInterval(pollStatus,5000);updateModeDescription();
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
    doc["display_mode"] = (int)displayMode;
    doc["ip"] = wifiConnected ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
    doc["heap"] = ESP.getFreeHeap();
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
      
      if (startWiFiConnect(ssid, pass)) {
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
  
  // API: Display Mode
  server.on("/api/display", HTTP_GET, [](AsyncWebServerRequest *req) {
    DynamicJsonDocument doc(512);
    doc["current_mode"] = displayMode;
    doc["available_modes"]["SOLID"] = DISPLAY_SOLID;
    doc["available_modes"]["PROGRESS_BAR"] = DISPLAY_PROGRESS_BAR;
    doc["available_modes"]["PULSE_DOTS"] = DISPLAY_PULSE_DOTS;
    doc["available_modes"]["GRADIENT"] = DISPLAY_GRADIENT;
    doc["available_modes"]["BINARY"] = DISPLAY_BINARY;
    doc["available_modes"]["HOUR_MARKER"] = DISPLAY_HOUR_MARKER;
    doc["available_modes"]["STROBE"] = DISPLAY_STROBE;
    doc["available_modes"]["RAINBOW"] = DISPLAY_RAINBOW;
    doc["available_modes"]["FLAME"] = DISPLAY_FLAME;
    
    if (req->hasParam("mode")) {
      int newMode = atoi(req->getParam("mode")->value().c_str());
      if (newMode >= 0 && newMode < DISPLAY_MAX) {
        displayMode = (DisplayMode)newMode;
        doc["changed"] = true;
        doc["new_mode"] = displayMode;
      } else {
        doc["error"] = "Invalid mode";
      }
    }
    
    String response;
    serializeJson(doc, response);
    req->send(200, "application/json", response);
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
          return;
        }
        Serial.print("[OTA] Updating: ");
        Serial.println(filename);
      }
      
      if (Update.write(data, len) == len) {} else {
        Update.printError(Serial);
      }
      
      if (final) {
        if (Update.end(true)) {
          Serial.println("\n[OTA] Update Success!");
        } else {
          Serial.println("\n[OTA] Update Failed!");
          Update.printError(Serial);
        }
      }
    }
  );
  
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
  WiFi.softAP(AP_SSID, AP_PASS);
  WiFi.beginSmartConfig();
  Serial.println("[WiFi] AP: " + String(AP_SSID));
  Serial.println("[WiFi] AP IP: " + WiFi.softAPIP().toString());
  
  // Try to auto-connect to previously saved network
  WiFi.begin();
}

void setupDNS() {
  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
  Serial.println("[DNS] Captive portal ready");
}

void setupOTA() {
  ArduinoOTA.setPassword(OTA_PASS);
  ArduinoOTA.begin();
  Serial.println("[OTA] Ready");
}

// ============================================================================
// Main Setup & Loop
// ============================================================================

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n\n");
  Serial.println("  ESP8266 LED Strip Clock v" + String(FW_VERSION_BASE) + " (" + FW_BUILD_TIME + ")");
  Serial.println("  60-LED NeoPixel Clock with NTP Sync");
  Serial.println("\n");
  
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
  dnsServer.processNextRequest();
  if (mdnsStarted) {
    MDNS.update();
  } else if (WiFi.status() == WL_CONNECTED && !mdnsStarted) {
    MDNS.begin("ledclock");
    mdnsStarted = true;
    Serial.println("[mDNS] Available at http://ledclock.local");
  }
  
  checkWiFi();
  updateWiFiConnect();
  ArduinoOTA.handle();
  
  // Trigger NTP sync after WiFi connects
  if (wifiConnected && millis() - lastNtpSync > 3600000) {
    syncTimeNTP();
  }
  
  // Refresh timezone daily if auto-detected
  if (wifiConnected && tz.autoDetected && millis() - lastTzCheck > 86400000) {
    detectTimezone();
  }
  
  // Update LED clock display
  displayClock();
  
  delay(100);
}
