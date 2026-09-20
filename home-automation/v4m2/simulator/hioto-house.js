'use strict';

// V4m2 HIOTO simulator — simulates the FULL real HIOTO device set (all
// registrations from devices.json) using HIOTO's actual topic convention:
//   Sensor, Aktuator, Status, sensor_suhu/<guid>, sensor_water_tank/<guid>,
//   smart_bell/<guid>, Log/<guid>.
//
// Switches are classified from rule_devices: 1-bit ("0"/"1") publish 0..1,
// 2-channel ("00"/"01"/"10"/"11") publish 0..3 (binary).
//
//   node hioto-house.js

const mqtt = require('mqtt');
const fs = require('fs');
const path = require('path');

const BROKER = process.env.MQTT_HOST || '192.168.137.44';
const PORT = Number(process.env.MQTT_PORT || 1883);
const USER = process.env.MQTT_USER || '/smarthome:smarthome';
const PASS = process.env.MQTT_PASS || 'Ssm4rt2!';
const RATE_LIMIT = Number(process.env.RATE_LIMIT || 3);

const data = JSON.parse(fs.readFileSync(path.join(__dirname, 'devices.json'), 'utf8'));
const registrations = data.registrations || [];
const rules = data.rule_devices || [];

// --- classify switches: 1-bit vs 2-channel, from rule_devices input values ---
const switchBits = {};
for (const r of rules) {
  const s = switchBits[r.input_guid] || new Set();
  s.add(String(r.input_value));
  switchBits[r.input_guid] = s;
}
for (const g of Object.keys(switchBits)) {
  switchBits[g] = [...switchBits[g]].some((v) => v === '10' || v === '11') ? 2 : 1;
}

// --- topic per category ---
function topicFor(d) {
  switch (d.type) {
    case 'AKTUATOR': return 'Aktuator';
    case 'SENSOR_SUHU': return `sensor_suhu/${d.guid}`;
    case 'SENSOR_GAS_DETECTOR': return `sensor_gas_detector/${d.guid}`;
    case 'SENSOR_WATER_TANK': return `sensor_water_tank/${d.guid}`;
    case 'SENSOR_WEATHER': return `sensor_weather/${d.guid}`;
    case 'SENSOR_BELL': return `smart_bell/${d.guid}`;
    case 'SENSOR_SMART_RELAY':
    case 'SENSOR_SMART_PLUG': return `Log/${d.guid}`;
    default: return 'Sensor'; // SENSOR, SENSOR_CAMERA, DI/DO, DI_DO
  }
}

// --- build device objects with type-specific state (normalize + dedup guid) ---
const devices = [];
const byGuid = {};
for (const r of registrations) {
  const guid = String(r.guid || '').trim().replace(/^:\s*/, '');
  if (!guid || byGuid[guid]) continue; // skip empty/duplicate (trailing-space) rows
  const d = { guid, type: r.type, name: r.name, mac: r.mac, value: 0 };
  if (r.type === 'SENSOR_SUHU') {
    d.temperature = round1(25 + Math.random() * 4);
    d.humidity = round1(55 + Math.random() * 20);
  } else if (r.type === 'SENSOR_WATER_TANK') {
    d.value = Math.round(100 + Math.random() * 80);
  } else if (r.type === 'SENSOR_SMART_RELAY') {
    d.voltage = 220; d.current = 1.2; d.power = 260; d.energy = 1.0; d.frequency = 50; d.pf = 0.95;
    d.value = d.power;
  } else if (r.type === 'SENSOR_CAMERA') {
    d.filename = null;
  }
  devices.push(d);
  byGuid[guid] = d;
}

// --- payload per category ---
function payloadFor(d) {
  switch (d.type) {
    case 'SENSOR_SUHU':
      return { guid: d.guid, value: d.temperature, temperature: d.temperature, humidity: d.humidity };
    case 'SENSOR_WATER_TANK':
      return { guid: d.guid, value: d.value, unit: 'cm' };
    case 'SENSOR_SMART_RELAY':
      return { guid: d.guid, value: d.power, voltage: d.voltage, current: d.current, power: d.power, energy: d.energy, frequency: d.frequency, pf: d.pf };
    case 'SENSOR_CAMERA':
      return { guid: d.guid, value: 0, filename: d.filename || `${d.guid}-0001.jpg` };
    default:
      return { guid: d.guid, value: d.value };
  }
}

