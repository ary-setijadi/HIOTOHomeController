'use strict';

// v5.0 combined dashboard: device control + rules + security & traffic monitor.
// Connects to the broker over MQTTS with the "monitor" certificate, observes
// every message on home/#, and serves one tabbed UI.
//   node server.js   ->   http://127.0.0.1:3005

const http = require('http');
const https = require('https');
const fs = require('fs');
const path = require('path');
const { spawn, execFile } = require('child_process');
const mqtt = require('mqtt');
const lib = require('./lib');

const PORT = Number(process.env.PORT) || 3005;
const PUBLIC_DIR = path.join(__dirname, 'public');
const BROKER_API_HOST = process.env.BROKER_API_HOST || '192.168.137.44';
const BROKER_API_PORT = Number(process.env.BROKER_API_PORT || 15671);
const BROKER_API_USER = process.env.BROKER_API_USER || 'admin';
const BROKER_API_PASS = process.env.BROKER_API_PASS || '123456Aa!';
const LOGGER_HOST = process.env.LOGGER_HOST || '192.168.137.44';
const LOGGER_PORT = Number(process.env.LOGGER_PORT || 8080);
const MAX_MEM_BYTES = 10 * 1024 * 1024; // keep at most ~10 MB of recent traffic in RAM
const MANAGER_HOST = process.env.MANAGER_HOST || '192.168.137.44';
const MANAGER_PORT = Number(process.env.MANAGER_PORT || 8081);

const devices = {};
const overrides = {};

const DEFAULT_RULES = (() => {
  const r = [];
  const sw = ['SNS-SW-001', 'SNS-SW-002', 'SNS-SW-003', 'SNS-SW-004', 'SNS-SW-005', 'SNS-SW-006'];
  const lp = ['ACT-LMP-001', 'ACT-LMP-002', 'ACT-LMP-003', 'ACT-LMP-004', 'ACT-LMP-005', 'ACT-LMP-006'];
  sw.forEach((s, i) => r.push({ name: 'light-' + s, when: { sensor: s, op: '==', threshold: 1 }, then: [{ actuator: lp[i], value: 1 }], else: [{ actuator: lp[i], value: 0 }] }));
  r.push({ name: 'water-pump', when: { sensor: 'SNS-FLW-001', op: '<', threshold: 2.0, hysteresis: 1.0, min_duration_ms: 5000 }, then: [{ actuator: 'ACT-PMP-001', value: 1 }], else: [{ actuator: 'ACT-PMP-001', value: 0 }] });
  [['SNS-TMP-001', 'ACT-AC-001'], ['SNS-TMP-002', 'ACT-AC-002'], ['SNS-TMP-003', 'ACT-AC-003'], ['SNS-TMP-004', 'ACT-AC-004']].forEach(([t, a]) => r.push({ name: 'ac-' + t, when: { sensor: t, op: '>', threshold: 26.0, hysteresis: 1.5 }, then: [{ actuator: a, value: 0.8 }], else: [{ actuator: a, value: 0.1 }] }));
  r.push({ name: 'air-purifier', when: { sensor: 'SNS-AIR-001', op: '>', threshold: 100, hysteresis: 20 }, then: [{ actuator: 'ACT-APR-001', value: 1.0 }], else: [{ actuator: 'ACT-APR-001', value: 0.3 }] });
  r.push({ name: 'master-off', priority: 100, when: { sensor: 'SNS-SW-007', op: '==', threshold: 1 }, then: lp.map((a) => ({ actuator: a, value: 0 })).concat([{ actuator: 'ACT-PMP-001', value: 0 }]) });
  // v5.0 keeps the v4.0 time trigger — "at 18:00 turn all lamps ON once"
  r.push({ name: 'evening-on', mode: 'trigger', when: { type: 'time', at: '18:00' }, then: lp.map((a) => ({ actuator: a, value: 1 })) });
  return r;
})();
const rules = {};
for (const r of DEFAULT_RULES) rules[r.name] = r;

// --- traffic feed state ---
const messages = [];
const counts = { total: 0, flows: {} };
let memBytes = 0;

