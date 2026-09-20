'use strict';

// V4m device registration app (plaintext). Serves a camera page that scans a
// device QR code and saves the device's connection config locally.
//   node server.js   ->   http://127.0.0.1:3007

const http = require('http');
const fs = require('fs');
const path = require('path');

const PORT = Number(process.env.PORT) || 3007;
const PUBLIC_DIR = path.join(__dirname, 'public');
const DEVICES_DIR = path.join(__dirname, '..', 'simulator', 'devices');
const JSQR_PATH = path.join(__dirname, 'node_modules', 'jsqr', 'dist', 'jsQR.js');

const MIME = { '.html': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8' };

function sendJson(res, code, obj) { res.writeHead(code, { 'Content-Type': 'application/json' }); res.end(JSON.stringify(obj)); }
function readBody(req) {
  return new Promise((resolve) => {
    let d = '';
    req.on('data', (c) => { d += c; if (d.length > 1e6) req.destroy(); });
    req.on('end', () => { try { resolve(JSON.parse(d || '{}')); } catch { resolve({}); } });
  });
}

const server = http.createServer(async (req, res) => {
  const url = (req.url || '').split('?')[0];
  if (url === '/jsqr.js') {
    fs.readFile(JSQR_PATH, (err, data) => {
      if (err) { res.writeHead(404); return res.end('jsqr not found'); }
      res.writeHead(200, { 'Content-Type': 'text/javascript' });
      res.end(data);
    });
    return;
  }
  if (url === '/api/config' && req.method === 'POST') {
    const b = await readBody(req);
    if (!b.guid) return sendJson(res, 400, { ok: false, error: 'guid required' });
    fs.mkdirSync(DEVICES_DIR, { recursive: true });
    fs.writeFileSync(path.join(DEVICES_DIR, b.guid + '.json'), JSON.stringify(b, null, 2));
    return sendJson(res, 200, { ok: true, guid: b.guid, broker: b.broker });
  }
  if (req.method === 'GET') {
    const rel = url === '/' ? 'index.html' : decodeURIComponent(url).replace(/^\/+/, '');
    const fp = path.join(PUBLIC_DIR, rel);
    if (!fp.startsWith(PUBLIC_DIR)) { res.writeHead(403); return res.end('Forbidden'); }
    return fs.readFile(fp, (err, data) => {
      if (err) { res.writeHead(404, { 'Content-Type': 'text/plain' }); return res.end('Not found'); }
      res.writeHead(200, { 'Content-Type': MIME[path.extname(fp)] || 'application/octet-stream', 'Cache-Control': 'no-cache' });
      res.end(data);
    });
  }
  res.writeHead(404); res.end('Not found');
});

server.listen(PORT, '127.0.0.1', () => console.log(`V4m device registration app -> http://127.0.0.1:${PORT}`));
