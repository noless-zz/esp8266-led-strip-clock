#include "network.h"
#include "globals.h"
#include "storage.h"
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <ESP8266mDNS.h>
#include <time.h>

// ============================================================================
// Setup helpers
// ============================================================================

void setupWiFi() {
  Serial.println("[WiFi] Starting AP+STA mode...");
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASS);
  WiFi.beginSmartConfig();
  Serial.println("[WiFi] AP: " + String(AP_SSID));
  Serial.println("[WiFi] AP IP: " + WiFi.softAPIP().toString());

  // Try to auto-connect to previously saved network
  WiFi.begin();
}

void setupDNS() {
  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
  Serial.println("[DNS] Captive portal ready");
}

void setupOTA() {
  ArduinoOTA.setPassword(OTA_PASS);
  ArduinoOTA.begin();
  Serial.println("[OTA] Ready");
}

// ============================================================================
// Per-loop helpers
// ============================================================================

void processDNS() {
  dnsServer.processNextRequest();
}

void updateMDNS() {
  if (mdnsStarted) {
    MDNS.update();
  } else if (WiFi.status() == WL_CONNECTED) {
    MDNS.begin("ledclock");
    mdnsStarted = true;
    Serial.println("[mDNS] Available at http://ledclock.local");
  }
}

void handleOTA() {
  ArduinoOTA.handle();
}

// ============================================================================
// WiFi status monitoring
// ============================================================================

void checkWiFi() {
  static unsigned long lastCheck = 0;
  if (millis() - lastCheck < 5000) return;
  lastCheck = millis();

  bool connected = (WiFi.status() == WL_CONNECTED);
  if (connected != wifiConnected) {
    wifiConnected = connected;
    Serial.println("[WiFi] " + String(connected ? "Connected" : "Disconnected"));
    if (connected) detectTimezone();
  }
}

// ============================================================================
// WiFi scan
// ============================================================================

String getWifiScanJson() {
  DynamicJsonDocument doc(2048);
  JsonArray arr = doc["networks"].to<JsonArray>();

  int scanState = WiFi.scanComplete();
  if (scanState == WIFI_SCAN_RUNNING) {
    doc["scanning"] = true;
  } else if (scanState == WIFI_SCAN_FAILED || scanState < 0) {
    WiFi.scanDelete();
    WiFi.scanNetworks(true, true);
    doc["scanning"] = true;
  } else {
    doc["scanning"] = false;
    scanCacheCount = 0;
    for (int i = 0; i < scanState; i++) {
      String ssid = WiFi.SSID(i);
      if (ssid.length() == 0) continue;
      JsonObject obj = arr.add<JsonObject>();
      obj["ssid"] = ssid;
      obj["rssi"] = WiFi.RSSI(i);
      obj["enc"]  = (WiFi.encryptionType(i) != ENC_TYPE_NONE);

      if (scanCacheCount < MAX_SCAN_CACHE) {
        scanCache[scanCacheCount].ssid    = ssid;
        scanCache[scanCacheCount].rssi    = WiFi.RSSI(i);
        scanCache[scanCacheCount].channel = WiFi.channel(i);
        scanCache[scanCacheCount].enc     = WiFi.encryptionType(i);
        scanCacheCount++;
      }
    }
    scanCacheUpdatedAt = millis();
    WiFi.scanDelete();
    if (scanState <= 0) {
      WiFi.scanNetworks(true, true);
      doc["scanning"] = true;
    }
  }

  String out;
  serializeJson(doc, out);
  return out;
}

// ============================================================================
// WiFi connection management
// ============================================================================

bool startWiFiConnect(const String& ssid, const String& pass) {
  if (ssid.length() == 0) return false;
  if (wifiConnect.active) return false;

  wifiConnect.active        = true;
  wifiConnect.connecting    = true;
  wifiConnect.attemptedSsid = ssid;
  wifiConnect.startedAt     = millis();
  wifiConnect.lastStatus    = WiFi.status();

  saveEEPROMSettings(ssid, pass);

  // Check scan cache for channel hint
  int targetChannel = 0;
  for (int i = 0; i < scanCacheCount; i++) {
    if (scanCache[i].ssid == ssid) {
      targetChannel = scanCache[i].channel;
      break;
    }
  }

  if (targetChannel > 0) {
    Serial.printf("[WiFi] Connecting to %s on channel %d\n",
                  ssid.c_str(), targetChannel);
    WiFi.begin(ssid.c_str(), pass.c_str(), targetChannel);
  } else {
    WiFi.begin(ssid.c_str(), pass.c_str());
  }

  return true;
}

void updateWiFiConnect() {
  if (!wifiConnect.active) return;

  wl_status_t status = WiFi.status();
  if (status == WL_CONNECTED) {
    wifiConnect.active     = false;
    wifiConnect.connecting = false;
    wifiConnect.success    = true;
    wifiConnected          = true;
    Serial.print("[WiFi] Connected! IP: ");
    Serial.println(WiFi.localIP());
    detectTimezone();
  } else if (status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL) {
    wifiConnect.active     = false;
    wifiConnect.connecting = false;
    wifiConnect.success    = false;
    wifiConnect.error      = "Connection failed";
  } else if (millis() - wifiConnect.startedAt > 30000) {
    wifiConnect.active     = false;
    wifiConnect.connecting = false;
    wifiConnect.success    = false;
    wifiConnect.error      = "Connection timeout";
  }
}

// ============================================================================
// NTP / timezone
// ============================================================================

