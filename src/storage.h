#pragma once

#include "config.h"

// ============================================================================
// EEPROM persistence
// ============================================================================

void loadEEPROMSettings();
void saveEEPROMSettings(const String& ssid, const String& pass);
