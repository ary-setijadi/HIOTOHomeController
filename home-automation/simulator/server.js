'use strict';

// v2.0 browser UI: live switch/lamp state + supervisory override + dynamic
// switch creation + rule registration (compounding rules).
//
//   node server.js   ->   http://127.0.0.1:3002

const http = require('http');
const fs = require('fs');
const path = require('path');
const { spawn } = require('child_process');
const mqtt = require('mqtt');
const lib = require('./lib');

const PORT = Number(process.env.PORT) || 3002;
const PUBLIC_DIR = path.join(__dirname, 'public');

const values = {};     // serial -> 0|1
const kinds = {};      // serial -> 'sensor' | 'actuator'
const overrides = {};  // actuator serial -> bool
const switchSerials = new Set(lib.SENSORS);
const actuatorSerials = new Set(lib.ACTUATORS);

// Rules (must match controller-v2 defaultRules).
const DEFAULT_RULES = [
  { switch: 'SNS-SW-0001', mappings: [{ actuator: 'ACT-LMP-0001', on: 1, off: 0 }, { actuator: 'ACT-LMP-0003', on: 1, off: 0 }] },
  { switch: 'SNS-SW-0002', mappings: [{ actuator: 'ACT-LMP-0002', on: 1, off: 0 }] },
  { switch: 'SNS-SW-0003', mappings: [{ actuator: 'ACT-LMP-0005', on: 1, off: 0 }] },
  { switch: 'SNS-SW-0004', mappings: [{ actuator: 'ACT-LMP-0004', on: 1, off: 0 }] },
];
const rules = {}; // switch -> {switch, mappings}
for (const r of DEFAULT_RULES) rules[r.switch] = r;

const sseClients = new Set();

const client = mqtt.connect(lib.brokerUrl(), {
  username: lib.USER,
  password: lib.PASS,
  clientId: 'home-ui-v2-' + lib.uuid().slice(0, 8),
  reconnectPeriod: 3000,
});

function broadcast(obj) {
  const data = `data: ${JSON.stringify(obj)}\n\n`;
  for (const res of sseClients) res.write(data);
}

client.on('connect', () => {
  client.subscribe('home/+/+/+/state', { qos: 1 });
  client.subscribe('home/actuator/+/+/override', { qos: 1 });
  client.subscribe('home/config/rules', { qos: 1 });
  console.log('UI v2 connected');
  broadcast({ type: 'status', connected: true });
});
client.on('close', () => broadcast({ type: 'status', connected: false }));

client.on('message', (topic, buf) => {
  const parts = topic.split('/');

  if (topic === 'home/config/rules') {
    let env; try { env = JSON.parse(buf.toString()); } catch { return; }
    const p = env.payload || {};
    if (p.action === 'add' && p.switch) {
      rules[p.switch] = { switch: p.switch, mappings: p.mappings || [] };
      broadcast({ type: 'rule', rule: rules[p.switch] });
    } else if (p.action === 'remove' && p.switch) {
      delete rules[p.switch];
      broadcast({ type: 'rule-removed', switch: p.switch });
    }
    return;
  }

  if (topic.endsWith('/override')) {
    const serial = parts[3];
    let env; try { env = JSON.parse(buf.toString()); } catch { return; }
    const p = env.payload || {};
    if (typeof p.override === 'boolean') {
      overrides[serial] = p.override;
      broadcast({ type: 'override', serial, override: p.override });
    }
    return;
  }

  const kind = parts[1];
  const serial = parts[3];
  let env; try { env = JSON.parse(buf.toString()); } catch { return; }
  const p = env.payload || {};
  const val = Array.isArray(p.digital_value) ? p.digital_value[0] : undefined;
  values[serial] = val;
  kinds[serial] = kind;
  if (kind === 'sensor') switchSerials.add(serial);
  if (kind === 'actuator') actuatorSerials.add(serial);
  broadcast({ type: 'state', serial, kind, value: val });
});