void syncTimeNTP() {
  if (!wifiConnected) return;
  Serial.println("[NTP] Syncing with " + String(NTP_SERVER));
  configTime(tz.utcOffset, 0, NTP_SERVER);
  time_t now = time(nullptr);
  int attempts = 50;
  while (now < 86400 && attempts-- > 0) { delay(100); now = time(nullptr); }
  if (now > 86400) {
    Serial.println("[NTP] Time synced");
  }
  lastNtpSync = millis();
}

void detectTimezone() {
  tzDiag.source         = "ip-api.com";
  tzDiag.lastAttemptMs  = millis();
  tzDiag.status         = "running";
  tzDiag.message        = "starting detection";
  tzDiag.httpCode       = 0;
  tzDiag.responseSample = "";

  if (!wifiConnected) {
    tzDiag.status  = "error";
    tzDiag.message = "wifi not connected";
    Serial.println("[GEO] Not connected to WiFi, skipping timezone detection");
    return;
  }
  Serial.println("[GEO] Detecting timezone from IP geolocation...");

  struct TzProvider { const char* host; const char* path; const char* label; };
  TzProvider providers[] = {
    {"ip-api.com", "/json/?fields=status,message,timezone,offset,country,city", "ip-api"},
    {"ipwho.is",   "/",                                                          "ipwho.is"}
  };

  bool detected = false;
  for (size_t p = 0; p < (sizeof(providers) / sizeof(providers[0])); p++) {
    WiFiClient client;
    tzDiag.source = providers[p].label;

    if (!client.connect(providers[p].host, 80)) {
      tzDiag.status  = "error";
      tzDiag.message = String("connect failed to ") + providers[p].host;
      continue;
    }

    client.print("GET ");
    client.print(providers[p].path);
    client.print(" HTTP/1.0\r\nHost: ");
    client.print(providers[p].host);
    client.print("\r\nUser-Agent: LED-Clock/2.0\r\nConnection: close\r\n\r\n");

    String line;
    while (client.connected()) {
      line = client.readStringUntil('\n');
      if (line.startsWith("HTTP/")) {
        int sp = line.indexOf(' ');
        if (sp > 0 && sp + 4 <= (int)line.length()) {
          tzDiag.httpCode = line.substring(sp + 1, sp + 4).toInt();
        }
      }
      if (line == "\r") break;
    }

    String body;
    unsigned long readStart = millis();
    while (millis() - readStart < 5000) {
      while (client.available()) {
        body += (char)client.read();
        readStart = millis();
      }
      if (!client.connected() && !client.available()) break;
      delay(5);
    }
    client.stop();
    body.trim();

    if (body.length() == 0) {
      tzDiag.status         = "error";
      tzDiag.message        = "empty response body";
      tzDiag.responseSample = "<empty>";
      continue;
    }

    int startObj = body.indexOf('{');
    int endObj   = body.lastIndexOf('}');
    if (startObj >= 0 && endObj > startObj) {
      body = body.substring(startObj, endObj + 1);
    }

    tzDiag.responseSample = body.substring(0, 160);

    DynamicJsonDocument doc(1024);
    DeserializationError error = deserializeJson(doc, body);
    if (error) {
      tzDiag.status  = "error";
      tzDiag.message = (body[0] != '{' && body[0] != '[')
        ? ("non-json response (http " + String(tzDiag.httpCode) + ")")
        : ("json parse failed: " + String(error.c_str()));
      continue;
    }

    if (doc["status"] && doc["status"].as<String>() != "success") {
      tzDiag.status  = "error";
      tzDiag.message = "api status=" + doc["status"].as<String>() +
                       (doc["message"] ? (", " + doc["message"].as<String>()) : "");
      continue;
    }
    if (doc["success"] && !doc["success"].as<bool>()) {
      tzDiag.status  = "error";
      tzDiag.message = "api success=false" +
                       (doc["message"] ? (", " + doc["message"].as<String>()) : "");
      continue;
    }

    String  tzName    = "";
    int32_t tzOffset  = 0;
    bool    hasOffset = false;

    if (doc["timezone"] && doc["timezone"].is<const char*>()) {
      tzName = doc["timezone"].as<String>();
    } else if (doc["timezone"] && doc["timezone"].is<JsonObject>() && doc["timezone"]["id"]) {
      tzName = doc["timezone"]["id"].as<String>();
    }

    if (doc["offset"] && !doc["offset"].isNull()) {
      tzOffset  = doc["offset"].as<int32_t>();
      hasOffset = true;
    } else if (doc["timezone"] && doc["timezone"].is<JsonObject>() &&
               doc["timezone"]["offset"] && !doc["timezone"]["offset"].isNull()) {
      tzOffset  = doc["timezone"]["offset"].as<int32_t>();
      hasOffset = true;
    }

    if (tzName.length() == 0 || !hasOffset) {
      tzDiag.status  = "error";
      tzDiag.message = "missing timezone/offset fields";
      continue;
    }

    tz.name              = tzName;
    tz.utcOffset         = tzOffset;
    tz.autoDetected      = true;
    tzDiag.status        = "ok";
    tzDiag.message       = "timezone=" + tz.name + ", offset=" + String(tz.utcOffset);
    tzDiag.lastSuccessMs = millis();
    detected             = true;
    break;
  }

  if (!detected) {
    Serial.println("[GEO] All timezone providers failed: " + tzDiag.message);
    return;
  }

  syncTimeNTP();
  lastTzCheck = millis();
}
