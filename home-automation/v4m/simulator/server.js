'use strict';

// V4m dashboard (plaintext, local-only): device control + rules + device
// registration (QR) + rule import, served over plaintext MQTT.
//   node server.js   ->   http://127.0.0.1:3005

const http = require('http');
const fs = require('fs');
const path = require('path');
const mqtt = require('mqtt');
const lib = require('./lib');

const PORT = Number(process.env.PORT) || 3005;
const PUBLIC_DIR = path.join(__dirname, 'public');
const MANAGER_HOST = process.env.MANAGER_HOST || '192.168.137.44';
const MANAGER_PORT = Number(process.env.MANAGER_PORT || 8081);

const devices = {};
const overrides = {};
let registry = {}; // guid -> { name, type (category), kind, value_type } from controller

const sseClients = new Set();
const client = mqtt.connect(lib.brokerUrl(), lib.clientOptions('ui'));

function broadcast(obj) {
  const data = `data: ${JSON.stringify(obj)}\n\n`;
  for (const res of sseClients) res.write(data);
}

function fetchJSON(path) {
  return new Promise((resolve) => {
    http.get({ host: MANAGER_HOST, port: MANAGER_PORT, path }, (res) => {
      let d = '';
      res.on('data', (c) => { d += c; });
      res.on('end', () => { try { resolve(JSON.parse(d)); } catch { resolve(null); } });
    }).on('error', () => resolve(null));
  });
}

// Load the device registry (guid -> name/category/kind/value_type) so HIOTO
// flat-topic messages ({guid, value}) can be attributed to a device + rendered.
async function refreshRegistry() {
  const devs = await fetchJSON('/api/devices');
  if (!Array.isArray(devs)) return;
  const r = {};
  for (const d of devs) r[d.guid] = { name: d.name, type: d.type, kind: d.kind, value_type: d.value_type };
  registry = r;
}

client.on('connect', () => {
  client.subscribe('home/+/+/+/state', { qos: 0 });
  client.subscribe('home/actuator/+/+/override', { qos: 0 });
  // HIOTO topics (flat + per-guid subtopics)
  for (const t of ['Sensor', 'Aktuator', 'Status', 'sensor_suhu/#', 'sensor_water_tank/#', 'sensor_gas_detector/#', 'sensor_weather/#', 'smart_bell/#', 'Log/#']) {
    client.subscribe(t, { qos: 0 });
  }
  refreshRegistry();
  console.log('V4m dashboard connected (plaintext MQTT)');
  broadcast({ type: 'status', connected: true });
});
client.on('close', () => broadcast({ type: 'status', connected: false }));
client.on('error', (e) => console.error('mqtt error:', e.message));

function handleHioto(topic, msg) {
  const guid = msg && msg.guid;
  if (!guid) return;
  const reg = registry[guid] || {};
  const kind = reg.kind || (topic === 'Aktuator' ? 'actuator' : 'sensor');
  let value;
  if (typeof msg.value === 'number') value = msg.value;
  else if (typeof msg.temperature === 'number') value = msg.temperature;
  else if (typeof msg.power === 'number') value = msg.power;
  const type = reg.value_type === 'analog' ? 2 : 1;
  devices[guid] = { kind, type, value, name: reg.name, category: reg.type };
  broadcast({ type: 'state', serial: guid, name: reg.name, category: reg.type, kind, type, value });
}

client.on('message', (topic, buf) => {
  if (!topic.startsWith('home/')) {
    let msg; try { msg = JSON.parse(buf.toString()); } catch { return; }
    handleHioto(topic, msg);
    return;
  }
  const parts = topic.split('/');
  if (topic.endsWith('/override')) {
    const serial = parts[3];
    let env; try { env = JSON.parse(buf.toString()); } catch { return; }
    const p = env.payload || {};
    if (typeof p.override === 'boolean') { overrides[serial] = p.override; broadcast({ type: 'override', serial, override: p.override }); }
    return;
  }
  const kind = parts[1];
  const type = Number(parts[2]) || 1;
  const serial = parts[3];
  let env; try { env = JSON.parse(buf.toString()); } catch { return; }
  const p = env.payload || {};
  let value;
  if (Array.isArray(p.digital_value)) value = p.digital_value[0];
  else if (Array.isArray(p.analog_value)) value = p.analog_value[0];
  devices[serial] = { kind, type, value };
  broadcast({ type: 'state', serial, kind, type, value });
});

function snapshot() {
  const sensors = [], actuators = [];
  for (const [serial, d] of Object.entries(devices)) {
    const item = { serial, name: d.name, category: d.category, type: d.type, value: d.value };
    if (d.kind === 'sensor') sensors.push(item);
    else actuators.push({ serial, name: d.name, category: d.category, type: d.type, value: d.value, override: !!overrides[serial] });
  }
  return { connected: client.connected, sensors, actuators };
}

