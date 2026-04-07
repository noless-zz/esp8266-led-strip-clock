#pragma once

#include "config.h"

void syncTimeNTP();
void detectTimezone();
void checkWiFi();
String getWifiScanJson();
bool startWiFiConnect(const String& ssid, const String& pass, bool saveToEeprom = false);
void updateWiFiConnect();
void setupWiFi();
void setupDNS();
void setupOTA();
uint32_t getMaxUpdateSize();
const char* updateErrorToString(uint8_t error);
void processDNS();
void updateMDNS();
void handleOTA();
