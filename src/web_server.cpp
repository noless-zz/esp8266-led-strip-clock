#include "web_server.h"
#include "globals.h"
#include "web_pages.h"
#include "network.h"
#include "storage.h"
#include <ArduinoJson.h>
#include <Updater.h>
#include <time.h>

// ============================================================================
// OTA size helper (used only by the web upload handler)
// ============================================================================

static uint32_t getMaxUpdateSize() {
  return (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
}

// ============================================================================
// Web server route registration
// ============================================================================

void setupWebServer() {
  // Main page
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send_P(200, "text/html", INDEX_HTML);
  });

  // Settings page
  server.on("/settings.html", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send_P(200, "text/html", SETTINGS_HTML);
  });

  // API: Status
  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest* req) {
    DynamicJsonDocument doc(1408);
    time_t now = time(nullptr);
    struct tm* t = localtime(&now);
    String fullVersion = String(FW_VERSION_BASE) + " (" + FW_BUILD_TIME + ")";
    doc["fw_version"]                  = fullVersion;
    doc["fw_version_base"]             = FW_VERSION_BASE;
    doc["fw_build_time"]               = FW_BUILD_TIME;
    doc["time_hour"]                   = t->tm_hour;
    doc["time_minute"]                 = t->tm_min;
    doc["time_second"]                 = t->tm_sec;
    doc["ntp_synced"]                  = (now > 86400);
    doc["wifi_connected"]              = wifiConnected;
    doc["wifi_ssid"]                   = WiFi.SSID();
    doc["wifi_rssi"]                   = wifiConnected ? WiFi.RSSI() : 0;
    doc["timezone"]                    = tz.name;
    doc["timezone_auto_detected"]      = tz.autoDetected;
    doc["timezone_utc_offset_hours"]   = tz.utcOffset / 3600;
    doc["timezone_utc_offset_seconds"] = tz.utcOffset;
    doc["tz_detect_source"]            = tzDiag.source;
    doc["tz_detect_status"]            = tzDiag.status;
    doc["tz_detect_message"]           = tzDiag.message;
    doc["tz_detect_http_code"]         = tzDiag.httpCode;
    doc["tz_detect_last_attempt_ms"]   = tzDiag.lastAttemptMs;
    doc["tz_detect_last_success_ms"]   = tzDiag.lastSuccessMs;
    doc["tz_detect_response_sample"]   = tzDiag.responseSample;
    doc["brightness"]                  = (ledBrightness * 100) / 255;
    doc["display_mode"]                = (int)displayMode;
    doc["ip"]   = wifiConnected ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
    doc["heap"] = ESP.getFreeHeap();
    String json;
    serializeJson(doc, json);
    req->send(200, "application/json", json);
  });

  // API: WiFi Scan
  server.on("/api/wifi/scan", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send(200, "application/json", getWifiScanJson());
  });

  // API: WiFi Connect
  server.on("/api/wifi/connect", HTTP_GET, [](AsyncWebServerRequest* req) {
    DynamicJsonDocument doc(256);

    if (req->hasParam("ssid")) {
      String ssid = req->getParam("ssid")->value();
      String pass = req->hasParam("pass") ? req->getParam("pass")->value() : "";

      if (startWiFiConnect(ssid, pass)) {
        doc["connecting"] = true;
      } else {
        doc["connecting"] = false;
        doc["error"]      = "Connection already in progress or invalid SSID";
      }
    } else {
      updateWiFiConnect();
      if (wifiConnect.active) {
        doc["connecting"] = true;
      } else if (wifiConnect.success) {
        doc["connecting"] = false;
        doc["connected"]  = true;
        doc["ip"]         = WiFi.localIP().toString();
        wifiConnect.success = false;
      } else {
        doc["connecting"] = false;
        doc["connected"]  = wifiConnected;
        if (wifiConnect.error.length() > 0) {
          doc["error"]       = wifiConnect.error;
          wifiConnect.error  = "";
        }
      }
    }

    String json;
    serializeJson(doc, json);
    req->send(200, "application/json", json);
  });

  // API: Brightness
  server.on("/api/brightness", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (req->hasParam("value")) {
      int val       = atoi(req->getParam("value")->value().c_str());
      ledBrightness = (val * 255) / 100;
      if (ledStrip) ledStrip->setBrightness(ledBrightness);
    }
    req->send(200, "text/plain", "OK");
  });

  // API: Display Mode
  server.on("/api/display", HTTP_GET, [](AsyncWebServerRequest* req) {
    DynamicJsonDocument doc(512);
    doc["current_mode"]                   = displayMode;
    doc["available_modes"]["SOLID"]       = DISPLAY_SOLID;
    doc["available_modes"]["SIMPLE"]      = DISPLAY_SIMPLE;
    doc["available_modes"]["PULSE"]       = DISPLAY_PULSE;
    doc["available_modes"]["BINARY"]      = DISPLAY_BINARY;
    doc["available_modes"]["HOUR_MARKER"] = DISPLAY_HOUR_MARKER;
    doc["available_modes"]["FLAME"]       = DISPLAY_FLAME;
    doc["available_modes"]["PASTEL"]      = DISPLAY_PASTEL;
    doc["available_modes"]["NEON"]        = DISPLAY_NEON;
    doc["available_modes"]["COMET"]       = DISPLAY_COMET;

    if (req->hasParam("mode")) {
      int newMode = atoi(req->getParam("mode")->value().c_str());
      if (newMode >= 0 && newMode < DISPLAY_MAX) {
        displayMode     = (DisplayMode)newMode;
        doc["changed"]  = true;
        doc["new_mode"] = displayMode;
      } else {
        doc["error"] = "Invalid mode";
      }
    }

    String response;
    serializeJson(doc, response);
    req->send(200, "application/json", response);
  });

  // API: Timezone
  server.on("/api/timezone", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (req->hasParam("mode")) {
      if (req->getParam("mode")->value() == "auto") {
        tz.autoDetected  = true;
        tzDiag.status    = "running";
        tzDiag.message   = "auto mode requested";
        detectTimezone();
        req->send(200, "text/plain", "Auto-detecting...");
      } else if (req->hasParam("offset")) {
        tz.utcOffset    = (int32_t)(atof(req->getParam("offset")->value().c_str()) * 3600);
        tz.autoDetected = false;
        tz.name         = "Manual";
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
  server.on("/api/ntp", HTTP_GET, [](AsyncWebServerRequest* req) {
    syncTimeNTP();
    req->send(200, "text/plain", "Syncing...");
  });

  // API: OTA Precheck
  server.on("/api/update/precheck", HTTP_GET, [](AsyncWebServerRequest* req) {
    DynamicJsonDocument doc(256);
    if (!req->hasParam("name") || !req->hasParam("size") || !req->hasParam("magic")) {
      doc["ok"]    = false;
      doc["error"] = "Missing parameters";
    } else {
      String   name    = req->getParam("name")->value();
      size_t   size    = (size_t)req->getParam("size")->value().toInt();
      int      magic   = req->getParam("magic")->value().toInt();
      uint32_t maxSize = getMaxUpdateSize();

      bool extOk   = name.endsWith(".bin") || name.endsWith(".BIN");
      bool sizeOk  = size > 0 && size <= maxSize;
      bool magicOk = (magic == 0xE9);

      doc["ok"] = extOk && sizeOk && magicOk;
      if (doc["ok"].as<bool>()) {
        doc["summary"] = String(name + " (" + String(size) + " bytes)");
      } else {
        String err;
        if (!extOk)   err += "Invalid file type (must be .bin). ";
        if (!sizeOk)  err += "File too large (max " + String(maxSize) + " bytes). ";
        if (!magicOk) err += "Invalid firmware magic. ";
        doc["error"] = err;
      }
    }
    String json;
    serializeJson(doc, json);
    req->send(200, "application/json", json);
  });

  // API: OTA Upload
  server.on("/api/update", HTTP_POST,
    [](AsyncWebServerRequest* req) {
      bool success = !Update.hasError() && Update.isFinished();
      req->send(success ? 200 : 500, "application/json",
        String("{\"ok\":") + (success ? "true" : "false") +
        ",\"written\":" + String(Update.progress()) + "}");
      if (success) {
        delay(500);
        ESP.restart();
      }
    },
    [](AsyncWebServerRequest* req, String filename,
       size_t index, uint8_t* data, size_t len, bool final) {
      if (index == 0) {
        Update.runAsync(true);
        uint32_t maxSize = getMaxUpdateSize();
        if (!Update.begin(maxSize, U_FLASH)) {
          Serial.println("[OTA] Update begin failed");
          return;
        }
        Serial.print("[OTA] Updating: ");
        Serial.println(filename);
      }

      if (Update.write(data, len) != len) {
        Update.printError(Serial);
      }

      if (final) {
        if (Update.end(true)) {
          Serial.println("\n[OTA] Update Success!");
        } else {
          Serial.println("\n[OTA] Update Failed!");
          Update.printError(Serial);
        }
      }
    }
  );

  // Catchall – redirect to captive portal
  server.onNotFound([](AsyncWebServerRequest* req) {
    req->redirect("http://" + WiFi.softAPIP().toString() + "/");
  });

  server.begin();
  Serial.println("[Web] Server started @ http://192.168.4.1");
}
