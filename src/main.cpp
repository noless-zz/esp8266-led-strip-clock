/*
 * ESP8266 LED Strip Clock
 * 60-LED NeoPixel time display with NTP sync and timezone auto-detection
 * Features: WiFi captive portal, firmware OTA upload, time sync, color blending
 */

#include "config.h"
#include "globals.h"
#include "debug.h"
#include "led.h"
#include "storage.h"
#include "buttons.h"
#include "network.h"
#include "web_server.h"
#include <ArduinoOTA.h>
#include <ESP8266mDNS.h>

// ============================================================================
// String constant definitions
// ============================================================================

const char* FW_VERSION_BASE = FW_VERSION_STR;
const char* FW_BUILD_TIME   = __DATE__ " " __TIME__;
// AP_SSID is built at runtime with last 3 MAC bytes, e.g. "LED-Clock-A1B2C3"
String AP_SSID_STR;
const char* AP_SSID = nullptr; // set in setup() after WiFi.macAddress() is available
const char* AP_PASS = "";
const char* OTA_PASS = "admin123";
const char* NTP_SERVER = "pool.ntp.org";

// ============================================================================
// W-channel brightness levels
// ============================================================================

const uint8_t W_BRIGHT_LEVELS[4] = {0, 85, 170, 255}; // 0%, ~33%, ~67%, 100%

// ============================================================================
// Global variable definitions
// ============================================================================

// LED globals
Adafruit_NeoPixel *ledStrip = nullptr;
uint8_t ledBrightness = 76;  // 30%
bool    autoBrightEnabled  = false;
uint8_t autoBrightDimVal   = 26;   // 10% of 255  (night minimum)
uint8_t autoBrightPeakVal  = 255;  // 100%        (day maximum)
uint8_t autoBrightDimHour  = 2;    // 2 am
uint8_t autoBrightPeakHour = 14;   // 2 pm
bool ledRgbw = false;        // false=RGB, true=RGBW
bool ledReversed = false;    // false=normal (0→59), true=reversed (59→0)
DisplayMode displayMode = DISPLAY_SOLID;
bool ledsOff = false;        // true = LEDs blanked via button; persisted to EEPROM
uint8_t wBrightLevel = 0;    // W channel brightness level index (0-3)
CRGB leds[NUM_LEDS];
ModeDisplayConfig modeConfigs[DISPLAY_MAX];

// Web server
AsyncWebServer server(80);
DNSServer dnsServer;
bool mdnsStarted = false;

// WiFi state
ScanCacheEntry scanCache[MAX_SCAN_CACHE];
int scanCacheCount = 0;
unsigned long scanCacheUpdatedAt = 0;
WiFiConnectState wifiConnect;
bool wifiConnected = false;
bool forceClockDisplay = false;   // show clock even before NTP
bool forceStatusDisplay = false;  // show boot animation even after NTP synced
String savedSsid;
String savedPass;

// Time globals
unsigned long lastNtpSync = 0, lastTzCheck = 0;
TimezoneState tz = {0, "UTC", true};
TzDiagState tzDiag;

// OTA globals
OTAStatus otaStatus;
String otaFromUrl = "";
unsigned long otaFromUrlAt = 0;

// Debug globals
bool debugRemoteEnabled = false;
String debugServerIp = "";
uint16_t debugServerPort = 7878;
WiFiUDP debugUdp;
String cachedBootInfo = "";
bool bootInfoSent = false;

// Button globals
ButtonState btn1State = {false, 0, false};
ButtonState btn2State = {false, 0, false};

// Boot stage
BootStage bootStage = BOOT_STAGE_INIT;

// LED animation globals
uint32_t simpleFadeMs = 400;

void setup() {
  Serial.begin(115200);
  delay(100);

  // Build unique AP SSID from last 3 MAC bytes, e.g. "LED-Clock-A1B2C3"
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char macSuffix[7];
  snprintf(macSuffix, sizeof(macSuffix), "%02X%02X%02X", mac[3], mac[4], mac[5]);
  AP_SSID_STR = String("LED-Clock-") + macSuffix;
  AP_SSID = AP_SSID_STR.c_str();

  Serial.println("\n\n");
  Serial.println("  ESP8266 LED Strip Clock v" + String(FW_VERSION_BASE) + " (" + FW_BUILD_TIME + ")");
  Serial.println("  60-LED NeoPixel Clock with NTP Sync");
  Serial.println("\n");
  
  captureBootInfo();
  setupLEDs();
  setupButtons();
  loadEEPROMSettings();
  setupWiFi();
  setupDNS();
  setupOTA();
  setupWebServer();
  
  WiFi.scanNetworks(true, true);  // Start first scan
  
  Serial.println("[READY] Access point: " + String(AP_SSID));
  Serial.println("[READY] Open browser: http://192.168.4.1\n");
}

