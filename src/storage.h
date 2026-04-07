#pragma once

#include "config.h"

void loadEEPROMSettings();
void saveDisplayModeToEEPROM();
void saveModeConfigToEEPROM(uint8_t mode);
void loadModeConfigsFromEEPROM();
void saveFadeMs();
void saveAutoBrightSettings();
void saveButtonSettings();
void saveDebugConfig();
void saveEEPROMSettings(const String& ssid, const String& pass);
