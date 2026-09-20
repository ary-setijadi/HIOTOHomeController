#!/usr/bin/env node
'use strict';

/*
 * MQTT Simulator UI — a local web dashboard for the MQTT simulator.
 *
 * Bridges a browser to the MQTT broker: the server holds one MQTT connection,
 * pushes received messages to the browser over Server-Sent Events (SSE), and
 * accepts publish / subscribe / simulator commands over a small REST API.
 *
 * Run:  node server.js   ->  http://127.0.0.1:3000
 */

const http = require('http');
const fs = require('fs');
const path = require('path');
const mqtt = require('mqtt');

const PORT = Number(process.env.PORT) || 3000;
const HOST = '127.0.0.1';
const PUBLIC_DIR = path.join(__dirname, 'public');

const state = {
  broker: process.env.BROKER || 'mqtt://192.168.137.44:1883',
  username: process.env.MQTT_USER || 'admin',
  password: process.env.MQTT_PASS || '123456Aa!',
  connected: false,
};

let mqttClient = null;
const sseClients = new Set();
const subscriptions = new Map(); // topic -> qos
let sim = null;

function broadcast(obj) {
  const data = `data: ${JSON.stringify(obj)}\n\n`;
  for (const res of sseClients) res.write(data);
}

function connectMqtt() {
  if (mqttClient) { try { mqttClient.end(true); } catch (_) { /* noop */ } }

  const opts = {
    clientId: `mqtt-ui-${Math.random().toString(16).slice(2, 8)}`,
    clean: true,
    reconnectPeriod: 3000,
  };
  if (state.username) opts.username = state.username;
  if (state.password) opts.password = state.password;

  mqttClient = mqtt.connect(state.broker, opts);

  mqttClient.on('connect', () => {
    state.connected = true;
    for (const [topic, qos] of subscriptions) mqttClient.subscribe(topic, { qos });
    broadcast({ type: 'status', connected: true, broker: state.broker });
  });

  mqttClient.on('close', () => {
    if (state.connected) {
      state.connected = false;
      broadcast({ type: 'status', connected: false, broker: state.broker });
    }
  });

  mqttClient.on('error', (err) => {
    broadcast({ type: 'error', message: err.message });
  });

  mqttClient.on('message', (topic, payload) => {
    broadcast({ type: 'message', topic, payload: payload.toString(), ts: Date.now() });
  });
}

// ---------------------------------------------------------------------------
// Simulator (runs virtual devices on the shared MQTT connection)
// ---------------------------------------------------------------------------
function stopSimulator() {
  if (sim && sim.timer) clearInterval(sim.timer);
  sim = null;
  broadcast({ type: 'sim', running: false, ids: [] });
}

function startSimulator(cfg) {
  stopSimulator();

  const count = Math.max(1, Number(cfg.count) || 1);
  const device = String(cfg.device || 'sim');
  const interval = Math.max(0.5, Number(cfg.interval) || 2);
  const telemetryTpl = String(cfg.telemetry || 'iot/{id}/telemetry');
  const commandTpl = String(cfg.command || 'iot/{id}/commands');

  const ids = count === 1
    ? [device]
    : Array.from({ length: count }, (_, i) => `${device}-${String(i + 1).padStart(3, '0')}`);

  // Subscribe the shared connection to the simulator's telemetry + command
  // patterns so those messages appear in the dashboard log.
  const telemetryPat = telemetryTpl.replace(/\{id\}/g, '+');
  const commandPat = commandTpl.replace(/\{id\}/g, '+');
  for (const [pat, qos] of [[telemetryPat, 1], [commandPat, 1]]) {
    mqttClient.subscribe(pat, { qos });
    subscriptions.set(pat, qos);
  }

  const sensors = {};
  for (const id of ids) {
    sensors[id] = { temp: 20 + Math.random() * 8, hum: 45 + Math.random() * 20, battery: 100 };
  }

  sim = { timer: null, ids, telemetryTpl, commandTpl, interval };

  const tick = () => {
    for (const id of ids) {
      const s = sensors[id];
      s.temp += (Math.random() - 0.5) * 0.6;
      s.hum += (Math.random() - 0.5) * 1.2;
      s.battery = Math.max(0, s.battery - Math.random() * 0.1);
      const reading = {
        device: id,
        ts: new Date().toISOString(),
        temperature: +s.temp.toFixed(2),
        humidity: +s.hum.toFixed(1),
        battery: +s.battery.toFixed(2),
      };
      mqttClient.publish(telemetryTpl.replace(/\{id\}/g, id), JSON.stringify(reading), { qos: 1 });
    }
  };

  tick();
  sim.timer = setInterval(tick, interval * 1000);
  broadcast({ type: 'sim', running: true, ids, telemetryTpl, commandTpl, interval });
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
function readBody(req) {
  return new Promise((resolve) => {
    let data = '';
    req.on('data', (c) => {
      data += c;
      if (data.length > 1e6) req.destroy();
    });
    req.on('end', () => {
      try { resolve(JSON.parse(data || '{}')); } catch (_) { resolve({}); }
    });
  });
}

function sendJson(res, code, obj) {
  res.writeHead(code, { 'Content-Type': 'application/json' });
  res.end(JSON.stringify(obj));
}

const MIME = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.json': 'application/json',
  '.svg': 'image/svg+xml',
};

