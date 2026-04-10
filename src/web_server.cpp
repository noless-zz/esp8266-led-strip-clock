#include "web_server.h"
#include "globals.h"
#include "web_pages.h"
#include "network.h"
#include "storage.h"
#include "debug.h"
#include "led.h"
#include <ArduinoJson.h>
#include <Updater.h>
#include <EEPROM.h>
#include <time.h>

// All AsyncWebServer handlers (request, upload, response) run in SYS context —
// the ESP8266 cooperative scheduler's cont_yield() frame.  In SYS context,
// calling yield() (or delay(), or any function that calls them) causes
// "Panic core_esp8266_main.cpp:191 __yield  ctx: sys".
//
// Serial.printf() uses a 256-byte TX ring buffer; when the ring buffer is full it
// calls yield() to wait for the UART to drain.  The buffer can fill up when the
// periodic debug heartbeat prints large DLOGI messages (BOOT + HB ≈ 220 bytes)
// right before an upload callback fires.
//
// ets_printf() is an ESP8266 ROM function that writes directly to the UART FIFO,
// busy-waiting (not yielding) if the 128-byte HW FIFO is full.  It is safe in any
// context and is used for all logging inside SYS-context callbacks.
extern "C" int ets_printf(const char *format, ...) __attribute__((format(printf, 1, 2)));

