#include "network.h"
#include "globals.h"
#include "debug.h"
#include "storage.h"
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <ESP8266mDNS.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Updater.h>
#include <time.h>

// ets_printf() is an ESP8266 ROM function that writes directly to the UART FIFO
// (busy-wait, never yields) and is safe to call from SYS / interrupt context.
extern "C" int ets_printf(const char *format, ...) __attribute__((format(printf, 1, 2)));

void syncTimeNTP() {
  if (!wifiConnected) return;
  // Only show NTP_WAIT animation during initial boot (not on hourly refreshes)
  if (bootStage < BOOT_STAGE_RUNNING) bootStage = BOOT_STAGE_NTP_WAIT;
  Serial.println("[NTP] Syncing with " + String(NTP_SERVER));
  configTime(tz.utcOffset, 0, NTP_SERVER);
  time_t now = time(nullptr);
  int attempts = 50;
  while (now < 86400 && attempts-- > 0) { delay(100); now = time(nullptr); }
  if (now > 86400) {
    bootStage = BOOT_STAGE_RUNNING;
    Serial.println("[NTP] Time synced");
    DLOGI("NTP", "Synced  epoch=%lu  tz=%s  heap=%u", (unsigned long)now, tz.name.c_str(), ESP.getFreeHeap());
  } else {
    DLOGW("NTP", "Sync failed after %d attempts  heap=%u", 50, ESP.getFreeHeap());
  }
  lastNtpSync = millis();
}

void detectTimezone() {
  tzDiag.source = "ip-api.com";
  tzDiag.lastAttemptMs = millis();
  tzDiag.status = "running";
  tzDiag.message = "starting detection";
  tzDiag.httpCode = 0;
  tzDiag.responseSample = "";

  if (!wifiConnected) {
    tzDiag.status = "error";
    tzDiag.message = "wifi not connected";
    Serial.println("[GEO] Not connected to WiFi, skipping timezone detection");
    return;
  }
  DLOGI("TZ", "Auto-detect start  heap=%u", ESP.getFreeHeap());

  struct TzProvider { const char* host; const char* path; const char* label; };
  TzProvider providers[] = {
    {"ip-api.com", "/json/?fields=status,message,timezone,offset,country,city", "ip-api"},
    {"ipwho.is", "/", "ipwho.is"}
  };

  bool detected = false;
  for (size_t p = 0; p < (sizeof(providers) / sizeof(providers[0])); p++) {
    WiFiClient client;
    tzDiag.source = providers[p].label;

    DLOGI("TZ", "Trying %s (heap=%u)", providers[p].label, ESP.getFreeHeap());
    if (!client.connect(providers[p].host, 80)) {
      tzDiag.status = "error";
      tzDiag.message = String("connect failed to ") + providers[p].host;
      DLOGW("TZ", "%s connect failed", providers[p].label);
      continue;
    }
    DLOGI("TZ", "%s connected", providers[p].label);

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
        if (sp > 0 && (unsigned int)(sp + 4) <= line.length()) {
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
      tzDiag.status = "error";
      tzDiag.message = "empty response body";
      tzDiag.responseSample = "<empty>";
      continue;
    }

    int startObj = body.indexOf('{');
    int endObj = body.lastIndexOf('}');
    if (startObj >= 0 && endObj > startObj) {
      body = body.substring(startObj, endObj + 1);
    }

    tzDiag.responseSample = body.substring(0, 160);

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, body);
    if (error) {
      tzDiag.status = "error";
      tzDiag.message = (body[0] != '{' && body[0] != '[')
        ? ("non-json response (http " + String(tzDiag.httpCode) + ")")
        : ("json parse failed: " + String(error.c_str()));
      continue;
    }

    if (doc["status"] && doc["status"].as<String>() != "success") {
      tzDiag.status = "error";
      tzDiag.message = "api status=" + doc["status"].as<String>() + (doc["message"] ? (", " + doc["message"].as<String>()) : "");
      continue;
    }
    if (doc["success"] && !doc["success"].as<bool>()) {
      tzDiag.status = "error";
      tzDiag.message = "api success=false" + (doc["message"] ? (", " + doc["message"].as<String>()) : "");
      continue;
    }

    String tzName = "";
    int32_t tzOffset = 0;
    bool hasOffset = false;

    if (doc["timezone"] && doc["timezone"].is<const char*>()) {
      tzName = doc["timezone"].as<String>();
    } else if (doc["timezone"] && doc["timezone"].is<JsonObject>() && doc["timezone"]["id"]) {
      tzName = doc["timezone"]["id"].as<String>();
    }

    if (doc["offset"] && !doc["offset"].isNull()) {
      tzOffset = doc["offset"].as<int32_t>();
      hasOffset = true;
    } else if (doc["timezone"] && doc["timezone"].is<JsonObject>() && doc["timezone"]["offset"] && !doc["timezone"]["offset"].isNull()) {
      tzOffset = doc["timezone"]["offset"].as<int32_t>();
      hasOffset = true;
    }

    if (tzName.length() == 0 || !hasOffset) {
      tzDiag.status = "error";
      tzDiag.message = "missing timezone/offset fields";
      continue;
    }

    tz.name = tzName;
    tz.utcOffset = tzOffset;
    tz.autoDetected = true;
    tzDiag.status = "ok";
    tzDiag.message = "timezone=" + tz.name + ", offset=" + String(tz.utcOffset);
    tzDiag.lastSuccessMs = millis();
    detected = true;
    DLOGI("TZ", "OK  tz=%s  offset=%ld  via=%s  heap=%u",
          tz.name.c_str(), (long)tz.utcOffset, providers[p].label, ESP.getFreeHeap());
    break;
  }

  if (!detected) {
    DLOGE("TZ", "All providers failed: %s", tzDiag.message.c_str());
    Serial.println("[GEO] All timezone providers failed: " + tzDiag.message);
    return;
  }
  
  syncTimeNTP();
  lastTzCheck = millis();
}

