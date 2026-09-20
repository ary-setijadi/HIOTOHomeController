'use strict';

// v5.0 Security & Traffic Monitor.
//
// Observes every message on the home.automation exchange (over MQTTS with the
// "monitor" certificate), classifies the logical flow of each message, and
// presents it in a live dashboard. It also surfaces the broker's TLS/mTLS
// connection table and can run an impostor-device test on demand.
//
//   node monitor.js   ->   http://127.0.0.1:3006

const http = require('http');
const https = require('https');
const fs = require('fs');
const path = require('path');
const { execFile } = require('child_process');
const mqtt = require('mqtt');
const lib = require('../simulator/lib');

const PORT = Number(process.env.PORT) || 3006;
const PUBLIC_DIR = path.join(__dirname, 'public');
const BROKER_API_HOST = process.env.BROKER_API_HOST || '192.168.137.44';
const BROKER_API_PORT = Number(process.env.BROKER_API_PORT || 15671);
const BROKER_API_USER = process.env.BROKER_API_USER || 'admin';
const BROKER_API_PASS = process.env.BROKER_API_PASS || '123456Aa!';

const MAX_MSGS = 500;
const messages = [];
const counts = { total: 0, flows: {} };
const sseClients = new Set();

// Classify a topic into its logical flow across the system.
// Topic shape: home/<kind>/<type>/<serial>/<class>  (5 segments)
function classify(topic) {
  const p = topic.split('/');
  if (topic === 'home/config/rules') return { flow: 'UI → CONTROLLER (rule)', tag: 'ui-ctrl' };
  if (p.length === 5 && p[1] === 'sensor' && p[4] === 'state') return { flow: 'SENSOR → CONTROLLER', tag: 'dev-ctrl' };
  if (p.length === 5 && p[1] === 'actuator' && p[4] === 'state') return { flow: 'ACTUATOR → UI', tag: 'dev-ui' };
  if (p.length === 5 && p[1] === 'actuator' && p[4] === 'cmd') return { flow: 'CONTROLLER → DEVICE', tag: 'ctrl-dev' };
  if (p.length === 5 && p[1] === 'actuator' && p[4] === 'override') return { flow: 'UI → CONTROLLER (override)', tag: 'ui-ctrl' };
  return { flow: topic, tag: 'other' };
}

function broadcast(obj) {
  const data = `data: ${JSON.stringify(obj)}\n\n`;
  for (const res of sseClients) res.write(data);
}

const client = mqtt.connect(lib.brokerUrl(), lib.clientOptions('monitor', 'mon'));

client.on('connect', () => {
  client.subscribe('home/#', { qos: 1 });
  console.log('monitor connected (MQTTS, monitor cert)');
  broadcast({ type: 'status', connected: true });
});
client.on('close', () => broadcast({ type: 'status', connected: false }));
client.on('error', (e) => console.error('monitor error:', e.message));

client.on('message', (topic, buf) => {
  let env; try { env = JSON.parse(buf.toString()); } catch { env = null; }
  const cls = classify(topic);
  const rec = {
    t: new Date().toISOString(),
    flow: cls.flow,
    tag: cls.tag,
    topic,
    source: env ? env.source : '',
    class: env ? env.message_class : '',
    payload: env ? env.payload : null,
  };
  messages.unshift(rec);
  if (messages.length > MAX_MSGS) messages.pop();
  counts.total++;
  counts.flows[cls.flow] = (counts.flows[cls.flow] || 0) + 1;
  broadcast({ type: 'message', rec, counts });
});

// --- security info from the RabbitMQ management API (TLS/mTLS connection table) ---
function fetchSecurity(cb) {
  const req = https.request({
    host: BROKER_API_HOST,
    port: BROKER_API_PORT,
    path: '/api/connections',
    method: 'GET',
    rejectUnauthorized: false, // broker cert is signed by our internal CA
    auth: `${BROKER_API_USER}:${BROKER_API_PASS}`,
  }, (res) => {
    let d = '';
    res.on('data', (c) => { d += c; });
    res.on('end', () => { try { cb(JSON.parse(d)); } catch { cb([]); } });
  });
  req.on('error', () => cb([]));
  req.setTimeout(8000, () => { req.destroy(); cb([]); });
  req.end();
}

function securitySummary(conns) {
  const total = conns.length;
  const tls = conns.filter((c) => c.ssl).length;
  const certs = conns.map((c) => ({
    protocol: c.protocol,
    ssl: !!c.ssl,
    cn: String(c.peer_cert_subject || '').replace(/^CN=/, '') || '(none)',
    name: c.name,
  }));
  return { total, tls, allTls: total > 0 && tls === total, certs };
}

function runImpostor(cb) {
  execFile(process.execPath, [path.join(__dirname, 'impostor.js')], { timeout: 45000 }, (err, stdout, stderr) => {
    if (err) { cb({ ok: false, error: String(stderr || err.message) }); return; }
    try { cb({ ok: true, results: JSON.parse(stdout) }); } catch { cb({ ok: false, error: stdout }); }
  });
}

// --- HTTP server ---
function sendJson(res, code, obj) { res.writeHead(code, { 'Content-Type': 'application/json' }); res.end(JSON.stringify(obj)); }
const MIME = { '.html': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8', '.css': 'text/css; charset=utf-8' };

function serveStatic(req, res) {
  const rel = (req.url === '/' ? 'index.html' : decodeURIComponent((req.url || '').split('?')[0]).replace(/^\/+/, ''));
  const fp = path.join(PUBLIC_DIR, rel);
  if (!fp.startsWith(PUBLIC_DIR)) { res.writeHead(403); return res.end('Forbidden'); }
  fs.readFile(fp, (err, data) => {
    if (err) { res.writeHead(404, { 'Content-Type': 'text/plain' }); return res.end('Not found'); }
    res.writeHead(200, { 'Content-Type': MIME[path.extname(fp)] || 'application/octet-stream' });
    res.end(data);
  });
}

const server = http.createServer((req, res) => {
  const url = (req.url || '').split('?')[0];
  if (url === '/events') {
    res.writeHead(200, { 'Content-Type': 'text/event-stream', 'Cache-Control': 'no-cache', 'Connection': 'keep-alive' });
    res.write(`data: ${JSON.stringify({ type: 'hello', messages, counts, connected: client.connected })}\n\n`);
    sseClients.add(res);
    req.on('close', () => sseClients.delete(res));
    return;
  }
  if (url === '/api/messages') return sendJson(res, 200, { messages, counts });
  if (url === '/api/security') return fetchSecurity((conns) => sendJson(res, 200, securitySummary(conns)));
  if (url === '/api/impostor' && req.method === 'POST') return runImpostor((r) => sendJson(res, 200, r));
  if (req.method === 'GET') return serveStatic(req, res);
  res.writeHead(404); res.end('Not found');
});

server.listen(PORT, '127.0.0.1', () => console.log(`Security & Traffic Monitor v5 -> http://127.0.0.1:${PORT}`));