const client = mqtt.connect(`mqtt://${BROKER}:${PORT}`, {
  username: USER, password: PASS,
  clientId: 'hioto-v4m2-' + Math.random().toString(16).slice(2, 10),
  reconnectPeriod: 3000,
  connectTimeout: 10000,
});

// --- rate limiter: latest-wins per (topic, guid), at most RATE_LIMIT msg/s ---
const pending = new Map(); // key "topic::guid" -> {topic, payload}
setInterval(() => {
  const key = pending.keys().next().value;
  if (key === undefined) return;
  const item = pending.get(key);
  pending.delete(key);
  client.publish(item.topic, item.payload, { qos: 0 });
}, Math.round(1000 / RATE_LIMIT));

function pub(topic, guid, obj) {
  pending.set(`${topic}::${guid}`, { topic, payload: JSON.stringify(obj) });
}

function publish(d) {
  pub(topicFor(d), d.guid, payloadFor(d));
}

client.on('connect', () => {
  console.log(`[hioto-v4m2] connected to ${BROKER} (${devices.length} devices)`);
  client.subscribe('Aktuator', { qos: 0 });

  // initial population
  for (const d of devices) publish(d);

  setInterval(() => {
    // switches: toggle a random subset
    for (const d of devices) {
      if (d.type !== 'SENSOR' && d.type !== 'DI/DO' && d.type !== 'DI_DO') continue;
      if (Math.random() >= 0.25) continue;
      const bits = switchBits[d.guid] || 1;
      d.value = bits === 2 ? Math.floor(Math.random() * 4) : (d.value ? 0 : 1);
      publish(d);
    }
    // analog random walk (every tick, but cheap)
    for (const d of devices) {
      if (d.type === 'SENSOR_SUHU') {
        d.temperature = round1(clamp(d.temperature + (Math.random() - 0.5) * 0.8, 22, 33));
        d.humidity = round1(clamp(d.humidity + (Math.random() - 0.5) * 4, 40, 85));
        publish(d);
      } else if (d.type === 'SENSOR_WATER_TANK') {
        d.value = Math.round(clamp(d.value + (Math.random() - 0.5) * 6, 30, 200));
        publish(d);
      } else if (d.type === 'SENSOR_SMART_RELAY') {
        d.power = round1(clamp(d.power + (Math.random() - 0.5) * 40, 80, 450));
        d.current = round1(d.power / 220);
        d.value = d.power;
        publish(d);
      }
    }
    // occasional bell press + camera capture
    const bell = devices.find((d) => d.type === 'SENSOR_BELL');
    if (bell && Math.random() < 0.12) { bell.value = 1; publish(bell); }
    for (const d of devices) {
      if (d.type === 'SENSOR_CAMERA' && Math.random() < 0.04) {
        d.filename = `${d.guid}-${Date.now()}.jpg`;
        publish(d);
      }
    }
    // status heartbeat for a random device
    if (Math.random() < 0.3) {
      const d = devices[Math.floor(Math.random() * devices.length)];
      pub('Status', d.guid, { guid: d.guid });
    }
  }, 3000);
});

client.on('message', (topic, buf) => {
  if (topic !== 'Aktuator') return;
  let m; try { m = JSON.parse(buf.toString()); } catch { return; }
  const d = byGuid[m.guid];
  if (d && typeof m.value === 'number') {
    d.value = m.value ? 1 : 0;
    console.log(`[hioto-v4m2] ${d.name || d.guid} <- cmd ${d.value ? 'ON' : 'OFF'}`);
    publish(d);
  }
});

client.on('error', (e) => console.error('[hioto-v4m2] error:', e.message));

function clamp(v, lo, hi) { return Math.max(lo, Math.min(hi, v)); }
function round1(v) { return Math.round(v * 10) / 10; }
