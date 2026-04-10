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
      DLOGI("TEST", "Debug test from web UI  heap=%u  uptime=%lus", ESP.getFreeHeap(), millis()/1000);
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
        detectTimezone();
        req->send(200, "text/plain", "Auto-detecting...");
      } else if (req->hasParam("offset")) {
        tz.utcOffset = (int32_t)(atof(req->getParam("offset")->value().c_str()) * 3600);
        tz.autoDetected = false;
        tz.name = "Manual";
        syncTimeNTP();
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
    syncTimeNTP();
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
  // Registered BEFORE /api/update so that ESPAsyncWebServer matches this exact
  // path first and does not accidentally route it into the upload handler below.
  server.on("/api/update/from-url", HTTP_POST, [](AsyncWebServerRequest *req) {
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
    [](AsyncWebServerRequest *req) {
      // Response handler also runs in sys context — DLOGI/DLOGE send UDP which calls yield() → panic.
      // Use Serial only here; the loop() heartbeat will pick up OTA completion for UDP logging.
      //
      // Read outcome from otaStatus, which the upload callback populated BEFORE calling
      // Update.end().  Do NOT call Update.progress() here: Update.end() calls _reset()
      // internally, which zeroes _progress, so Update.progress() always returns 0 by the
      // time this response handler runs (causing a false-negative "written=0" failure).
      uint32_t written = otaStatus.writtenBytes;
      bool success = otaStatus.lastSuccess && (written > 0);
      if (success) {
        Serial.printf("[OTA] RESPONSE ok  written=%u  heap=%u\n", written, ESP.getFreeHeap());
      } else {
        Serial.printf("[OTA] RESPONSE fail  written=%u  errStr=%s  heap=%u\n",
                      written, otaStatus.lastErrorText.c_str(), ESP.getFreeHeap());
      }
      req->send(success ? 200 : 500, "application/json",
        String("{\"ok\":") + (success ? "true" : "false") +
        ",\"written\":" + String(written) +
        ",\"error\":\"" + (success ? "" : otaStatus.lastErrorText) + "\"}");
      if (success) {
        // delay() is not safe in sys context (calls yield() → panic).
        // Schedule the restart from loop() context instead; 500 ms gives lwIP time to
        // flush the HTTP response before the device reboots.
        otaRestartPending = true;
        otaRestartAt = millis();
      }
    },
    [](AsyncWebServerRequest *req, String filename, size_t index, uint8_t *data, size_t len, bool final) {
      // Static state shared across all chunks of a single upload.  The ESP8266 is
      // single-threaded and ESPAsyncWebServer never overlaps upload callbacks, so
      // these are safe without additional synchronisation.
      static bool     _beginOk      = false;  // true after Update.begin() succeeds
      static uint32_t _lastLoggedKB = 0;      // tracks last logged progress band

      if (index == 0) {
        // Reset per-upload state at the start of every new upload.
        _beginOk      = false;
        _lastLoggedKB = 0;
        otaStatus.inProgress    = false;
        otaStatus.lastSuccess   = false;
        otaStatus.writtenBytes  = 0;
        otaStatus.lastErrorCode = 0;
        otaStatus.lastErrorText = "";
        otaStatus.lastFileName  = filename;
        otaStatus.startedAt     = millis();
        otaStatus.finishedAt    = 0;

        // If a previous upload was interrupted before Update.end() was called
        // (e.g., client dropped the connection mid-stream), the Updater library
        // remains in a "started" state and a subsequent Update.begin() will fail.
        // Abort it unconditionally; end(false) is a no-op when not running.
        Update.end(false);

        // runAsync(false) = synchronous writes — we feed WDT manually between chunks.
        // runAsync(true) defers writes to a queue but Update.process() must be called
        // from loop() to drain it; without that, all writes flush at once in one huge
        // blocking burst and the lwIP stack crashes (Soft WDT, exccause=4, 0x4024xxxx).
        uint32_t maxSize = getMaxUpdateSize();
        // All logging in this callback MUST use Serial only.
        // DLOGI/DLOGE send UDP packets which call yield() internally — illegal in sys context
        // and causes "Panic core_esp8266_main.cpp:191 __yield  ctx: sys".
        Serial.printf("[OTA] Upload start  file=%s  maxSize=%u  heap=%u  frag=%u\n",
                      filename.c_str(), maxSize, ESP.getFreeHeap(), ESP.getHeapFragmentation());
        if (!Update.begin(maxSize, U_FLASH)) {
          otaStatus.lastErrorCode = Update.getError();
          otaStatus.lastErrorText = Update.getErrorString();
          Serial.printf("[OTA] begin FAILED  err=%u  %s  heap=%u\n",
                        (unsigned)Update.getError(), Update.getErrorString().c_str(), ESP.getFreeHeap());
          return;
        }
        _beginOk = true;
        otaStatus.inProgress = true;
      }

      // Skip write processing when begin() failed — Update was never started so
      // Update.write() would silently return 0 and pollute the log.
      if (!_beginOk) return;

      // wdtFeed() is safe in any context; yield() is not — do not add it here.
      ESP.wdtFeed();
      size_t written = Update.write(data, len);
      if (written != len) {
        Serial.printf("[OTA] write FAILED  offset=%u  want=%u  got=%u  %s\n",
                      (unsigned)index, (unsigned)len, (unsigned)written, Update.getErrorString().c_str());
      }

      // Print progress every ~64 KB
      uint32_t nowKB = (index + len) / 65536;
      if (nowKB != _lastLoggedKB) {
        _lastLoggedKB = nowKB;
        Serial.printf("[OTA] progress  %u KB  heap=%u  frag=%u\n",
                      (index + len) / 1024, ESP.getFreeHeap(), ESP.getHeapFragmentation());
      }

      if (final) {
        _lastLoggedKB = 0;
        _beginOk = false;
        otaStatus.inProgress = false;
        // Save progress BEFORE Update.end(): end() calls _reset() which zeroes _progress,
        // so Update.progress() would return 0 if read after end() returns.
        otaStatus.writtenBytes = Update.progress();
        if (Update.end(true)) {
          otaStatus.lastSuccess  = true;
          otaStatus.finishedAt   = millis();
          Serial.printf("[OTA] end OK  totalWritten=%u  heap=%u\n",
                        otaStatus.writtenBytes, ESP.getFreeHeap());
        } else {
          otaStatus.lastSuccess   = false;
          otaStatus.lastErrorCode = Update.getError();
          otaStatus.lastErrorText = Update.getErrorString();
          Serial.printf("[OTA] end FAILED  err=%u  %s  written=%u  heap=%u\n",
                        (unsigned)Update.getError(), Update.getErrorString().c_str(),
                        otaStatus.writtenBytes, ESP.getFreeHeap());
        }
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

