/*
 * ESP8266 WS2812 LED Controller with Captive Portal & OTA
 * --------------------------------------------------------
 * - Captive WiFi hotspot with web UI
 * - WS2812 LED chain control (5 LEDs)
 * - OTA firmware update over WiFi
 * - Multiple LED effects & color control
 */

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESPAsyncWebServer.h>
#include <ESPAsyncTCP.h>
#include <DNSServer.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <ESP8266mDNS.h>
#include <FastLED.h>
#include <Updater.h>

// ─── Configuration ───────────────────────────────────────
#ifndef NUM_LEDS
#define NUM_LEDS 5
#endif
#ifndef LED_PIN
#define LED_PIN D4
#endif

const char* AP_SSID_BASE = "LED-Controller";
const char* AP_PASS     = "";          // Open network for captive portal
const char* OTA_PASS    = "admin123";  // OTA & upload password
const byte  DNS_PORT    = 53;
const int   MAX_SCAN_CACHE = 30;
const unsigned long STA_RETRY_INTERVAL = 180000; // 3 minutes

// ─── Globals ─────────────────────────────────────────────
CRGB leds[NUM_LEDS];
AsyncWebServer server(80);
DNSServer dnsServer;
String deviceId;
String apSsid;
String localHostname;
bool mdnsStarted = false;

struct ScanCacheEntry {
  String ssid;
  int32_t rssi = 0;
  int channel = -1;
  int enc = -1;
};
ScanCacheEntry scanCache[MAX_SCAN_CACHE];
int scanCacheCount = 0;
unsigned long scanCacheUpdatedAt = 0;

// Periodic STA retry & session tracking
unsigned long lastStaRetryAt = 0;
unsigned long lastClientActivity = 0;
int activeClientCount = 0;

struct WifiConnectState {
  bool active = false;
  bool resultReady = false;
  bool success = false;
  String error;
  unsigned long startedAt = 0;
  unsigned long timeoutMs = 15000;
  bool reportResult = true;
  String attemptedSsid;
  uint8_t passLen = 0;
  wl_status_t lastStatus = WL_IDLE_STATUS;
  bool targetSeenInScan = false;
  int targetChannel = -1;
  int32_t targetRssi = 0;
  int targetEnc = -1;
  unsigned long lastElapsedMs = 0;
} wifiConnect;

bool beginWiFiConnectAttempt(unsigned long timeoutMs, bool reportResult) {
  if (wifiConnect.active) {
    return false;
  }

  wifiConnect.active = true;
  wifiConnect.resultReady = false;
  wifiConnect.success = false;
  wifiConnect.error = "";
  wifiConnect.startedAt = millis();
  wifiConnect.timeoutMs = timeoutMs;
  wifiConnect.reportResult = reportResult;
  return true;
}

// LED State
struct LedState {
  bool     power       = true;
  uint8_t  brightness  = 128;
  uint8_t  effect      = 0;   // 0=solid,1=rainbow,2=chase,3=breathe,4=fire,5=twinkle,6=wave,7=gradient
  CRGB     color1      = CRGB(255, 0, 80);
  CRGB     color2      = CRGB(0, 120, 255);
  uint8_t  speed       = 128;
} state;

uint8_t effectHue = 0;
unsigned long lastUpdate = 0;

// ─── Forward Declarations ────────────────────────────────
void setupWiFi();
void setupDNS();
void setupMDNS();
void setupOTA();
void setupWebServer();
void setupLEDs();
void updateLEDs();
void effectSolid();
void effectRainbow();
void effectChase();
void effectBreathe();
void effectFire();
void effectTwinkle();
void effectWave();
void effectGradient();
String getStatusJson();
String getWifiScanJson();
void buildDeviceIdentity();
bool startWiFiConnect(const String& ssid, const String& pass);
void updateWiFiConnect();
void checkPeriodicStaRetry();
void ensureApFallback();
void trackClientActivity();
const char* wifiStatusToString(wl_status_t status);
const char* wifiEncryptionToString(int enc);
String buildWifiFailureHint();
String getCaptivePortalUrl();

