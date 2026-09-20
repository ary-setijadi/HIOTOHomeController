'use strict';

// V4m Device + Rule Manager — a small Node.js UI to manage the controller's
// registrations and rule_devices. It serves a static single-page UI and proxies
// every /api/* request to the controller (:8081).
//   node server.js  ->  http://127.0.0.1:3006

const http = require('http');
const fs = require('fs');
const path = require('path');

const PORT = Number(process.env.PORT) || 3006;
const CONTROLLER_HOST = process.env.CONTROLLER_HOST || '192.168.1.22';
const CONTROLLER_PORT = Number(process.env.CONTROLLER_PORT || 8081);
const PUBLIC_DIR = path.join(__dirname, 'public');

const MIME = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.json': 'application/json',
  '.png': 'image/png',
};

function proxy(req, res) {
  const chunks = [];
  req.on('data', (c) => chunks.push(c));
  req.on('end', () => {
    const body = Buffer.concat(chunks);
    const headers = { Host: `${CONTROLLER_HOST}:${CONTROLLER_PORT}` };
    if (body.length) { headers['Content-Type'] = 'application/json'; headers['Content-Length'] = body.length; }
    const fail = (code, msg) => {
      if (res.headersSent) { res.end(); return; }
      res.writeHead(code, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({ error: msg }));
    };
    const preq = http.request({ host: CONTROLLER_HOST, port: CONTROLLER_PORT, path: req.url, method: req.method, headers }, (pres) => {
      if (res.headersSent) { pres.resume(); return; }
      res.writeHead(pres.statusCode, { 'Content-Type': pres.headers['content-type'] || 'application/json' });
      pres.pipe(res);
    });
    preq.on('error', () => fail(502, 'controller unreachable'));
    preq.setTimeout(20000, () => { preq.destroy(); fail(504, 'timeout'); });
    if (body.length) preq.write(body);
    preq.end();
  });
}

function serveStatic(req, res) {
  const rel = (req.url === '/' ? 'index.html' : decodeURIComponent((req.url || '').split('?')[0]).replace(/^\/+/, ''));
  const fp = path.join(PUBLIC_DIR, rel);
  if (!fp.startsWith(PUBLIC_DIR)) { res.writeHead(403); return res.end('Forbidden'); }
  fs.readFile(fp, (err, data) => {
    if (err) { res.writeHead(404, { 'Content-Type': 'text/plain' }); return res.end('Not found'); }
    res.writeHead(200, { 'Content-Type': MIME[path.extname(fp)] || 'application/octet-stream', 'Cache-Control': 'no-cache' });
    res.end(data);
  });
}

const server = http.createServer((req, res) => {
  if ((req.url || '').startsWith('/api/')) return proxy(req, res);
  if (req.method === 'GET') return serveStatic(req, res);
  res.writeHead(404); res.end('Not found');
});

server.listen(PORT, '127.0.0.1', () => console.log(`V4m Manager -> http://127.0.0.1:${PORT}  (controller ${CONTROLLER_HOST}:${CONTROLLER_PORT})`));