function serveStatic(req, res) {
  const urlPath = decodeURIComponent((req.url || '/').split('?')[0]);
  const rel = urlPath === '/' ? 'index.html' : urlPath.replace(/^\/+/, '');
  const filePath = path.join(PUBLIC_DIR, rel);
  if (!filePath.startsWith(PUBLIC_DIR)) { res.writeHead(403); return res.end('Forbidden'); }
  fs.readFile(filePath, (err, data) => {
    if (err) { res.writeHead(404, { 'Content-Type': 'text/plain' }); return res.end('Not found'); }
    res.writeHead(200, { 'Content-Type': MIME[path.extname(filePath)] || 'application/octet-stream' });
    res.end(data);
  });
}

// ---------------------------------------------------------------------------
// HTTP server
// ---------------------------------------------------------------------------
const server = http.createServer((req, res) => {
  const url = (req.url || '').split('?')[0];

  if (url === '/events') {
    res.writeHead(200, {
      'Content-Type': 'text/event-stream',
      'Cache-Control': 'no-cache',
      'Connection': 'keep-alive',
      'X-Accel-Buffering': 'no',
    });
    res.write(`data: ${JSON.stringify({
      type: 'hello',
      connected: state.connected,
      broker: state.broker,
      subscriptions: [...subscriptions.keys()],
      simRunning: !!sim,
    })}\n\n`);
    sseClients.add(res);
    req.on('close', () => sseClients.delete(res));
    return;
  }

  if (url === '/api/state' && req.method === 'GET') {
    return sendJson(res, 200, {
      connected: state.connected,
      broker: state.broker,
      username: state.username,
      subscriptions: [...subscriptions.keys()],
      simRunning: !!sim,
    });
  }

  if (url === '/api/connect' && req.method === 'POST') {
    return readBody(req).then((b) => {
      if (b.broker) state.broker = b.broker;
      if (b.username !== undefined) state.username = b.username;
      if (b.password !== undefined) state.password = b.password;
      connectMqtt();
      sendJson(res, 200, { ok: true });
    });
  }

  if (url === '/api/subscribe' && req.method === 'POST') {
    return readBody(req).then((b) => {
      const topic = String(b.topic || '').trim();
      if (!topic) return sendJson(res, 400, { ok: false, error: 'topic required' });
      const qos = Number(b.qos) || 1;
      subscriptions.set(topic, qos);
      mqttClient.subscribe(topic, { qos }, (err) => {
        if (err) return sendJson(res, 500, { ok: false, error: err.message });
        sendJson(res, 200, { ok: true, topic });
      });
    });
  }

  if (url === '/api/unsubscribe' && req.method === 'POST') {
    return readBody(req).then((b) => {
      const topic = String(b.topic || '');
      subscriptions.delete(topic);
      mqttClient.unsubscribe(topic);
      sendJson(res, 200, { ok: true });
    });
  }

  if (url === '/api/publish' && req.method === 'POST') {
    return readBody(req).then((b) => {
      const topic = String(b.topic || '').trim();
      if (!topic) return sendJson(res, 400, { ok: false, error: 'topic required' });
      const message = b.message !== undefined ? String(b.message) : '';
      mqttClient.publish(topic, message, { qos: Number(b.qos) || 1 });
      sendJson(res, 200, { ok: true, topic, message });
    });
  }

  if (url === '/api/sim/start' && req.method === 'POST') {
    return readBody(req).then((b) => {
      if (!state.connected) return sendJson(res, 400, { ok: false, error: 'not connected to broker' });
      startSimulator(b);
      sendJson(res, 200, { ok: true });
    });
  }

  if (url === '/api/sim/stop' && req.method === 'POST') {
    stopSimulator();
    return sendJson(res, 200, { ok: true });
  }

  if (req.method === 'GET') return serveStatic(req, res);

  res.writeHead(404);
  res.end('Not found');
});

server.listen(PORT, HOST, () => {
  console.log(`MQTT Simulator UI  ->  http://${HOST}:${PORT}`);
  connectMqtt();
});