// ─── HTML Page (embedded) ────────────────────────────────
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0, user-scalable=no">
<title>LED Controller</title>
<style>
  @import url('https://fonts.googleapis.com/css2?family=JetBrains+Mono:wght@300;400;600&display=swap');
  :root {
    --bg: #0a0a0f;
    --card: #12121a;
    --border: #1e1e2e;
    --text: #e0e0e8;
    --dim: #6a6a7a;
    --accent: #ff0050;
    --accent2: #0078ff;
    --glow: rgba(255,0,80,0.3);
  }
  * { margin:0; padding:0; box-sizing:border-box; }
  body {
    font-family: 'JetBrains Mono', monospace;
    background: var(--bg);
    color: var(--text);
    min-height: 100vh;
    padding: 16px;
    -webkit-font-smoothing: antialiased;
  }
  .container { max-width: 420px; margin: 0 auto; }
  h1 {
    font-size: 1.1rem;
    font-weight: 600;
    letter-spacing: 2px;
    text-transform: uppercase;
    text-align: center;
    margin-bottom: 20px;
    background: linear-gradient(135deg, var(--accent), var(--accent2));
    -webkit-background-clip: text;
    -webkit-text-fill-color: transparent;
  }
  .card {
    background: var(--card);
    border: 1px solid var(--border);
    border-radius: 12px;
    padding: 16px;
    margin-bottom: 12px;
  }
  .card-title {
    font-size: 0.65rem;
    letter-spacing: 3px;
    text-transform: uppercase;
    color: var(--dim);
    margin-bottom: 12px;
  }
  .power-row {
    display: flex;
    align-items: center;
    justify-content: space-between;
  }
  .led-preview {
    height: 6px;
    border-radius: 3px;
    background: linear-gradient(90deg, var(--accent), var(--accent2), var(--accent));
    background-size: 200% 100%;
    animation: shimmer 3s ease infinite;
    margin-bottom: 16px;
    opacity: 0.8;
  }
  @keyframes shimmer { 0%,100%{background-position:0% 50%} 50%{background-position:100% 50%} }
  .toggle {
    width: 48px; height: 26px;
    background: #2a2a3a;
    border-radius: 13px;
    position: relative;
    cursor: pointer;
    transition: background 0.3s;
  }
  .toggle.on { background: var(--accent); box-shadow: 0 0 12px var(--glow); }
  .toggle::after {
    content: '';
    width: 20px; height: 20px;
    background: #fff;
    border-radius: 50%;
    position: absolute;
    top: 3px; left: 3px;
    transition: transform 0.3s;
  }
  .toggle.on::after { transform: translateX(22px); }
  .slider-group { margin-bottom: 14px; }
  .slider-label {
    display: flex;
    justify-content: space-between;
    font-size: 0.7rem;
    color: var(--dim);
    margin-bottom: 6px;
  }
  input[type=range] {
    -webkit-appearance: none;
    width: 100%;
    height: 4px;
    border-radius: 2px;
    background: #2a2a3a;
    outline: none;
  }
  input[type=range]::-webkit-slider-thumb {
    -webkit-appearance: none;
    width: 18px; height: 18px;
    border-radius: 50%;
    background: var(--accent);
    cursor: pointer;
    box-shadow: 0 0 8px var(--glow);
  }
  .color-row {
    display: flex;
    gap: 12px;
    align-items: center;
  }
  .color-pick {
    flex: 1;
    display: flex;
    flex-direction: column;
    align-items: center;
    gap: 6px;
  }
  .color-pick label { font-size: 0.6rem; color: var(--dim); letter-spacing: 1px; }
  input[type=color] {
    -webkit-appearance: none;
    width: 100%; height: 36px;
    border: 1px solid var(--border);
    border-radius: 8px;
    background: transparent;
    cursor: pointer;
    padding: 2px;
  }
  input[type=color]::-webkit-color-swatch-wrapper { padding: 0; }
  input[type=color]::-webkit-color-swatch { border: none; border-radius: 6px; }
  .effects-grid {
    display: grid;
    grid-template-columns: repeat(4, 1fr);
    gap: 6px;
  }
  .fx-btn {
    padding: 8px 4px;
    font-family: inherit;
    font-size: 0.6rem;
    letter-spacing: 0.5px;
    background: #1a1a2a;
    border: 1px solid var(--border);
    border-radius: 8px;
    color: var(--dim);
    cursor: pointer;
    transition: all 0.2s;
    text-align: center;
  }
  .fx-btn:hover { border-color: var(--accent); color: var(--text); }
  .fx-btn.active {
    background: linear-gradient(135deg, rgba(255,0,80,0.15), rgba(0,120,255,0.15));
    border-color: var(--accent);
    color: #fff;
    box-shadow: 0 0 8px var(--glow);
  }
  .upload-area {
    border: 1px dashed var(--border);
    border-radius: 8px;
    padding: 16px;
    text-align: center;
    cursor: pointer;
    transition: border-color 0.3s;
  }
  .upload-area:hover { border-color: var(--accent); }
  .upload-area p { font-size: 0.7rem; color: var(--dim); }
  .upload-btn {
    display: inline-block;
    margin-top: 10px;
    padding: 8px 20px;
    font-family: inherit;
    font-size: 0.7rem;
    background: var(--accent);
    color: #fff;
    border: none;
    border-radius: 6px;
    cursor: pointer;
    letter-spacing: 1px;
  }
  .upload-btn:disabled { opacity: 0.4; cursor: not-allowed; }
  .progress-bar {
    width: 100%;
    height: 4px;
    background: #2a2a3a;
    border-radius: 2px;
    margin-top: 10px;
    display: none;
  }
  .progress-fill {
    height: 100%;
    background: var(--accent);
    border-radius: 2px;
    width: 0%;
    transition: width 0.3s;
  }
  .status-msg {
    font-size: 0.65rem;
    margin-top: 8px;
    text-align: center;
    min-height: 1em;
  }
  .status-ok { color: #4caf50; }
  .status-err { color: #ff4444; }
  .info-row {
    display: flex;
    justify-content: space-between;
    font-size: 0.6rem;
    color: var(--dim);
    padding: 4px 0;
  }
  .wifi-row {
    display: flex;
    gap: 8px;
    align-items: center;
  }
  select, input[type=password] {
    width: 100%;
    background: #1a1a2a;
    color: var(--text);
    border: 1px solid var(--border);
    border-radius: 8px;
    padding: 8px;
    font-family: inherit;
    font-size: 0.7rem;
  }
  .mini-btn {
    padding: 8px 10px;
    font-family: inherit;
    font-size: 0.65rem;
    background: #1a1a2a;
    color: var(--text);
    border: 1px solid var(--border);
    border-radius: 8px;
    cursor: pointer;
    white-space: nowrap;
  }
  .mini-btn:hover { border-color: var(--accent); }
</style>
</head>
<body>
<div class="container">
  <h1>&#9670; LED Controller</h1>
  <div class="led-preview" id="preview"></div>

  <!-- Power & Brightness -->
  <div class="card">
    <div class="power-row">
      <span class="card-title" style="margin:0">POWER</span>
      <div class="toggle on" id="powerToggle" onclick="togglePower()"></div>
    </div>
  </div>

  <div class="card">
    <div class="card-title">BRIGHTNESS</div>
    <div class="slider-group">
      <div class="slider-label"><span>Dim</span><span id="brVal">128</span><span>Full</span></div>
      <input type="range" id="brightness" min="0" max="255" value="128" oninput="sendSlider('brightness',this.value)">
    </div>
    <div class="card-title" style="margin-top:8px">SPEED</div>
    <div class="slider-group">
      <div class="slider-label"><span>Slow</span><span id="spVal">128</span><span>Fast</span></div>
      <input type="range" id="speed" min="1" max="255" value="128" oninput="sendSlider('speed',this.value)">
    </div>
  </div>

  <!-- Colors -->
  <div class="card">
    <div class="card-title">COLORS</div>
    <div class="color-row">
      <div class="color-pick">
        <label>PRIMARY</label>
        <input type="color" id="color1" value="#ff0050" onchange="sendColor()">
      </div>
      <div class="color-pick">
        <label>SECONDARY</label>
        <input type="color" id="color2" value="#0078ff" onchange="sendColor()">
      </div>
    </div>
  </div>

  <!-- Effects -->
  <div class="card">
    <div class="card-title">EFFECTS</div>
    <div class="effects-grid" id="fxGrid">
      <button class="fx-btn active" onclick="setEffect(0)">Solid</button>
      <button class="fx-btn" onclick="setEffect(1)">Rainbow</button>
      <button class="fx-btn" onclick="setEffect(2)">Chase</button>
      <button class="fx-btn" onclick="setEffect(3)">Breathe</button>
      <button class="fx-btn" onclick="setEffect(4)">Fire</button>
      <button class="fx-btn" onclick="setEffect(5)">Twinkle</button>
      <button class="fx-btn" onclick="setEffect(6)">Wave</button>
      <button class="fx-btn" onclick="setEffect(7)">Gradient</button>
    </div>
  </div>

  <!-- WiFi Setup -->
  <div class="card">
    <div class="card-title">WIFI SETUP</div>
    <div class="wifi-row" style="margin-bottom:8px">
      <select id="ssidList"></select>
      <button class="mini-btn" onclick="scanWifi()">SCAN</button>
    </div>
    <div class="wifi-row">
      <input type="password" id="wifiPass" placeholder="WiFi password">
      <button class="mini-btn" onclick="connectWifi()">CONNECT</button>
    </div>
    <div class="status-msg" id="wifiMsg"></div>
  </div>

  <!-- OTA Update -->
  <div class="card">
    <div class="card-title">FIRMWARE UPDATE</div>
    <div class="upload-area" onclick="document.getElementById('fwFile').click()">
      <p id="fileName">Select .bin firmware file</p>
      <input type="file" id="fwFile" accept=".bin" style="display:none" onchange="fileSelected(this)">
    </div>
    <button class="upload-btn" id="uploadBtn" onclick="uploadFirmware()" disabled>UPLOAD</button>
    <div class="progress-bar" id="progBar"><div class="progress-fill" id="progFill"></div></div>
    <div class="status-msg" id="statusMsg"></div>
  </div>

  <!-- Info -->
  <div class="card">
    <div class="card-title">DEVICE INFO</div>
    <div class="info-row"><span>LEDs</span><span>65</span></div>
    <div class="info-row"><span>AP SSID</span><span id="apSsid">-</span></div>
    <div class="info-row"><span>Host</span><span id="host">-</span></div>
    <div class="info-row"><span>IP</span><span id="ip">-</span></div>
    <div class="info-row"><span>Heap</span><span id="heap">-</span></div>
  </div>

  <!-- Connection Stats -->
  <div class="card" id="staCard" style="display:none">
    <div class="card-title">WIFI CONNECTION</div>
    <div class="info-row"><span>SSID</span><span id="staSsid">-</span></div>
    <div class="info-row"><span>Signal</span><span id="staSignal">-</span></div>
    <div class="info-row"><span>Quality</span><span id="staQuality">-</span></div>
    <div class="info-row"><span>Channel</span><span id="staChannel">-</span></div>
    <div class="info-row"><span>BSSID</span><span id="staBssid">-</span></div>
  </div>
</div>

<script>
const API = '';
let debounce = null;

function send(url) {
  fetch(API + url).catch(e => console.warn(e));
}

function togglePower() {
  const t = document.getElementById('powerToggle');
  t.classList.toggle('on');
  send('/api/power?on=' + (t.classList.contains('on') ? '1' : '0'));
}

function sendSlider(param, val) {
  document.getElementById(param === 'brightness' ? 'brVal' : 'spVal').textContent = val;
  clearTimeout(debounce);
  debounce = setTimeout(() => send('/api/' + param + '?value=' + val), 50);
}

function sendColor() {
  const c1 = document.getElementById('color1').value;
  const c2 = document.getElementById('color2').value;
  send('/api/color?c1=' + encodeURIComponent(c1) + '&c2=' + encodeURIComponent(c2));
}

function setEffect(idx) {
  document.querySelectorAll('.fx-btn').forEach((b, i) => b.classList.toggle('active', i === idx));
  send('/api/effect?id=' + idx);
}

function fileSelected(input) {
  if (input.files.length > 0) {
    document.getElementById('fileName').textContent = input.files[0].name;
    document.getElementById('uploadBtn').disabled = false;
  }
}

function uploadFirmware() {
  const file = document.getElementById('fwFile').files[0];
  if (!file) return;
  const formData = new FormData();
  formData.append('firmware', file);
  const xhr = new XMLHttpRequest();
  const progBar = document.getElementById('progBar');
  const progFill = document.getElementById('progFill');
  const statusMsg = document.getElementById('statusMsg');
  progBar.style.display = 'block';
  statusMsg.textContent = '';
  statusMsg.className = 'status-msg';
  document.getElementById('uploadBtn').disabled = true;

  xhr.upload.addEventListener('progress', (e) => {
    if (e.lengthComputable) progFill.style.width = (e.loaded / e.total * 100) + '%';
  });
  xhr.addEventListener('load', () => {
    if (xhr.status === 200) {
      statusMsg.textContent = 'Update OK! Rebooting...';
      statusMsg.className = 'status-msg status-ok';
      setTimeout(() => location.reload(), 5000);
    } else {
      statusMsg.textContent = 'Upload failed: ' + xhr.responseText;
      statusMsg.className = 'status-msg status-err';
      document.getElementById('uploadBtn').disabled = false;
    }
  });
  xhr.addEventListener('error', () => {
    statusMsg.textContent = 'Connection error';
    statusMsg.className = 'status-msg status-err';
    document.getElementById('uploadBtn').disabled = false;
  });
  xhr.open('POST', '/api/update');
  xhr.send(formData);
}

function scanWifi(attempt = 0) {
  const list = document.getElementById('ssidList');
  const msg = document.getElementById('wifiMsg');
  if (attempt === 0) {
    msg.textContent = 'Scanning...';
    msg.className = 'status-msg';
  }
  fetch('/api/wifi/scan').then(r => r.json()).then(d => {
    if (d.scanning) {
      if (attempt > 25) {
        msg.textContent = 'Scan timeout';
        msg.className = 'status-msg status-err';
        return;
      }
      setTimeout(() => scanWifi(attempt + 1), 350);
      return;
    }

    list.innerHTML = '';
    if (!d.networks || d.networks.length === 0) {
      const opt = document.createElement('option');
      opt.value = '';
      opt.textContent = 'No networks found';
      list.appendChild(opt);
      msg.textContent = 'No SSIDs detected';
      return;
    }
    d.networks.forEach(n => {
      const opt = document.createElement('option');
      opt.value = n.ssid;
      opt.textContent = `${n.ssid} (${n.rssi} dBm)`;
      list.appendChild(opt);
    });
    msg.textContent = `Found ${d.networks.length} networks`;
    msg.className = 'status-msg status-ok';
  }).catch(() => {
    msg.textContent = 'Scan failed';
    msg.className = 'status-msg status-err';
  });
}

function connectWifi() {
  const ssid = document.getElementById('ssidList').value;
  const pass = document.getElementById('wifiPass').value;
  const msg = document.getElementById('wifiMsg');
  if (!ssid) {
    msg.textContent = 'Select an SSID first';
    msg.className = 'status-msg status-err';
    return;
  }
  msg.textContent = `Connecting to ${ssid}...`;
  msg.className = 'status-msg';

  const pollConnect = (attempt = 0) => {
    fetch('/api/wifi/connect').then(r => r.json()).then(d => {
      if (d.connecting) {
        if (attempt > 60) {
          msg.textContent = 'Connection timeout';
          msg.className = 'status-msg status-err';
          return;
        }
        setTimeout(() => pollConnect(attempt + 1), 500);
        return;
      }

      if (d.connected) {
        msg.textContent = `Connected! Open http://${d.hostname}.local/ or ${d.ip}`;
        msg.className = 'status-msg status-ok';
      } else {
        msg.textContent = d.error || 'Connection failed';
        msg.className = 'status-msg status-err';
      }
      pollStatus();
    }).catch(() => {
      msg.textContent = 'Connection status check failed';
      msg.className = 'status-msg status-err';
    });
  };

  const p = new URLSearchParams({ ssid, pass });
  fetch('/api/wifi/connect?' + p.toString()).then(r => r.json()).then(d => {
    if (!d.connecting) {
      msg.textContent = d.error || 'Failed to start connection';
      msg.className = 'status-msg status-err';
      return;
    }
    pollConnect();
  }).catch(() => {
    msg.textContent = 'Connection request failed';
    msg.className = 'status-msg status-err';
  });
}

// Poll status
function pollStatus() {
  fetch('/api/status').then(r => r.json()).then(d => {
    document.getElementById('heap').textContent = Math.round(d.heap / 1024) + ' KB';
    document.getElementById('apSsid').textContent = d.ap_ssid || '-';
    document.getElementById('host').textContent = d.hostname ? (d.hostname + '.local') : '-';
    document.getElementById('ip').textContent = d.ip || '-';
    document.getElementById('powerToggle').classList.toggle('on', d.power);
    document.getElementById('brightness').value = d.brightness;
    document.getElementById('brVal').textContent = d.brightness;
    document.getElementById('speed').value = d.speed;
    document.getElementById('spVal').textContent = d.speed;
    document.querySelectorAll('.fx-btn').forEach((b, i) => b.classList.toggle('active', i === d.effect));
    
    // Update connection stats
    const staCard = document.getElementById('staCard');
    if (d.connected && d.sta_ssid) {
      staCard.style.display = 'block';
      document.getElementById('staSsid').textContent = d.sta_ssid;
      document.getElementById('staSignal').textContent = d.sta_rssi + ' dBm';
      document.getElementById('staQuality').textContent = d.sta_quality;
      document.getElementById('staChannel').textContent = d.sta_channel;
      document.getElementById('staBssid').textContent = d.sta_bssid;
    } else {
      staCard.style.display = 'none';
    }
  }).catch(() => {});
}
pollStatus();
scanWifi();
setInterval(pollStatus, 5000);
</script>
</body>
</html>
)rawliteral";

// ─── Setup ───────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial.println("\n\n=== LED Controller Starting ===");

  buildDeviceIdentity();
  setupLEDs();
  setupWiFi();
  setupDNS();
  setupMDNS();
  setupOTA();
  setupWebServer();

  Serial.println("=== Ready! AP: " + apSsid + " / Host: " + localHostname + ".local ===");
}