function overrideActuator(serial, value) {
  const d = devices[serial];
  const type = d ? d.type : 1;
  client.publish(lib.actuatorOverrideTopic(serial, type), lib.envelope('ui', 'event', { serial_number: serial, override: true }), { qos: 0, retain: false });
  const payload = type === 2 ? { analog_value: [value] } : { digital_value: [value] };
  client.publish(lib.actuatorCmdTopic(serial, type), lib.envelope('ui', 'cmd', payload), { qos: 0 });
}
function releaseActuator(serial) {
  const d = devices[serial];
  const type = d ? d.type : 1;
  client.publish(lib.actuatorOverrideTopic(serial, type), lib.envelope('ui', 'event', { serial_number: serial, override: false }), { qos: 0, retain: false });
}

// Manual sensor override: publish a sensor state as if the sensor reported it,
// and flag it so the simulator stops auto-updating that sensor (persists).
function setSensor(serial, value) {
  const d = devices[serial];
  const type = d ? d.type : 1;
  const payload = type === 2
    ? { serial_number: serial, device_kind: 'sensor', device_type: type, analog_value: [value] }
    : { serial_number: serial, device_kind: 'sensor', device_type: type, digital_value: [value] };
  client.publish(lib.sensorStateTopic(serial, type), lib.envelope('ui', 'state', payload), { qos: 0 });
  client.publish(`home/sensor/manual/${serial}`, JSON.stringify({ override: true, value }), { qos: 0, retain: true });
}
function releaseSensor(serial) {
  client.publish(`home/sensor/manual/${serial}`, JSON.stringify({ override: false }), { qos: 0, retain: true });
}

// --- proxy to the V4m controller's device-management API (port 8081) ---
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
  req.on('error', () => once({ status: 502, body: { error: 'controller unreachable' } }));
  req.setTimeout(15000, () => { req.destroy(); once({ status: 504, body: { error: 'timeout' } }); });
  if (data) req.write(data);
  req.end();
}

function readBody(req) {
  return new Promise((resolve) => {
    let d = '';
    req.on('data', (c) => { d += c; if (d.length > 1e6) req.destroy(); });
    req.on('end', () => { try { resolve(JSON.parse(d || '{}')); } catch { resolve({}); } });
  });
}
function sendJson(res, code, obj) { res.writeHead(code, { 'Content-Type': 'application/json' }); res.end(JSON.stringify(obj)); }
const MIME = { '.html': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8', '.css': 'text/css; charset=utf-8' };
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
  const url = (req.url || '').split('?')[0];
  if (url === '/events') {
    res.writeHead(200, { 'Content-Type': 'text/event-stream', 'Cache-Control': 'no-cache', 'Connection': 'keep-alive' });
    res.write(`data: ${JSON.stringify({ type: 'hello', ...snapshot() })}\n\n`);
    sseClients.add(res);
    req.on('close', () => sseClients.delete(res));
    return;
  }
  if (url === '/api/state' && req.method === 'GET') return sendJson(res, 200, snapshot());
  if (url === '/api/override' && req.method === 'POST') return readBody(req).then((b) => { overrideActuator(String(b.serial || ''), Number(b.value)); sendJson(res, 200, { ok: true }); });
  if (url === '/api/release' && req.method === 'POST') return readBody(req).then((b) => { releaseActuator(String(b.serial || '')); sendJson(res, 200, { ok: true }); });
  if (url === '/api/sensor' && req.method === 'POST') return readBody(req).then((b) => { setSensor(String(b.serial || ''), Number(b.value)); sendJson(res, 200, { ok: true }); });
  if (url === '/api/sensor-release' && req.method === 'POST') return readBody(req).then((b) => { releaseSensor(String(b.serial || '')); sendJson(res, 200, { ok: true }); });
  if (url === '/api/devices' && req.method === 'GET') return proxyManager('GET', '/api/devices', null, (r) => sendJson(res, r.status, r.body));
  if (url === '/api/register-device' && req.method === 'POST') return readBody(req).then((b) => proxyManager('POST', '/api/register', b, (r) => sendJson(res, r.status, r.body)));
  if (url === '/api/rules' && req.method === 'GET') return proxyManager('GET', '/api/rules', null, (r) => sendJson(res, r.status, r.body));
  if (url === '/api/import-rules' && req.method === 'POST') return readBody(req).then((b) => proxyManager('POST', '/api/import-rules', b, (r) => sendJson(res, r.status, r.body)));
  if (url.startsWith('/api/revoke-device/') && req.method === 'DELETE') {
    const guid = url.replace('/api/revoke-device/', '');
    return proxyManager('DELETE', `/api/devices/${encodeURIComponent(guid)}`, null, (r) => sendJson(res, r.status, r.body));
  }
  if (req.method === 'GET') return serveStatic(req, res);
  res.writeHead(404); res.end('Not found');
});

server.listen(PORT, '127.0.0.1', () => console.log(`Home Automation V4m dashboard -> http://127.0.0.1:${PORT}`));
setInterval(refreshRegistry, 30000);
