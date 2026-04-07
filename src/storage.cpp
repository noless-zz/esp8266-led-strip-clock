#include "storage.h"
#include "globals.h"
#include "debug.h"
#include "led.h"
#include <EEPROM.h>
#include <ESP8266WiFi.h>

void saveDisplayModeToEEPROM() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.write(EEPROM_DISPLAY_MODE_ADDR, (uint8_t)displayMode);
  EEPROM.commit();
}

void saveModeConfigToEEPROM(uint8_t mode) {
  if (mode >= DISPLAY_MAX) return;
  int addr = EEPROM_MODE_CFG_BASE_ADDR + (int)mode * (int)sizeof(ModeDisplayConfig);
  EEPROM.begin(EEPROM_SIZE);
  const uint8_t *raw = reinterpret_cast<const uint8_t*>(&modeConfigs[mode]);
  for (unsigned int i = 0; i < sizeof(ModeDisplayConfig); i++) {
    EEPROM.write(addr + (int)i, raw[i]);
  }
  EEPROM.write(EEPROM_MODE_CFG_MAGIC_ADDR, EEPROM_MODE_CFG_MAGIC);
  EEPROM.commit();
}

void loadModeConfigsFromEEPROM() {
  setDefaultModeConfigs();
  EEPROM.begin(EEPROM_SIZE);

  if (EEPROM.read(EEPROM_MODE_CFG_MAGIC_ADDR) != EEPROM_MODE_CFG_MAGIC) {
    for (uint8_t m = 0; m < DISPLAY_MAX; m++) {
      saveModeConfigToEEPROM(m);
    }
    return;
  }

  for (uint8_t m = 0; m < DISPLAY_MAX; m++) {
    int addr = EEPROM_MODE_CFG_BASE_ADDR + (int)m * (int)sizeof(ModeDisplayConfig);
    ModeDisplayConfig loaded;
    uint8_t *raw = reinterpret_cast<uint8_t*>(&loaded);
    for (unsigned int i = 0; i < sizeof(ModeDisplayConfig); i++) {
      raw[i] = EEPROM.read(addr + (int)i);
    }
    if (isModeConfigValid(loaded)) {
      modeConfigs[m] = loaded;
    }
  }
}