void loop() {
  dnsServer.processNextRequest();
  if (mdnsStarted) {
    MDNS.update();
  } else if (WiFi.status() == WL_CONNECTED) {
    setupMDNS();
  }
  updateWiFiConnect();
  checkPeriodicStaRetry();
  trackClientActivity();
  ArduinoOTA.handle();
  updateLEDs();
}

void buildDeviceIdentity() {
  String mac = WiFi.macAddress();
  mac.replace(":", "");
  deviceId = mac.substring(mac.length() - 6);
  deviceId.toUpperCase();
  apSsid = String(AP_SSID_BASE) + "-" + deviceId;
  localHostname = String("ledportal-") + deviceId;
  localHostname.toLowerCase();
}

// ─── WiFi AP ─────────────────────────────────────────────
void setupWiFi() {
  WiFi.persistent(true);
  WiFi.mode(WIFI_AP_STA);
  WiFi.hostname(localHostname.c_str());
  WiFi.softAP(apSsid.c_str(), AP_PASS);
  delay(100);
  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());

  if (WiFi.SSID().length() > 0 && beginWiFiConnectAttempt(8000, false)) {
    WiFi.begin(); // try saved STA credentials (non-blocking)
    Serial.println("Trying saved STA credentials...");
  }
}

// ─── DNS (Captive Portal) ────────────────────────────────
void setupDNS() {
  if ((WiFi.getMode() & WIFI_AP) == 0) {
    return;
  }
  // Redirect all DNS queries to our IP → captive portal
  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
}

