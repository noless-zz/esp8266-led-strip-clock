#pragma once

#include "config.h"
#include <Adafruit_NeoPixel.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>

// ============================================================================
// Global state – defined in main.cpp, declared here for use across modules
// ============================================================================

// LEDs
extern Adafruit_NeoPixel* ledStrip;
extern uint8_t            ledBrightness;
extern DisplayMode        displayMode;
extern CRGB               leds[NUM_LEDS];

// Web server / DNS
extern AsyncWebServer server;
extern DNSServer      dnsServer;
extern bool           mdnsStarted;

// WiFi scan cache
extern ScanCacheEntry scanCache[MAX_SCAN_CACHE];
extern int            scanCacheCount;
extern unsigned long  scanCacheUpdatedAt;

// WiFi connection state
extern WiFiConnectState wifiConnect;
extern bool             wifiConnected;

// Time / timezone
extern unsigned long lastNtpSync;
extern unsigned long lastTzCheck;
extern TimezoneState tz;
extern TzDiagState   tzDiag;

// OTA
extern OTAStatus otaStatus;
