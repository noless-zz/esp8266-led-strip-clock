#pragma once

#include "config.h"

// ============================================================================
// Network setup (call once in setup())
// ============================================================================

void setupWiFi();
void setupDNS();
void setupOTA();

// ============================================================================
// Per-loop helpers
// ============================================================================

void processDNS();
void updateMDNS();
void handleOTA();
void checkWiFi();

// ============================================================================
// WiFi scan & connection management
// ============================================================================

String getWifiScanJson();
bool   startWiFiConnect(const String& ssid, const String& pass);
void   updateWiFiConnect();

// ============================================================================
// Time / NTP / timezone
// ============================================================================

void syncTimeNTP();
void detectTimezone();