void setupMDNS() {
  if (WiFi.status() != WL_CONNECTED || mdnsStarted) {
    return;
  }
  if (MDNS.begin(localHostname.c_str())) {
    mdnsStarted = true;
    Serial.print("mDNS: http://");
    Serial.print(localHostname);
    Serial.println(".local");
  } else {
    Serial.println("mDNS start failed");
  }
}

// ─── OTA ─────────────────────────────────────────────────
void setupOTA() {
  ArduinoOTA.setHostname(localHostname.c_str());
  ArduinoOTA.setPassword(OTA_PASS);
  ArduinoOTA.onStart([]() {
    FastLED.clear(true);
    Serial.println("OTA Start");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    uint8_t pct = progress / (total / 100);
    int litLeds = map(pct, 0, 100, 0, NUM_LEDS);
    FastLED.clear();
    for (int i = 0; i < litLeds; i++) {
      leds[i] = CRGB(0, 255, 0);
    }
    FastLED.show();
  });
  ArduinoOTA.onEnd([]() {
    fill_solid(leds, NUM_LEDS, CRGB::Green);
    FastLED.show();
    Serial.println("OTA Done!");
  });
  ArduinoOTA.onError([](ota_error_t error) {
    fill_solid(leds, NUM_LEDS, CRGB::Red);
    FastLED.show();
    Serial.printf("OTA Error[%u]\n", error);
  });
  ArduinoOTA.begin();
}