function snapshot() {
  return {
    connected: client.connected,
    switches: [...switchSerials].map((s) => ({ serial: s, value: values[s] })),
    lamps: [...actuatorSerials].map((a) => ({ serial: a, value: values[a], override: !!overrides[a] })),
    rules: Object.values(rules),
  };
}

function overrideLamp(serial, val) {
  client.publish(lib.actuatorOverrideTopic(serial), lib.envelope('ui', 'event', { serial_number: serial, override: true }), { qos: 1, retain: true });
  client.publish(lib.actuatorCmdTopic(serial), lib.envelope('ui', 'cmd', { digital_value: [val] }), { qos: 1 });
}
function releaseLamp(serial) {
  client.publish(lib.actuatorOverrideTopic(serial), lib.envelope('ui', 'event', { serial_number: serial, override: false }), { qos: 1, retain: true });
}
function addRule(sw, mappings) {
  client.publish(lib.rulesTopic(), lib.envelope('ui', 'event', { action: 'add', switch: sw, mappings }), { qos: 1 });
}
function removeRule(sw) {
  client.publish(lib.rulesTopic(), lib.envelope('ui', 'event', { action: 'remove', switch: sw }), { qos: 1 });
}

// ---------------------------------------------------------------------------
// HTTP
// ---------------------------------------------------------------------------
function readBody(req) {
  return new Promise((resolve) => {
    let data = '';
    req.on('data', (c) => { data += c; if (data.length > 1e6) req.destroy(); });
    req.on('end', () => { try { resolve(JSON.parse(data || '{}')); } catch (_) { resolve({}); } });
  });
}
function sendJson(res, code, obj) {
  res.writeHead(code, { 'Content-Type': 'application/json' });
  res.end(JSON.stringify(obj));
}
const MIME = { '.html': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8', '.css': 'text/css; charset=utf-8' };
function serveStatic(req, res) {
  const rel = (req.url || '/') === '/' ? 'index.html' : decodeURIComponent((req.url || '').split('?')[0]).replace(/^\/+/, '');
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
    res.write(`data: ${JSON.stringify({ type: 'hello', ...snapshot() })}\n\n`);
    sseClients.add(res);
    req.on('close', () => sseClients.delete(res));
    return;
  }
  if (url === '/api/state' && req.method === 'GET') return sendJson(res, 200, snapshot());
  if (url === '/api/override' && req.method === 'POST') {
    return readBody(req).then((b) => {
      const serial = String(b.serial || '');
      if (!actuatorSerials.has(serial)) return sendJson(res, 400, { ok: false, error: 'unknown actuator' });
      overrideLamp(serial, b.value ? 1 : 0);
      sendJson(res, 200, { ok: true });
    });
  }
  if (url === '/api/release' && req.method === 'POST') {
    return readBody(req).then((b) => {
      releaseLamp(String(b.serial || ''));
      sendJson(res, 200, { ok: true });
    });
  }
  if (url === '/api/switch' && req.method === 'POST') {
    return readBody(req).then((b) => {
      const serial = String(b.serial || '').trim();
      if (!serial) return sendJson(res, 400, { ok: false, error: 'serial required' });
      const child = spawn(process.execPath, [path.join(__dirname, 'create-switch.js'), serial], { detached: true, stdio: 'ignore' });
      child.unref();
      switchSerials.add(serial);
      broadcast({ type: 'switch-created', serial });
      sendJson(res, 200, { ok: true, serial });
    });
  }
  if (url === '/api/rule' && req.method === 'POST') {
    return readBody(req).then((b) => {
      const sw = String(b.switch || '').trim();
      if (!sw) return sendJson(res, 400, { ok: false, error: 'switch required' });
      if (b.action === 'remove') { removeRule(sw); return sendJson(res, 200, { ok: true }); }
      addRule(sw, b.mappings || []);
      sendJson(res, 200, { ok: true });
    });
  }
  if (req.method === 'GET') return serveStatic(req, res);
  res.writeHead(404); res.end('Not found');
});

server.listen(PORT, '127.0.0.1', () => console.log(`Home Automation UI v2 -> http://127.0.0.1:${PORT}`));
