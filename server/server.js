/**
 * Trilaterasi LoRa — Express.js Server
 * 
 * Instalasi:
 *   npm install
 * 
 * Jalankan:
 *   node server.js
 * 
 * Dashboard:
 *   http://localhost:5000
 */

const express  = require('express');
const cors     = require('cors');
const path     = require('path');

const app  = express();
const PORT = 5000;

app.use(cors());
app.use(express.json());

// ─── Konfigurasi Path Loss ────────────────────────────────────────────────────
const CONFIG = {
  TX_POWER_DBM      : 17,    // Harus sama dengan LORA_TX_POWER di anchor
  PATH_LOSS_EXP     : 2.7,   // n: 2.0 = LOS terbuka, 2.7–3.5 = dalam gedung
  RSSI_1M           : -40,   // RSSI terukur pada jarak 1 meter (kalibrasi)
  ANCHOR_TIMEOUT_MS : 10000, // Anchor dianggap mati jika tidak ada data > 10 detik
  MIN_ANCHORS       : 3,     // Jumlah anchor minimum untuk trilaterasi
  MAX_HISTORY       : 200,   // Maksimum history posisi yang disimpan
};

// ─── State ───────────────────────────────────────────────────────────────────

// Buffer RSSI per tag: { tagId: { anchorId: { rssi, snr, ax, ay, distance, ts } } }
const anchorBuffer = {};

// Posisi terakhir per tag: { tagId: { x, y, ts } }
const positions = {};

// History posisi: [ { tag, x, y, ts } ]
const positionHistory = [];

// SSE clients
const sseClients = new Set();

// ─── Helper: RSSI → Jarak ────────────────────────────────────────────────────
function rssiToDistance(rssi) {
  if (rssi >= CONFIG.RSSI_1M) return 0.1; // clamp minimum 10 cm
  const distance = Math.pow(10, (CONFIG.RSSI_1M - rssi) / (10 * CONFIG.PATH_LOSS_EXP));
  return Math.round(distance * 1000) / 1000;
}

// ─── Algoritma Trilaterasi (Least Squares) ───────────────────────────────────
/**
 * anchors: [ { ax, ay, distance }, ... ]
 * return:  { x, y } atau null
 */
function trilaterate(anchors) {
  if (anchors.length < 3) return null;

  const [ref, ...rest] = anchors;
  const { ax: x1, ay: y1, distance: d1 } = ref;

  const A = [];
  const b = [];

  for (const { ax: xi, ay: yi, distance: di } of rest) {
    A.push([2 * (xi - x1), 2 * (yi - y1)]);
    b.push(di ** 2 - xi ** 2 - yi ** 2 - d1 ** 2 + x1 ** 2 + y1 ** 2);
  }

  // Least squares: x = (A^T A)^-1 A^T b  (manual untuk 2x2)
  try {
    const At    = transpose(A);
    const AtA   = matMul(At, A);
    const AtAInv = invert2x2(AtA);
    if (!AtAInv) return null;

    const Atb = matVecMul(At, b);
    const x   = AtAInv[0][0] * Atb[0] + AtAInv[0][1] * Atb[1];
    const y   = AtAInv[1][0] * Atb[0] + AtAInv[1][1] * Atb[1];

    return {
      x: Math.round(x * 1000) / 1000,
      y: Math.round(y * 1000) / 1000,
    };
  } catch {
    return null;
  }
}

// ─── Matrix helpers (tanpa library eksternal) ─────────────────────────────────
function transpose(M) {
  return M[0].map((_, j) => M.map(row => row[j]));
}

function matMul(A, B) {
  const rows = A.length, cols = B[0].length, inner = B.length;
  return Array.from({ length: rows }, (_, i) =>
    Array.from({ length: cols }, (_, j) =>
      Array.from({ length: inner }, (_, k) => A[i][k] * B[k][j])
        .reduce((s, v) => s + v, 0)
    )
  );
}

function matVecMul(M, v) {
  return M.map(row => row.reduce((s, val, j) => s + val * v[j], 0));
}

function invert2x2(M) {
  const det = M[0][0] * M[1][1] - M[0][1] * M[1][0];
  if (Math.abs(det) < 1e-10) return null;
  return [
    [ M[1][1] / det, -M[0][1] / det],
    [-M[1][0] / det,  M[0][0] / det],
  ];
}