// ─── LEDs ────────────────────────────────────────────────
void setupLEDs() {
  FastLED.addLeds<WS2812, LED_PIN, GRB>(leds, NUM_LEDS);
  FastLED.setBrightness(state.brightness);
  FastLED.clear(true);
}

void updateLEDs() {
  if (!state.power) {
    EVERY_N_MILLISECONDS(100) {
      FastLED.clear(true);
    }
    return;
  }

  unsigned long interval = map(state.speed, 1, 255, 60, 5);
  unsigned long now = millis();
  if (now - lastUpdate < interval) return;
  lastUpdate = now;

  switch (state.effect) {
    case 0: effectSolid();    break;
    case 1: effectRainbow();  break;
    case 2: effectChase();    break;
    case 3: effectBreathe();  break;
    case 4: effectFire();     break;
    case 5: effectTwinkle();  break;
    case 6: effectWave();     break;
    case 7: effectGradient(); break;
  }
  FastLED.show();
}

// ─── Effects ─────────────────────────────────────────────
void effectSolid() {
  fill_solid(leds, NUM_LEDS, state.color1);
}

void effectRainbow() {
  fill_rainbow(leds, NUM_LEDS, effectHue++, 7);
}

void effectChase() {
  fadeToBlackBy(leds, NUM_LEDS, 40);
  int pos = beatsin16(map(state.speed, 1, 255, 10, 100), 0, NUM_LEDS - 1);
  leds[pos] = state.color1;
}

void effectBreathe() {
  uint8_t val = beatsin8(map(state.speed, 1, 255, 10, 60), 30, 255);
  CRGB c = state.color1;
  c.nscale8(val);
  fill_solid(leds, NUM_LEDS, c);
}

void effectFire() {
  static byte heat[NUM_LEDS];
  // Cool down
  for (int i = 0; i < NUM_LEDS; i++) {
    heat[i] = qsub8(heat[i], random8(0, ((55 * 10) / NUM_LEDS) + 2));
  }
  // Heat drifts up
  for (int k = NUM_LEDS - 1; k >= 2; k--) {
    heat[k] = (heat[k - 1] + heat[k - 2] + heat[k - 2]) / 3;
  }
  // Ignite
  if (random8() < 160) {
    int y = random8(7);
    heat[y] = qadd8(heat[y], random8(160, 255));
  }
  // Map to colors
  for (int j = 0; j < NUM_LEDS; j++) {
    leds[j] = HeatColor(heat[j]);
  }
}

void effectTwinkle() {
  fadeToBlackBy(leds, NUM_LEDS, 20);
  if (random8() < 80) {
    int pos = random16(NUM_LEDS);
    leds[pos] = random8() > 128 ? state.color1 : state.color2;
  }
}

void effectWave() {
  for (int i = 0; i < NUM_LEDS; i++) {
    uint8_t wave = sin8(i * 10 + effectHue);
    leds[i] = blend(state.color1, state.color2, wave);
  }
  effectHue += 2;
}

void effectGradient() {
  fill_gradient_RGB(leds, 0, state.color1, NUM_LEDS - 1, state.color2);
}

