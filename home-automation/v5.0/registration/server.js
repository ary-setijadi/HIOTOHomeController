'use strict';

// Device registration app (v5.0).
// Serves a camera page that scans a device's QR code, then fetches that
// device's certificate from the Pi's device-manager and saves it locally
// (so the device can connect over MQTTS with its own certificate).
//   node server.js   ->   http://127.0.0.1:3007

const http = require('http');
const fs = require('fs');
const path = require('path');

const PORT = Number(process.env.PORT) || 3007;
const PUBLIC_DIR = path.join(__dirname, 'public');
const CERTS_DIR = path.join(__dirname, '..', 'certs');
const JSQR_PATH = path.join(__dirname, 'node_modules', 'jsqr', 'dist', 'jsQR.js');

const MIME = { '.html': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8', '.css': 'text/css; charset=utf-8' };

function sendJson(res, code, obj) { res.writeHead(code, { 'Content-Type': 'application/json' }); res.end(JSON.stringify(obj)); }
function readBody(req) {
  return new Promise((resolve) => {
    let d = '';
    req.on('data', (c) => { d += c; if (d.length > 1e6) req.destroy(); });
    req.on('end', () => { try { resolve(JSON.parse(d || '{}')); } catch { resolve({}); } });
  });
}

// Fetch the device's ca/cert/key from the enroll URL and save them locally.
function enrollDevice(url, cb) {
  const req = http.request(url, { timeout: 10000 }, (res) => {
    let d = '';
    res.on('data', (c) => { d += c; });
    res.on('end', () => {
      let p; try { p = JSON.parse(d); } catch { return cb({ ok: false, error: 'bad enroll response' }); }
      if (!p.serial || !p.cert || !p.key) return cb({ ok: false, error: 'enroll payload incomplete' });
      fs.mkdirSync(CERTS_DIR, { recursive: true });
      fs.writeFileSync(path.join(CERTS_DIR, p.serial + '.crt'), p.cert);
      fs.writeFileSync(path.join(CERTS_DIR, p.serial + '.key'), p.key);
      fs.writeFileSync(path.join(CERTS_DIR, 'ca.crt'), p.ca);
      cb({ ok: true, serial: p.serial, broker: p.broker });
    });
  });
  req.on('error', () => cb({ ok: false, error: 'enroll endpoint unreachable' }));
  req.on('timeout', () => { req.destroy(); cb({ ok: false, error: 'enroll timeout' }); });
  req.end();
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
  if (url === '/api/enroll' && req.method === 'POST') {
    const b = await readBody(req);
    if (!b.enroll) return sendJson(res, 400, { ok: false, error: 'enroll URL required' });
    return enrollDevice(b.enroll, (r) => sendJson(res, r.ok ? 200 : 502, r));
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

server.listen(PORT, '127.0.0.1', () => console.log(`Device registration app -> http://127.0.0.1:${PORT}`));