function classify(topic) {
  const p = topic.split('/'); // home/<kind>/<type>/<serial>/<class>
  if (topic === 'home/config/rules') return { flow: 'UI → CONTROLLER (rule)', tag: 'ui-ctrl' };
  if (p.length === 5 && p[1] === 'sensor' && p[4] === 'state') return { flow: 'SENSOR → CONTROLLER', tag: 'dev-ctrl' };
  if (p.length === 5 && p[1] === 'actuator' && p[4] === 'state') return { flow: 'ACTUATOR → UI', tag: 'dev-ui' };
  if (p.length === 5 && p[1] === 'actuator' && p[4] === 'cmd') return { flow: 'CONTROLLER → DEVICE', tag: 'ctrl-dev' };
  if (p.length === 5 && p[1] === 'actuator' && p[4] === 'override') return { flow: 'UI → CONTROLLER (override)', tag: 'ui-ctrl' };
  return { flow: topic, tag: 'other' };
}

const sseClients = new Set();
// UI connects over MQTTS with the "monitor" client certificate.
const client = mqtt.connect(lib.brokerUrl(), lib.clientOptions('monitor', 'ui'));

function broadcast(obj) {
  const data = `data: ${JSON.stringify(obj)}\n\n`;
  for (const res of sseClients) res.write(data);
}

client.on('connect', () => {
  client.subscribe('home/#', { qos: 1 });
  console.log('UI v5 connected (MQTTS, monitor cert)');
  broadcast({ type: 'status', connected: true });
});
client.on('close', () => broadcast({ type: 'status', connected: false }));
client.on('error', (e) => console.error('UI mqtt error:', e.message));

client.on('message', (topic, buf) => {
  let env; try { env = JSON.parse(buf.toString()); } catch { env = null; }

  // 1) traffic feed — every message, classified by direction
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
  rec._sz = JSON.stringify(rec).length;
  memBytes += rec._sz;
  while (memBytes > MAX_MEM_BYTES && messages.length) memBytes -= messages.pop()._sz;
  counts.total++;
  counts.flows[cls.flow] = (counts.flows[cls.flow] || 0) + 1;
  broadcast({ type: 'message', rec, counts });

  // 2) device/rules state — only state/override/rules topics
  const parts = topic.split('/');
  if (topic === 'home/config/rules') {
    const p = env ? (env.payload || {}) : {};
    if (p.action === 'add' && p.rule && p.rule.name) { rules[p.rule.name] = p.rule; broadcast({ type: 'rule', rule: p.rule }); }
    else if (p.action === 'remove' && p.name) { delete rules[p.name]; broadcast({ type: 'rule-removed', name: p.name }); }
    return;
  }
  if (topic.endsWith('/override')) {
    const serial = parts[3];
    const p = env ? (env.payload || {}) : {};
    if (typeof p.override === 'boolean') { overrides[serial] = p.override; broadcast({ type: 'override', serial, override: p.override }); }
    return;
  }
  if (parts.length === 5 && parts[4] === 'state') {
    const kind = parts[1];
    const type = Number(parts[2]) || 1;
    const serial = parts[3];
    const p = env ? (env.payload || {}) : {};
    let value;
    if (Array.isArray(p.digital_value)) value = p.digital_value[0];
    else if (Array.isArray(p.analog_value)) value = p.analog_value[0];
    devices[serial] = { kind, type, value };
    broadcast({ type: 'state', serial, kind, type, value });
  }
  // cmd and any other topics only appear in the traffic feed
});

function snapshot() {
  const sensors = [], actuators = [];
  for (const [serial, d] of Object.entries(devices)) {
    if (d.kind === 'sensor') sensors.push({ serial, type: d.type, value: d.value });
    else actuators.push({ serial, type: d.type, value: d.value, override: !!overrides[serial] });
  }
  return { connected: client.connected, sensors, actuators, rules: Object.values(rules) };
}

function overrideActuator(serial, value) {
  const d = devices[serial];
  const type = d ? d.type : 1;
  client.publish(lib.actuatorOverrideTopic(serial, type), lib.envelope('ui', 'event', { serial_number: serial, override: true }), { qos: 1, retain: true });
  const payload = type === 2 ? { analog_value: [value] } : { digital_value: [value] };
  client.publish(lib.actuatorCmdTopic(serial, type), lib.envelope('ui', 'cmd', payload), { qos: 1 });
}
function releaseActuator(serial) {
  const d = devices[serial];
  const type = d ? d.type : 1;
  client.publish(lib.actuatorOverrideTopic(serial, type), lib.envelope('ui', 'event', { serial_number: serial, override: false }), { qos: 1, retain: true });
}
function addRule(rule) { client.publish(lib.rulesTopic(), lib.envelope('ui', 'event', { action: 'add', rule }), { qos: 1 }); }
function removeRule(name) { client.publish(lib.rulesTopic(), lib.envelope('ui', 'event', { action: 'remove', name }), { qos: 1 }); }

