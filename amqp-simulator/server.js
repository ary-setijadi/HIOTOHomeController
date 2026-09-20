#!/usr/bin/env node
'use strict';

/*
 * AMQP Dashboard — a local web UI for the AMQP device simulator.
 *
 * Bridges a browser to RabbitMQ (AMQP 0-9-1) over a topic exchange. The server
 * holds one connection/channel, streams received messages to the browser over
 * Server-Sent Events, and exposes a small REST API for publish / subscribe /
 * simulator control.
 *
 * Run:  node server.js   ->   http://127.0.0.1:3001
 */

const http = require('http');
const fs = require('fs');
const path = require('path');
const amqp = require('amqplib');

const PORT = Number(process.env.PORT) || 3001;
const HOST = '127.0.0.1';
const PUBLIC_DIR = path.join(__dirname, 'public');

const state = {
  host: process.env.AMQP_HOST || '192.168.137.44',
  port: Number(process.env.AMQP_PORT || '5672'),
  vhost: process.env.AMQP_VHOST || '/',
  user: process.env.AMQP_USER || 'admin',
  password: process.env.AMQP_PASS || '123456Aa!',
  exchange: process.env.EXCHANGE || 'iot',
  connected: false,
};

let conn = null;
let ch = null;
let suppressAutoReconnect = false;
let reconnecting = false;

const sseClients = new Set();
const subscriptions = new Map(); // bindingKey -> { queue, consumerTag, autoNamed }
let sim = null; // { timer, ids, telemetryKeyTpl, commandKeyTpl, interval, cmdQueue, cmdConsumerTag }

function broadcast(obj) {
  const data = `data: ${JSON.stringify(obj)}\n\n`;
  for (const res of sseClients) res.write(data);
}

function connOpts() {
  return {
    protocol: 'amqp',
    hostname: state.host,
    port: state.port,
    vhost: state.vhost,
    username: state.user,
    password: state.password,
    heartbeat: 30,
  };
}

async function recreateSubscription(key, s) {
  const q = await ch.assertQueue(s.autoNamed ? '' : s.queue, {
    exclusive: s.autoNamed,
    autoDelete: s.autoNamed,
    durable: !s.autoNamed,
  });
  await ch.bindQueue(q.queue, state.exchange, key);
  const { consumerTag } = await ch.consume(q.queue, (msg) => {
    if (msg) {
      broadcast({ type: 'message', routingKey: msg.fields.routingKey, payload: msg.content.toString(), ts: Date.now() });
      ch.ack(msg);
    }
  });
  s.queue = q.queue;
  s.consumerTag = consumerTag;
}

async function setupSimCommandConsumer() {
  if (!sim) return;
  const commandPat = sim.commandKeyTpl.replace(/\{id\}/g, '*');
  const q = await ch.assertQueue('', { exclusive: true, autoDelete: true });
  await ch.bindQueue(q.queue, state.exchange, commandPat);
  const { consumerTag } = await ch.consume(q.queue, (msg) => {
    if (msg) {
      broadcast({ type: 'message', routingKey: msg.fields.routingKey, payload: msg.content.toString(), ts: Date.now(), source: 'command' });
      ch.ack(msg);
    }
  });
  sim.cmdQueue = q.queue;
  sim.cmdConsumerTag = consumerTag;
}

async function connect() {
  conn = await amqp.connect(connOpts());

  conn.on('error', () => { /* handled via close */ });
  conn.on('close', () => {
    if (state.connected) {
      state.connected = false;
      broadcast({ type: 'status', connected: false });
    }
    if (!suppressAutoReconnect) scheduleReconnect();
  });

  ch = await conn.createChannel();
  await ch.assertExchange(state.exchange, 'topic', { durable: true });

  for (const [key, s] of subscriptions) await recreateSubscription(key, s);
  await setupSimCommandConsumer();

  state.connected = true;
  broadcast({ type: 'status', connected: true, host: state.host, exchange: state.exchange });
}

function scheduleReconnect() {
  if (reconnecting) return;
  reconnecting = true;
  setTimeout(async () => {
    reconnecting = false;
    try { await connect(); } catch (_) { scheduleReconnect(); }
  }, 3000);
}

// ---------------------------------------------------------------------------
// Simulator
// ---------------------------------------------------------------------------
function startSimulator(cfg) {
  stopSimulator(false);

  const count = Math.max(1, Number(cfg.count) || 1);
  const device = String(cfg.device || 'sim');
  const interval = Math.max(0.5, Number(cfg.interval) || 2);
  const telemetryKeyTpl = String(cfg.telemetryKey || 'iot.{id}.telemetry');
  const commandKeyTpl = String(cfg.commandKey || 'iot.{id}.commands');

  const ids = count === 1
    ? [device]
    : Array.from({ length: count }, (_, i) => `${device}-${String(i + 1).padStart(3, '0')}`);

  const sensors = {};
  for (const id of ids) sensors[id] = { temp: 20 + Math.random() * 8, hum: 45 + Math.random() * 20, battery: 100 };

  sim = { timer: null, ids, telemetryKeyTpl, commandKeyTpl, interval, cmdQueue: null, cmdConsumerTag: null };

  const tick = () => {
    for (const id of ids) {
      const s = sensors[id];
      s.temp += (Math.random() - 0.5) * 0.6;
      s.hum += (Math.random() - 0.5) * 1.2;
      s.battery = Math.max(0, s.battery - Math.random() * 0.1);
      const payload = JSON.stringify({
        device: id,
        ts: new Date().toISOString(),
        temperature: +s.temp.toFixed(2),
        humidity: +s.hum.toFixed(1),
        battery: +s.battery.toFixed(2),
      });
      const key = telemetryKeyTpl.replace(/\{id\}/g, id);
      try { ch.publish(state.exchange, key, Buffer.from(payload), { persistent: true }); } catch (_) { /* noop */ }
      broadcast({ type: 'message', routingKey: key, payload, ts: Date.now(), source: 'simulator' });
    }
  };

  tick();
  sim.timer = setInterval(tick, interval * 1000);
  setupSimCommandConsumer().catch(() => {});
  broadcast({ type: 'sim', running: true, ids, telemetryKeyTpl, commandKeyTpl, interval });
}