void setupWebServer() {
  // Main page - just clock & status
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *req) { 
    req->send_P(200, "text/html", INDEX_HTML); 
  });
  
  // Settings page
  server.on("/settings.html", HTTP_GET, [](AsyncWebServerRequest *req) { 
    req->send_P(200, "text/html", SETTINGS_HTML); 
  });
  
  // API: Status
  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *req) {
    JsonDocument doc;
    time_t now = time(nullptr);
    struct tm* t = localtime(&now);
    String fullVersion = String(FW_VERSION_BASE) + " (" + FW_BUILD_TIME + ")";
    doc["fw_version"] = fullVersion;
    doc["fw_version_base"] = FW_VERSION_BASE;
    doc["fw_build_time"] = FW_BUILD_TIME;
    doc["fw_git_hash"] = FW_GIT_HASH;
    doc["time_hour"] = t->tm_hour;
    doc["time_minute"] = t->tm_min;
    doc["time_second"] = t->tm_sec;
    doc["ntp_synced"] = (now > 86400);
    doc["wifi_connected"] = wifiConnected;
    doc["wifi_ssid"] = WiFi.SSID();
    doc["wifi_rssi"] = wifiConnected ? WiFi.RSSI() : 0;
    doc["timezone"] = tz.name;
    doc["timezone_auto_detected"] = tz.autoDetected;
    doc["timezone_utc_offset_hours"] = tz.utcOffset / 3600;
    doc["timezone_utc_offset_seconds"] = tz.utcOffset;
    doc["tz_detect_source"] = tzDiag.source;
    doc["tz_detect_status"] = tzDiag.status;
    doc["tz_detect_message"] = tzDiag.message;
    doc["tz_detect_http_code"] = tzDiag.httpCode;
    doc["tz_detect_last_attempt_ms"] = tzDiag.lastAttemptMs;
    doc["tz_detect_last_success_ms"] = tzDiag.lastSuccessMs;
    doc["tz_detect_response_sample"] = tzDiag.responseSample;
    doc["brightness"] = (ledBrightness * 100) / 255;
    doc["auto_bright_enabled"]  = autoBrightEnabled;
    doc["auto_bright_dim_pct"]  = (autoBrightDimVal  * 100) / 255;
    doc["auto_bright_peak_pct"] = (autoBrightPeakVal * 100) / 255;
    doc["auto_bright_dim_hour"]  = autoBrightDimHour;
    doc["auto_bright_peak_hour"] = autoBrightPeakHour;
    doc["effective_brightness"]  = (autoBrightEnabled ? computeAutoBrightness() : ledBrightness) * 100 / 255;
    doc["led_rgbw"] = ledRgbw;
    doc["led_reversed"] = ledReversed;
    doc["display_mode"] = (int)displayMode;
    doc["boot_stage"] = (int)bootStage;
    bool _ntpSynced = (now >= 86400);
    unsigned long _ntpInterval = _ntpSynced ? 3600UL : 20UL;
    unsigned long _ntpAge = min((millis() - lastNtpSync) / 1000, _ntpInterval);
    int _ntpPct = (int)((_ntpAge * 100) / _ntpInterval);
    const char* stageNames[] = {"Booting","AP Ready","Scanning","Connecting","WiFi OK","NTP Wait","Running"};
    if (bootStage == BOOT_STAGE_RUNNING) {
      char stageBuf[48];
      snprintf(stageBuf, sizeof(stageBuf), "Running (NTP %s, refresh %d%%)",
               _ntpSynced ? "OK" : "fail", _ntpPct);
      doc["boot_stage_name"] = stageBuf;
    } else {
      doc["boot_stage_name"] = bootStage <= BOOT_STAGE_RUNNING ? stageNames[(int)bootStage] : "Unknown";
    }
    bool clockShowing = ((now >= 86400) || forceClockDisplay) && !forceStatusDisplay;
    doc["ring_mode"] = clockShowing ? "clock" : "status";
    doc["ring_force_clock"] = forceClockDisplay;
    doc["ring_force_status"] = forceStatusDisplay;
    const ModeDisplayConfig &cfg = modeConfigs[(int)displayMode];
    doc["display_cfg"]["hour"]["r"] = cfg.hourR;
    doc["display_cfg"]["hour"]["g"] = cfg.hourG;
    doc["display_cfg"]["hour"]["b"] = cfg.hourB;
    doc["display_cfg"]["minute"]["r"] = cfg.minuteR;
    doc["display_cfg"]["minute"]["g"] = cfg.minuteG;
    doc["display_cfg"]["minute"]["b"] = cfg.minuteB;
    doc["display_cfg"]["second"]["r"] = cfg.secondR;
    doc["display_cfg"]["second"]["g"] = cfg.secondG;
    doc["display_cfg"]["second"]["b"] = cfg.secondB;
    doc["display_cfg"]["width"]["hour"] = cfg.hourWidth;
    doc["display_cfg"]["width"]["minute"] = cfg.minuteWidth;
    doc["display_cfg"]["width"]["second"] = cfg.secondWidth;
    doc["display_cfg"]["spectrum"] = cfg.spectrum;
    doc["ip"] = wifiConnected ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
    doc["heap"] = ESP.getFreeHeap();
    doc["heap_frag"] = ESP.getHeapFragmentation();
    doc["debug_enabled"] = debugRemoteEnabled;
    doc["debug_ip"] = debugServerIp;
    doc["debug_port"] = debugServerPort;
    doc["simple_fade_ms"] = simpleFadeMs;
    String json;
    serializeJson(doc, json);
    req->send(200, "application/json", json);
  });
  
  // API: WiFi Scan
  server.on("/api/wifi/scan", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send(200, "application/json", getWifiScanJson());
  });
  
  // API: WiFi Connect
  server.on("/api/wifi/connect", HTTP_GET, [](AsyncWebServerRequest *req) {
    JsonDocument doc;
    
    if (req->hasParam("ssid")) {
      String ssid = req->getParam("ssid")->value();
      String pass = req->hasParam("pass") ? req->getParam("pass")->value() : "";
      
      if (startWiFiConnect(ssid, pass, true)) {
        doc["connecting"] = true;
      } else {
        doc["connecting"] = false;
        doc["error"] = "Connection already in progress or invalid SSID";
      }
    } else {
      updateWiFiConnect();
      if (wifiConnect.active) {
        doc["connecting"] = true;
      } else if (wifiConnect.success) {
        doc["connecting"] = false;
        doc["connected"] = true;
        doc["ip"] = WiFi.localIP().toString();
        wifiConnect.success = false;
      } else {
        doc["connecting"] = false;
        doc["connected"] = wifiConnected;
        if (wifiConnect.error.length() > 0) {
          doc["error"] = wifiConnect.error;
          wifiConnect.error = "";
        }
      }
    }
    
    String json;
    serializeJson(doc, json);
    req->send(200, "application/json", json);
  });
  
  // API: Brightness
  server.on("/api/brightness", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (req->hasParam("value")) {
      int val = atoi(req->getParam("value")->value().c_str());
      ledBrightness = (val * 255) / 100;
      if (ledStrip) ledStrip->setBrightness(ledBrightness);
    }
    req->send(200, "text/plain", "OK");
  });

  // API: Adaptive brightness settings
  server.on("/api/autobright", HTTP_GET, [](AsyncWebServerRequest *req) {
    bool anyParam = false;
    if (req->hasParam("enabled"))   { autoBrightEnabled  = req->getParam("enabled")->value() == "1"; anyParam = true; }
    if (req->hasParam("dim_pct"))   { int v = atoi(req->getParam("dim_pct")->value().c_str());  if (v >= 0 && v <= 100) { autoBrightDimVal  = (uint8_t)((v * 255) / 100); anyParam = true; } }
    if (req->hasParam("peak_pct"))  { int v = atoi(req->getParam("peak_pct")->value().c_str()); if (v >= 0 && v <= 100) { autoBrightPeakVal = (uint8_t)((v * 255) / 100); anyParam = true; } }
    if (req->hasParam("dim_hour"))  { int v = atoi(req->getParam("dim_hour")->value().c_str());  if (v >= 0 && v < 24 && v != (int)autoBrightPeakHour) { autoBrightDimHour  = (uint8_t)v; anyParam = true; } }
    if (req->hasParam("peak_hour")) { int v = atoi(req->getParam("peak_hour")->value().c_str()); if (v >= 0 && v < 24 && v != (int)autoBrightDimHour)  { autoBrightPeakHour = (uint8_t)v; anyParam = true; } }
    if (anyParam) saveAutoBrightSettings();
    uint8_t effectiveBr = autoBrightEnabled ? computeAutoBrightness() : ledBrightness;
    String json = String("{\"enabled\":") + (autoBrightEnabled ? "true" : "false") +
      ",\"dim_pct\":"   + (int)(autoBrightDimVal  * 100 / 255) +
      ",\"peak_pct\":"  + (int)(autoBrightPeakVal * 100 / 255) +
      ",\"dim_hour\":"  + autoBrightDimHour +
      ",\"peak_hour\":" + autoBrightPeakHour +
      ",\"effective_pct\":" + (int)(effectiveBr * 100 / 255) + "}";
    req->send(200, "application/json", json);
  });

  // API: LED type (RGB / RGBW)
  server.on("/api/ledtype", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (req->hasParam("rgbw")) {
      bool newRgbw = (req->getParam("rgbw")->value() == "1");
      if (newRgbw != ledRgbw) {
        ledRgbw = newRgbw;
        EEPROM.begin(EEPROM_SIZE);
        EEPROM.write(EEPROM_RGBW_ADDR, ledRgbw ? 0x01 : 0x00);
        EEPROM.commit();
        setupLEDs();  // reinit strip with correct pixel type
        Serial.printf("[LED] Switched to %s mode\n", ledRgbw ? "RGBW" : "RGB");
      }
    }
    String json = String("{\"rgbw\":") + (ledRgbw ? "true" : "false") + "}";
    req->send(200, "application/json", json);
  });
  
  // API: LED direction
  server.on("/api/leddirection", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (req->hasParam("reversed")) {
      bool newReversed = (req->getParam("reversed")->value() == "1");
      if (newReversed != ledReversed) {
        ledReversed = newReversed;
        EEPROM.begin(EEPROM_SIZE);
        EEPROM.write(EEPROM_REVERSED_ADDR, ledReversed ? 0x01 : 0x00);
        EEPROM.commit();
        Serial.printf("[LED] Direction: %s\n", ledReversed ? "reversed" : "normal");
      }
    }
    String json = String("{\"reversed\":") + (ledReversed ? "true" : "false") + "}";
    req->send(200, "application/json", json);
  });


  // API: Remote debug logging config
  server.on("/api/debug", HTTP_GET, [](AsyncWebServerRequest *req) {
    bool changed = false;
    if (req->hasParam("enabled")) {
      debugRemoteEnabled = (req->getParam("enabled")->value() == "1");
      changed = true;
    }
    if (req->hasParam("ip")) {
      String newIp = req->getParam("ip")->value();
      newIp.trim();
      if (newIp.length() <= 15) { debugServerIp = newIp; changed = true; }
    }
    if (req->hasParam("port")) {
      int p = atoi(req->getParam("port")->value().c_str());
      if (p > 0 && p < 65536) { debugServerPort = (uint16_t)p; changed = true; }
    }
    if (changed) saveDebugConfig();
    if (req->hasParam("test")) {
      // DLOGI is unsafe in sys context: UDP endPacket() may call yield() → panic.
      // Use ets_printf() for Serial-only output; UDP delivery is skipped here.
      ets_printf("[%lu][INF][TEST] Debug test from web UI  heap=%u  uptime=%lus\n",
                 millis(), ESP.getFreeHeap(), millis() / 1000);
    }
    String json = String("{\"enabled\":") + (debugRemoteEnabled ? "true" : "false") +
                  ",\"ip\":\"" + debugServerIp + "\",\"port\":" + debugServerPort + "}";
    req->send(200, "application/json", json);
  });

  // API: Simple mode fade duration  (?ms=400 to set, no params to read)
  server.on("/api/simple/fade", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (req->hasParam("ms")) {
      int ms = atoi(req->getParam("ms")->value().c_str());
      if (ms >= 0 && ms <= 2000) {
        simpleFadeMs = (uint32_t)ms;
        saveFadeMs();
      }
    }
    String json = String("{\"fade_ms\":") + simpleFadeMs + "}";
    req->send(200, "application/json", json);
  });

  // API: Ring control --" force_clock=1 â†' show clock, force_clock=0 â†' show status animation
  server.on("/api/ring", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (req->hasParam("force_clock")) {
      bool wantClock = (req->getParam("force_clock")->value() == "1");
      forceClockDisplay  = wantClock;
      forceStatusDisplay = !wantClock;
    }
    bool ntpSynced = (time(nullptr) >= 86400);
    bool clockShowing = (ntpSynced || forceClockDisplay) && !forceStatusDisplay;
    const char* stageNames[] = {"Booting","AP Ready","Scanning","Connecting","WiFi OK","NTP Wait","Running"};
    const char* stageName = bootStage <= BOOT_STAGE_RUNNING ? stageNames[(int)bootStage] : "Unknown";
    String json = String("{\"ring_mode\":\"") + (clockShowing ? "clock" : "status") +
                  "\",\"boot_stage\":" + (int)bootStage +
                  ",\"boot_stage_name\":\"" + stageName +
                  "\",\"force_clock\":" + (forceClockDisplay ? "true" : "false") +
                  ",\"force_status\":" + (forceStatusDisplay ? "true" : "false") + "}";
    req->send(200, "application/json", json);
  });

  // API: Display Mode
  server.on("/api/display", HTTP_GET, [](AsyncWebServerRequest *req) {
    JsonDocument doc;
    doc["current_mode"] = displayMode;
    doc["available_modes"]["SOLID"] = DISPLAY_SOLID;
    doc["available_modes"]["SIMPLE"] = DISPLAY_SIMPLE;
    doc["available_modes"]["PULSE"] = DISPLAY_PULSE;
    doc["available_modes"]["BINARY"] = DISPLAY_BINARY;
    doc["available_modes"]["HOUR_MARKER"] = DISPLAY_HOUR_MARKER;
    doc["available_modes"]["FLAME"] = DISPLAY_FLAME;
    doc["available_modes"]["PASTEL"] = DISPLAY_PASTEL;
    doc["available_modes"]["NEON"] = DISPLAY_NEON;
    doc["available_modes"]["COMET"] = DISPLAY_COMET;
    
    if (req->hasParam("mode")) {
      int newMode = atoi(req->getParam("mode")->value().c_str());
      if (newMode >= 0 && newMode < DISPLAY_MAX) {
        displayMode = (DisplayMode)newMode;
        saveDisplayModeToEEPROM();
        doc["changed"] = true;
        doc["new_mode"] = displayMode;
      } else {
        doc["error"] = "Invalid mode";
      }
    }

    const ModeDisplayConfig &cfg = modeConfigs[(int)displayMode];
    doc["config"]["hour"]["r"] = cfg.hourR;
    doc["config"]["hour"]["g"] = cfg.hourG;
    doc["config"]["hour"]["b"] = cfg.hourB;
    doc["config"]["minute"]["r"] = cfg.minuteR;
    doc["config"]["minute"]["g"] = cfg.minuteG;
    doc["config"]["minute"]["b"] = cfg.minuteB;
    doc["config"]["second"]["r"] = cfg.secondR;
    doc["config"]["second"]["g"] = cfg.secondG;
    doc["config"]["second"]["b"] = cfg.secondB;
    doc["config"]["width"]["hour"] = cfg.hourWidth;
    doc["config"]["width"]["minute"] = cfg.minuteWidth;
    doc["config"]["width"]["second"] = cfg.secondWidth;
    doc["config"]["spectrum"] = cfg.spectrum;
    
    String response;
    serializeJson(doc, response);
    req->send(200, "application/json", response);
  });

  // API: Display Mode Configuration (per mode, persistent)
  server.on("/api/mode/config", HTTP_GET, [](AsyncWebServerRequest *req) {
    JsonDocument doc;

    int mode = (int)displayMode;
    if (req->hasParam("mode")) {
      mode = atoi(req->getParam("mode")->value().c_str());
    }

    if (mode < 0 || mode >= DISPLAY_MAX) {
      doc["ok"] = false;
      doc["error"] = "Invalid mode";
      String out;
      serializeJson(doc, out);
      req->send(400, "application/json", out);
      return;
    }

    bool shouldPersist = false;

    if (req->hasParam("reset")) {
      modeConfigs[mode] = defaultModeConfigFor((uint8_t)mode);
      shouldPersist = true;
      doc["reset"] = true;
    }

    if (req->hasParam("set")) {
      ModeDisplayConfig cfg = modeConfigs[mode];
      auto parseByte = [&](const char* key, uint8_t currentValue) -> uint8_t {
        if (!req->hasParam(key)) return currentValue;
        int v = atoi(req->getParam(key)->value().c_str());
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        return (uint8_t)v;
      };

      auto parseWidth = [&](const char* key, uint8_t currentValue) -> uint8_t {
        if (!req->hasParam(key)) return currentValue;
        int v = atoi(req->getParam(key)->value().c_str());
        if (v < 1) v = 1;
        if (v > 30) v = 30;
        return (uint8_t)v;
      };

      cfg.hourR = parseByte("hr", cfg.hourR);
      cfg.hourG = parseByte("hg", cfg.hourG);
      cfg.hourB = parseByte("hb", cfg.hourB);
      cfg.minuteR = parseByte("mr", cfg.minuteR);
      cfg.minuteG = parseByte("mg", cfg.minuteG);
      cfg.minuteB = parseByte("mb", cfg.minuteB);
      cfg.secondR = parseByte("sr", cfg.secondR);
      cfg.secondG = parseByte("sg", cfg.secondG);
      cfg.secondB = parseByte("sb", cfg.secondB);
      cfg.hourWidth = parseWidth("hw", cfg.hourWidth);
      cfg.minuteWidth = parseWidth("mw", cfg.minuteWidth);
      cfg.secondWidth = parseWidth("sw", cfg.secondWidth);
      cfg.spectrum = (uint8_t)min(2, max(0, req->hasParam("sp") ? atoi(req->getParam("sp")->value().c_str()) : (int)cfg.spectrum));

      if (!isModeConfigValid(cfg)) {
        doc["ok"] = false;
        doc["error"] = "Invalid config values";
        String out;
        serializeJson(doc, out);
        req->send(400, "application/json", out);
        return;
      }

      modeConfigs[mode] = cfg;
      int persistFlag = req->hasParam("persist") ? atoi(req->getParam("persist")->value().c_str()) : 1;
      if (persistFlag != 0) shouldPersist = true;
      doc["saved"] = true;
      doc["persisted"] = (persistFlag != 0);
    }

    if (shouldPersist) {
      saveModeConfigToEEPROM((uint8_t)mode);
      doc["persisted"] = true;
    }

    const ModeDisplayConfig &cfg = modeConfigs[mode];
    doc["ok"] = true;
    doc["mode"] = mode;
    doc["hour"]["r"] = cfg.hourR;
    doc["hour"]["g"] = cfg.hourG;
    doc["hour"]["b"] = cfg.hourB;
    doc["minute"]["r"] = cfg.minuteR;
    doc["minute"]["g"] = cfg.minuteG;
    doc["minute"]["b"] = cfg.minuteB;
    doc["second"]["r"] = cfg.secondR;
    doc["second"]["g"] = cfg.secondG;
    doc["second"]["b"] = cfg.secondB;
    doc["width"]["hour"] = cfg.hourWidth;
    doc["width"]["minute"] = cfg.minuteWidth;
    doc["width"]["second"] = cfg.secondWidth;
    doc["spectrum"] = cfg.spectrum;

    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
  });
  
  // API: Timezone
  server.on("/api/timezone", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (req->hasParam("mode")) {
      if (req->getParam("mode")->value() == "auto") {
        tz.autoDetected = true;
        tzDiag.status = "running";
        tzDiag.message = "auto mode requested";
        // detectTimezone() uses delay()/DLOGI internally → calls yield() → panic in sys context.
        // Reset the check timer so loop() triggers detection on the next iteration instead.
        lastTzCheck = 0;
        req->send(200, "text/plain", "Auto-detecting...");
      } else if (req->hasParam("offset")) {
        tz.utcOffset = (int32_t)(atof(req->getParam("offset")->value().c_str()) * 3600);
        tz.autoDetected = false;
        tz.name = "Manual";
        // syncTimeNTP() uses delay() internally → calls yield() → panic in sys context.
        // Reset the NTP timer so loop() re-syncs with the new UTC offset on the next iteration.
        lastNtpSync = 0;
        req->send(200, "text/plain", "Manual timezone set");
      } else {
        req->send(400, "text/plain", "Missing offset param");
      }
    } else {
      req->send(400, "text/plain", "Missing mode param");
    }
  });
  
  // API: NTP Sync
  server.on("/api/ntp", HTTP_GET, [](AsyncWebServerRequest *req) {
    // syncTimeNTP() uses delay() internally → calls yield() → panic in sys context.
    // Reset the NTP timer so loop() triggers a sync on the next iteration instead.
    lastNtpSync = 0;
    req->send(200, "text/plain", "Syncing...");
  });
  
  // API: OTA Precheck
  server.on("/api/update/precheck", HTTP_GET, [](AsyncWebServerRequest *req) {
    JsonDocument doc;
    if (!req->hasParam("name") || !req->hasParam("size") || !req->hasParam("magic")) {
      doc["ok"] = false;
      doc["error"] = "Missing parameters";
    } else {
      String name = req->getParam("name")->value();
      size_t size = (size_t) req->getParam("size")->value().toInt();
      int magic = req->getParam("magic")->value().toInt();
      uint32_t maxSize = getMaxUpdateSize();
      
      bool extOk = name.endsWith(".bin") || name.endsWith(".BIN");
      bool sizeOk = size > 0 && size <= maxSize;
      bool magicOk = (magic == 0xE9);
      
      doc["ok"] = extOk && sizeOk && magicOk;
      if (doc["ok"].as<bool>()) {
        doc["summary"] = String(name + " (" + String(size) + " bytes)");
      } else {
        String err;
        if (!extOk) err += "Invalid file type (must be .bin). ";
        if (!sizeOk) err += "File too large (max " + String(maxSize) + " bytes). ";
        if (!magicOk) err += "Invalid firmware magic. ";
        doc["error"] = err;
      }
    }
    String json;
    serializeJson(doc, json);
    req->send(200, "application/json", json);
  });
  
  // API: OTA from URL (device downloads firmware from GitHub and flashes itself).
  // Accepts both GET and POST so the browser JS (fetch POST) and the stress-test
  // script (GET) both work without body-parsing issues on empty POST bodies.
  // Uses /api/ota/from-url (not a sub-path of /api/update) to avoid
  // ESPAsyncWebServer's prefix-match behaviour.
  server.on("/api/ota/from-url", HTTP_ANY, [](AsyncWebServerRequest *req) {
    if (!req->hasParam("url")) {
      req->send(400, "application/json", "{\"ok\":false,\"error\":\"Missing url parameter\"}");
      return;
    }
    if (!wifiConnected) {
      req->send(400, "application/json", "{\"ok\":false,\"error\":\"Device not connected to WiFi\"}");
      return;
    }
    String url = req->getParam("url")->value();
    if (url.length() == 0) {
      req->send(400, "application/json", "{\"ok\":false,\"error\":\"Empty URL\"}");
      return;
    }
    otaFromUrl = url;
    otaFromUrlAt = millis();
    req->send(202, "application/json", "{\"ok\":true,\"status\":\"downloading\"}");
  });

  // API: OTA Upload
  server.on("/api/update", HTTP_POST,
    // ── Response handler (SYS context) ──────────────────────────────────────
    // With the async ring-buffer design the actual flash write and Update.end()
    // happen in loop() (CONT context), AFTER this response handler has already
    // fired.  We therefore store the request pointer and let loop() send the
    // HTTP response once Update.end() has returned.
    // If begin() failed (active=false, beginOk=false) we send an immediate 500.
    [](AsyncWebServerRequest *req) {
      if (!otaAsync.active && !otaAsync.beginOk) {
        // begin() failed — send error now (no flash writes were attempted).
        uint32_t written = otaStatus.writtenBytes;
        ets_printf("[OTA] RESPONSE (begin failed)  written=%u  errStr=%s  heap=%u\n",
                      written, otaStatus.lastErrorText.c_str(), ESP.getFreeHeap());
        req->send(500, "application/json",
          String(F("{\"ok\":false,\"written\":")) + written +
          F(",\"error\":\"") + otaStatus.lastErrorText + F("\"}"));
      } else {
        // Normal path: loop() will call req->send() after Update.end() returns.
        otaAsync.req = req;
      }
    },
    // ── Upload callback (SYS context) ────────────────────────────────────────
    // MUST NOT call Update.write() or Update.end() here: both eventually call
    // _writeBuffer() → yield() → panic ("__yield ctx: sys").
    // All flash writes are deferred to loop() via the otaAsync ring buffer.
    [](AsyncWebServerRequest *req, String filename, size_t index, uint8_t *data, size_t len, bool final) {
      if (index == 0) {
        // ── First chunk: initialise per-upload state ─────────────────────────
        otaAsync.head        = 0;
        otaAsync.tail        = 0;
        otaAsync.active      = false;
        otaAsync.beginOk     = false;
        otaAsync.uploadFinal = false;
        otaAsync.overflowed  = false;
        otaAsync.req         = nullptr;

        otaStatus.inProgress    = false;
        otaStatus.lastSuccess   = false;
        otaStatus.writtenBytes  = 0;
        otaStatus.lastErrorCode = 0;
        otaStatus.lastErrorText = "";
        otaStatus.lastFileName  = filename;
        otaStatus.startedAt     = millis();
        otaStatus.finishedAt    = 0;

        // Allocate ring buffer (only held during the upload, freed by loop()).
        if (otaAsync.data == nullptr) {
          otaAsync.data = (uint8_t*)malloc(OtaAsyncBuf::CAP);
        }
        if (otaAsync.data == nullptr) {
          ets_printf("[OTA] FAILED to allocate ring buffer (%u B)  heap=%u\n",
                       (unsigned)OtaAsyncBuf::CAP, ESP.getFreeHeap());
          return;  // beginOk stays false → response handler sends 500
        }

        // Abort any leftover Updater state from a previously interrupted upload.
        Update.end(false);

        uint32_t maxSize = getMaxUpdateSize();
        ets_printf("[OTA] Upload start  file=%s  maxSize=%u  heap=%u  frag=%u\n",
                      filename.c_str(), maxSize, ESP.getFreeHeap(), ESP.getHeapFragmentation());
        if (!Update.begin(maxSize, U_FLASH)) {
          otaStatus.lastErrorCode = Update.getError();
          otaStatus.lastErrorText = Update.getErrorString();
          ets_printf("[OTA] begin FAILED  err=%u  %s  heap=%u\n",
                        (unsigned)Update.getError(), Update.getErrorString().c_str(), ESP.getFreeHeap());
          return;  // beginOk stays false
        }
        otaAsync.beginOk      = true;
        otaAsync.active       = true;
        otaStatus.inProgress  = true;
      }

      if (!otaAsync.beginOk) return;  // begin() failed — discard remaining chunks

      // ── Copy chunk into ring buffer (no flash writes, no yield) ────────────
      uint32_t avail = OtaAsyncBuf::CAP - (otaAsync.head - otaAsync.tail);
      if (avail < (uint32_t)len) {
        ets_printf("[OTA] RING BUFFER OVERFLOW  avail=%u  len=%u\n",
                     (unsigned)avail, (unsigned)len);
        otaAsync.overflowed = true;
        return;
      }
      uint32_t headMod = otaAsync.head & (OtaAsyncBuf::CAP - 1);
      uint32_t chunk1  = min((uint32_t)len, OtaAsyncBuf::CAP - headMod);
      memcpy(otaAsync.data + headMod, data, chunk1);
      if (chunk1 < (uint32_t)len) {
        memcpy(otaAsync.data, data + chunk1, len - chunk1);
      }
      otaAsync.head += len;  // single 32-bit store — atomic on Xtensa

      // Print receive progress every ~64 KB (safe: ets_printf never yields).
      static uint32_t _lastLoggedKB = 0;
      uint32_t nowKB = (uint32_t)((index + len) / 65536);
      if (index == 0) _lastLoggedKB = 0;
      if (nowKB != _lastLoggedKB) {
        _lastLoggedKB = nowKB;
        ets_printf("[OTA] received %u KB  heap=%u\n",
                     (unsigned)((index + len) / 1024), ESP.getFreeHeap());
      }

      if (final) {
        otaAsync.uploadFinal = true;  // loop() will call Update.end()
      }
    }
  );

  // Catchall
  server.onNotFound([](AsyncWebServerRequest *req) {
    req->redirect("http://" + WiFi.softAPIP().toString() + "/");
  });
  
  server.begin();
  Serial.println("[Web] Server started @ http://192.168.4.1");
}