// ─── SSE Helper ──────────────────────────────────────────────────────────────
function ssePush(eventType, data) {
  const message = `event: ${eventType}\ndata: ${JSON.stringify(data)}\n\n`;
  for (const res of sseClients) {
    try { res.write(message); } catch { sseClients.delete(res); }
  }
}

// ─── Routes ──────────────────────────────────────────────────────────────────

// POST /api/anchor-report — terima data dari anchor ESP32
app.post('/api/anchor-report', (req, res) => {
  const { anchor, tag, rssi, snr = 0, ax, ay, seq = 0 } = req.body;

  if (anchor == null || tag == null || rssi == null || ax == null || ay == null) {
    return res.status(400).json({ error: 'Missing required fields: anchor, tag, rssi, ax, ay' });
  }

  const distance = rssiToDistance(rssi);
  const now      = Date.now();

  // Simpan ke buffer
  if (!anchorBuffer[tag]) anchorBuffer[tag] = {};
  anchorBuffer[tag][anchor] = { rssi, snr, ax, ay, distance, seq, ts: now };

  // Push update RSSI ke dashboard
  ssePush('rssi_update', { anchor, tag, rssi, snr, distance, seq, ts: now });

  // Cek apakah cukup anchor aktif untuk trilaterasi
  const activeAnchors = Object.entries(anchorBuffer[tag])
    .filter(([, info]) => now - info.ts < CONFIG.ANCHOR_TIMEOUT_MS)
    .map(([id, info]) => ({ id: Number(id), ...info }));

  if (activeAnchors.length >= CONFIG.MIN_ANCHORS) {
    const pos = trilaterate(activeAnchors);

    if (pos) {
      positions[tag] = { ...pos, ts: now };

      // Simpan history
      positionHistory.push({ tag, ...pos, ts: now });
      if (positionHistory.length > CONFIG.MAX_HISTORY) positionHistory.shift();

      // Push posisi baru ke dashboard
      ssePush('position_update', {
        tag,
        x      : pos.x,
        y      : pos.y,
        anchors: activeAnchors.map(a => ({
          id: a.id, rssi: a.rssi, distance: a.distance, ax: a.ax, ay: a.ay,
        })),
        ts: now,
      });
    }
  }

  res.json({ status: 'ok', distance });
});

// GET /api/status — status semua anchor dan posisi tag
app.get('/api/status', (req, res) => {
  const now    = Date.now();
  const result = {};

  for (const [tagId, anchors] of Object.entries(anchorBuffer)) {
    result[tagId] = {
      anchors : Object.fromEntries(
        Object.entries(anchors).map(([aid, info]) => [
          aid, { ...info, active: now - info.ts < CONFIG.ANCHOR_TIMEOUT_MS }
        ])
      ),
      position: positions[tagId] ?? null,
    };
  }

  res.json(result);
});

// GET /api/history — history posisi semua tag
app.get('/api/history', (req, res) => {
  res.json(positionHistory);
});

// GET /events — SSE endpoint untuk dashboard
app.get('/events', (req, res) => {
  res.setHeader('Content-Type',  'text/event-stream');
  res.setHeader('Cache-Control', 'no-cache');
  res.setHeader('Connection',    'keep-alive');
  res.flushHeaders();

  // Kirim status awal
  res.write(`event: init\ndata: ${JSON.stringify({ status: 'connected' })}\n\n`);
  sseClients.add(res);

  // Keepalive setiap 20 detik
  const keepalive = setInterval(() => {
    try { res.write(': ping\n\n'); } catch { clearInterval(keepalive); }
  }, 20000);

  req.on('close', () => {
    clearInterval(keepalive);
    sseClients.delete(res);
  });
});

// GET / — serve dashboard HTML
app.get('/', (req, res) => {
  res.send(DASHBOARD_HTML);
});