// --- security: RabbitMQ management API (TLS/mTLS connection table) ---
function fetchSecurity(cb) {
  let done = false;
  const once = (x) => { if (!done) { done = true; cb(x); } };
  const req = https.request({
    host: BROKER_API_HOST, port: BROKER_API_PORT, path: '/api/connections',
    method: 'GET', rejectUnauthorized: false, auth: `${BROKER_API_USER}:${BROKER_API_PASS}`,
  }, (res) => {
    let d = '';
    res.on('data', (c) => { d += c; });
    res.on('end', () => { try { once(JSON.parse(d)); } catch { once([]); } });
  });
  req.on('error', () => once([]));
  req.setTimeout(8000, () => { req.destroy(); once([]); });
  req.end();
}
function securitySummary(conns) {
  const total = conns.length;
  const tls = conns.filter((c) => c.ssl).length;
  const certs = conns.map((c) => ({ protocol: c.protocol, ssl: !!c.ssl, cn: String(c.peer_cert_subject || '').replace(/^CN=/, '') || '(none)', name: c.name }));
  return { total, tls, allTls: total > 0 && tls === total, certs };
}

// --- history: page through the Pi's 100 MB rolling traffic file ---
function convertRecord(r) {
  const topic = String(r.rk || '').replace(/\./g, '/');
  const cls = classify(topic);
  const env = r.env || {};
  return { t: r.t, flow: cls.flow, tag: cls.tag, topic, source: env.source || '', class: env.message_class || '', payload: env.payload || null };
}
function fetchHistory(bytes, cb) {
  let done = false;
  const once = (x) => { if (!done) { done = true; cb(x); } };
  const n = Math.max(1, Math.min(bytes || 1048576, 10 * 1024 * 1024));
  const req = http.request({
    host: LOGGER_HOST, port: LOGGER_PORT, path: `/tail?bytes=${n}`, method: 'GET',
  }, (res) => {
    let d = '';
    res.on('data', (c) => { d += c; });
    res.on('end', () => {
      const recs = d.split('\n').filter((l) => l.trim()).map((l) => { try { return convertRecord(JSON.parse(l)); } catch { return null; } }).filter(Boolean);
      once(recs);
    });
  });
  req.on('error', () => once([]));
  req.setTimeout(10000, () => { req.destroy(); once([]); });
  req.end();
}

// --- impostor test ---
function runImpostor(cb) {
  execFile(process.execPath, [path.join(__dirname, '..', 'monitor', 'impostor.js')], { timeout: 45000 }, (err, stdout, stderr) => {
    if (err) { cb({ ok: false, error: String(stderr || err.message) }); return; }
    try { cb({ ok: true, results: JSON.parse(stdout) }); } catch { cb({ ok: false, error: stdout }); }
  });
}

// --- device management: proxy the Pi's device-manager ---
function proxyManager(method, path, body, cb) {
  let done = false;
  const once = (x) => { if (!done) { done = true; cb(x); } };
  const data = body ? JSON.stringify(body) : null;
  const req = http.request({
    host: MANAGER_HOST, port: MANAGER_PORT, path, method,
    headers: data ? { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(data) } : {},
  }, (res) => {
    let d = '';
    res.on('data', (c) => { d += c; });
    res.on('end', () => { try { once({ status: res.statusCode, body: JSON.parse(d) }); } catch { once({ status: res.statusCode, body: d }); } });
  });
  req.on('error', () => once({ status: 502, body: { error: 'device-manager unreachable' } }));
  req.setTimeout(30000, () => { req.destroy(); once({ status: 504, body: { error: 'timeout' } }); });
  if (data) req.write(data);
  req.end();
}

