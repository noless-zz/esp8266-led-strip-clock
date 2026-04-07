#include "storage.h"
#include "globals.h"
#include <EEPROM.h>
#include <ESP8266WiFi.h>

// ============================================================================
// EEPROM persistence
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
  int32_t tzOff =
    (EEPROM.read(EEPROM_TZ_OFFSET_ADDR + 0) << 24) |
    (EEPROM.read(EEPROM_TZ_OFFSET_ADDR + 1) << 16) |
    (EEPROM.read(EEPROM_TZ_OFFSET_ADDR + 2) <<  8) |
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
    EEPROM.write(EEPROM_SSID_ADDR + i, i < (int)ssid.length() ? ssid[i] : 0);
  }

  for (int i = 0; i < MAX_PASS_LEN; i++) {
    EEPROM.write(EEPROM_PASS_ADDR + i, i < (int)pass.length() ? pass[i] : 0);
  }

  EEPROM.write(EEPROM_BRIGHTNESS_ADDR, ledBrightness);
  EEPROM.write(EEPROM_TZ_OFFSET_ADDR + 0, (tz.utcOffset >> 24) & 0xFF);
  EEPROM.write(EEPROM_TZ_OFFSET_ADDR + 1, (tz.utcOffset >> 16) & 0xFF);
  EEPROM.write(EEPROM_TZ_OFFSET_ADDR + 2, (tz.utcOffset >>  8) & 0xFF);
  EEPROM.write(EEPROM_TZ_OFFSET_ADDR + 3,  tz.utcOffset        & 0xFF);
  EEPROM.write(EEPROM_DISPLAY_MODE_ADDR, (uint8_t)displayMode);

  EEPROM.commit();
  Serial.println("[EEPROM] Saved settings for " + ssid);
}