void loadEEPROMSettings() {
  EEPROM.begin(EEPROM_SIZE);
  uint8_t magic = EEPROM.read(EEPROM_MAGIC_ADDR);
  if (magic != EEPROM_MAGIC) {
    EEPROM.write(EEPROM_MAGIC_ADDR, EEPROM_MAGIC);
    EEPROM.write(EEPROM_DISPLAY_MODE_ADDR, (uint8_t)displayMode);
    EEPROM.commit();
    loadModeConfigsFromEEPROM();
    Serial.println("[EEPROM] Initialized new settings");
    return;
  }
  
  // Load WiFi SSID -- reject if any byte is non-printable (garbage/uninitialized EEPROM)
  String ssid = "";
  bool ssidValid = true;
  for (int i = 0; i < MAX_SSID_LEN; i++) {
    char c = EEPROM.read(EEPROM_SSID_ADDR + i);
    if (c == 0) break;
    if (c < 0x20 || c > 0x7E) { ssidValid = false; break; }
    ssid += c;
  }
  if (!ssidValid || ssid.length() == 0) {
    Serial.println("[EEPROM] No valid WiFi credentials stored");
  } else {
    String pass = "";
    for (int i = 0; i < MAX_PASS_LEN; i++) {
      char c = EEPROM.read(EEPROM_PASS_ADDR + i);
      if (c == 0) break;
      pass += c;
    }
    Serial.println("[EEPROM] Loaded WiFi: " + ssid);
    // DEBUG: dump password bytes to verify EEPROM integrity
    Serial.printf("[EEPROM] Pass len=%d chars: [", pass.length());
    for (unsigned int i = 0; i < pass.length(); i++) {
      Serial.printf("%c(0x%02X)", pass[i], (uint8_t)pass[i]);
      if (i < pass.length() - 1) Serial.print(' ');
    }
    Serial.println("]");
    savedSsid = ssid;
    savedPass = pass;  // will be used in setupWiFi() after AP is up
  }
  
  // Load brightness
  uint8_t br = EEPROM.read(EEPROM_BRIGHTNESS_ADDR);
  if (br > 0 && br <= 255) ledBrightness = br;

  // Load LED type
  uint8_t rgbwFlag = EEPROM.read(EEPROM_RGBW_ADDR);
  if (rgbwFlag == 0x01 || rgbwFlag == 0x00) ledRgbw = (rgbwFlag == 0x01);

  // Load LED direction
  uint8_t revFlag = EEPROM.read(EEPROM_REVERSED_ADDR);
  if (revFlag == 0x01 || revFlag == 0x00) ledReversed = (revFlag == 0x01);
  
  // Load timezone offset — accumulate via uint32_t to avoid signed-shift UB,
  // then reinterpret as int32_t.
  uint32_t tzRaw = ((uint32_t)(uint8_t)EEPROM.read(EEPROM_TZ_OFFSET_ADDR + 0) << 24) |
                   ((uint32_t)(uint8_t)EEPROM.read(EEPROM_TZ_OFFSET_ADDR + 1) << 16) |
                   ((uint32_t)(uint8_t)EEPROM.read(EEPROM_TZ_OFFSET_ADDR + 2) << 8)  |
                    (uint32_t)(uint8_t)EEPROM.read(EEPROM_TZ_OFFSET_ADDR + 3);
  int32_t tzOff = (int32_t)tzRaw;
  if (tzOff != 0 && tzOff >= -43200 && tzOff <= 43200) tz.utcOffset = tzOff;
  
  // Load display mode
  uint8_t mode = EEPROM.read(EEPROM_DISPLAY_MODE_ADDR);
  if (mode < DISPLAY_MAX) displayMode = (DisplayMode)mode;

  loadModeConfigsFromEEPROM();

  // Load debug remote logging settings
  if (EEPROM.read(EEPROM_DBG_MAGIC_ADDR) == EEPROM_DBG_MAGIC) {
    debugRemoteEnabled = (EEPROM.read(EEPROM_DBG_ENABLED_ADDR) == 0x01);
    char ipBuf[17] = {};
    for (int i = 0; i < 16; i++) ipBuf[i] = (char)EEPROM.read(EEPROM_DBG_IP_ADDR + i);
    ipBuf[16] = 0;
    debugServerIp = String(ipBuf);
    uint16_t pHi = EEPROM.read(EEPROM_DBG_PORT_ADDR);
    uint16_t pLo = EEPROM.read(EEPROM_DBG_PORT_ADDR + 1);
    uint16_t p = (pHi << 8) | pLo;
    if (p > 0 && p <= 65535) debugServerPort = p;
  }

  // Load Simple fade duration (0=off, 50–2000ms valid range)
  {
    uint16_t fHi = EEPROM.read(EEPROM_FADE_MS_ADDR);
    uint16_t fLo = EEPROM.read(EEPROM_FADE_MS_ADDR + 1);
    uint16_t fms = (fHi << 8) | fLo;
    if (fms <= 2000) simpleFadeMs = fms;  // 0 = disabled, up to 2000ms
  }

  // Load adaptive brightness settings
  if (EEPROM.read(EEPROM_AUTO_BRIGHT_ADDR) == EEPROM_AUTO_BRIGHT_MAGIC) {
    autoBrightEnabled  = (EEPROM.read(EEPROM_AUTO_BRIGHT_ADDR + 1) != 0);
    uint8_t dv = EEPROM.read(EEPROM_AUTO_BRIGHT_ADDR + 2);
    uint8_t pv = EEPROM.read(EEPROM_AUTO_BRIGHT_ADDR + 3);
    uint8_t dh = EEPROM.read(EEPROM_AUTO_BRIGHT_ADDR + 4);
    uint8_t ph = EEPROM.read(EEPROM_AUTO_BRIGHT_ADDR + 5);
    if (dh < 24 && ph < 24 && dh != ph) {
      autoBrightDimVal   = dv;
      autoBrightPeakVal  = pv;
      autoBrightDimHour  = dh;
      autoBrightPeakHour = ph;
    }
  }

  // Load button / W-channel settings
  if (EEPROM.read(EEPROM_BTN_MAGIC_ADDR) == EEPROM_BTN_MAGIC) {
    uint8_t wbl = EEPROM.read(EEPROM_W_BRIGHT_ADDR);
    if (wbl < W_BRIGHT_LEVEL_COUNT) wBrightLevel = wbl;
    ledsOff = (EEPROM.read(EEPROM_LEDS_OFF_ADDR) == 1);
  }
}