function readBody(req) {
  return new Promise((resolve) => {
    let data = '';
    req.on('data', (c) => { data += c; if (data.length > 1e6) req.destroy(); });
    req.on('end', () => { try { resolve(JSON.parse(data || '{}')); } catch (_) { resolve({}); } });
  });
}
function sendJson(res, code, obj) { res.writeHead(code, { 'Content-Type': 'application/json' }); res.end(JSON.stringify(obj)); }
const MIME = { '.html': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8', '.css': 'text/css; charset=utf-8' };
function serveStatic(req, res) {
  const rel = (req.url || '/') === '/' ? 'index.html' : decodeURIComponent((req.url || '').split('?')[0]).replace(/^\/+/, '');
  const fp = path.join(PUBLIC_DIR, rel);
  if (!fp.startsWith(PUBLIC_DIR)) { res.writeHead(403); return res.end('Forbidden'); }
  fs.readFile(fp, (err, data) => {
    if (err) { res.writeHead(404, { 'Content-Type': 'text/plain' }); return res.end('Not found'); }
    res.writeHead(200, { 'Content-Type': MIME[path.extname(fp)] || 'application/octet-stream', 'Cache-Control': 'no-cache' });
    res.end(data);
  });
}

const server = http.createServer((req, res) => {
  const url = (req.url || '').split('?')[0];
  if (url === '/events') {
    res.writeHead(200, { 'Content-Type': 'text/event-stream', 'Cache-Control': 'no-cache', 'Connection': 'keep-alive' });
    res.write(`data: ${JSON.stringify({ type: 'hello', ...snapshot(), messages, counts })}\n\n`);
    sseClients.add(res);
    req.on('close', () => sseClients.delete(res));
    return;
  }
  if (url === '/api/state' && req.method === 'GET') return sendJson(res, 200, snapshot());
  if (url === '/api/messages' && req.method === 'GET') return sendJson(res, 200, { messages, counts });
  if (url === '/api/history' && req.method === 'GET') {
    const q = (req.url.split('?')[1] || '');
    const m = q.match(/bytes=(\d+)/);
    const bytes = m ? Number(m[1]) : 1048576;
    return fetchHistory(bytes, (recs) => sendJson(res, 200, { messages: recs, count: recs.length }));
  }
  if (url === '/api/devices' && req.method === 'GET') return proxyManager('GET', '/api/devices', null, (r) => sendJson(res, r.status, r.body));
  if (url === '/api/register-device' && req.method === 'POST') return readBody(req).then((b) => proxyManager('POST', '/api/register', b, (r) => sendJson(res, r.status, r.body)));
  if (url.startsWith('/api/revoke-device/') && req.method === 'DELETE') {
    const serial = url.replace('/api/revoke-device/', '');
    return proxyManager('DELETE', `/api/devices/${encodeURIComponent(serial)}`, null, (r) => sendJson(res, r.status, r.body));
  }
  if (url === '/api/security' && req.method === 'GET') return fetchSecurity((conns) => sendJson(res, 200, securitySummary(conns)));
  if (url === '/api/impostor' && req.method === 'POST') return runImpostor((r) => sendJson(res, 200, r));
  if (url === '/api/override' && req.method === 'POST') return readBody(req).then((b) => { overrideActuator(String(b.serial || ''), Number(b.value)); sendJson(res, 200, { ok: true }); });
  if (url === '/api/release' && req.method === 'POST') return readBody(req).then((b) => { releaseActuator(String(b.serial || '')); sendJson(res, 200, { ok: true }); });
  if (url === '/api/device' && req.method === 'POST') {
    return readBody(req).then((b) => {
      const serial = String(b.serial || '').trim();
      const kind = b.kind === 'actuator' ? 'actuator' : 'sensor';
      const type = Number(b.type) === 2 ? 2 : 1;
      if (!serial) return sendJson(res, 400, { ok: false, error: 'serial required' });
      const child = spawn(process.execPath, [path.join(__dirname, 'create-device.js'), serial, kind, String(type)], { detached: true, stdio: 'ignore' });
      child.unref();
      sendJson(res, 200, { ok: true, serial });
    });
  }
  if (url === '/api/rule' && req.method === 'POST') {
    return readBody(req).then((b) => {
      if (b.action === 'remove') { removeRule(String(b.name || '')); return sendJson(res, 200, { ok: true }); }
      addRule(b.rule || {});
      sendJson(res, 200, { ok: true });
    });
  }
  if (req.method === 'GET') return serveStatic(req, res);
  res.writeHead(404); res.end('Not found');
});

server.listen(PORT, '127.0.0.1', () => console.log(`Home Automation v5 combined dashboard -> http://127.0.0.1:${PORT}`));
