#pragma once

#include <Arduino.h>
#include <ESP8266WiFi.h>

// ============================================================================
// Hardware / build macros
// ============================================================================

#ifndef NUM_LEDS
#define NUM_LEDS 60
#endif
#ifndef LED_PIN
#define LED_PIN D4
#endif
#ifndef BTN1_PIN
#define BTN1_PIN D5   // Button 1: short=cycle mode (on) / turn on (off); long=turn off LEDs
#endif
#ifndef BTN2_PIN
#define BTN2_PIN D6   // Button 2: short=cycle W-channel brightness through 4 levels
#endif

// Version is injected by scripts/auto_version.py pre-build script.
#ifndef FW_VERSION_STR
#  define FW_VERSION_STR "0.0.0+0"   // fallback if script didn't run
#endif
#ifndef FW_GIT_HASH
#  define FW_GIT_HASH "unknown"
#endif

// ============================================================================
// String constant extern declarations (defined in main.cpp)
// ============================================================================

extern const char* FW_VERSION_BASE;
extern const char* FW_BUILD_TIME;
extern String AP_SSID_STR;
extern const char* AP_SSID;
extern const char* AP_PASS;
extern const char* OTA_PASS;
extern const char* NTP_SERVER;

// ============================================================================
// Network constants
// ============================================================================

const byte DNS_PORT = 53;
const int MAX_SCAN_CACHE = 30;

// ============================================================================
// Display modes
// ============================================================================

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

// ============================================================================
// EEPROM Layout constants
// ============================================================================

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
const int EEPROM_DBG_MAGIC_ADDR   = 279;   // 1 byte: magic 0xD8
const int EEPROM_DBG_ENABLED_ADDR = 280;   // 1 byte: 1=enabled
const int EEPROM_DBG_IP_ADDR      = 281;   // 16 bytes: null-terminated IP string
const int EEPROM_DBG_PORT_ADDR    = 297;   // 2 bytes: port (big-endian)
const int EEPROM_FADE_MS_ADDR     = 299;   // 2 bytes: Simple mode fade duration (big-endian, 0=off)
const int EEPROM_AUTO_BRIGHT_ADDR = 301;   // 6 bytes: magic(1), enabled(1), dim_val(1), peak_val(1), dim_hour(1), peak_hour(1)
const uint8_t EEPROM_AUTO_BRIGHT_MAGIC = 0xAB;
const int EEPROM_BTN_MAGIC_ADDR  = 307;   // 1 byte: magic 0xBC (button / W-channel settings)
const int EEPROM_W_BRIGHT_ADDR   = 308;   // 1 byte: W channel brightness level index (0-3)
const int EEPROM_LEDS_OFF_ADDR   = 309;   // 1 byte: LEDs off state (1=off, 0=on)
const uint8_t EEPROM_BTN_MAGIC   = 0xBC;
const uint8_t EEPROM_DBG_MAGIC    = 0xD8;
const uint8_t EEPROM_MODE_CFG_MAGIC = 0x5C;
const uint8_t EEPROM_MAGIC = 0xA7;
const int MAX_SSID_LEN = 32;
const int MAX_PASS_LEN = 64;

// ============================================================================
// W-channel brightness levels
// ============================================================================

constexpr uint8_t W_BRIGHT_LEVEL_COUNT = 4;
extern const uint8_t W_BRIGHT_LEVELS[4];

// ============================================================================
// Struct type definitions
// ============================================================================

struct CRGB { uint8_t r, g, b; };

struct ModeDisplayConfig {
  uint8_t hourR, hourG, hourB;
  uint8_t minuteR, minuteG, minuteB;
  uint8_t secondR, secondG, secondB;
  uint8_t hourWidth;
  uint8_t minuteWidth;
  uint8_t secondWidth;
  uint8_t spectrum;
};

struct ScanCacheEntry {
  String ssid;
  int32_t rssi = 0;
  int channel = -1;
  int enc = -1;
  bool hasBssid = false;
};

struct WiFiConnectState {
  bool active = false;
  bool connecting = false;
  bool success = false;
  String attemptedSsid;
  String error = "";
  unsigned long startedAt = 0;
  wl_status_t lastStatus = WL_DISCONNECTED;
};

struct TimezoneState {
  int32_t utcOffset;
  String name;
  bool autoDetected;
};

struct TzDiagState {
  String source = "ip-api.com";
  String status = "idle";
  String message = "not started";
  String responseSample = "";
  int httpCode = 0;
  unsigned long lastAttemptMs = 0;
  unsigned long lastSuccessMs = 0;
};

struct OTAStatus {
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
};

// RTC memory boot record -- survives WDT/exception resets, cleared on power-cycle
struct BootRecord {
  uint32_t magic;       // RTC_BOOT_MAGIC when valid
  uint32_t uptime_s;    // last saved uptime in seconds (updated every 30s)
  uint32_t boot_count;  // incremented each reboot
};
#define RTC_BOOT_MAGIC 0xB007C10CUL
#define RTC_BOOT_SLOT  0  // offset 0 in user RTC memory (SDK reserves lower words)

// Button state tracking
struct ButtonState {
  bool wasPressed;
  unsigned long pressedAt;
  bool actionFired;
};

// Per-hand fade state: tracks old/new positions independently of RTC/millis alignment.
struct HandFade {
  int    fromPos  = -1;
  int    toPos    = -1;
  uint32_t startMs = 0;
};

// ============================================================================
// Boot progress stages
// ============================================================================

enum BootStage {
  BOOT_STAGE_INIT     = 0,  // just started          -- red,    LEDs 0-14
  BOOT_STAGE_AP_UP    = 1,  // AP is up              -- orange, LEDs 0-29
  BOOT_STAGE_SCANNING = 2,  // scanning networks     -- yellow, LEDs 0-44
  BOOT_STAGE_STA_CONN = 3,  // STA connecting        -- blue,   LEDs 0-59
  BOOT_STAGE_WIFI_OK  = 4,  // WiFi connected        -- green,  fill 60
  BOOT_STAGE_NTP_WAIT = 5,  // waiting for NTP       -- cyan,   full bounce
  BOOT_STAGE_RUNNING  = 6,  // fully operational     -- clock displayed
};

// ============================================================================
// Button timing constants
// ============================================================================

const unsigned long BTN_LONG_PRESS_MS  = 800;  // hold >= 800ms = long press
const unsigned long BTN_DEBOUNCE_MS    = 50;   // ignore releases < 50ms