void saveFadeMs() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.write(EEPROM_FADE_MS_ADDR,     (simpleFadeMs >> 8) & 0xFF);
  EEPROM.write(EEPROM_FADE_MS_ADDR + 1, simpleFadeMs & 0xFF);
  EEPROM.commit();
}

void saveAutoBrightSettings() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.write(EEPROM_AUTO_BRIGHT_ADDR,     EEPROM_AUTO_BRIGHT_MAGIC);
  EEPROM.write(EEPROM_AUTO_BRIGHT_ADDR + 1, autoBrightEnabled ? 1 : 0);
  EEPROM.write(EEPROM_AUTO_BRIGHT_ADDR + 2, autoBrightDimVal);
  EEPROM.write(EEPROM_AUTO_BRIGHT_ADDR + 3, autoBrightPeakVal);
  EEPROM.write(EEPROM_AUTO_BRIGHT_ADDR + 4, autoBrightDimHour);
  EEPROM.write(EEPROM_AUTO_BRIGHT_ADDR + 5, autoBrightPeakHour);
  EEPROM.commit();
}

void saveButtonSettings() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.write(EEPROM_BTN_MAGIC_ADDR, EEPROM_BTN_MAGIC);
  EEPROM.write(EEPROM_W_BRIGHT_ADDR,  wBrightLevel);
  EEPROM.write(EEPROM_LEDS_OFF_ADDR,  ledsOff ? 1 : 0);
  EEPROM.commit();
}

void saveDebugConfig() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.write(EEPROM_DBG_MAGIC_ADDR, EEPROM_DBG_MAGIC);
  EEPROM.write(EEPROM_DBG_ENABLED_ADDR, debugRemoteEnabled ? 0x01 : 0x00);
  for (int i = 0; i < 16; i++) {
    EEPROM.write(EEPROM_DBG_IP_ADDR + i, (uint8_t)(i < (int)debugServerIp.length() ? debugServerIp[i] : 0));
  }
  EEPROM.write(EEPROM_DBG_PORT_ADDR,     (debugServerPort >> 8) & 0xFF);
  EEPROM.write(EEPROM_DBG_PORT_ADDR + 1, debugServerPort & 0xFF);
  EEPROM.commit();
}

void saveEEPROMSettings(const String& ssid, const String& pass) {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.write(EEPROM_MAGIC_ADDR, EEPROM_MAGIC);
  
  for (unsigned int i = 0; i < MAX_SSID_LEN; i++) {
    EEPROM.write(EEPROM_SSID_ADDR + i, i < ssid.length() ? ssid[i] : 0);
  }

  for (unsigned int i = 0; i < MAX_PASS_LEN; i++) {
    EEPROM.write(EEPROM_PASS_ADDR + i, i < pass.length() ? pass[i] : 0);
  }
  
  EEPROM.write(EEPROM_BRIGHTNESS_ADDR, ledBrightness);
  EEPROM.write(EEPROM_TZ_OFFSET_ADDR + 0, (tz.utcOffset >> 24) & 0xFF);
  EEPROM.write(EEPROM_TZ_OFFSET_ADDR + 1, (tz.utcOffset >> 16) & 0xFF);
  EEPROM.write(EEPROM_TZ_OFFSET_ADDR + 2, (tz.utcOffset >> 8) & 0xFF);
  EEPROM.write(EEPROM_TZ_OFFSET_ADDR + 3, tz.utcOffset & 0xFF);
  EEPROM.write(EEPROM_DISPLAY_MODE_ADDR, (uint8_t)displayMode);
  
  EEPROM.commit();
  Serial.println("[EEPROM] Saved settings for " + ssid);
  DLOGI("EEPROM", "Settings saved  ssid=%s  heap=%u", ssid.c_str(), ESP.getFreeHeap());
}