static const char* wlStatusName(wl_status_t s) {
  switch (s) {
    case WL_IDLE_STATUS:     return "IDLE";
    case WL_NO_SSID_AVAIL:  return "NO_SSID_AVAIL";
    case WL_SCAN_COMPLETED:  return "SCAN_COMPLETED";
    case WL_CONNECTED:       return "CONNECTED";
    case WL_CONNECT_FAILED:  return "CONNECT_FAILED";
    case WL_CONNECTION_LOST: return "CONNECTION_LOST";
    case WL_DISCONNECTED:    return "DISCONNECTED";
    default:                 return "UNKNOWN";
  }
}

void checkWiFi() {
  static unsigned long lastCheck = 0;
  static wl_status_t lastKnownStatus = WL_IDLE_STATUS;
  if (millis() - lastCheck < 5000) return;
  lastCheck = millis();

  wl_status_t status = WiFi.status();

  // Log every status change (even outside an active connect attempt)
  if (status != lastKnownStatus) {
    Serial.printf("[WiFi] checkWiFi: status %s -> %s\n",
                  wlStatusName(lastKnownStatus), wlStatusName(status));
    lastKnownStatus = status;
  }

  bool connected = (status == WL_CONNECTED);
  if (connected != wifiConnected) {
    wifiConnected = connected;
    if (connected) {
      Serial.println("[WiFi] Connected! IP: " + WiFi.localIP().toString() + "  SSID: " + WiFi.SSID());
      if (lastTzCheck == 0) {
        detectTimezone();
      } else {
        DLOGI("WiFi", "Reconnected -- skipping TZ detect (already done)  uptime=%lus", millis()/1000);
      }
    } else {
      Serial.printf("[WiFi] Disconnected! Last status: %d (%s)\n", (int)status, wlStatusName(status));
      DLOGW("WiFi", "Disconnected  status=%s  uptime=%lus  heap=%u",
            wlStatusName(status), millis()/1000, ESP.getFreeHeap());
    }
  }
}

