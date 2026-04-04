#!/usr/bin/env node
/**
 * ESP8266 Remote Debug Server
 *
 * Receives UDP log packets from the LED clock firmware and stores them.
 * Also serves a live log viewer over HTTP.
 *
 * Usage:  node scripts/debug-server.js [udp-port] [http-port]
 * Defaults: UDP 7878, HTTP 7879
 *
 * Configure the device at http://ledclock.local/settings → Debug Logging card.
 * View logs at http://localhost:7879
 */

'use strict';

const dgram = require('dgram');
const fs    = require('fs');
const http  = require('http');
const path  = require('path');
const os    = require('os');

const UDP_PORT  = parseInt(process.argv[2]) || 7878;
const HTTP_PORT = parseInt(process.argv[3]) || 7879;
const LOG_DIR   = path.join(__dirname, '..', 'debug-logs');
const MAX_LINES = 5000;   // keep in RAM
const SESSION_FILE = path.join(LOG_DIR, `session-${dateTag()}.log`);

function dateTag() {
  const d = new Date();
  return d.toISOString().replace(/[:.]/g, '-').replace('T', '_').slice(0, 19);
}

// Ensure log directory exists
if (!fs.existsSync(LOG_DIR)) fs.mkdirSync(LOG_DIR, { recursive: true });

/** Circular log buffer (in-RAM) */
const logBuf = [];

function appendLog(line) {
  const ts  = new Date().toISOString();
  const out = `${ts}  ${line}`;
  process.stdout.write(out + '\n');
  logBuf.push(out);
  if (logBuf.length > MAX_LINES) logBuf.shift();
  fs.appendFileSync(SESSION_FILE, out + '\n');
}

// ─── UDP receiver ────────────────────────────────────────────────────────────

const udp = dgram.createSocket('udp4');

udp.on('error', (err) => {
  console.error('[UDP] Error:', err.message);
  udp.close();
});

udp.on('message', (msg, rinfo) => {
  const text = msg.toString('utf8').replace(/\r?\n$/, '');
  const line = `[UDP ${rinfo.address}:${rinfo.port}] ${text}`;
  appendLog(line);
});

udp.bind(UDP_PORT, () => {
  console.log(`[UDP] Listening on 0.0.0.0:${UDP_PORT}`);
});

// ─── HTTP log viewer ─────────────────────────────────────────────────────────

const PAGE_STYLE = `
  body  { background:#111; color:#ccc; font-family:monospace; margin:0; padding:0; }
  h1    { background:#222; margin:0; padding:12px 20px; font-size:14px; color:#4CAF50; }
  pre   { padding:16px; white-space:pre-wrap; word-break:break-all; font-size:12px; line-height:1.5; }
  .err  { color:#ff5555; }
  .wrn  { color:#ffaa00; }
  .inf  { color:#55ff55; }
  .hb   { color:#888; }
  .tst  { color:#55aaff; }
  .boot { color:#ff55ff; }
  .toolbar { background:#222; padding:8px 20px; display:flex; gap:12px; align-items:center; }
  button { background:#333; color:#eee; border:1px solid #555; padding:4px 10px; cursor:pointer; border-radius:4px; }
  button:hover { background:#444; }
  .count { color:#888; font-size:11px; margin-left:auto; }
`;

function colorLine(line) {
  const escaped = line.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');
  if (/\[ERR\]/.test(line)) return `<span class="err">${escaped}</span>`;
  if (/\[WRN\]/.test(line)) return `<span class="wrn">${escaped}</span>`;
  if (/\[HB\]/.test(line))  return `<span class="hb">${escaped}</span>`;
  if (/\[TEST\]/.test(line)) return `<span class="tst">${escaped}</span>`;
  if (/\[BOOT\]/.test(line)) return `<span class="boot">${escaped}</span>`;
  return `<span class="inf">${escaped}</span>`;
}

const httpServer = http.createServer((req, res) => {
  const url = req.url.split('?')[0];

  if (url === '/raw') {
    res.writeHead(200, { 'Content-Type': 'text/plain; charset=utf-8' });
    res.end(logBuf.join('\n'));
    return;
  }

  if (url === '/clear') {
    logBuf.length = 0;
    appendLog('[SERVER] Log cleared via HTTP');
    res.writeHead(200, { 'Content-Type': 'text/plain' });
    res.end('cleared');
    return;
  }

  if (url === '/') {
    const lines = logBuf.slice(-500);
    const html = `<!DOCTYPE html>
<html><head><meta charset="utf-8">
<title>ESP Debug Log</title>
<style>${PAGE_STYLE}</style>
<script>
let autoScroll = true;
let lastCount = 0;
function poll() {
  fetch('/raw').then(r => r.text()).then(t => {
    const lines = t ? t.split('\\n') : [];
    if (lines.length !== lastCount) {
      lastCount = lines.length;
      const pre = document.getElementById('log');
      pre.innerHTML = lines.map(l => {
        const e = l.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');
        if (/\\[ERR\\]/.test(l)) return '<span class="err">'+e+'</span>';
        if (/\\[WRN\\]/.test(l)) return '<span class="wrn">'+e+'</span>';
        if (/\\[HB\\]/.test(l))  return '<span class="hb">'+e+'</span>';
        if (/\\[TEST\\]/.test(l)) return '<span class="tst">'+e+'</span>';
        if (/\\[BOOT\\]/.test(l)) return '<span class="boot">'+e+'</span>';
        return '<span class="inf">'+e+'</span>';
      }).join('\\n');
      document.getElementById('cnt').textContent = lines.length + ' lines';
      if (autoScroll) window.scrollTo(0, document.body.scrollHeight);
    }
  }).catch(() => {});
  setTimeout(poll, 1500);
}
function toggleScroll() {
  autoScroll = !autoScroll;
  document.getElementById('scrollBtn').textContent = autoScroll ? 'Auto-scroll ON' : 'Auto-scroll OFF';
}
window.onload = () => { poll(); };
</script>
</head>
<body>
<h1>ESP8266 Debug Log &mdash; UDP:${UDP_PORT} | File: ${path.basename(SESSION_FILE)}</h1>
<div class="toolbar">
  <button onclick="fetch('/clear').then(()=>location.reload())">Clear</button>
  <button onclick="window.open('/raw')">Raw</button>
  <button id="scrollBtn" onclick="toggleScroll()">Auto-scroll ON</button>
  <span class="count" id="cnt">${logBuf.length} lines</span>
</div>
<pre id="log">${lines.map(colorLine).join('\n')}</pre>
</body></html>`;
    res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' });
    res.end(html);
    return;
  }

  res.writeHead(404);
  res.end('Not found');
});

httpServer.listen(HTTP_PORT, () => {
  // Print all local IPs so you know which to give the device
  const nets = os.networkInterfaces();
  const addrs = [];
  for (const name of Object.keys(nets)) {
    for (const n of nets[name]) {
      if (n.family === 'IPv4' && !n.internal) addrs.push(n.address);
    }
  }
  console.log(`[HTTP] Log viewer at http://localhost:${HTTP_PORT}`);
  console.log(`[INFO] Your local IPs: ${addrs.join(', ')}`);
  console.log(`[INFO] Enter one of these IPs in the Debug card on http://ledclock.local/settings`);
  console.log(`[INFO] Session log: ${SESSION_FILE}`);
  console.log('[INFO] Waiting for packets...\n');
});