// ─── Status JSON ─────────────────────────────────────────
String getStatusJson() {
  JsonDocument doc;
  doc["power"]      = state.power;
  doc["brightness"] = state.brightness;
  doc["effect"]     = state.effect;
  doc["speed"]      = state.speed;
  doc["heap"]       = ESP.getFreeHeap();
  doc["ap_ssid"]    = apSsid;
  doc["hostname"]   = localHostname;
  doc["connected"]  = WiFi.status() == WL_CONNECTED;
  doc["ip"]         = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  
  // Connection stats
  if (WiFi.status() == WL_CONNECTED) {
    doc["sta_ssid"]  = WiFi.SSID();
    doc["sta_rssi"]  = WiFi.RSSI();
    doc["sta_channel"] = WiFi.channel();
    doc["sta_bssid"] = WiFi.BSSIDstr();
    int rssi = WiFi.RSSI();
    String quality = "Excellent";
    if (rssi < -80) quality = "Poor";
    else if (rssi < -70) quality = "Fair";
    else if (rssi < -60) quality = "Good";
    doc["sta_quality"] = quality;
  } else {
    doc["sta_ssid"]  = WiFi.SSID().length() > 0 ? WiFi.SSID() : "";
    doc["sta_rssi"]  = 0;
    doc["sta_channel"] = 0;
    doc["sta_bssid"] = "";
    doc["sta_quality"] = "Not connected";
  }
  doc["ap_active"]  = (WiFi.getMode() & WIFI_AP) != 0;
  doc["ap_clients"] = WiFi.softAPgetStationNum();
  
  char c1[8], c2[8];
  snprintf(c1, sizeof(c1), "#%02x%02x%02x", state.color1.r, state.color1.g, state.color1.b);
  snprintf(c2, sizeof(c2), "#%02x%02x%02x", state.color2.r, state.color2.g, state.color2.b);
  doc["color1"] = c1;
  doc["color2"] = c2;
  String out;
  serializeJson(doc, out);
  return out;
}