function stopSimulator(notify = true) {
  if (sim) {
    if (sim.timer) clearInterval(sim.timer);
    if (sim.cmdConsumerTag && ch) ch.cancel(sim.cmdConsumerTag).catch(() => {});
    if (sim.cmdQueue && ch) ch.deleteQueue(sim.cmdQueue).catch(() => {});
    sim = null;
  }
  if (notify) broadcast({ type: 'sim', running: false, ids: [] });
}

// ---------------------------------------------------------------------------
// Helpers
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

const MIME = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
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
      host: state.host,
      exchange: state.exchange,
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
      host: state.host,
      port: state.port,
      vhost: state.vhost,
      user: state.user,
      exchange: state.exchange,
      subscriptions: [...subscriptions.keys()],
      simRunning: !!sim,
    });
  }

  if (url === '/api/connect' && req.method === 'POST') {
    return readBody(req).then(async (b) => {
      if (b.host !== undefined) state.host = String(b.host);
      if (b.port !== undefined) state.port = Number(b.port);
      if (b.vhost !== undefined) state.vhost = String(b.vhost);
      if (b.user !== undefined) state.user = String(b.user);
      if (b.password !== undefined) state.password = String(b.password);
      if (b.exchange !== undefined) state.exchange = String(b.exchange);

      suppressAutoReconnect = true;
      if (conn) { try { await conn.close(); } catch (_) { /* noop */ } }
      suppressAutoReconnect = false;
      try {
        await connect();
        sendJson(res, 200, { ok: true });
      } catch (e) {
        sendJson(res, 500, { ok: false, error: e.message });
      }
    });
  }

  if (url === '/api/subscribe' && req.method === 'POST') {
    return readBody(req).then(async (b) => {
      const key = String(b.bindingKey || b.key || '').trim();
      if (!key) return sendJson(res, 400, { ok: false, error: 'binding key required' });
      if (!state.connected) return sendJson(res, 400, { ok: false, error: 'not connected' });
      if (subscriptions.has(key)) return sendJson(res, 200, { ok: true, duplicate: true, key });

      const qname = String(b.queue || '');
      const autoNamed = !qname;
      try {
        const q = await ch.assertQueue(qname, { exclusive: autoNamed, autoDelete: autoNamed, durable: !autoNamed });
        await ch.bindQueue(q.queue, state.exchange, key);
        const { consumerTag } = await ch.consume(q.queue, (msg) => {
          if (msg) {
            broadcast({ type: 'message', routingKey: msg.fields.routingKey, payload: msg.content.toString(), ts: Date.now() });
            ch.ack(msg);
          }
        });
        subscriptions.set(key, { queue: q.queue, consumerTag, autoNamed });
        broadcast({ type: 'subscriptions', list: [...subscriptions.keys()] });
        sendJson(res, 200, { ok: true, key, queue: q.queue });
      } catch (e) {
        sendJson(res, 500, { ok: false, error: e.message });
      }
    });
  }

  if (url === '/api/unsubscribe' && req.method === 'POST') {
    return readBody(req).then(async (b) => {
      const key = String(b.bindingKey || b.key || '');
      const s = subscriptions.get(key);
      if (s) {
        try { await ch.cancel(s.consumerTag); } catch (_) { /* noop */ }
        try { await ch.unbindQueue(s.queue, state.exchange, key); } catch (_) { /* noop */ }
        if (s.autoNamed) { try { await ch.deleteQueue(s.queue); } catch (_) { /* noop */ } }
        subscriptions.delete(key);
        broadcast({ type: 'subscriptions', list: [...subscriptions.keys()] });
      }
      sendJson(res, 200, { ok: true });
    });
  }

  if (url === '/api/publish' && req.method === 'POST') {
    return readBody(req).then((b) => {
      const key = String(b.routingKey || b.key || '').trim();
      if (!key) return sendJson(res, 400, { ok: false, error: 'routing key required' });
      const message = b.message !== undefined ? String(b.message) : '';
      try {
        ch.publish(state.exchange, key, Buffer.from(message), { persistent: true });
        sendJson(res, 200, { ok: true, key, message });
      } catch (e) {
        sendJson(res, 500, { ok: false, error: e.message });
      }
    });
  }

  if (url === '/api/sim/start' && req.method === 'POST') {
    return readBody(req).then((b) => {
      if (!state.connected) return sendJson(res, 400, { ok: false, error: 'not connected' });
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
  console.log(`AMQP Dashboard  ->  http://${HOST}:${PORT}`);
  connect().catch((e) => {
    console.error(`initial connect failed: ${e.message}; retrying...`);
    scheduleReconnect();
  });
});
