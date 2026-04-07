#pragma once

#include <Arduino.h>
#include <ESP8266WiFi.h>

// ============================================================================
// Build-time configuration (overridable via platformio.ini build_flags)
// ============================================================================

#ifndef NUM_LEDS
#define NUM_LEDS 60
#endif
#ifndef LED_PIN
#define LED_PIN D4
#endif

// ============================================================================
// Firmware / network string constants (defined in main.cpp)
// ============================================================================

extern const char* FW_VERSION_BASE;
extern const char* FW_BUILD_TIME;
extern const char* AP_SSID;
extern const char* AP_PASS;
extern const char* OTA_PASS;
extern const char* NTP_SERVER;

const byte DNS_PORT      = 53;
const int  MAX_SCAN_CACHE = 30;

// ============================================================================
// Display modes
// ============================================================================

enum DisplayMode {
  DISPLAY_SOLID       = 0,  // Rainbow orbit with HMS markers
  DISPLAY_SIMPLE      = 1,  // Clean 3-LED HMS (red/green/blue)
  DISPLAY_PULSE       = 2,  // Subtle pulse with HMS
  DISPLAY_BINARY      = 3,  // Binary clock stretched to 60 LEDs
  DISPLAY_HOUR_MARKER = 4,  // Minute progress with hour beacon
  DISPLAY_FLAME       = 5,  // Optimized flame effect with HMS
  DISPLAY_PASTEL      = 6,  // Soft pastel HMS colors
  DISPLAY_NEON        = 7,  // Bright neon HMS
  DISPLAY_COMET       = 8,  // Animated HMS comet trails
  DISPLAY_MAX         = 9
};

// ============================================================================
// EEPROM layout
// ============================================================================

const int     EEPROM_SIZE              = 512;
const int     EEPROM_MAGIC_ADDR        = 0;
const int     EEPROM_SSID_ADDR         = 1;
const int     EEPROM_PASS_ADDR         = 65;
const int     EEPROM_BRIGHTNESS_ADDR   = 129;
const int     EEPROM_TZ_OFFSET_ADDR    = 130;
const int     EEPROM_DISPLAY_MODE_ADDR = 134;
const uint8_t EEPROM_MAGIC             = 0xA7;
const int     MAX_SSID_LEN             = 32;
const int     MAX_PASS_LEN             = 64;

// ============================================================================
// Shared struct types
// ============================================================================

struct CRGB {
  uint8_t r, g, b;
};

struct ScanCacheEntry {
  String  ssid;
  int32_t rssi     = 0;
  int     channel  = -1;
  int     enc      = -1;
  bool    hasBssid = false;
};

struct WiFiConnectState {
  bool          active        = false;
  bool          connecting    = false;
  bool          success       = false;
  String        attemptedSsid;
  String        error         = "";
  unsigned long startedAt     = 0;
  wl_status_t   lastStatus    = WL_DISCONNECTED;
};

struct TimezoneState {
  int32_t utcOffset;
  String  name;
  bool    autoDetected;
};

struct TzDiagState {
  String        source         = "ip-api.com";
  String        status         = "idle";
  String        message        = "not started";
  String        responseSample = "";
  int           httpCode       = 0;
  unsigned long lastAttemptMs  = 0;
  unsigned long lastSuccessMs  = 0;
};

struct OTAStatus {
  bool          inProgress    = false;
  bool          approved      = false;
  bool          lastSuccess   = false;
  uint8_t       lastErrorCode = 0;
  String        lastErrorText = "";
  String        lastFileName  = "";
  uint32_t      expectedBytes = 0;
  uint32_t      writtenBytes  = 0;
  unsigned long startedAt     = 0;
  unsigned long finishedAt    = 0;
};