String getWifiScanJson() {
  JsonDocument doc;
  JsonArray arr = doc["networks"].to<JsonArray>();

  int scanState = WiFi.scanComplete();

  if (scanState == WIFI_SCAN_RUNNING) {
    doc["scanning"] = true;
  } else if (scanState == WIFI_SCAN_FAILED) {
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

const char* wifiStatusToString(wl_status_t status) {
  switch (status) {
    case WL_NO_SHIELD: return "NO_SHIELD";
    case WL_IDLE_STATUS: return "IDLE";
    case WL_NO_SSID_AVAIL: return "NO_SSID";
    case WL_SCAN_COMPLETED: return "SCAN_COMPLETED";
    case WL_CONNECTED: return "CONNECTED";
    case WL_CONNECT_FAILED: return "CONNECT_FAILED";
    case WL_CONNECTION_LOST: return "CONNECTION_LOST";
    case WL_WRONG_PASSWORD: return "WRONG_PASSWORD";
    case WL_DISCONNECTED: return "DISCONNECTED";
    default: return "UNKNOWN";
  }
}

const char* wifiEncryptionToString(int enc) {
  switch (enc) {
    case ENC_TYPE_NONE: return "OPEN";
    case ENC_TYPE_WEP: return "WEP";
    case ENC_TYPE_TKIP: return "WPA_TKIP";
    case ENC_TYPE_CCMP: return "WPA2_CCMP";
    case ENC_TYPE_AUTO: return "AUTO";
    default: return "UNKNOWN";
  }
}

String buildWifiFailureHint() {
  if (!wifiConnect.targetSeenInScan) {
    if (scanCacheCount <= 0 || scanCacheUpdatedAt == 0) {
      return "No recent scan cache available. Press Scan again before Connect. ESP8266 is 2.4GHz-only (cannot join 5GHz).";
    }
    return "SSID not seen in scan. ESP8266 is 2.4GHz-only (cannot join 5GHz). Ensure 2.4GHz SSID broadcast is enabled.";
  }

  if (wifiConnect.lastStatus == WL_WRONG_PASSWORD) {
    return "Wrong password or incompatible security mode. Use WPA2/WPA mixed mode (WPA3-only is not supported on ESP8266).";
  }

  if (wifiConnect.lastStatus == WL_NO_SSID_AVAIL) {
    return "AP not available right now. Check signal, channel, and whether MAC filtering is enabled.";
  }

  if (wifiConnect.lastStatus == WL_CONNECT_FAILED) {
    return "Association/authentication failed. Try WPA2-PSK AES, avoid WPA3-only/enterprise, and verify router compatibility.";
  }

  return "Connection timed out. Verify 2.4GHz SSID, password, security mode, and signal quality.";
}

String getCaptivePortalUrl() {
  if ((WiFi.getMode() & WIFI_AP) != 0) {
    return String("http://") + WiFi.softAPIP().toString() + "/";
  }
  return String("http://") + localHostname + ".local/";
}

bool startWiFiConnect(const String& ssid, const String& pass) {
  if (ssid.length() == 0) {
    return false;
  }

  if (!beginWiFiConnectAttempt(15000, true)) {
    return false;
  }

  WiFi.mode(WIFI_AP_STA);
  WiFi.hostname(localHostname.c_str());

  wifiConnect.attemptedSsid = ssid;
  wifiConnect.passLen = pass.length();
  wifiConnect.lastStatus = WiFi.status();
  wifiConnect.targetSeenInScan = false;
  wifiConnect.targetChannel = -1;
  wifiConnect.targetRssi = 0;
  wifiConnect.targetEnc = -1;
  wifiConnect.lastElapsedMs = 0;

  if (scanCacheCount > 0) {
    for (int i = 0; i < scanCacheCount; i++) {
      if (scanCache[i].ssid == ssid) {
        wifiConnect.targetSeenInScan = true;
        wifiConnect.targetChannel = scanCache[i].channel;
        wifiConnect.targetRssi = scanCache[i].rssi;
        wifiConnect.targetEnc = scanCache[i].enc;
        break;
      }
    }
  }

  WiFi.begin(ssid.c_str(), pass.c_str());
  Serial.println("[WiFi] Connecting...");
  Serial.println("[WiFi] SSID: " + ssid);
  Serial.printf("[WiFi] Password length: %u\n", wifiConnect.passLen);
  Serial.printf("[WiFi] Status before begin: %s (%d)\n", wifiStatusToString(wifiConnect.lastStatus), wifiConnect.lastStatus);
  if (wifiConnect.targetSeenInScan) {
    Serial.printf("[WiFi] Target seen in scan: yes, CH=%d RSSI=%d ENC=%s (%d)\n",
      wifiConnect.targetChannel,
      (int)wifiConnect.targetRssi,
      wifiEncryptionToString(wifiConnect.targetEnc),
      wifiConnect.targetEnc);
  } else {
    Serial.printf("[WiFi] Target seen in last scan cache: no (cache_count=%d, age_ms=%lu)\n", scanCacheCount, millis() - scanCacheUpdatedAt);
  }

  return true;
}

void updateWiFiConnect() {
  if (!wifiConnect.active) {
    return;
  }

  wifiConnect.lastStatus = WiFi.status();
  wifiConnect.lastElapsedMs = millis() - wifiConnect.startedAt;

  if (WiFi.status() == WL_CONNECTED) {
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    dnsServer.stop();
    setupMDNS();
    Serial.print("WiFi connected. IP: ");
    Serial.println(WiFi.localIP());

    wifiConnect.active = false;
    wifiConnect.resultReady = wifiConnect.reportResult;
    wifiConnect.success = true;
    wifiConnect.error = "";
    Serial.printf("[WiFi] Connected to %s in %lu ms\n", wifiConnect.attemptedSsid.c_str(), wifiConnect.lastElapsedMs);
    Serial.printf("[WiFi] STA IP: %s, BSSID: %s, CH: %d, RSSI: %d\n",
      WiFi.localIP().toString().c_str(),
      WiFi.BSSIDstr().c_str(),
      WiFi.channel(),
      WiFi.RSSI());
    return;
  }

  if (millis() - wifiConnect.startedAt >= wifiConnect.timeoutMs) {
    String hint = buildWifiFailureHint();
    String errorText = String("Could not connect to '") + wifiConnect.attemptedSsid +
      "'. status=" + wifiStatusToString(wifiConnect.lastStatus) +
      " (" + String((int)wifiConnect.lastStatus) + "). " + hint;

    Serial.println("[WiFi] Connect failed");
    Serial.printf("[WiFi] SSID: %s, elapsed: %lu ms\n", wifiConnect.attemptedSsid.c_str(), wifiConnect.lastElapsedMs);
    Serial.printf("[WiFi] Final status: %s (%d)\n", wifiStatusToString(wifiConnect.lastStatus), wifiConnect.lastStatus);
    if (wifiConnect.targetSeenInScan) {
      Serial.printf("[WiFi] Scan cache target: CH=%d RSSI=%d ENC=%s (%d)\n",
        wifiConnect.targetChannel,
        (int)wifiConnect.targetRssi,
        wifiEncryptionToString(wifiConnect.targetEnc),
        wifiConnect.targetEnc);
    } else {
      Serial.println("[WiFi] Scan cache target: not seen");
    }
    Serial.println("[WiFi] Hint: " + hint);

    wifiConnect.active = false;
    wifiConnect.resultReady = wifiConnect.reportResult;
    wifiConnect.success = false;
    wifiConnect.error = errorText;
    
    // Fallback to AP mode on failure
    ensureApFallback();
  }
}

void ensureApFallback() {
  if ((WiFi.getMode() & WIFI_AP) == 0) {
    Serial.println("[WiFi] STA failed, re-enabling AP fallback");
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(apSsid.c_str(), AP_PASS);
    delay(100);
    setupDNS();
    Serial.print("[WiFi] AP fallback IP: ");
    Serial.println(WiFi.softAPIP());
  }
}

void trackClientActivity() {
  static unsigned long lastCheck = 0;
  if (millis() - lastCheck < 5000) return;
  lastCheck = millis();
  
  activeClientCount = WiFi.softAPgetStationNum();
  if (activeClientCount > 0) {
    lastClientActivity = millis();
  }
}

void checkPeriodicStaRetry() {
  // Only retry if:
  // 1. Not currently connecting
  // 2. Not connected
  // 3. Have saved credentials
  // 4. Retry interval elapsed
  // 5. No active clients (or client inactive for 2min)
  
  if (wifiConnect.active) return;
  if (WiFi.status() == WL_CONNECTED) return;
  if (WiFi.SSID().length() == 0) return;
  if (millis() - lastStaRetryAt < STA_RETRY_INTERVAL) return;
  
  bool hasRecentActivity = (millis() - lastClientActivity < 120000);
  if (activeClientCount > 0 && hasRecentActivity) return;
  
  Serial.println("[WiFi] Periodic STA retry attempt");
  lastStaRetryAt = millis();
  
  if (beginWiFiConnectAttempt(15000, false)) {
    WiFi.mode(WIFI_AP_STA);
    WiFi.hostname(localHostname.c_str());
    WiFi.begin();
    Serial.printf("[WiFi] Retrying saved SSID: %s\n", WiFi.SSID().c_str());
  }
}

// ─── Parse hex color ─────────────────────────────────────
CRGB parseHexColor(const String& hex) {
  String h = hex;
  if (h.startsWith("#")) h = h.substring(1);
  long val = strtol(h.c_str(), NULL, 16);
  return CRGB((val >> 16) & 0xFF, (val >> 8) & 0xFF, val & 0xFF);
}

// ─── Web Server ──────────────────────────────────────────
void setupWebServer() {
  // ── Captive portal detection endpoints ──
  // Android
  server.on("/generate_204", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->redirect(getCaptivePortalUrl());
  });
  server.on("/gen_204", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->redirect(getCaptivePortalUrl());
  });
  // Apple
  server.on("/hotspot-detect.html", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->redirect(getCaptivePortalUrl());
  });
  server.on("/library/test/success.html", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->redirect(getCaptivePortalUrl());
  });
  // Microsoft
  server.on("/connecttest.txt", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->redirect(getCaptivePortalUrl());
  });
  server.on("/ncsi.txt", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->redirect(getCaptivePortalUrl());
  });
  // Firefox
  server.on("/canonical.html", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->redirect(getCaptivePortalUrl());
  });

  // ── Main page ──
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send_P(200, "text/html", INDEX_HTML);
  });

  // ── API Endpoints ──
  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send(200, "application/json", getStatusJson());
  });

  server.on("/api/power", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (req->hasParam("on")) {
      state.power = req->getParam("on")->value() == "1";
    }
    req->send(200, "application/json", getStatusJson());
  });

  server.on("/api/brightness", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (req->hasParam("value")) {
      state.brightness = req->getParam("value")->value().toInt();
      FastLED.setBrightness(state.brightness);
    }
    req->send(200, "application/json", getStatusJson());
  });

  server.on("/api/speed", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (req->hasParam("value")) {
      state.speed = req->getParam("value")->value().toInt();
    }
    req->send(200, "application/json", getStatusJson());
  });

  server.on("/api/effect", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (req->hasParam("id")) {
      state.effect = req->getParam("id")->value().toInt();
    }
    req->send(200, "application/json", getStatusJson());
  });

  server.on("/api/color", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (req->hasParam("c1")) {
      state.color1 = parseHexColor(req->getParam("c1")->value());
    }
    if (req->hasParam("c2")) {
      state.color2 = parseHexColor(req->getParam("c2")->value());
    }
    req->send(200, "application/json", getStatusJson());
  });

  server.on("/api/wifi/scan", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send(200, "application/json", getWifiScanJson());
  });

  server.on("/api/wifi/connect", HTTP_GET, [](AsyncWebServerRequest *req) {
    JsonDocument doc;
    doc["hostname"] = localHostname;

    if (req->hasParam("ssid")) {
      if (wifiConnect.active) {
        doc["connecting"] = true;
      } else {
        String ssid = req->getParam("ssid")->value();
        String pass = req->hasParam("pass") ? req->getParam("pass")->value() : "";
        if (startWiFiConnect(ssid, pass)) {
          doc["connecting"] = true;
        } else {
          doc["connecting"] = false;
          doc["connected"] = false;
          doc["error"] = "Missing SSID or connection already in progress";
        }
      }
    } else if (wifiConnect.active) {
      doc["connecting"] = true;
    } else if (wifiConnect.resultReady) {
      doc["connecting"] = false;
      doc["connected"] = wifiConnect.success;
      doc["ip"] = wifiConnect.success ? WiFi.localIP().toString() : "";
      if (!wifiConnect.success) {
        doc["error"] = wifiConnect.error;
      }
      wifiConnect.resultReady = false;
    } else {
      doc["connecting"] = false;
      doc["connected"] = WiFi.status() == WL_CONNECTED;
      doc["ip"] = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : "";
      if (WiFi.status() != WL_CONNECTED) {
        doc["error"] = "No active connection attempt";
      }
    }

    JsonObject debug = doc["debug"].to<JsonObject>();
    debug["ssid"] = wifiConnect.attemptedSsid;
    debug["status"] = wifiStatusToString(wifiConnect.lastStatus);
    debug["status_code"] = (int)wifiConnect.lastStatus;
    debug["elapsed_ms"] = wifiConnect.lastElapsedMs;
    debug["target_seen"] = wifiConnect.targetSeenInScan;
    debug["target_channel"] = wifiConnect.targetChannel;
    debug["target_rssi"] = wifiConnect.targetRssi;
    debug["target_encryption"] = wifiEncryptionToString(wifiConnect.targetEnc);
    debug["target_encryption_code"] = wifiConnect.targetEnc;
    debug["scan_cache_count"] = scanCacheCount;
    debug["scan_cache_age_ms"] = scanCacheUpdatedAt > 0 ? (millis() - scanCacheUpdatedAt) : -1;
    debug["hint"] = buildWifiFailureHint();

    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
  });

  // ── Firmware Update (HTTP) ──
  server.on("/api/update", HTTP_POST,
    // Response handler
    [](AsyncWebServerRequest *req) {
      bool success = !Update.hasError();
      AsyncWebServerResponse *resp = req->beginResponse(
        success ? 200 : 500,
        "text/plain",
        success ? "OK - Rebooting..." : "Update Failed"
      );
      resp->addHeader("Connection", "close");
      req->send(resp);
      if (success) {
        delay(500);
        ESP.restart();
      }
    },
    // Upload handler
    [](AsyncWebServerRequest *req, String filename, size_t index, uint8_t *data, size_t len, bool final) {
      if (index == 0) {
        Serial.printf("Update: %s (%u bytes)\n", filename.c_str(), req->contentLength());
        // Disable LEDs during update
        FastLED.clear(true);
        uint32_t maxSize = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
        if (!Update.begin(maxSize, U_FLASH)) {
          Update.printError(Serial);
        }
      }
      if (!Update.hasError()) {
        if (Update.write(data, len) != len) {
          Update.printError(Serial);
        }
        // Show progress on LEDs
        if (req->contentLength() > 0) {
          int pct = (index + len) * 100 / req->contentLength();
          int litLeds = map(pct, 0, 100, 0, NUM_LEDS);
          FastLED.clear();
          for (int i = 0; i < litLeds; i++) leds[i] = CRGB::Blue;
          FastLED.show();
        }
      }
      if (final) {
        if (Update.end(true)) {
          Serial.printf("Update OK: %u bytes\n", index + len);
          fill_solid(leds, NUM_LEDS, CRGB::Green);
          FastLED.show();
        } else {
          Update.printError(Serial);
          fill_solid(leds, NUM_LEDS, CRGB::Red);
          FastLED.show();
        }
      }
    }
  );

  // ── Catch-all for captive portal ──
  server.onNotFound([](AsyncWebServerRequest *req) {
    req->redirect(getCaptivePortalUrl());
  });

  server.begin();
  Serial.println("HTTP server started");
}