String getWifiScanJson() {
  JsonDocument doc;
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
      obj["enc"] = (WiFi.encryptionType(i) != ENC_TYPE_NONE);
      
      if (scanCacheCount < MAX_SCAN_CACHE) {
        scanCache[scanCacheCount].ssid = ssid;
        scanCache[scanCacheCount].rssi = WiFi.RSSI(i);
        scanCache[scanCacheCount].channel = WiFi.channel(i);
        scanCache[scanCacheCount].enc = WiFi.encryptionType(i);
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

bool startWiFiConnect(const String& ssid, const String& pass, bool saveToEeprom) {
  if (ssid.length() == 0) {
    Serial.println("[WiFi] startWiFiConnect: SSID is empty, aborting");
    return false;
  }
  if (wifiConnect.active) {
    Serial.println("[WiFi] startWiFiConnect: already connecting, aborting");
    return false;
  }

  Serial.println("[WiFi] --- Connection attempt ---");
  Serial.println("[WiFi] Target SSID : " + ssid);
  // DEBUG: print password bytes to catch any EEPROM corruption
  Serial.printf("[WiFi] Pass (%d): [", pass.length());
  for (unsigned int i = 0; i < pass.length(); i++) Serial.printf("%c", pass[i]);
  Serial.println("]");
  Serial.println("[WiFi] Current status: " + String(WiFi.status()));
  Serial.println("[WiFi] MAC address  : " + WiFi.macAddress());

  wifiConnect.active = true;
  wifiConnect.connecting = true;
  wifiConnect.attemptedSsid = ssid;
  wifiConnect.startedAt = millis();
  wifiConnect.lastStatus = WiFi.status();

  if (saveToEeprom) saveEEPROMSettings(ssid, pass);

  // Check scan cache for channel
  int targetChannel = 0;
  for (int i = 0; i < scanCacheCount; i++) {
    if (scanCache[i].ssid == ssid) {
      targetChannel = scanCache[i].channel;
      Serial.printf("[WiFi] Found in scan cache: ch=%d  rssi=%d  enc=%d\n",
                    scanCache[i].channel, scanCache[i].rssi, scanCache[i].enc);
      break;
    }
  }
  if (targetChannel == 0) {
    Serial.println("[WiFi] Not in scan cache -- connecting without channel hint");
  }

  // ESP8266 AP+STA constraint: both AP and STA must share the same radio channel.
  // Restart AP on the router's channel before connecting, or STA stays DISCONNECTED.
  if (targetChannel > 0 && targetChannel != (int)WiFi.channel()) {
    Serial.printf("[WiFi] Restarting AP on ch=%d to match router\n", targetChannel);
    WiFi.softAP(AP_SSID, AP_PASS, targetChannel);
  }

  Serial.printf("[WiFi] Calling WiFi.begin(\"%s\", <pass>)\n", ssid.c_str());
  WiFi.begin(ssid.c_str(), pass.c_str());
  Serial.println("[WiFi] WiFi.begin() called, polling for result...");
  DLOGI("WiFi", "Connecting  ssid=%s  ch=%d  heap=%u",
        ssid.c_str(), targetChannel, ESP.getFreeHeap());

  return true;
}

void updateWiFiConnect() {
  if (!wifiConnect.active) return;

  wl_status_t status = WiFi.status();

  // Log every status change during connection attempt
  if (status != wifiConnect.lastStatus) {
    unsigned long elapsed = millis() - wifiConnect.startedAt;
    Serial.printf("[WiFi] Status changed: %s -> %s  (+%lums)\n",
                  wlStatusName(wifiConnect.lastStatus), wlStatusName(status), elapsed);
    wifiConnect.lastStatus = status;
  }

  if (status == WL_CONNECTED) {
    wifiConnect.active = false;
    wifiConnect.connecting = false;
    wifiConnect.success = true;
    wifiConnected = true;
    bootStage = BOOT_STAGE_WIFI_OK;
    Serial.print("[WiFi] Connected! IP: ");
    Serial.println(WiFi.localIP());
    DLOGI("WiFi", "Connected  ssid=%s  ip=%s  rssi=%d  ch=%d  heap=%u",
          WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(),
          WiFi.RSSI(), (int)WiFi.channel(), ESP.getFreeHeap());
    if (!bootInfoSent) {
      bootInfoSent = true;
      DLOGI("BOOT", "fw=%s git:%s built:%s  %s", FW_VERSION_BASE, FW_GIT_HASH, FW_BUILD_TIME, cachedBootInfo.c_str());
    }
    if (lastTzCheck == 0) detectTimezone();  // skip on reconnects — already detected
  } else if (status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL) {
    wifiConnect.active = false;
    wifiConnect.connecting = false;
    wifiConnect.success = false;
    wifiConnect.error = "Connection failed";
    Serial.printf("[WiFi] Failed with status %d (%s) after %lums\n",
                  (int)status, wlStatusName(status), millis() - wifiConnect.startedAt);
    DLOGE("WiFi", "FAILED  status=%s  ssid=%s  after=%lums  heap=%u",
          wlStatusName(status), wifiConnect.attemptedSsid.c_str(),
          millis() - wifiConnect.startedAt, ESP.getFreeHeap());
  } else if (millis() - wifiConnect.startedAt > 20000) {
    // Timeout -- clear state, trigger a fresh scan, retry after scan completes
    wifiConnect.active = false;
    wifiConnect.connecting = false;
    wifiConnect.success = false;
    wifiConnect.error = "Connection timeout";
    Serial.printf("[WiFi] Timeout! Last status: %d (%s) -- rescanning and will retry\n",
                  (int)status, wlStatusName(status));
    DLOGW("WiFi", "TIMEOUT  status=%s  ssid=%s  heap=%u",
          wlStatusName(status), wifiConnect.attemptedSsid.c_str(), ESP.getFreeHeap());
    WiFi.disconnect(false);
    WiFi.scanNetworks(true, true);  // async rescan; boot auto-connect logic will retry
  } else {
    // Heartbeat every 5s so we can see it's still trying
    static unsigned long lastHeartbeat = 0;
    if (millis() - lastHeartbeat > 5000) {
      lastHeartbeat = millis();
      Serial.printf("[WiFi] Still connecting... status=%s  elapsed=%lus\n",
                    wlStatusName(status), (millis() - wifiConnect.startedAt) / 1000);
    }
  }
}

uint32_t getMaxUpdateSize() {
  return (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
}

const char* updateErrorToString(uint8_t error) {
  switch (error) {
    case UPDATE_ERROR_OK: return "OK";
    case UPDATE_ERROR_WRITE: return "Flash write failed";
    case UPDATE_ERROR_ERASE: return "Flash erase failed";
    case UPDATE_ERROR_READ: return "Flash read failed";
    case UPDATE_ERROR_SPACE: return "Not enough flash space";
    case UPDATE_ERROR_SIZE: return "Binary size mismatch";
    case UPDATE_ERROR_STREAM: return "Upload stream timeout/error";
    case UPDATE_ERROR_MD5: return "MD5 validation failed";
    case UPDATE_ERROR_MAGIC_BYTE: return "Invalid firmware magic byte";
    case UPDATE_ERROR_FLASH_CONFIG: return "Flash config mismatch";
    case UPDATE_ERROR_NEW_FLASH_CONFIG: return "New flash config invalid";
    case UPDATE_ERROR_BOOTSTRAP: return "Bootstrap validation failed";
    case UPDATE_ERROR_OOM: return "Out of memory during update";
    case UPDATE_ERROR_NO_DATA: return "No OTA data received";
    default: return "Unknown OTA error";
  }
}

void setupWiFi() {
  Serial.println("[WiFi] Starting AP+STA mode...");
  WiFi.mode(WIFI_AP_STA);

  // Log when a device connects/disconnects from our AP.
  // WiFi event callbacks run in SYS (interrupt) context — use ets_printf(), not
  // Serial.printf().  Serial.printf() can call yield() when the TX ring buffer is full,
  // which crashes the device from interrupt context.
  WiFi.onSoftAPModeStationConnected([](const WiFiEventSoftAPModeStationConnected& e) {
    char mac[18];
    snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
             e.mac[0], e.mac[1], e.mac[2], e.mac[3], e.mac[4], e.mac[5]);
    ets_printf("[AP] Client connected:    MAC=%s  aid=%d\n", mac, e.aid);
  });
  WiFi.onSoftAPModeStationDisconnected([](const WiFiEventSoftAPModeStationDisconnected& e) {
    char mac[18];
    snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
             e.mac[0], e.mac[1], e.mac[2], e.mac[3], e.mac[4], e.mac[5]);
    ets_printf("[AP] Client disconnected: MAC=%s  aid=%d\n", mac, e.aid);
  });

  WiFi.softAP(AP_SSID, AP_PASS);
  bootStage = BOOT_STAGE_AP_UP;
  Serial.println("[WiFi] AP: " + String(AP_SSID));
  Serial.println("[WiFi] AP IP: " + WiFi.softAPIP().toString());

}

void setupDNS() {
  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
  Serial.println("[DNS] Captive portal ready");
}

void setupOTA() {
  ArduinoOTA.setPassword(OTA_PASS);
  ArduinoOTA.onStart([]() {
    DLOGI("OTA", "ArduinoOTA start  heap=%u", ESP.getFreeHeap());
  });
  ArduinoOTA.onEnd([]() {
    DLOGI("OTA", "ArduinoOTA end -- rebooting");
  });
  ArduinoOTA.onError([](ota_error_t err) {
    DLOGE("OTA", "ArduinoOTA error %u", (unsigned)err);
  });
  ArduinoOTA.begin();
  Serial.println("[OTA] Ready");
}

void processDNS() {
  dnsServer.processNextRequest();
}

void updateMDNS() {
  MDNS.update();
}

void handleOTA() {
  ArduinoOTA.handle();
}

void doOtaFromUrl(const String& url) {
  if (!wifiConnected) {
    Serial.println("[OTA-URL] Not connected to WiFi");
    return;
  }
  if (ESP.getFreeHeap() < 20000) {
    Serial.printf("[OTA-URL] Heap too low: %u bytes\n", ESP.getFreeHeap());
    return;
  }

  Serial.println("[OTA-URL] Starting: " + url);
  DLOGI("OTA-URL", "Start  heap=%u", ESP.getFreeHeap());

  BearSSL::WiFiClientSecure client;
  // Certificate validation is skipped: the firmware URL comes from the GitHub
  // Releases API (fetched by the browser over its own validated TLS connection),
  // and ESP8266 BearSSL cannot verify github.com / objects.githubusercontent.com
  // certificate chains without embedding a CA bundle into the firmware.
  client.setInsecure();

  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(30000);
  if (!http.begin(client, url)) {
    Serial.println("[OTA-URL] http.begin failed");
    return;
  }
  http.addHeader("User-Agent", "LED-Clock-OTA/1.0");

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("[OTA-URL] HTTP GET failed: %d\n", httpCode);
    http.end();
    return;
  }

  int contentLength = http.getSize();
  uint32_t maxSize = getMaxUpdateSize();
  Serial.printf("[OTA-URL] Content-Length: %d  maxSize: %u  heap: %u\n",
                contentLength, maxSize, ESP.getFreeHeap());

  if (contentLength > 0 && (uint32_t)contentLength > maxSize) {
    Serial.printf("[OTA-URL] Firmware too large: %d > %u\n", contentLength, maxSize);
    http.end();
    return;
  }

  uint32_t updateSize = (contentLength > 0) ? (uint32_t)contentLength : maxSize;
  if (!Update.begin(updateSize)) {
    Serial.printf("[OTA-URL] Update.begin failed: %s\n", Update.getErrorString().c_str());
    http.end();
    return;
  }

  otaStatus.inProgress = true;
  WiFiClient* stream = http.getStreamPtr();
  if (!stream) {
    Serial.println("[OTA-URL] getStreamPtr returned null, aborting");
    Update.end(false);
    http.end();
    otaStatus.inProgress = false;
    return;
  }
  uint8_t buf[256];
  size_t totalWritten = 0;
  unsigned long lastData = millis();

  while (http.connected() && (contentLength < 0 || totalWritten < (size_t)contentLength)) {
    size_t available = stream->available();
    if (available) {
      size_t toRead = min((size_t)sizeof(buf), available);
      size_t nread = stream->readBytes(buf, toRead);
      if (nread > 0) {
        size_t written = Update.write(buf, nread);
        if (written != nread) {
          Serial.printf("[OTA-URL] Update.write failed: wrote %u of %u  err=%s\n",
                        (unsigned)written, (unsigned)nread, Update.getErrorString().c_str());
          break;
        }
        totalWritten += nread;
        lastData = millis();
        if (totalWritten % 65536 == 0) {
          Serial.printf("[OTA-URL] Progress: %u KB  heap=%u\n",
                        (unsigned)(totalWritten / 1024), ESP.getFreeHeap());
        }
      }
    } else {
      if (millis() - lastData > 15000) {
        Serial.println("[OTA-URL] Stream read timeout");
        break;
      }
      ESP.wdtFeed();
      delay(1);
    }
  }

  http.end();
  otaStatus.inProgress = false;

  if (Update.end(true)) {
    Serial.printf("[OTA-URL] Flash OK  written=%u bytes  rebooting\n", (unsigned)totalWritten);
    // Do NOT call DLOGI or delay() here: both yield, and any yield after Update.end()
    // can fire the /api/update response handler (which sees the now-reset Update state
    // as written=0, success=true) and then calls delay() in sys context → crash.
    ESP.restart();
  } else {
    Serial.printf("[OTA-URL] Update.end FAILED: %s\n", Update.getErrorString().c_str());
    DLOGE("OTA-URL", "FAILED  err=%s  written=%u",
          Update.getErrorString().c_str(), (unsigned)totalWritten);
  }
}