// ─── Start Server ─────────────────────────────────────────────────────────────
app.listen(PORT, '0.0.0.0', () => {
  const { networkInterfaces } = require('os');
  const nets = networkInterfaces();
  let localIP = 'localhost';
  for (const ifaces of Object.values(nets)) {
    for (const iface of ifaces) {
      if (iface.family === 'IPv4' && !iface.internal) { localIP = iface.address; break; }
    }
  }

  console.log('='.repeat(55));
  console.log('  Trilaterasi LoRa — Indoor Positioning Server');
  console.log('='.repeat(55));
  console.log(`  Dashboard  : http://localhost:${PORT}`);
  console.log(`  API        : http://localhost:${PORT}/api/anchor-report`);
  console.log(`  IP PC ini  : ${localIP}  ← masukkan ke SERVER_IP di lora_config.h`);
  console.log(`  TX Power   : ${CONFIG.TX_POWER_DBM} dBm`);
  console.log(`  Path Loss  : n = ${CONFIG.PATH_LOSS_EXP}`);
  console.log(`  RSSI @1m   : ${CONFIG.RSSI_1M} dBm`);
  console.log('='.repeat(55));
});

// ─── Dashboard HTML ───────────────────────────────────────────────────────────
const DASHBOARD_HTML = `<!DOCTYPE html>
<html lang="id">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Trilaterasi LoRa — Indoor Positioning</title>
<style>
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body { font-family: 'Segoe UI', sans-serif; background: #0f172a; color: #e2e8f0; min-height: 100vh; }

  header { background: #1e293b; padding: 16px 24px; display: flex; align-items: center; gap: 12px; border-bottom: 1px solid #334155; }
  header h1 { font-size: 1.2rem; font-weight: 600; color: #f1f5f9; }
  .badge { background: #10b981; color: white; font-size: 0.7rem; padding: 2px 8px; border-radius: 999px; }
  .badge.offline { background: #ef4444; }

  .grid { display: grid; grid-template-columns: 1fr 340px; gap: 16px; padding: 16px; height: calc(100vh - 61px); }
  .card { background: #1e293b; border: 1px solid #334155; border-radius: 12px; padding: 16px; }
  .card h2 { font-size: 0.85rem; font-weight: 600; color: #94a3b8; text-transform: uppercase; letter-spacing: .05em; margin-bottom: 12px; }

  #map-card { display: flex; flex-direction: column; }
  #map-canvas { flex: 1; border-radius: 8px; background: #0f172a; }

  .sidebar { display: flex; flex-direction: column; gap: 12px; overflow-y: auto; }
  .anchor-list { display: flex; flex-direction: column; gap: 8px; }
  .anchor-item { background: #0f172a; border-radius: 8px; padding: 10px 12px; display: flex; justify-content: space-between; align-items: center; border-left: 3px solid #334155; }
  .anchor-item.active { border-left-color: #10b981; }
  .anchor-name { font-weight: 600; font-size: .9rem; }
  .anchor-meta { font-size: .75rem; color: #64748b; margin-top: 2px; }
  .rssi-val { font-size: 1.1rem; font-weight: 700; color: #10b981; }
  .rssi-val.weak { color: #f59e0b; }
  .rssi-val.bad  { color: #ef4444; }

  .pos-display { text-align: center; padding: 8px; }
  .pos-coords  { font-size: 1.4rem; font-weight: 700; color: #38bdf8; font-family: monospace; white-space: pre; }
  .pos-sub     { font-size: .75rem; color: #64748b; margin-top: 4px; }

  #log { font-family: monospace; font-size: .72rem; color: #64748b; max-height: 160px; overflow-y: auto; display: flex; flex-direction: column; }
  .log-line { padding: 2px 0; border-bottom: 1px solid #0f172a; }
  .log-line.ok   { color: #10b981; }
  .log-line.warn { color: #f59e0b; }

  .status-bar { display: flex; gap: 16px; font-size: .75rem; color: #64748b; margin-left: auto; }
  .dot { width:8px;height:8px;border-radius:50%;background:#10b981;display:inline-block;margin-right:4px;animation:pulse 2s infinite; }
  @keyframes pulse{0%,100%{opacity:1}50%{opacity:.3}}
</style>
</head>
<body>
<header>
  <span>📡</span>
  <h1>Trilaterasi LoRa — Indoor Positioning</h1>
  <span class="badge offline" id="conn-badge">Connecting...</span>
  <div class="status-bar">
    <span><span class="dot"></span>Live</span>
    <span id="pkt-count">0 paket</span>
  </div>
</header>

<div class="grid">
  <div class="card" id="map-card">
    <h2>Peta Posisi Real-time</h2>
    <canvas id="map-canvas"></canvas>
  </div>

  <div class="sidebar">
    <div class="card">
      <h2>Posisi Tag</h2>
      <div class="pos-display">
        <div class="pos-coords" id="pos-xy">—</div>
        <div class="pos-sub" id="pos-sub">Menunggu data dari 3 anchor...</div>
      </div>
    </div>

    <div class="card">
      <h2>Status Anchor</h2>
      <div class="anchor-list" id="anchor-list">
        <div class="anchor-item" id="anc-1"><div><div class="anchor-name">Anchor 1</div><div class="anchor-meta">Menunggu...</div></div><div class="rssi-val">—</div></div>
        <div class="anchor-item" id="anc-2"><div><div class="anchor-name">Anchor 2</div><div class="anchor-meta">Menunggu...</div></div><div class="rssi-val">—</div></div>
        <div class="anchor-item" id="anc-3"><div><div class="anchor-name">Anchor 3</div><div class="anchor-meta">Menunggu...</div></div><div class="rssi-val">—</div></div>
      </div>
    </div>

    <div class="card" style="flex:1">
      <h2>Log</h2>
      <div id="log"></div>
    </div>
  </div>
</div>

<script>
const ROOM_W = 10, ROOM_H = 10;
let anchorData = {}, tagPosition = null, tagTrail = [], pktCount = 0;

const canvas = document.getElementById('map-canvas');
const ctx    = canvas.getContext('2d');

function resizeCanvas() {
  const card = document.getElementById('map-card');
  canvas.width  = card.clientWidth  - 32;
  canvas.height = card.clientHeight - 50;
  drawMap();
}

function w2c(wx, wy) {
  const pad = 44, W = canvas.width - pad*2, H = canvas.height - pad*2;
  return { x: pad + (wx/ROOM_W)*W, y: pad + (1 - wy/ROOM_H)*H };
}

function drawMap() {
  ctx.clearRect(0, 0, canvas.width, canvas.height);
  const pad = 44, W = canvas.width-pad*2, H = canvas.height-pad*2;

  // Grid
  ctx.strokeStyle = '#1e293b'; ctx.lineWidth = 1;
  for (let i=0; i<=ROOM_W; i++) {
    const x = pad + (i/ROOM_W)*W;
    ctx.beginPath(); ctx.moveTo(x, pad); ctx.lineTo(x, pad+H); ctx.stroke();
    ctx.fillStyle='#334155'; ctx.font='10px sans-serif'; ctx.textAlign='center';
    ctx.fillText(i+'m', x, pad+H+14);
  }
  for (let j=0; j<=ROOM_H; j++) {
    const y = pad + (1-j/ROOM_H)*H;
    ctx.beginPath(); ctx.moveTo(pad, y); ctx.lineTo(pad+W, y); ctx.stroke();
    ctx.fillStyle='#334155'; ctx.font='10px sans-serif'; ctx.textAlign='right';
    ctx.fillText(j+'m', pad-6, y+4);
  }

  // Lingkaran jarak anchor
  const scale = W / ROOM_W;
  for (const [aid, info] of Object.entries(anchorData)) {
    if (!info.distance) continue;
    const c = w2c(info.ax, info.ay);
    ctx.beginPath(); ctx.arc(c.x, c.y, info.distance * scale, 0, Math.PI*2);
    ctx.strokeStyle='rgba(56,189,248,0.12)'; ctx.lineWidth=1.5; ctx.stroke();
  }

  // Trail tag
  if (tagTrail.length > 1) {
    ctx.beginPath(); ctx.strokeStyle='rgba(16,185,129,0.35)'; ctx.lineWidth=2;
    const s = w2c(tagTrail[0].x, tagTrail[0].y);
    ctx.moveTo(s.x, s.y);
    tagTrail.forEach((p,i)=>{ if(i>0){const c=w2c(p.x,p.y);ctx.lineTo(c.x,c.y);} });
    ctx.stroke();
  }

  // Anchor nodes
  const COLORS = ['#f59e0b','#a78bfa','#fb7185'];
  for (const [aid, info] of Object.entries(anchorData)) {
    const c = w2c(info.ax, info.ay);
    const col = COLORS[(parseInt(aid)-1) % COLORS.length];
    ctx.beginPath(); ctx.arc(c.x, c.y, 11, 0, Math.PI*2);
    ctx.fillStyle=col; ctx.fill();
    ctx.fillStyle='#0f172a'; ctx.font='bold 9px sans-serif';
    ctx.textAlign='center'; ctx.textBaseline='middle';
    ctx.fillText('A'+aid, c.x, c.y);
    ctx.fillStyle='#94a3b8'; ctx.font='10px sans-serif'; ctx.textBaseline='top';
    ctx.fillText('('+info.ax+','+info.ay+')', c.x, c.y+14);
  }

  // Tag
  if (tagPosition) {
    const c = w2c(tagPosition.x, tagPosition.y);
    const g = ctx.createRadialGradient(c.x,c.y,0,c.x,c.y,22);
    g.addColorStop(0,'rgba(16,185,129,0.35)'); g.addColorStop(1,'rgba(16,185,129,0)');
    ctx.beginPath(); ctx.arc(c.x,c.y,22,0,Math.PI*2); ctx.fillStyle=g; ctx.fill();
    ctx.beginPath(); ctx.arc(c.x,c.y,9,0,Math.PI*2);
    ctx.fillStyle='#10b981'; ctx.fill();
    ctx.fillStyle='white'; ctx.font='bold 8px sans-serif';
    ctx.textAlign='center'; ctx.textBaseline='middle';
    ctx.fillText('T', c.x, c.y);
    ctx.fillStyle='#10b981'; ctx.font='bold 11px sans-serif'; ctx.textBaseline='bottom';
    ctx.fillText('('+tagPosition.x.toFixed(2)+', '+tagPosition.y.toFixed(2)+')', c.x, c.y-14);
  }
}

window.addEventListener('resize', resizeCanvas);
setTimeout(resizeCanvas, 100);

function updateAnchorUI(id, info) {
  const el = document.getElementById('anc-'+id); if (!el) return;
  el.querySelector('.rssi-val').textContent = info.rssi+' dBm';
  el.querySelector('.rssi-val').className = 'rssi-val'+(info.rssi>-65?'':info.rssi>-80?' weak':' bad');
  el.querySelector('.anchor-meta').textContent = info.distance.toFixed(2)+' m | SNR: '+info.snr+' dB';
  el.className = 'anchor-item active';
}

function addLog(msg, type='ok') {
  const log=document.getElementById('log');
  const line=document.createElement('div');
  line.className='log-line '+type;
  line.textContent='['+new Date().toLocaleTimeString()+'] '+msg;
  log.prepend(line);
  if (log.children.length>80) log.removeChild(log.lastChild);
}

// SSE
const badge = document.getElementById('conn-badge');
const es    = new EventSource('/events');

es.addEventListener('init', () => {
  badge.textContent='Online'; badge.className='badge';
  addLog('Terhubung ke server');
});

es.addEventListener('rssi_update', e => {
  const d = JSON.parse(e.data);
  pktCount++;
  document.getElementById('pkt-count').textContent = pktCount+' paket';
  anchorData[d.anchor] = { rssi:d.rssi, snr:d.snr, distance:d.distance,
    ax: anchorData[d.anchor]?.ax ?? 0, ay: anchorData[d.anchor]?.ay ?? 0 };
  updateAnchorUI(d.anchor, anchorData[d.anchor]);
  addLog('Anchor '+d.anchor+' → Tag '+d.tag+' | RSSI: '+d.rssi+' dBm | '+d.distance.toFixed(2)+' m');
  drawMap();
});

es.addEventListener('position_update', e => {
  const d = JSON.parse(e.data);
  tagPosition = { x:d.x, y:d.y };
  d.anchors.forEach(a => {
    anchorData[a.id] = { ...(anchorData[a.id]||{}), ax:a.ax, ay:a.ay, rssi:a.rssi, distance:a.distance };
  });
  tagTrail.push({x:d.x,y:d.y});
  if (tagTrail.length>60) tagTrail.shift();
  document.getElementById('pos-xy').textContent = 'X: '+d.x.toFixed(2)+' m\\nY: '+d.y.toFixed(2)+' m';
  document.getElementById('pos-sub').textContent = 'Update: '+new Date().toLocaleTimeString();
  addLog('Posisi Tag '+d.tag+' → X: '+d.x.toFixed(2)+' m, Y: '+d.y.toFixed(2)+' m');
  drawMap();
});

es.onerror = () => {
  badge.textContent='Offline'; badge.className='badge offline';
  addLog('Koneksi terputus','warn');
};
</script>
</body>
</html>`;