/*
 * ESP8266 LED Strip Clock
 * 60-LED NeoPixel time display with NTP sync and timezone auto-detection
 * Features: WiFi captive portal, firmware OTA upload, time sync, color blending
 *
 * Source layout:
 *   config.h          – build-time constants, enums, shared struct types
 *   globals.h         – extern declarations for all global state
 *   led.h / led.cpp   – LED hardware + all display-mode functions
 *   storage.h / .cpp  – EEPROM persistence
 *   network.h / .cpp  – WiFi, NTP, timezone, DNS, ArduinoOTA
 *   web_pages.h       – embedded INDEX_HTML and SETTINGS_HTML
 *   web_server.h/.cpp – HTTP API endpoints
 *   main.cpp          – global variable definitions, setup(), loop()
 */

#include "config.h"
#include "globals.h"
#include "led.h"
#include "storage.h"
#include "network.h"
#include "web_server.h"

// ============================================================================
// String constant definitions (declared extern in config.h)
// ============================================================================

const char* FW_VERSION_BASE = "2.0.0";
const char* FW_BUILD_TIME   = __DATE__ " " __TIME__;
const char* AP_SSID         = "LED-Clock";
const char* AP_PASS         = "";
const char* OTA_PASS        = "admin123";
const char* NTP_SERVER      = "pool.ntp.org";

// ============================================================================
// Global state definitions (declared extern in globals.h)
// ============================================================================

// LEDs
Adafruit_NeoPixel* ledStrip     = nullptr;
uint8_t            ledBrightness = 76;  // ~30%
DisplayMode        displayMode   = DISPLAY_SOLID;
CRGB               leds[NUM_LEDS];

// Web server / DNS
AsyncWebServer server(80);
DNSServer      dnsServer;
bool           mdnsStarted = false;

// WiFi scan cache
ScanCacheEntry scanCache[MAX_SCAN_CACHE];
int            scanCacheCount     = 0;
unsigned long  scanCacheUpdatedAt = 0;

// WiFi connection state
WiFiConnectState wifiConnect;
bool             wifiConnected = false;

// Time / timezone
unsigned long lastNtpSync = 0;
unsigned long lastTzCheck = 0;
TimezoneState tz          = {0, "UTC", true};
TzDiagState   tzDiag;

// OTA
OTAStatus otaStatus;

// ============================================================================
// Arduino entry points
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

  WiFi.scanNetworks(true, true);  // Start first async scan

  Serial.println("[READY] Access point: " + String(AP_SSID));
  Serial.println("[READY] Open browser: http://192.168.4.1\n");
}

void loop() {
  processDNS();
  updateMDNS();
  checkWiFi();
  updateWiFiConnect();
  handleOTA();

  // Re-sync NTP every hour while connected
  if (wifiConnected && millis() - lastNtpSync > 3600000) {
    syncTimeNTP();
  }

  // Refresh timezone daily when auto-detected
  if (wifiConnected && tz.autoDetected && millis() - lastTzCheck > 86400000) {
    detectTimezone();
  }

  // Update LED clock display
  displayClock();

  delay(100);
}
