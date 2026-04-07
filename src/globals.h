#pragma once

#include "config.h"
#include <Adafruit_NeoPixel.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <WiFiUdp.h>

// ============================================================================
// String constant definitions (defined in main.cpp)
// ============================================================================

// (declared extern in config.h, defined in main.cpp)

// ============================================================================
// LED globals
// ============================================================================

extern Adafruit_NeoPixel *ledStrip;
extern uint8_t ledBrightness;
extern bool    autoBrightEnabled;
extern uint8_t autoBrightDimVal;
extern uint8_t autoBrightPeakVal;
extern uint8_t autoBrightDimHour;
extern uint8_t autoBrightPeakHour;
extern bool ledRgbw;
extern bool ledReversed;
extern DisplayMode displayMode;
extern bool ledsOff;
extern uint8_t wBrightLevel;
extern CRGB leds[NUM_LEDS];
extern ModeDisplayConfig modeConfigs[DISPLAY_MAX];

// ============================================================================
// Web server globals
// ============================================================================

extern AsyncWebServer server;
extern DNSServer dnsServer;
extern bool mdnsStarted;

// ============================================================================
// WiFi globals
// ============================================================================

extern ScanCacheEntry scanCache[MAX_SCAN_CACHE];
extern int scanCacheCount;
extern unsigned long scanCacheUpdatedAt;
extern WiFiConnectState wifiConnect;
extern bool wifiConnected;
extern bool forceClockDisplay;
extern bool forceStatusDisplay;
extern String savedSsid;
extern String savedPass;

// ============================================================================
// Time globals
// ============================================================================

extern unsigned long lastNtpSync;
extern unsigned long lastTzCheck;
extern TimezoneState tz;
extern TzDiagState tzDiag;

// ============================================================================
// OTA globals
// ============================================================================

extern OTAStatus otaStatus;

// ============================================================================
// Debug globals
// ============================================================================

extern bool debugRemoteEnabled;
extern String debugServerIp;
extern uint16_t debugServerPort;
extern WiFiUDP debugUdp;
extern String cachedBootInfo;
extern bool bootInfoSent;

// ============================================================================
// Button globals
// ============================================================================

extern ButtonState btn1State;
extern ButtonState btn2State;

// ============================================================================
// Boot stage
// ============================================================================

extern BootStage bootStage;

// ============================================================================
// LED animation globals
// ============================================================================

extern uint32_t simpleFadeMs;
