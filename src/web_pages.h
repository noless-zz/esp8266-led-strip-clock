#pragma once

#include <Arduino.h>

// ============================================================================
// Embedded HTML pages (stored in program flash via PROGMEM)
// ============================================================================

const char INDEX_HTML[] PROGMEM = R"html(
<!DOCTYPE html><html><head><meta charset='utf-8'/><meta name='viewport' content='width=device-width, initial-scale=1'/>
<title>LED Clock</title><style>
*{margin:0;padding:0;box-sizing:border-box}body{font-family:system-ui;background:linear-gradient(135deg,#667eea,#764ba2);
min-height:100vh;padding:20px;color:#333}.container{max-width:600px;margin:0 auto}.clock-card{background:#fff;
border-radius:20px;padding:40px 20px;margin-bottom:20px;box-shadow:0 10px 40px rgba(0,0,0,0.3);text-align:center}
.clock-time{font-size:72px;font-weight:300;letter-spacing:4px;color:#667eea;margin-bottom:10px;font-variant-numeric:tabular-nums}
.card{background:#fff;border-radius:16px;padding:20px;margin-bottom:15px;box-shadow:0 5px 20px rgba(0,0,0,0.2)}
h3{font-size:14px;letter-spacing:2px;text-transform:uppercase;color:#999;margin-bottom:15px}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:15px}.item{padding:12px;background:#f5f5f5;border-radius:8px;text-align:center}
.label{font-size:11px;color:#999;text-transform:uppercase;}
.value{font-size:16px;font-weight:600;color:#333;margin-top:5px}
.btn{width:100%;padding:12px;border:none;border-radius:6px;font-weight:bold;cursor:pointer;text-transform:uppercase;letter-spacing:1px;
background:linear-gradient(135deg,#667eea,#764ba2);color:#fff;margin-bottom:10px}
.btn:active{opacity:0.9}
</style></head><body><div class='container'>
<div class='clock-card'><div class='clock-time' id='time'>--:--:--</div><div style='font-size:16px;color:#999;margin-top:10px;' id='date'>Loading</div></div>
<div class='card'><h3>System</h3><div class='grid'>
<div class='item'><div class='label'>WiFi</div><div class='value' id='wifi'>--</div></div>
<div class='item'><div class='label'>Signal</div><div class='value' id='signal'>--</div></div>
<div class='item'><div class='label'>Timezone</div><div class='value' id='tz'>UTC</div></div>
<div class='item'><div class='label'>NTP</div><div class='value' id='ntp'>--</div></div>
</div></div>
<div class='card'><h3>Device</h3><div class='grid'>
<div class='item'><div class='label'>Firmware</div><div class='value' id='fw' title='Build timestamp' style='cursor:help;font-size:11px'>-</div></div>
<div class='item'><div class='label'>TZ Debug</div><div class='value' id='tz_debug'>manual UTC</div></div>
<div class='item'><div class='label'>Brightness</div><div class='value' id='bright'>-</div></div>
<div class='item'><div class='label'>IP</div><div class='value' style='font-size:12px' id='ip'>-</div></div>
<div class='item'><div class='label'>Heap</div><div class='value' id='heap'>-</div></div>
</div></div>
<button class='btn' onclick='location.href="/settings.html"'>Settings</button>
</div>
<script>
function updateStatus(){fetch('/api/status').then(r=>r.json()).then(d=>{
const now=new Date();document.getElementById('time').textContent=now.toLocaleTimeString('en-US',{hour:'2-digit',minute:'2-digit',second:'2-digit'});
document.getElementById('date').textContent=now.toLocaleDateString('en-US',{weekday:'short',month:'short',day:'numeric'});
document.getElementById('wifi').textContent=d.wifi_connected?'✓ '+d.wifi_ssid:'✗ Offline';
document.getElementById('signal').textContent=d.wifi_rssi?d.wifi_rssi+' dBm':'--';
const tzDebug=d.timezone_auto_detected?'Auto '+d.timezone_utc_offset_hours+'h':'Manual '+d.timezone_utc_offset_hours+'h';
document.getElementById('tz').textContent=d.timezone||'UTC';
document.getElementById('tz_debug').textContent=tzDebug;
document.getElementById('ntp').textContent=d.ntp_synced?'✓ Synced':'⏱ Wait';
document.getElementById('fw').textContent=d.fw_version_base||'-';document.getElementById('fw').title='Build: '+(d.fw_build_time||'unknown');
document.getElementById('bright').textContent=d.brightness+'%';
document.getElementById('ip').textContent=d.ip||'-';
document.getElementById('heap').textContent=Math.round(d.heap/1024)+' KB';
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
<div class='form-group'><label>Available Networks</label><div class='wifi-row'>
<select id='ssidList' size='6'><option value=''>Scanning...</option></select>
<button class='mini-btn' id='scanBtn' onclick='scanWifi()' style='height:36px'>SCAN</button>
</div></div>
<div class='form-group'><label>Password</label><input type='password' id='wifiPass' placeholder='Network password'/></div>
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
</div>

<div class='card'><h2>Brightness</h2>
<div class='form-group' style='margin-bottom:5px'><label>LED Brightness</label>
<input type='range' id='brightness' min='10' max='255' value='76' oninput='updateBrightnessLabel()' style='width:100%'/></div>
<div style='text-align:center;font-size:12px;color:#666'>
<span id='brightLabel'>30%</span> (<span id='brightValue'>76</span>/255)</div>
<button class='btn btn-secondary' onclick='saveBrightness()'>Save Brightness</button>
</div>

<div class='card'><h2>Firmware Update</h2>
<div class='upload-area' onclick='document.getElementById("fwFile").click()' id='uploadArea'>
<p id='fileName'>📁 Click to select .bin firmware file</p>
<input type='file' id='fwFile' accept='.bin' style='display:none' onchange='fileSelected(this)'>
</div>
<button class='upload-btn' id='uploadBtn' onclick='uploadFirmware()' disabled>Upload Firmware</button>
<div class='progress-bar' id='progBar'><div class='progress-fill' id='progFill'></div></div>
<div class='status-msg' id='statusMsg'></div>
<div class='note'>Max size: <span id='maxSize'>-</span> bytes</div>
</div>

<div class='card'><h2>Device Information</h2>
<div style='font-size:12px;line-height:1.8;color:#666'>
<div>Firmware: <span id='fwVersion'>-</span> <span id='fwBuildTime' style='font-size:10px;color:#999'></span></div>
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
let fwFile=null,uploading=false;function updateBrightnessLabel(){const v=document.getElementById('brightness').value;
document.getElementById('brightValue').textContent=v;document.getElementById('brightLabel').textContent=(Math.round(v/255*100))+'%';}
function toggleTzMode(){document.getElementById('manualTz').style.display=document.querySelector('input[name="tzmode"]:checked').value==='manual'?'block':'none';}
function fileSelected(input){const sb=document.getElementById('statusMsg');const ub=document.getElementById('uploadBtn');fwFile=null;ub.disabled=true;
if(input.files.length===0)return;fwFile=input.files[0];document.getElementById('fileName').textContent='📄 '+fwFile.name;sb.textContent='Checking firmware...';sb.className='status-msg status-info';
const r=new FileReader();r.onload=()=>{const b=new Uint8Array(r.result);const m=b.length>0?b[0]:0;
fetch('/api/update/precheck?name='+encodeURIComponent(fwFile.name)+'&size='+fwFile.size+'&magic='+m).then(r=>r.json()).then(d=>{
if(d.ok){ub.disabled=false;sb.textContent='✓ '+d.summary;sb.className='status-msg status-ok';}else{ub.disabled=true;sb.textContent='✗ '+d.error;sb.className='status-msg status-err';}}).catch(e=>{ub.disabled=true;sb.textContent='Check failed: '+e;sb.className='status-msg status-err';});};
r.onerror=()=>{ub.disabled=true;sb.textContent='Failed to read file';sb.className='status-msg status-err';};r.readAsArrayBuffer(fwFile.slice(0,1));}
function uploadFirmware(){if(!fwFile||uploading)return;if(!confirm('Upload '+fwFile.name+'?'))return;uploading=true;document.getElementById('uploadBtn').disabled=true;
const sb=document.getElementById('statusMsg');const pb=document.getElementById('progBar');const pf=document.getElementById('progFill');pb.style.display='block';sb.textContent='';
const fd=new FormData();fd.append('firmware',fwFile);const x=new XMLHttpRequest();
x.upload.addEventListener('progress',(e)=>{if(e.lengthComputable)pf.style.width=(e.loaded/e.total*100)+'%';});
x.addEventListener('load',()=>{uploading=false;try{const p=JSON.parse(x.responseText);if(x.status===200&&p.ok){sb.textContent='✓ Update OK ('+p.written+' bytes). Rebooting...';sb.className='status-msg status-ok';setTimeout(()=>location.reload(),2000);}else{const e=p?p.error:x.responseText;sb.textContent='✗ '+e;sb.className='status-msg status-err';document.getElementById('uploadBtn').disabled=false;}}catch(e){sb.textContent='✗ Upload failed';sb.className='status-msg status-err';document.getElementById('uploadBtn').disabled=false;}});
x.addEventListener('error',()=>{uploading=false;sb.textContent='✗ Connection error';sb.className='status-msg status-err';document.getElementById('uploadBtn').disabled=false;});
x.open('POST','/api/update?approve=1');x.send(fd);}
function scanWifi(attempt=0){const list=document.getElementById('ssidList');const sb=document.getElementById('scanBtn');const msg=document.getElementById('wifiMsg');
if(attempt===0){msg.textContent='Scanning...';msg.className='status-msg status-info';sb.disabled=true;}
fetch('/api/wifi/scan').then(r=>r.json()).then(d=>{if(d.scanning){if(attempt>25){msg.textContent='Scan timeout';msg.className='status-msg status-err';sb.disabled=false;return;}
setTimeout(()=>scanWifi(attempt+1),350);return;}
list.innerHTML='';if(!d.networks||d.networks.length===0){list.innerHTML='<option>No networks found</option>';msg.textContent='';msg.className='status-msg';sb.disabled=false;return;}
d.networks.forEach(n=>{const o=document.createElement('option');o.value=n.ssid;o.textContent=n.ssid+' ('+n.rssi+' dBm)';list.appendChild(o);});
msg.textContent='Found '+d.networks.length+' networks';msg.className='status-msg status-ok';sb.disabled=false;}).catch(e=>{list.innerHTML='<option>Scan failed</option>';msg.textContent='Error: '+e;msg.className='status-msg status-err';sb.disabled=false;});}
function connectWifi(){const s=document.getElementById('ssidList').value;const p=document.getElementById('wifiPass').value;const msg=document.getElementById('wifiMsg');
if(!s){msg.textContent='Select a network first';msg.className='status-msg status-err';return;}msg.textContent='Connecting...';msg.className='status-msg status-info';
const sp=new URLSearchParams({ssid:s,pass:p});fetch('/api/wifi/connect?'+sp.toString()).then(r=>r.json()).then(d=>{
if(d.connected){msg.textContent='✓ Connected! IP: '+d.ip;msg.className='status-msg status-ok';}else if(d.connecting){msg.textContent='Connecting, please wait...';msg.className='status-msg status-info';setTimeout(()=>connectWifi(),500);}else{msg.textContent='✗ '+(d.error||'Connection failed');msg.className='status-msg status-err';}}).catch(e=>{msg.textContent='Connection failed: '+e;msg.className='status-msg status-err';});}
function syncNTP(){const msg=document.getElementById('tzMsg');msg.textContent='Syncing...';msg.className='status-msg status-info';
fetch('/api/ntp').then(r=>r.text()).then(t=>{msg.textContent='✓ Syncing with NTP server...';msg.className='status-msg status-ok';}).catch(e=>{msg.textContent='✗ Error: '+e;msg.className='status-msg status-err';});}
function saveTimezone(){const m=document.querySelector('input[name="tzmode"]:checked').value;const o=m==='manual'?document.getElementById('tzOffset').value:'0';
const b=document.getElementById('tzMsg');b.textContent='Saving...';b.className='status-msg status-info';
fetch('/api/timezone?mode='+m+'&offset='+o).then(r=>r.text()).then(t=>{b.textContent='✓ Saved!';b.className='status-msg status-ok';}).catch(e=>{b.textContent='✗ Error: '+e;b.className='status-msg status-err';});}
function saveBrightness(){const v=document.getElementById('brightness').value;fetch('/api/brightness?value='+Math.round(v/255*100)).catch(e=>console.warn(e));}
function saveDisplayMode(){const m=document.getElementById('displayMode').value;fetch('/api/display?mode='+m).catch(e=>console.warn(e));updateModeDescription();}
function updateModeDescription(){const m=parseInt(document.getElementById('displayMode').value);
const desc={0:'Rainbow orbit background with clear red hour, green minute, and blue second markers (5-3-7 LED spread).',
1:'Minimal clean mode: exactly 3 LEDs each for red hour, green minute, blue second. No background.',
2:'Very subtle background pulse (reduced from before) with strong HMS markers for easy readability.',
3:'Binary clock stretched across all 60 LEDs: 20 groups × 3 LEDs showing hour/minute/second bits in color.',
4:'Minute fills like a progress bar, hour shown as bright beacon, second has moving trail.',
5:'Optimized warm flame effect (20fps update) with clear HMS markers. Performance improved.',
6:'Soft pastel colors: pink hour, mint green minute, sky blue second. Gentle and easy on eyes.',
7:'Bright neon colors: magenta hour, cyan minute, yellow second. Vivid and energetic.',
8:'Animated comet trails: red hour (7 LED), green minute (5 LED), blue second (10 LED fast tail).'};
document.getElementById('modeDesc').textContent=desc[m]||'Mode '+m;}
function pollStatus(){fetch('/api/status').then(r=>r.json()).then(d=>{
document.getElementById('fwVersion').textContent=d.fw_version_base||'-';
document.getElementById('fwBuildTime').textContent='('+d.fw_build_time+')';
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
if(d.display_mode!==undefined){document.getElementById('displayMode').value=d.display_mode;updateModeDescription();}
}).catch(e=>console.warn(e));}
function getMaxSize(){fetch('/api/status').then(r=>r.json()).then(d=>{document.getElementById('maxSize').textContent=(d.heap||262144).toString();}).catch(e=>console.warn(e));}
pollStatus();getMaxSize();scanWifi();setInterval(pollStatus,5000);updateModeDescription();
</script></body></html>
)html";
