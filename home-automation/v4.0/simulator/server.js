'use strict';

// v4.0 browser UI: whole-house (digital + analog) + time/trigger rules.
//   node server.js   ->   http://127.0.0.1:3004

const http = require('http');
const fs = require('fs');
const path = require('path');
const { spawn } = require('child_process');
const mqtt = require('mqtt');
const lib = require('./lib');

const PORT = Number(process.env.PORT) || 3004;
const PUBLIC_DIR = path.join(__dirname, 'public');

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
  // v4.0: time trigger — "at 18:00 turn all lamps ON once"
  r.push({ name: 'evening-on', mode: 'trigger', when: { type: 'time', at: '18:00' }, then: lp.map((a) => ({ actuator: a, value: 1 })) });
  return r;
})();
const rules = {};
for (const r of DEFAULT_RULES) rules[r.name] = r;

const sseClients = new Set();
const client = mqtt.connect(lib.brokerUrl(), { username: lib.USER, password: lib.PASS, clientId: 'home-ui-v4-' + lib.uuid().slice(0, 8), reconnectPeriod: 3000 });

function broadcast(obj) {
  const data = `data: ${JSON.stringify(obj)}\n\n`;
  for (const res of sseClients) res.write(data);
}

client.on('connect', () => {
  client.subscribe('home/+/+/+/state', { qos: 1 });
  client.subscribe('home/actuator/+/+/override', { qos: 1 });
  client.subscribe('home/config/rules', { qos: 1 });
  console.log('UI v4 connected');
  broadcast({ type: 'status', connected: true });
});
client.on('close', () => broadcast({ type: 'status', connected: false }));

client.on('message', (topic, buf) => {
  const parts = topic.split('/');
  if (topic === 'home/config/rules') {
    let env; try { env = JSON.parse(buf.toString()); } catch { return; }
    const p = env.payload || {};
    if (p.action === 'add' && p.rule && p.rule.name) { rules[p.rule.name] = p.rule; broadcast({ type: 'rule', rule: p.rule }); }
    else if (p.action === 'remove' && p.name) { delete rules[p.name]; broadcast({ type: 'rule-removed', name: p.name }); }
    return;
  }
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

server.listen(PORT, '127.0.0.1', () => console.log(`Home Automation UI v4 -> http://127.0.0.1:${PORT}`));