void loop() {
  handleButtons();

  // Auto-connect: wait for a scan to finish so we have a channel hint, then connect.
  // Also handles retries after timeout (updateWiFiConnect triggers a rescan on timeout).
  if (!wifiConnected && !wifiConnect.active && savedSsid.length() > 0) {
    int scanState = WiFi.scanComplete();
    if (scanState == WIFI_SCAN_RUNNING) {
      if (bootStage < BOOT_STAGE_SCANNING) bootStage = BOOT_STAGE_SCANNING;
    } else if (scanState >= 0) {
      Serial.printf("[WiFi] Scan done (%d networks). Connecting to: %s\n",
                    scanState, savedSsid.c_str());
      getWifiScanJson();  // populate scan cache

      bootStage = BOOT_STAGE_STA_CONN;
      startWiFiConnect(savedSsid, savedPass);  // AP channel switch now done inside startWiFiConnect
    } else if (scanState == WIFI_SCAN_FAILED) {
      Serial.println("[WiFi] Scan failed, connecting without channel hint");
      bootStage = BOOT_STAGE_STA_CONN;
      startWiFiConnect(savedSsid, savedPass);
    }
    // WIFI_SCAN_RUNNING: keep waiting
  }

  dnsServer.processNextRequest();
  if (mdnsStarted) {
    MDNS.update();
  } else if (WiFi.status() == WL_CONNECTED && !mdnsStarted) {
    MDNS.begin("ledclock");
    mdnsStarted = true;
    Serial.println("[mDNS] Available at http://ledclock.local");
  }
  
  // Serial command interface
  // Commands:  wifi <SSID> [password]   --" connect (omit password for open networks)
  //            scan                     --" trigger scan and print results
  //            status                   --" print current WiFi/time status
  //            disconnect               --" disconnect STA
  if (Serial.available()) {
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (line.length() > 0) {
      Serial.println("[CMD] > " + line);
      if (line.equalsIgnoreCase("status")) {
        Serial.printf("[CMD] WiFi: %s  SSID: %s  IP: %s\n",
          wifiConnected ? "connected" : "disconnected",
          WiFi.SSID().c_str(),
          WiFi.localIP().toString().c_str());
        Serial.printf("[CMD] AP: %s  ch=%d\n", AP_SSID, WiFi.channel());
        time_t now = time(nullptr);
        Serial.printf("[CMD] Time: %s  NTP: %s\n",
          ctime(&now), now > 86400 ? "synced" : "waiting");
        const char* modeNames[] = {"SOLID","SIMPLE","PULSE","BINARY","HOUR_MARKER","FLAME","PASTEL","NEON","COMET"};
        Serial.printf("[CMD] Display mode: %d (%s)  bootStage: %d  brightness: %d\n",
          (int)displayMode, displayMode < DISPLAY_MAX ? modeNames[displayMode] : "?",
          (int)bootStage, ledBrightness);
      } else if (line.equalsIgnoreCase("scan")) {
        Serial.println("[CMD] Starting scan...");
        WiFi.scanNetworks(true, true);
      } else if (line.equalsIgnoreCase("disconnect")) {
        WiFi.disconnect(false);
        wifiConnected = false;
        wifiConnect.active = false;
        savedSsid = "";
        savedPass = "";
        Serial.println("[CMD] Disconnected and cleared saved credentials");
      } else if (line.startsWith("wifi ") || line.startsWith("wifi\t")) {
        String args = line.substring(5);
        args.trim();
        int space = args.indexOf(' ');
        String ssid, pass;
        if (space < 0) {
          ssid = args;
          pass = "";
        } else {
          ssid = args.substring(0, space);
          pass = args.substring(space + 1);
          pass.trim();
        }
        if (ssid.length() == 0) {
          Serial.println("[CMD] Usage: wifi <SSID> [password]");
        } else {
          savedSsid = ssid;
          savedPass = pass;
          Serial.printf("[CMD] Connecting to \"%s\" %s\n",
            ssid.c_str(), pass.length() ? "with password" : "(open network)");
          wifiConnect.active = false;  // reset so startWiFiConnect accepts it
          startWiFiConnect(ssid, pass, true);
        }
      } else if (line.startsWith("mode ")) {
        int m = line.substring(5).toInt();
        if (m >= 0 && m < DISPLAY_MAX) {
          displayMode = (DisplayMode)m;
          saveDisplayModeToEEPROM();
          const char* modeNames[] = {"SOLID","SIMPLE","PULSE","BINARY","HOUR_MARKER","FLAME","PASTEL","NEON","COMET"};
          Serial.printf("[CMD] Display mode set to %d (%s)\n", m, modeNames[m]);
        } else {
          Serial.printf("[CMD] Mode must be 0-%d  (0=SOLID 1=SIMPLE 2=PULSE 3=BINARY 4=HOUR_MARKER 5=FLAME 6=PASTEL 7=NEON 8=COMET)\n", DISPLAY_MAX-1);
        }
      } else {
        Serial.println("[CMD] Commands: wifi <SSID> [pass]  |  scan  |  status  |  disconnect  |  mode <0-8>");
      }
    }
  }

  checkWiFi();
  updateWiFiConnect();

  ArduinoOTA.handle();

  // Trigger NTP sync after WiFi connects (skip during OTA or when heap is low)
  bool ntpSynced = (time(nullptr) > 86400);
  unsigned long ntpInterval = ntpSynced ? 3600000UL : 20000UL;
  bool otaActive = otaStatus.inProgress;
  bool heapOk = (ESP.getFreeHeap() >= 22000);
  if (!otaActive && heapOk && wifiConnected && millis() - lastNtpSync > ntpInterval) {
    syncTimeNTP();
  } else if (!heapOk && wifiConnected) {
    DLOGE("HEAP", "Skip NTP sync, heap too low: %u frag=%u", ESP.getFreeHeap(), ESP.getHeapFragmentation());
  }

  // Refresh timezone daily if auto-detected (skip during OTA or low heap)
  if (!otaActive && heapOk && wifiConnected && tz.autoDetected && millis() - lastTzCheck > 86400000) {
    detectTimezone();
  }
  
  // Periodic RTC uptime save + debug heartbeat (every 30s)
  {
    static unsigned long lastHb = 0;
    if (millis() - lastHb > 30000) {
      lastHb = millis();
      // Save current uptime to RTC so next boot can report it
      BootRecord br = {};
      ESP.rtcUserMemoryRead(RTC_BOOT_SLOT, (uint32_t*)&br, sizeof(br));
      br.magic    = RTC_BOOT_MAGIC;
      br.uptime_s = millis() / 1000;
      ESP.rtcUserMemoryWrite(RTC_BOOT_SLOT, (uint32_t*)&br, sizeof(br));
      // UDP heartbeat -- boot info resent on first HB in case early packets were ARP-dropped
      if (wifiConnected && debugRemoteEnabled) {
        static bool bootMsgConfirmed = false;
        if (!bootMsgConfirmed) {
          bootMsgConfirmed = true;
          DLOGI("BOOT", "fw=%s git:%s built:%s  %s", FW_VERSION_BASE, FW_GIT_HASH, FW_BUILD_TIME, cachedBootInfo.c_str());
        }
        DLOGI("HB", "heap=%u frag=%u uptime=%lus mode=%d ntp=%d rssi=%d",
              ESP.getFreeHeap(), ESP.getHeapFragmentation(),
              millis() / 1000, (int)displayMode, ntpSynced ? 1 : 0,
              wifiConnected ? WiFi.RSSI() : 0);
      }
    }
  }

  // Dispatch server-side OTA-from-URL (queued by /api/update/from-url handler)
  if (otaFromUrl.length() > 0 && millis() - otaFromUrlAt > 500) {
    String url = otaFromUrl;
    otaFromUrl = "";
    doOtaFromUrl(url);
  }

  // Update LED clock display
  displayClock();

  delay(100);
}
