#pragma once

#include <Arduino.h>

const char INDEX_HTML[] PROGMEM = R"html(
<!DOCTYPE html><html><head><meta charset='utf-8'/><meta name='viewport' content='width=device-width, initial-scale=1'/>
<title>LED Clock</title><style>
*{margin:0;padding:0;box-sizing:border-box}body{font-family:system-ui;background:linear-gradient(135deg,#667eea,#764ba2);min-height:100vh;padding:20px;color:#333}
.container{max-width:600px;margin:0 auto}.clock-card{background:#fff;border-radius:20px;padding:40px 20px;margin-bottom:20px;box-shadow:0 10px 40px rgba(0,0,0,0.3);text-align:center}
.clock-time{font-size:72px;font-weight:300;letter-spacing:4px;color:#667eea;margin-bottom:10px;font-variant-numeric:tabular-nums}
.card{background:#fff;border-radius:16px;padding:20px;margin-bottom:15px;box-shadow:0 5px 20px rgba(0,0,0,0.2)}
h3{font-size:14px;letter-spacing:2px;text-transform:uppercase;color:#999;margin-bottom:15px}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:15px}.item{padding:12px;background:#f5f5f5;border-radius:8px;text-align:center}
.label{font-size:11px;color:#999;text-transform:uppercase}.value{font-size:16px;font-weight:600;color:#333;margin-top:5px}
.btn{width:100%;padding:12px;border:none;border-radius:6px;font-weight:bold;cursor:pointer;text-transform:uppercase;letter-spacing:1px;background:linear-gradient(135deg,#667eea,#764ba2);color:#fff;margin-bottom:10px}
.btn:active{opacity:.9}.btn-sm{padding:8px 16px;border:none;border-radius:6px;font-weight:bold;cursor:pointer;font-size:12px;letter-spacing:1px}
.btn-clock{background:#667eea;color:#fff}.btn-status{background:#eee;color:#555}
.stage-bar{display:flex;gap:4px;margin:12px 0}.stage-pip{flex:1;height:8px;border-radius:4px;background:#eee;transition:.4s}
.stage-pip.done{background:#4CAF50}.stage-pip.active{background:#667eea}.ring-badge{display:inline-block;padding:4px 12px;border-radius:20px;font-size:12px;font-weight:bold;letter-spacing:1px}
.badge-clock{background:#e8f5e9;color:#2e7d32}.badge-status{background:#e3f2fd;color:#1565c0}
</style></head><body><div class='container'>
<div class='clock-card'><div class='clock-time' id='time'>--:--:--</div><div style='font-size:16px;color:#999;margin-top:10px' id='date'>Loading</div></div>

<div class='card'><h3>Ring Display</h3>
<div style='display:flex;align-items:center;justify-content:space-between;margin-bottom:10px'>
<div><div class='label'>Showing</div><span class='ring-badge' id='ringBadge'>--</span></div>
<div><div class='label'>Boot Stage</div><div class='value' id='stageName'>--</div></div>
</div>
<div class='stage-bar'>
<div class='stage-pip' id='pip0' title='Booting'></div>
<div class='stage-pip' id='pip1' title='AP Ready'></div>
<div class='stage-pip' id='pip2' title='Scanning'></div>
<div class='stage-pip' id='pip3' title='Connecting'></div>
<div class='stage-pip' id='pip4' title='WiFi OK'></div>
<div class='stage-pip' id='pip5' title='NTP Sync'></div>
<div class='stage-pip' id='pip6' title='Running'></div>
</div>
<div style='display:flex;gap:8px;margin-top:12px'>
<button class='btn-sm btn-clock' style='flex:1' onclick='setRingMode(1)'>Show Clock</button>
<button class='btn-sm btn-status' style='flex:1' onclick='setRingMode(0)'>Show Status</button>
</div></div>

<div class='card'><h3>System</h3><div class='grid'>
<div class='item'><div class='label'>WiFi</div><div class='value' id='wifi'>--</div></div>
<div class='item'><div class='label'>Signal</div><div class='value' id='signal'>--</div></div>
<div class='item'><div class='label'>Timezone</div><div class='value' id='tz'>UTC</div></div>
<div class='item'><div class='label'>NTP</div><div class='value' id='ntp'>--</div></div>
</div></div>
<div class='card'><h3>Device</h3><div class='grid'>
<div class='item'><div class='label'>Firmware</div><div class='value' id='fw' title='Build timestamp' style='cursor:help;font-size:11px'>-</div></div>
<div class='item'><div class='label'>TZ Debug</div><div class='value' id='tz_debug'>manual UTC</div></div>
<div class='item'><div class='label'>Brightness</div><div class='value' id='bright' title='Effective brightness (auto or manual)'>-</div></div>
<div class='item'><div class='label'>IP</div><div class='value' style='font-size:12px' id='ip'>-</div></div>
<div class='item'><div class='label'>Heap</div><div class='value' id='heap'>-</div></div>
<div class='item'><div class='label'>Mode</div><div class='value' id='modeName'>-</div></div>
</div></div>
<button class='btn' onclick='location.href="/settings.html"'>Settings</button>
</div>
<script>
const STAGE_NAMES=['Booting','AP Ready','Scanning','Connecting','WiFi OK','NTP Wait','Running'];
const MODE_NAMES=['Solid','Simple','Pulse','Binary','HourMark','Flame','Pastel','Neon','Comet'];
function setRingMode(v){fetch('/api/ring?force_clock='+v).then(r=>r.json()).then(d=>{updateRingUI(d);}).catch(e=>console.warn(e));}
function updateRingUI(d){
  const isClock=d.ring_mode==='clock';
  const badge=document.getElementById('ringBadge');
  badge.textContent=isClock?'CLOCK':'STATUS';badge.className='ring-badge '+(isClock?'badge-clock':'badge-status');
  document.getElementById('stageName').textContent=d.boot_stage_name||'--';
  const stage=d.boot_stage||0;
  for(let i=0;i<7;i++){const p=document.getElementById('pip'+i);if(p)p.className='stage-pip'+(i<stage?' done':i===stage?' active':'');}
}
function updateStatus(){fetch('/api/status').then(r=>r.json()).then(d=>{
  const now=new Date();
  document.getElementById('time').textContent=now.toLocaleTimeString('en-US',{hour:'2-digit',minute:'2-digit',second:'2-digit'});
  document.getElementById('date').textContent=now.toLocaleDateString('en-US',{weekday:'short',month:'short',day:'numeric'});
  document.getElementById('wifi').textContent=d.wifi_connected?'\u2713 '+d.wifi_ssid:'\u2717 Offline';
  document.getElementById('signal').textContent=d.wifi_rssi?d.wifi_rssi+' dBm':'--';
  document.getElementById('tz').textContent=d.timezone||'UTC';
  document.getElementById('tz_debug').textContent=d.timezone_auto_detected?'Auto '+d.timezone_utc_offset_hours+'h':'Manual '+d.timezone_utc_offset_hours+'h';
  document.getElementById('ntp').textContent=d.ntp_synced?'\u2713 Synced':'\u23f1 Wait';
  document.getElementById('fw').textContent=d.fw_version_base||'-';document.getElementById('fw').title='Build: '+(d.fw_build_time||'unknown');
  var effBr=d.effective_brightness!==undefined?d.effective_brightness:d.brightness;
  document.getElementById('bright').textContent=effBr+'%'+(d.auto_bright_enabled?' (auto)':'');
  document.getElementById('ip').textContent=d.ip||'-';
  document.getElementById('heap').textContent=Math.round(d.heap/1024)+' KB';
  document.getElementById('modeName').textContent=MODE_NAMES[d.display_mode]||d.display_mode;
  updateRingUI({ring_mode:d.ring_mode,boot_stage:d.boot_stage,boot_stage_name:d.boot_stage_name});
}).catch(e=>console.warn(e));}
setInterval(updateStatus,3000);updateStatus();
</script></body></html>
)html";

const char SETTINGS_HTML[] PROGMEM = R"html(
<!DOCTYPE html><html><head><meta charset='utf-8'/><meta name='viewport' content='width=device-width, initial-scale=1'/>
<title>Settings</title><style>
*{margin:0;padding:0;box-sizing:border-box}body{font-family:system-ui;background:linear-gradient(135deg,#667eea,#764ba2);
min-height:100vh;padding:20px;color:#333}.container{max-width:700px;margin:0 auto}.header{color:#fff;margin-bottom:20px;display:flex;align-items:center;gap:15px}
.back{background:rgba(255,255,255,0.2);border:none;color:#fff;padding:10px 15px;border-radius:6px;cursor:pointer;font-size:14px;font-weight:bold}
.card{background:#fff;border-radius:12px;padding:20px;margin-bottom:20px;box-shadow:0 10px 30px rgba(0,0,0,0.2)}
.card h2{font-size:14px;text-transform:uppercase;letter-spacing:1px;color:#333;margin-bottom:15px;border-bottom:2px solid #667eea;padding-bottom:10px}
.form-group{margin-bottom:15px}.form-group label{display:block;font-size:12px;color:#666;text-transform:uppercase;letter-spacing:0.5px;margin-bottom:6px;font-weight:500}
.form-group select,.form-group input[type=text],.form-group input[type=password],.form-group input[type=number]{width:100%;padding:10px;border:1px solid #ddd;border-radius:6px;font-size:14px;font-family:inherit}
.form-group input:focus,.form-group select:focus{outline:none;border-color:#667eea;box-shadow:0 0 0 3px rgba(102,126,234,0.1)}
.wifi-row{display:flex;gap:8px;align-items:center;margin-bottom:10px}.wifi-row select{flex:1}.wifi-row .mini-btn{white-space:nowrap}
.btn{width:100%;padding:12px;border:none;border-radius:6px;font-size:14px;font-weight:bold;cursor:pointer;text-transform:uppercase;letter-spacing:1px;background:linear-gradient(135deg,#667eea,#764ba2);color:#fff;margin-bottom:10px}
.btn:hover{opacity:0.9}.btn-secondary{background:#666}
.mini-btn{display:inline-block;padding:8px 12px;font-size:12px;background:#f0f0f0;color:#333;border:1px solid #ddd;border-radius:4px;cursor:pointer;font-weight:bold}
.mini-btn:hover{background:#e0e0e0}.mini-btn:disabled{opacity:0.5;cursor:not-allowed}
.status-msg{font-size:12px;margin-top:8px;padding:8px;border-radius:4px;text-align:center;min-height:1em}
.status-ok{background:#d4edda;color:#155724;border:1px solid #c3e6cb}.status-err{background:#f8d7da;color:#721c24;border:1px solid #f5c6cb}.status-info{background:#d1ecf1;color:#0c5460;border:1px solid #bee5eb}
.upload-area{border:2px dashed #667eea;border-radius:8px;padding:20px;text-align:center;cursor:pointer;transition:all 0.3s;background:#f9f9f9}
.upload-area:hover{border-color:#764ba2;background:#f0f0f0}.upload-area p{font-size:12px;color:#666;margin:0}
.upload-btn{display:block;width:100%;margin-top:10px;padding:10px;font-family:inherit;font-size:12px;background:#667eea;color:#fff;border:none;border-radius:6px;cursor:pointer;font-weight:bold}
.upload-btn:disabled{opacity:0.4;cursor:not-allowed}
.progress-bar{width:100%;height:4px;background:#e0e0e0;border-radius:2px;margin-top:10px;display:none;overflow:hidden}
.progress-fill{height:100%;background:linear-gradient(90deg,#667eea,#764ba2);width:0%;transition:width 0.3s}
.note{font-size:11px;color:#999;margin-top:8px;font-style:italic}
</style></head><body><div class='container'><div class='header'><button class='back' onclick='history.back()'>&lt; Back</button><h1>Settings</h1></div>

<div class='card'><h2>WiFi Configuration</h2>
<div class='form-group'><label>Available Networks <span id='scanStatus' style='font-size:11px;color:#999'></span></label><div class='wifi-row'>
<select id='ssidList' size='6' onchange='networkSelected()'><option value=''>Scanning...</option></select>
<button class='mini-btn' id='scanBtn' onclick='scanWifi()' style='height:36px'>SCAN</button>
</div></div>
<div class='form-group'><label>SSID <span style='font-size:11px;color:#999'>(or type manually)</span></label>
<input type='text' id='wifiSsid' placeholder='Network name' style='font-family:monospace'/></div>
<div class='form-group'><label>Password <span id='openLabel' style='font-size:11px;color:#4a4'></span></label>
<input type='password' id='wifiPass' placeholder='Leave empty for open networks'/></div>
<button class='btn' onclick='connectWifi()'>Connect WiFi</button>
<div class='status-msg' id='wifiMsg'></div>
</div>

<div class='card'><h2>Time & Timezone</h2>
<div class='form-group'><label>NTP Server</label><input type='text' id='ntpServer' value='pool.ntp.org' placeholder='pool.ntp.org'/></div>
<button class='btn btn-secondary' onclick='syncNTP()'>Sync Now</button>

<div class='form-group' style='margin-top:15px'><label>Timezone Mode</label>
<label style='display:flex;align-items:center;margin-top:8px'><input type='radio' name='tzmode' value='auto' checked onchange='toggleTzMode()' style='margin-right:8px'/>Auto-detect</label>
<label style='display:flex;align-items:center;margin-top:8px'><input type='radio' name='tzmode' value='manual' onchange='toggleTzMode()' style='margin-right:8px'/>Manual offset</label></div>

<div id='manualTz' style='display:none'><div class='form-group'><label>UTC Offset (hours, -12 to +14)</label>
<input type='number' id='tzOffset' min='-12' max='14' value='0' placeholder='e.g., -5 for EST'/></div></div>
<button class='btn btn-secondary' onclick='saveTimezone()'>Save Timezone</button>
<div class='status-msg' id='tzMsg'></div>
</div>

<div class='card'><h2>Display Mode</h2>
<div class='form-group'><label>LED Display Style</label>
<select id='displayMode' onchange='saveDisplayMode()' style='width:100%;padding:8px;border:1px solid #ddd;border-radius:4px'>
<option value='0'>Marker Ring (rainbow orbit + HMS)</option>
<option value='1'>Simple HMS (clean 3-LED red/green/blue)</option>
<option value='2'>Pulse (subtle heartbeat + HMS)</option>
<option value='3'>Binary Clock (60 LEDs stretched bits)</option>
<option value='4'>Hour Beacon (minute progress + markers)</option>
<option value='5'>Flame HMS (warm flicker + markers)</option>
<option value='6'>Pastel HMS (soft pink/mint/sky)</option>
<option value='7'>Neon HMS (bright magenta/cyan/yellow)</option>
<option value='8'>Comet Trails (animated HMS tails)</option>
</select></div>
<div style='font-size:11px;color:#888;margin-top:5px' id='modeDesc'>Choose a display mode</div>
<button class='btn btn-secondary' onclick='updateModeDescription()'>Refresh Display</button>
<div class='form-group' style='margin-top:12px'>
<label>Hour Color</label><input type='color' id='hourColor' value='#ff0000' style='width:100%;height:36px;border:1px solid #ddd;border-radius:4px'/>
</div>
<div class='form-group'>
<label>Minute Color</label><input type='color' id='minuteColor' value='#00ff00' style='width:100%;height:36px;border:1px solid #ddd;border-radius:4px'/>
</div>
<div class='form-group'>
<label>Second Color</label><input type='color' id='secondColor' value='#0000ff' style='width:100%;height:36px;border:1px solid #ddd;border-radius:4px'/>
</div>
<div class='form-group'>
<label>Hour Width (pixels)</label><input type='range' id='hourWidth' min='1' max='21' value='5' style='width:100%'/>
<div style='font-size:10px;color:#777'>Pixels: <span id='hourWidthLabel'>5</span></div>
</div>
<div class='form-group'>
<label>Minute Width (pixels)</label><input type='range' id='minuteWidth' min='1' max='21' value='3' style='width:100%'/>
<div style='font-size:10px;color:#777'>Pixels: <span id='minuteWidthLabel'>3</span></div>
</div>
<div class='form-group'>
<label>Second Width (pixels)</label><input type='range' id='secondWidth' min='1' max='30' value='3' style='width:100%'/>
<div style='font-size:10px;color:#777'>Pixels: <span id='secondWidthLabel'>3</span></div>
</div>
<div class='form-group'>
<label>Color Spectrum</label>
<select id='spectrum' style='width:100%;padding:8px;border:1px solid #ddd;border-radius:4px'>
<option value='0'>Static</option>
<option value='1'>Rainbow blend</option>
<option value='2'>Pulse glow</option>
</select>
</div>
<div class='form-group' id='fadeGroup'>
<label>Simple HMS Transition Speed</label>
<input type='range' id='fadeMsSlider' min='0' max='2000' step='50' value='400' oninput='updateFadeMsLabel()' style='width:100%'/>
<div style='font-size:10px;color:#777'><span id='fadeMsLabel'>400</span> ms &nbsp;(0 = instant / no fade)</div>
</div>
<button class='btn btn-secondary' onclick='saveModeConfig()'>Save Mode Visuals</button>
<button class='btn btn-secondary' onclick='resetModeConfig()' style='margin-top:8px;background:#f8f8f8;border:1px solid #ddd;color:#555'>Reset Current Mode to Default</button>
<div class='status-msg' id='modeCfgMsg'></div>
</div>

<div class='card'><h2>Brightness</h2>
<div id='manualBrightGroup'>
<div class='form-group' style='margin-bottom:5px'><label>LED Brightness</label>
<input type='range' id='brightness' min='10' max='255' value='76' oninput='updateBrightnessLabel()' style='width:100%'/></div>
<div style='text-align:center;font-size:12px;color:#666'>
<span id='brightLabel'>30%</span> (<span id='brightValue'>76</span>/255)</div>
<button class='btn btn-secondary' onclick='saveBrightness()'>Save Brightness</button>
</div>
<div id='autoBrightNote' style='display:none;font-size:11px;color:#888;margin-top:6px'>Manual slider disabled &mdash; adaptive brightness is active. Current: <span id='effectiveBrPct'>-</span>%</div>
</div>

<div class='card'><h2>Adaptive Brightness</h2>
<div class='form-group' style='display:flex;align-items:center;gap:12px'>
<label style='margin:0'>Off</label>
<label style='position:relative;display:inline-block;width:48px;height:26px;margin:0'>
<input type='checkbox' id='autoBrightToggle' style='opacity:0;width:0;height:0' onchange='saveAutoBright()'>
<span id='autoBrightSlider' style='position:absolute;cursor:pointer;inset:0;background:#ccc;border-radius:26px;transition:.3s'></span>
</label>
<label style='margin:0'>On</label>
</div>
<div style='font-size:11px;color:#888;margin-top:4px;margin-bottom:12px'>Smoothly dims at night, brightens during day using a cosine curve between dim and peak hours.</div>
<div id='autoBrightControls'>
<div class='form-group'>
<label>Night Brightness (dim) &mdash; <span id='abDimHourLabel'>2</span>:00</label>
<input type='range' id='abDimHour' min='0' max='23' value='2' oninput='updateAutoBrightLabels()' style='width:100%'/>
</div>
<div class='form-group'>
<label>Day Brightness (peak) &mdash; <span id='abPeakHourLabel'>14</span>:00</label>
<input type='range' id='abPeakHour' min='0' max='23' value='14' oninput='updateAutoBrightLabels()' style='width:100%'/>
</div>
<div class='form-group'>
<label>Dim value: <span id='abDimPctLabel'>10</span>%</label>
<input type='range' id='abDimPct' min='0' max='100' value='10' oninput='updateAutoBrightLabels()' style='width:100%'/>
</div>
<div class='form-group'>
<label>Peak value: <span id='abPeakPctLabel'>100</span>%</label>
<input type='range' id='abPeakPct' min='0' max='100' value='100' oninput='updateAutoBrightLabels()' style='width:100%'/>
</div>
<button class='btn btn-secondary' onclick='saveAutoBright()'>Save Adaptive Brightness</button>
<div style='font-size:11px;color:#888;margin-top:8px'>Effective now: <span id='abEffectivePct'>-</span>%</div>
</div>
</div>

<div class='card'><h2>LED Strip Type</h2>
<div class='form-group' style='display:flex;align-items:center;gap:12px'>
<label style='margin:0'>RGB</label>
<label style='position:relative;display:inline-block;width:48px;height:26px;margin:0'>
<input type='checkbox' id='rgbwToggle' style='opacity:0;width:0;height:0' onchange='saveLedType()'>
<span id='rgbwSlider' style='position:absolute;cursor:pointer;inset:0;background:#ccc;border-radius:26px;transition:.3s'></span>
</label>
<label style='margin:0'>RGBW</label>
</div>
<div style='font-size:11px;color:#888;margin-top:6px'>Current: <span id='ledTypeLabel'>RGB</span> &mdash; change restarts the LED driver</div>
</div>

<div class='card'><h2>LED Direction</h2>
<div class='form-group' style='display:flex;align-items:center;gap:12px'>
<label style='margin:0'>Normal</label>
<label style='position:relative;display:inline-block;width:48px;height:26px;margin:0'>
<input type='checkbox' id='revToggle' style='opacity:0;width:0;height:0' onchange='saveLedDirection()'>
<span id='revSlider' style='position:absolute;cursor:pointer;inset:0;background:#ccc;border-radius:26px;transition:.3s'></span>
</label>
<label style='margin:0'>Reversed</label>
</div>
<div style='font-size:11px;color:#888;margin-top:6px'>Current: <span id='ledDirLabel'>Normal</span> &mdash; flips LED 0&harr;59 for opposite mounting orientation</div>
</div>

<div class='card'><h2>Debug Logging</h2>
<div class='form-group'><label>Debug Server IP</label>
<input type='text' id='dbgIp' placeholder='192.168.x.x' style='width:100%;padding:8px;box-sizing:border-box;border:1px solid #ddd;border-radius:4px;font-size:14px'></div>
<div class='form-group'><label>UDP Port (default 7878)</label>
<input type='number' id='dbgPort' value='7878' min='1' max='65535' style='width:100%;padding:8px;box-sizing:border-box;border:1px solid #ddd;border-radius:4px;font-size:14px'></div>
<div class='form-group' style='display:flex;align-items:center;gap:12px'>
<label style='margin:0'>Remote UDP Log</label>
<label style='position:relative;display:inline-block;width:48px;height:26px;margin:0'>
<input type='checkbox' id='dbgToggle' style='opacity:0;width:0;height:0' onchange='saveDebugConfig()'>
<span id='dbgSlider' style='position:absolute;cursor:pointer;inset:0;background:#ccc;border-radius:26px;transition:.3s'></span>
</label>
<span id='dbgEnabledLabel'>Disabled</span>
</div>
<div style='display:flex;gap:8px;margin-top:10px'>
<button onclick='saveDebugConfig()' class='btn btn-primary' style='padding:8px 16px'>Save</button>
<button onclick='sendDebugTest()' class='btn btn-secondary' style='padding:8px 16px'>Send Test</button>
</div>
<div id='dbgMsg' class='status-msg' style='margin-top:8px'></div>
<div style='font-size:11px;color:#888;margin-top:8px'>UDP log packets sent to server IP:port.<br>Run <code>node scripts/debug-server.js</code> in VSCode terminal &mdash; then open <code>http://localhost:7879</code> to view live logs.</div>
</div>

<div class='card'><h2>Firmware Update</h2>
<div style='margin-bottom:10px;display:flex;align-items:center;gap:10px;flex-wrap:wrap'>
<button class='btn btn-secondary' onclick='checkForUpdate()' id='checkUpdateBtn' style='padding:6px 14px;font-size:13px'>Check for Update</button>
<span id='updateStatus' style='font-size:12px;color:#888'></span>
</div>
<div id='updateInfo' style='display:none;margin-bottom:10px;padding:8px 10px;background:#f0f8ff;border:1px solid #b3d9ff;border-radius:6px;font-size:12px'>
<div>Latest: <strong id='latestTag'>-</strong> <a id='releaseLink' href='#' target='_blank' style='color:#2779bd;font-size:11px'>(view release)</a></div>
<div style='margin-top:4px'>Diff from <span id='currentHashSpan' style='font-family:monospace'></span>: <a id='diffLink' href='#' target='_blank' style='color:#2779bd'>compare on GitHub</a></div>
<div style='margin-top:8px;display:flex;gap:10px;flex-wrap:wrap;align-items:center'>
<a id='downloadLink' href='#' target='_blank' class='btn btn-secondary' style='font-size:12px;padding:6px 14px;text-decoration:none'>Download firmware.bin</a>
<button id='directFlashBtn' onclick='directFlash()' style='display:none;padding:6px 14px;background:#d35400;color:#fff;border:none;border-radius:4px;cursor:pointer;font-size:12px;font-weight:bold'>Flash directly to device</button>
</div>
<div id='directFlashStatus' style='display:none;margin-top:6px;font-size:11px'></div>
</div>
<div class='upload-area' onclick='document.getElementById("fwFile").click()' id='uploadArea'>
<p id='fileName'>Click to select .bin firmware file</p>
<input type='file' id='fwFile' accept='.bin' style='display:none' onchange='fileSelected(this)'>
</div>
<button class='upload-btn' id='uploadBtn' onclick='uploadFirmware()' disabled>Upload Firmware</button>
<div class='progress-bar' id='progBar'><div class='progress-fill' id='progFill'></div></div>
<div class='status-msg' id='statusMsg'></div>
<div class='note'>Max size: <span id='maxSize'>-</span> bytes</div>
</div>

<div class='card'><h2>Device Information</h2>
<div style='font-size:12px;line-height:1.8;color:#666'>
<div>Firmware: <span id='fwVersion'>-</span> <span id='fwBuildTime' style='font-size:10px;color:#999'></span> <span id='fwGitHash' style='font-size:10px;color:#666'></span></div>
<div>IP Address: <span id='deviceIp'>-</span></div>
<div>WiFi Mode: <span id='wifiMode'>AP</span></div>
<div>Signal: <span id='signal'>-</span></div>
<div>Timezone: <span id='tzDisplay'>UTC</span> <span id='tzMode' style='font-size:10px;color:#999'></span></div>
<div id='tzDebug' style='margin-top:8px;padding:4px;background:#f5f5f5;border-radius:3px;color:#666;font-size:10px;display:none'>
Offset: <span id='tzOffset'>0</span>h (<span id='tzOffsetSec'>0</span>s) | Auto-detected: <span id='tzAuto'>no</span>
</div>
<div style='margin-top:6px;font-size:10px;color:#666'>Detect status: <span id='tzDetectStatus'>-</span></div>
<div style='margin-top:2px;font-size:10px;color:#666'>Detect message: <span id='tzDetectMsg'>-</span></div>
</div></div>

</div>

<script>
let fwFile=null,uploading=false,_directUrl='';function updateBrightnessLabel(){const v=document.getElementById('brightness').value;
document.getElementById('brightValue').textContent=v;document.getElementById('brightLabel').textContent=(Math.round(v/255*100))+'%';}
let modeCfgSaveTimer=null,modeCfgPersistTimer=null;
function toggleTzMode(){document.getElementById('manualTz').style.display=document.querySelector('input[name="tzmode"]:checked').value==='manual'?'block':'none';}
function fileSelected(input){const sb=document.getElementById('statusMsg');const ub=document.getElementById('uploadBtn');fwFile=null;ub.disabled=true;
if(input.files.length===0)return;fwFile=input.files[0];document.getElementById('fileName').textContent='ðŸ"„ '+fwFile.name;sb.textContent='Checking firmware...';sb.className='status-msg status-info';
const r=new FileReader();r.onload=()=>{const b=new Uint8Array(r.result);const m=b.length>0?b[0]:0;
fetch('/api/update/precheck?name='+encodeURIComponent(fwFile.name)+'&size='+fwFile.size+'&magic='+m).then(r=>r.json()).then(d=>{
if(d.ok){ub.disabled=false;sb.textContent='\u2713 '+d.summary;sb.className='status-msg status-ok';}else{ub.disabled=true;sb.textContent='\u2717 '+d.error;sb.className='status-msg status-err';}}).catch(e=>{ub.disabled=true;sb.textContent='Check failed: '+e;sb.className='status-msg status-err';});};
r.onerror=()=>{ub.disabled=true;sb.textContent='Failed to read file';sb.className='status-msg status-err';};r.readAsArrayBuffer(fwFile.slice(0,1));}
function uploadFirmware(){if(!fwFile||uploading)return;if(!confirm('Upload '+fwFile.name+'?'))return;uploading=true;document.getElementById('uploadBtn').disabled=true;
const sb=document.getElementById('statusMsg');const pb=document.getElementById('progBar');const pf=document.getElementById('progFill');pb.style.display='block';sb.textContent='';
const fd=new FormData();fd.append('firmware',fwFile);const x=new XMLHttpRequest();
x.upload.addEventListener('progress',(e)=>{if(e.lengthComputable)pf.style.width=(e.loaded/e.total*100)+'%';});
x.addEventListener('load',()=>{uploading=false;try{const p=JSON.parse(x.responseText);if(x.status===200&&p.ok){sb.textContent='\u2713 Update OK ('+p.written+' bytes). Rebooting...';sb.className='status-msg status-ok';setTimeout(()=>location.reload(),2000);}else{const e=p?p.error:x.responseText;sb.textContent='\u2717 '+e;sb.className='status-msg status-err';document.getElementById('uploadBtn').disabled=false;}}catch(e){sb.textContent='\u2717 Upload failed';sb.className='status-msg status-err';document.getElementById('uploadBtn').disabled=false;}});
x.addEventListener('error',()=>{uploading=false;sb.textContent='\u2717 Connection error';sb.className='status-msg status-err';document.getElementById('uploadBtn').disabled=false;});
x.open('POST','/api/update?approve=1');x.send(fd);}
function checkForUpdate(){const btn=document.getElementById('checkUpdateBtn');const st=document.getElementById('updateStatus');const info=document.getElementById('updateInfo');
btn.disabled=true;st.textContent='Checking...';st.style.color='#888';
fetch('/api/status').then(r=>r.json()).then(d=>{
const currentVer=d.fw_version_base||'';const currentHash=d.fw_git_hash||'';
document.getElementById('currentHashSpan').textContent=currentHash;
fetch('https://api.github.com/repos/noless-zz/esp8266-led-strip-clock/releases/latest',{headers:{'Accept':'application/vnd.github+json'}})
.then(r=>r.json()).then(rel=>{
const tag=rel.tag_name||'';const releaseUrl=rel.html_url||'#';
document.getElementById('latestTag').textContent=tag+' ('+currentVer+' on device)';
document.getElementById('releaseLink').href=releaseUrl;
if(currentHash){document.getElementById('diffLink').href='https://github.com/noless-zz/esp8266-led-strip-clock/compare/'+currentHash+'...'+tag;}
const asset=(rel.assets||[]).find(a=>a.name==='firmware.bin');
if(asset){document.getElementById('downloadLink').href=asset.browser_download_url;_directUrl=asset.browser_download_url;document.getElementById('directFlashBtn').style.display='inline-block';}
else{document.getElementById('downloadLink').style.display='none';document.getElementById('directFlashBtn').style.display='none';}
info.style.display='block';
const upToDate=tag==='v'+currentVer.split('+')[0];
if(upToDate){st.textContent='\u2713 Up to date ('+tag+')';st.style.color='#3a3';}
else{st.textContent='\u25b2 Update available: '+tag;st.style.color='#c80';}
btn.disabled=false;
}).catch(e=>{st.textContent='GitHub error: '+e;st.style.color='#c33';btn.disabled=false;});
}).catch(e=>{st.textContent='Device error: '+e;st.style.color='#c33';btn.disabled=false;});}
var _dfProxies=['','https://api.allorigins.win/raw?url=','https://thingproxy.freeboard.io/fetch/'];
function _dfWaitReboot(btn,st,oldVer){
  var phase='offline',ticks=0,iv=setInterval(function(){
    ticks++;
    if(ticks>60){clearInterval(iv);st.textContent='\u2717 Timeout \u2014 device did not come back';st.style.color='#c33';btn.disabled=false;return;}
    fetch('/api/status').then(function(r){return r.json();}).then(function(s){
      if(phase==='online'){
        clearInterval(iv);
        var nv=s.fw_version_base||'?';
        if(oldVer&&nv===oldVer){st.textContent='\u2717 Rebooted but firmware unchanged ('+nv+') \u2014 bootloader rejected the image';st.style.color='#c33';btn.disabled=false;}
        else{st.textContent='\u2713 Updated '+(oldVer?oldVer+' \u2192 ':'')+nv;st.style.color='#3a3';setTimeout(function(){location.reload();},2000);}
      }
    }).catch(function(){if(phase==='offline'){phase='online';st.textContent='Device rebooting, waiting for it to come back\u2026';}});
  },1500);
}
function _dfUpload(btn,st,oldVer,blob){
  st.textContent='Uploading to device\u2026';
  var fd=new FormData();fd.append('firmware',blob,'firmware.bin');
  var x=new XMLHttpRequest();
  x.upload.addEventListener('progress',function(e){if(e.lengthComputable)st.textContent='Uploading: '+Math.round(e.loaded/e.total*100)+'%';});
  x.addEventListener('load',function(){
    var ok=false;
    try{var j=JSON.parse(x.responseText);ok=(j.ok===true);}catch(e){ok=(x.status>=200&&x.status<300);}
    console.log('[directFlash] upload response HTTP '+x.status+' ok='+ok+' body='+x.responseText.substring(0,200));
    if(!ok){st.textContent='\u2717 Upload rejected (HTTP '+x.status+'): '+x.responseText.substring(0,100);st.style.color='#c33';btn.disabled=false;return;}
    st.textContent='Upload done, waiting for reboot\u2026';
    _dfWaitReboot(btn,st,oldVer);
  });
  x.addEventListener('error',function(){st.textContent='\u2717 Upload network error';st.style.color='#c33';btn.disabled=false;});
  x.open('POST','/api/update?approve=1');x.send(fd);
}
function _dfPrecheck(btn,st,oldVer,buf){
  var blob=new Blob([buf],{type:'application/octet-stream'});
  var magic=new Uint8Array(buf,0,1)[0];
  console.log('[directFlash] precheck size='+blob.size+' magic=0x'+magic.toString(16));
  st.textContent='Checking firmware ('+Math.round(blob.size/1024)+' KB, magic=0x'+magic.toString(16)+')\u2026';
  fetch('/api/update/precheck?name=firmware.bin&size='+blob.size+'&magic='+magic)
  .then(function(r){return r.json();}).then(function(d){
    console.log('[directFlash] precheck response: ok='+d.ok+' err='+(d.error||'none')+' summary='+(d.summary||''));
    if(!d.ok){st.textContent='\u2717 Precheck failed: '+(d.error||'unknown');st.style.color='#c33';btn.disabled=false;return;}
    st.textContent='\u2713 Precheck OK: '+d.summary;
    _dfUpload(btn,st,oldVer,blob);
  }).catch(function(e){st.textContent='\u2717 Precheck error: '+e;st.style.color='#c33';btn.disabled=false;});
}
function _dfDownload(btn,st,oldVer,url,idx){
  if(idx>=_dfProxies.length){
    st.innerHTML='GitHub CORS blocked all proxies. <strong>Use the Download button above</strong>, save the .bin, then use the file picker below to upload.';
    st.style.color='#a05000';btn.disabled=false;return;
  }
  var proxy=_dfProxies[idx];
  var reqUrl=proxy?proxy+encodeURIComponent(url):url;
  var label=proxy?('proxy: '+proxy.split('/')[2]):'direct';
  st.textContent='Downloading firmware ('+label+')\u2026';
  console.log('[directFlash] downloading idx='+idx+' url='+reqUrl);
  var dl=new XMLHttpRequest();
  dl.open('GET',reqUrl,true);dl.responseType='arraybuffer';
  dl.onprogress=function(e){if(e.lengthComputable)st.textContent='Downloading: '+Math.round(e.loaded/e.total*100)+'% ('+label+')';};
  dl.onerror=function(){console.log('[directFlash] dl onerror idx='+idx);st.textContent='Blocked ('+label+'), trying next\u2026';st.style.color='#888';_dfDownload(btn,st,oldVer,url,idx+1);};
  dl.onload=function(){
    console.log('[directFlash] dl onload idx='+idx+' status='+dl.status+' size='+(dl.response?dl.response.byteLength:0));
    if(dl.status===403||dl.status===0||dl.status>=400){st.textContent='Got HTTP '+dl.status+' ('+label+'), trying next\u2026';st.style.color='#888';_dfDownload(btn,st,oldVer,url,idx+1);return;}
    if(dl.status<200||dl.status>=300){st.textContent='\u2717 Download failed HTTP '+dl.status;st.style.color='#c33';btn.disabled=false;return;}
    var buf=dl.response;
    if(!buf||buf.byteLength<4096){st.textContent='\u2717 Download too small ('+( buf?buf.byteLength:0)+' bytes) \u2014 likely a redirect page, not firmware';st.style.color='#c33';btn.disabled=false;return;}
    console.log('[directFlash] download ok size='+buf.byteLength);
    _dfPrecheck(btn,st,oldVer,buf);
  };
  dl.send();
}
function directFlash(){
  if(!_directUrl){alert('No firmware URL \u2014 run Check for Update first.');return;}
  if(!confirm('Flash firmware directly to device?\nYour browser downloads the binary, then uploads it to the device over local HTTP.'))return;
  var btn=document.getElementById('directFlashBtn');
  var st=document.getElementById('directFlashStatus');
  btn.disabled=true;st.style.display='block';st.style.color='#888';st.textContent='Reading current version\u2026';
  fetch('/api/status').then(function(r){return r.json();}).then(function(s){
    var oldVer=s.fw_version_base||'';
    console.log('[directFlash] current fw='+oldVer+' url='+_directUrl);
    _dfDownload(btn,st,oldVer,_directUrl,0);
  }).catch(function(){_dfDownload(btn,st,'',_directUrl,0);});
}
var _scanData=[];
function networkSelected(){const list=document.getElementById('ssidList');const ssid=list.value;if(!ssid)return;document.getElementById('wifiSsid').value=ssid;const net=_scanData.find(n=>n.ssid===ssid);const ol=document.getElementById('openLabel');if(net&&!net.enc){document.getElementById('wifiPass').value='';ol.textContent='open network';ol.style.color='#4a4';}else{ol.textContent='';}}
function scanWifi(attempt=0){const list=document.getElementById('ssidList');const sb=document.getElementById('scanBtn');const ss=document.getElementById('scanStatus');
if(attempt===0){ss.textContent='scanning...';sb.disabled=true;list.innerHTML='<option>Scanning...</option>';}
fetch('/api/wifi/scan').then(r=>r.json()).then(d=>{if(d.scanning){if(attempt>25){ss.textContent='timeout';sb.disabled=false;return;}setTimeout(()=>scanWifi(attempt+1),350);return;}
_scanData=d.networks||[];list.innerHTML='';if(!_scanData.length){list.innerHTML='<option>No networks found</option>';ss.textContent='none';sb.disabled=false;return;}
_scanData.forEach(n=>{const o=document.createElement('option');o.value=n.ssid;o.textContent=(n.enc?'\uD83D\uDD12 ':'\uD83D\uDD13 ')+n.ssid+' ('+n.rssi+' dBm)';list.appendChild(o);});
ss.textContent='found '+_scanData.length;sb.disabled=false;}).catch(e=>{list.innerHTML='<option>Scan failed</option>';ss.textContent='error';sb.disabled=false;});}
function pollConnect(s){const msg=document.getElementById('wifiMsg');fetch('/api/wifi/connect').then(r=>r.json()).then(d=>{
if(d.connected){msg.textContent='\u2713 Connected to "'+s+'"! IP: '+d.ip;msg.className='status-msg status-ok';}
else if(d.connecting){msg.textContent='Connecting to "'+s+'"...';setTimeout(()=>pollConnect(s),800);}
else{msg.textContent='\u2717 '+(d.error||'Connection failed');msg.className='status-msg status-err';}}).catch(e=>{msg.textContent='Error: '+e;msg.className='status-msg status-err';});}
function connectWifi(){const s=document.getElementById('wifiSsid').value.trim();const p=document.getElementById('wifiPass').value;const msg=document.getElementById('wifiMsg');
if(!s){msg.textContent='Enter or select a network name';msg.className='status-msg status-err';return;}
msg.textContent='Connecting to "'+s+'"'+(p?'':' (open)');msg.className='status-msg status-info';
const sp=new URLSearchParams({ssid:s,pass:p});fetch('/api/wifi/connect?'+sp.toString()).then(r=>r.json()).then(d=>{
if(d.connected){msg.textContent='\u2713 Connected! IP: '+d.ip;msg.className='status-msg status-ok';}
else if(d.connecting){setTimeout(()=>pollConnect(s),800);}
else{msg.textContent='\u2717 '+(d.error||'Connection failed');msg.className='status-msg status-err';}}).catch(e=>{msg.textContent='Error: '+e;msg.className='status-msg status-err';});}
function syncNTP(){const msg=document.getElementById('tzMsg');msg.textContent='Syncing...';msg.className='status-msg status-info';
fetch('/api/ntp').then(r=>r.text()).then(t=>{msg.textContent='\u2713 Syncing with NTP server...';msg.className='status-msg status-ok';}).catch(e=>{msg.textContent='\u2717 Error: '+e;msg.className='status-msg status-err';});}
function saveTimezone(){const m=document.querySelector('input[name="tzmode"]:checked').value;const o=m==='manual'?document.getElementById('tzOffset').value:'0';
const b=document.getElementById('tzMsg');b.textContent='Saving...';b.className='status-msg status-info';
fetch('/api/timezone?mode='+m+'&offset='+o).then(r=>r.text()).then(t=>{b.textContent='\u2713 Saved!';b.className='status-msg status-ok';}).catch(e=>{b.textContent='\u2717 Error: '+e;b.className='status-msg status-err';});}
function saveBrightness(){const v=document.getElementById('brightness').value;fetch('/api/brightness?value='+Math.round(v/255*100)).catch(e=>console.warn(e));}
function updateAutoBrightLabels(){
  document.getElementById('abDimHourLabel').textContent=document.getElementById('abDimHour').value;
  document.getElementById('abPeakHourLabel').textContent=document.getElementById('abPeakHour').value;
  document.getElementById('abDimPctLabel').textContent=document.getElementById('abDimPct').value;
  document.getElementById('abPeakPctLabel').textContent=document.getElementById('abPeakPct').value;
}
function saveAutoBright(){
  const en=document.getElementById('autoBrightToggle').checked?1:0;
  const dp=document.getElementById('abDimPct').value;
  const pp=document.getElementById('abPeakPct').value;
  const dh=document.getElementById('abDimHour').value;
  const ph=document.getElementById('abPeakHour').value;
  fetch('/api/autobright?enabled='+en+'&dim_pct='+dp+'&peak_pct='+pp+'&dim_hour='+dh+'&peak_hour='+ph)
    .then(r=>r.json()).then(d=>{
      document.getElementById('abEffectivePct').textContent=d.effective_pct;
      applyAutoBrightUI(en==1);
    }).catch(e=>console.warn(e));
}
function applyAutoBrightUI(enabled){
  document.getElementById('autoBrightSlider').style.background=enabled?'#4CAF50':'#ccc';
  document.getElementById('manualBrightGroup').style.opacity=enabled?'0.4':'1';
  document.getElementById('manualBrightGroup').style.pointerEvents=enabled?'none':'auto';
  document.getElementById('autoBrightNote').style.display=enabled?'block':'none';
}
function saveLedType(){const v=document.getElementById('rgbwToggle').checked?1:0;fetch('/api/ledtype?rgbw='+v).then(r=>r.json()).then(d=>{document.getElementById('rgbwSlider').style.background=d.rgbw?'#4CAF50':'#ccc';document.getElementById('ledTypeLabel').textContent=d.rgbw?'RGBW':'RGB';}).catch(e=>console.warn(e));}
function saveLedDirection(){const v=document.getElementById('revToggle').checked?1:0;fetch('/api/leddirection?reversed='+v).then(r=>r.json()).then(d=>{document.getElementById('revSlider').style.background=d.reversed?'#4CAF50':'#ccc';document.getElementById('ledDirLabel').textContent=d.reversed?'Reversed':'Normal';}).catch(e=>console.warn(e));}
function saveDebugConfig(){const ip=document.getElementById('dbgIp').value.trim();const port=document.getElementById('dbgPort').value||'7878';const en=document.getElementById('dbgToggle').checked?1:0;const msg=document.getElementById('dbgMsg');msg.textContent='Saving...';msg.className='status-msg status-info';fetch('/api/debug?enabled='+en+'&ip='+encodeURIComponent(ip)+'&port='+port).then(r=>r.json()).then(d=>{document.getElementById('dbgSlider').style.background=d.enabled?'#4CAF50':'#ccc';document.getElementById('dbgEnabledLabel').textContent=d.enabled?'Enabled':'Disabled';msg.textContent='\u2713 Saved';msg.className='status-msg status-ok';}).catch(e=>{msg.textContent='\u2717 '+e;msg.className='status-msg status-err';});}
function sendDebugTest(){const msg=document.getElementById('dbgMsg');msg.textContent='Sending...';msg.className='status-msg status-info';fetch('/api/debug?test=1').then(r=>r.json()).then(d=>{msg.textContent='\u2713 Test packet sent';msg.className='status-msg status-ok';}).catch(e=>{msg.textContent='\u2717 '+e;msg.className='status-msg status-err';});}
function rgbToHex(r,g,b){return'#'+[r,g,b].map(v=>{const h=Number(v).toString(16);return h.length===1?'0'+h:h;}).join('');}
function hexToRgb(hex){const v=(hex||'#000000').replace('#','');if(v.length!==6)return{r:0,g:0,b:0};return{r:parseInt(v.substring(0,2),16),g:parseInt(v.substring(2,4),16),b:parseInt(v.substring(4,6),16)};}
function updateWidthLabels(){document.getElementById('hourWidthLabel').textContent=document.getElementById('hourWidth').value;
document.getElementById('minuteWidthLabel').textContent=document.getElementById('minuteWidth').value;
document.getElementById('secondWidthLabel').textContent=document.getElementById('secondWidth').value;}
function applyModeCfgToControls(c){if(!c)return;
document.getElementById('hourColor').value=rgbToHex(c.hour.r,c.hour.g,c.hour.b);
document.getElementById('minuteColor').value=rgbToHex(c.minute.r,c.minute.g,c.minute.b);
document.getElementById('secondColor').value=rgbToHex(c.second.r,c.second.g,c.second.b);
document.getElementById('hourWidth').value=c.width.hour;
document.getElementById('minuteWidth').value=c.width.minute;
document.getElementById('secondWidth').value=c.width.second;
document.getElementById('spectrum').value=c.spectrum;updateWidthLabels();}
function loadModeConfig(){const m=document.getElementById('displayMode').value;
fetch('/api/mode/config?mode='+m).then(r=>r.json()).then(d=>{if(d&&d.ok)applyModeCfgToControls(d);}).catch(e=>console.warn(e));}
function buildModeCfgQuery(persist){const m=document.getElementById('displayMode').value;
const h=hexToRgb(document.getElementById('hourColor').value);
const mn=hexToRgb(document.getElementById('minuteColor').value);
const s=hexToRgb(document.getElementById('secondColor').value);
const hw=document.getElementById('hourWidth').value;
const mw=document.getElementById('minuteWidth').value;
const sw=document.getElementById('secondWidth').value;
const sp=document.getElementById('spectrum').value;
return `/api/mode/config?set=1&persist=${persist?1:0}&mode=${m}&hr=${h.r}&hg=${h.g}&hb=${h.b}&mr=${mn.r}&mg=${mn.g}&mb=${mn.b}&sr=${s.r}&sg=${s.g}&sb=${s.b}&hw=${hw}&mw=${mw}&sw=${sw}&sp=${sp}`;}
function queueModeConfigSave(){updateWidthLabels();
if(modeCfgSaveTimer)clearTimeout(modeCfgSaveTimer);
modeCfgSaveTimer=setTimeout(()=>saveModeConfig(true,false),100);
if(modeCfgPersistTimer)clearTimeout(modeCfgPersistTimer);
modeCfgPersistTimer=setTimeout(()=>saveModeConfig(true,true),1200);
}
function saveModeConfig(silent=false,persist=true){
const msg=document.getElementById('modeCfgMsg');
if(!silent){msg.textContent=persist?'Saving mode visuals...':'Applying mode visuals...';msg.className='status-msg status-info';}
const q=buildModeCfgQuery(persist);
fetch(q).then(r=>r.json()).then(d=>{if(d&&d.ok){if(!silent){msg.textContent='\u2713 Mode visuals saved';msg.className='status-msg status-ok';}applyModeCfgToControls(d);}else{msg.textContent='\u2717 '+((d&&d.error)||'Failed');msg.className='status-msg status-err';}})
.catch(e=>{msg.textContent='\u2717 '+e;msg.className='status-msg status-err';});}
function resetModeConfig(){const m=document.getElementById('displayMode').value;
const msg=document.getElementById('modeCfgMsg');
msg.textContent='Resetting mode visuals...';msg.className='status-msg status-info';
fetch('/api/mode/config?reset=1&persist=1&mode='+m).then(r=>r.json()).then(d=>{
if(d&&d.ok){msg.textContent='\u2713 Mode visuals reset to defaults';msg.className='status-msg status-ok';applyModeCfgToControls(d);}else{msg.textContent='\u2717 '+((d&&d.error)||'Failed');msg.className='status-msg status-err';}
}).catch(e=>{msg.textContent='\u2717 '+e;msg.className='status-msg status-err';});}
function updateFadeMsLabel(){const v=document.getElementById('fadeMsSlider').value;document.getElementById('fadeMsLabel').textContent=v;}
function saveFadeMs(){const ms=document.getElementById('fadeMsSlider').value;fetch('/api/simple/fade?ms='+ms).catch(e=>console.warn(e));}
function saveDisplayMode(){const m=document.getElementById('displayMode').value;fetch('/api/display?mode='+m).catch(e=>console.warn(e));updateModeDescription();loadModeConfig();}
function updateModeDescription(){const m=parseInt(document.getElementById('displayMode').value);
const desc={0:'Rainbow orbit background with clear red hour, green minute, and blue second markers (5-3-7 LED spread).',
1:'Minimal clean mode: exactly 3 LEDs each for red hour, green minute, blue second. No background.',
2:'Very subtle background pulse (reduced from before) with strong HMS markers for easy readability.',
3:'Binary clock stretched across all 60 LEDs: 20 groups Ã-- 3 LEDs showing hour/minute/second bits in color.',
4:'Minute fills like a progress bar, hour shown as bright beacon, second has moving trail.',
5:'Optimized warm flame effect (20fps update) with clear HMS markers. Performance improved.',
6:'Soft pastel colors: pink hour, mint green minute, sky blue second. Gentle and easy on eyes.',
7:'Bright neon colors: magenta hour, cyan minute, yellow second. Vivid and energetic.',
8:'Animated comet trails: red hour (7 LED), green minute (5 LED), blue second (10 LED fast tail).'};
document.getElementById('modeDesc').textContent=desc[m]||'Mode '+m;}
function pollStatus(){fetch('/api/status').then(r=>r.json()).then(d=>{
document.getElementById('fwVersion').textContent=d.fw_version_base||'-';
document.getElementById('fwBuildTime').textContent='('+d.fw_build_time+')';
if(d.fw_git_hash){var h=document.getElementById('fwGitHash');if(h)h.textContent='git:'+d.fw_git_hash;}
document.getElementById('deviceIp').textContent=d.ip||'-';
document.getElementById('wifiMode').textContent=d.wifi_connected?'STA (Connected)':'AP (Hotspot)';
document.getElementById('signal').textContent=d.wifi_connected?(d.wifi_rssi+' dBm'):'(AP mode)';
document.getElementById('tzDisplay').textContent=d.timezone||'UTC';
document.getElementById('tzMode').textContent=(d.timezone_auto_detected?'auto':'manual');
document.getElementById('tzOffset').textContent=d.timezone_utc_offset_hours;
document.getElementById('tzOffsetSec').textContent=d.timezone_utc_offset_seconds;
document.getElementById('tzAuto').textContent=d.timezone_auto_detected?'yes':'no';
document.getElementById('tzDetectStatus').textContent=d.tz_detect_status||'-';
document.getElementById('tzDetectMsg').textContent=d.tz_detect_message||'-';
document.getElementById('tzDebug').style.display='block';
if(d.display_mode!==undefined){
const current=document.getElementById('displayMode').value;
document.getElementById('displayMode').value=d.display_mode;
updateModeDescription();
if(String(current)!==String(d.display_mode)){loadModeConfig();}
}
if(d.display_cfg){applyModeCfgToControls(d.display_cfg);}
if(d.led_rgbw!==undefined){const isRgbw=d.led_rgbw===true;document.getElementById('rgbwToggle').checked=isRgbw;document.getElementById('rgbwSlider').style.background=isRgbw?'#4CAF50':'#ccc';document.getElementById('ledTypeLabel').textContent=isRgbw?'RGBW':'RGB';}
if(d.led_reversed!==undefined){const isRev=d.led_reversed===true;document.getElementById('revToggle').checked=isRev;document.getElementById('revSlider').style.background=isRev?'#4CAF50':'#ccc';document.getElementById('ledDirLabel').textContent=isRev?'Reversed':'Normal';}
if(d.debug_enabled!==undefined){const en=d.debug_enabled===true;document.getElementById('dbgToggle').checked=en;document.getElementById('dbgSlider').style.background=en?'#4CAF50':'#ccc';document.getElementById('dbgEnabledLabel').textContent=en?'Enabled':'Disabled';}
if(d.debug_ip){document.getElementById('dbgIp').value=d.debug_ip;}
if(d.debug_port){document.getElementById('dbgPort').value=d.debug_port;}
if(d.simple_fade_ms!==undefined){document.getElementById('fadeMsSlider').value=d.simple_fade_ms;updateFadeMsLabel();}
if(d.auto_bright_enabled!==undefined){
  const en=d.auto_bright_enabled===true;
  document.getElementById('autoBrightToggle').checked=en;
  applyAutoBrightUI(en);
}
if(d.auto_bright_dim_pct!==undefined){document.getElementById('abDimPct').value=d.auto_bright_dim_pct;document.getElementById('abDimPctLabel').textContent=d.auto_bright_dim_pct;}
if(d.auto_bright_peak_pct!==undefined){document.getElementById('abPeakPct').value=d.auto_bright_peak_pct;document.getElementById('abPeakPctLabel').textContent=d.auto_bright_peak_pct;}
if(d.auto_bright_dim_hour!==undefined){document.getElementById('abDimHour').value=d.auto_bright_dim_hour;document.getElementById('abDimHourLabel').textContent=d.auto_bright_dim_hour;}
if(d.auto_bright_peak_hour!==undefined){document.getElementById('abPeakHour').value=d.auto_bright_peak_hour;document.getElementById('abPeakHourLabel').textContent=d.auto_bright_peak_hour;}
if(d.effective_brightness!==undefined){document.getElementById('abEffectivePct').textContent=d.effective_brightness;document.getElementById('effectiveBrPct').textContent=d.effective_brightness;}
}).catch(e=>console.warn(e));}
function getMaxSize(){fetch('/api/status').then(r=>r.json()).then(d=>{document.getElementById('maxSize').textContent=(d.heap||262144).toString();}).catch(e=>console.warn(e));}
document.getElementById('hourWidth').addEventListener('input',updateWidthLabels);
document.getElementById('minuteWidth').addEventListener('input',updateWidthLabels);
document.getElementById('secondWidth').addEventListener('input',updateWidthLabels);
document.getElementById('hourColor').addEventListener('input',queueModeConfigSave);
document.getElementById('minuteColor').addEventListener('input',queueModeConfigSave);
document.getElementById('secondColor').addEventListener('input',queueModeConfigSave);
document.getElementById('hourWidth').addEventListener('input',queueModeConfigSave);
document.getElementById('minuteWidth').addEventListener('input',queueModeConfigSave);
document.getElementById('secondWidth').addEventListener('input',queueModeConfigSave);
document.getElementById('spectrum').addEventListener('change',queueModeConfigSave);
document.getElementById('fadeMsSlider').addEventListener('input',updateFadeMsLabel);
document.getElementById('fadeMsSlider').addEventListener('change',saveFadeMs);
['abDimHour','abPeakHour','abDimPct','abPeakPct'].forEach(id=>{
  document.getElementById(id).addEventListener('input',updateAutoBrightLabels);
  document.getElementById(id).addEventListener('change',saveAutoBright);
});
document.getElementById('hourColor').addEventListener('change',()=>saveModeConfig(true,true));
document.getElementById('minuteColor').addEventListener('change',()=>saveModeConfig(true,true));
document.getElementById('secondColor').addEventListener('change',()=>saveModeConfig(true,true));
document.getElementById('hourWidth').addEventListener('change',()=>saveModeConfig(true,true));
document.getElementById('minuteWidth').addEventListener('change',()=>saveModeConfig(true,true));
document.getElementById('secondWidth').addEventListener('change',()=>saveModeConfig(true,true));
document.getElementById('spectrum').addEventListener('change',()=>saveModeConfig(true,true));
pollStatus();getMaxSize();scanWifi();setInterval(pollStatus,5000);updateModeDescription();loadModeConfig();updateWidthLabels();
</script></body></html>
)html";
