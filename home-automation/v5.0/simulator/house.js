'use strict';

// Whole-house simulator (v5.0 mTLS): every device connects with its OWN
// client certificate (CN = serial) over MQTTS, proving per-device identity.
//   node house.js
//
// DI/DO (type 1): 7 switches, 6 lamps, pump + pump feedback.
// Analog (type 2): 4 temperature sensors, air-quality sensor, water-flow sensor,
//                  4 ACs, air purifier.

const mqtt = require('mqtt');
const lib = require('./lib');

// ---------------------------------------------------------------------------
// Simulation state (single source of truth)
// ---------------------------------------------------------------------------
const S = {};
for (const s of lib.SWITCHES) S[s] = Math.random() < 0.5 ? 1 : 0;
for (const a of lib.LAMPS) S[a] = 0;
S[lib.PUMP] = 0;
S[lib.PUMP_FB] = 0; // mirrors pump
for (const t of lib.TEMPS) S[t] = 24 + Math.random() * 4; // °C
S[lib.AIR] = 60 + Math.random() * 60;
S[lib.FLOW] = 6.0;
for (const a of lib.ACS) S[a] = 0.1;
S[lib.PURIFIER] = 0.3;
let lowFlowTicks = 0;

const TYPE = {};
for (const s of lib.SWITCHES) TYPE[s] = 1;
for (const a of lib.LAMPS) TYPE[a] = 1;
TYPE[lib.PUMP] = 1;
TYPE[lib.PUMP_FB] = 1;
for (const t of lib.TEMPS) TYPE[t] = 2;
TYPE[lib.AIR] = 2;
TYPE[lib.FLOW] = 2;
for (const a of lib.ACS) TYPE[a] = 2;
TYPE[lib.PURIFIER] = 2;

const ACTUATORS = new Set([...lib.LAMPS, lib.PUMP, ...lib.ACS, lib.PURIFIER]);
const SENSORS = new Set([...lib.SWITCHES, lib.PUMP_FB, ...lib.TEMPS, lib.AIR, lib.FLOW]);

const clients = {};

function round1(v) { return Math.round(v * 10) / 10; }
function clamp(v, lo, hi) { return Math.max(lo, Math.min(hi, v)); }

function valueOf(serial) {
  const v = serial === lib.PUMP_FB ? S[lib.PUMP] : S[serial];
  if (TYPE[serial] === 2) {
    if (serial === lib.AIR) return Math.round(v);
    return round1(v);
  }
  return v ? 1 : 0;
}

function sensorPayload(serial) {
  const base = { serial_number: serial, device_kind: 'sensor', device_type: TYPE[serial] };
  return TYPE[serial] === 2
    ? Object.assign(base, { analog_value: [valueOf(serial)] })
    : Object.assign(base, { digital_value: [valueOf(serial)] });
}
function actuatorPayload(serial) {
  const base = { serial_number: serial, device_kind: 'actuator', device_type: TYPE[serial] };
  return TYPE[serial] === 2
    ? Object.assign(base, { analog_value: [valueOf(serial)] })
    : Object.assign(base, { digital_value: [valueOf(serial)] });
}

function pubSensor(serial) {
  const c = clients[serial];
  if (c) c.publish(lib.sensorStateTopic(serial, TYPE[serial]), lib.envelope(serial, 'state', sensorPayload(serial)), { qos: 1, retain: true });
}
function pubActuator(serial) {
  const c = clients[serial];
  if (c) c.publish(lib.actuatorStateTopic(serial, TYPE[serial]), lib.envelope(serial, 'state', actuatorPayload(serial)), { qos: 1, retain: true });
}

// Handle a cmd addressed to one actuator (its own connection).
function applyCmd(serial, buf) {
  let env; try { env = JSON.parse(buf.toString()); } catch { return; }
  const p = env.payload || {};
  if (TYPE[serial] === 1 && Array.isArray(p.digital_value)) {
    const v = p.digital_value[0] ? 1 : 0;
    if (serial === lib.PUMP) {
      S[lib.PUMP] = v;
      console.log(`[house] PUMP <- cmd ${v ? 'ON' : 'OFF'}`);
      pubActuator(lib.PUMP);
      pubSensor(lib.PUMP_FB);
    } else {
      S[serial] = v;
      console.log(`[house] ${serial} <- cmd ${v ? 'ON' : 'OFF'}`);
      pubActuator(serial);
    }
  } else if (TYPE[serial] === 2 && Array.isArray(p.analog_value)) {
    const v = p.analog_value[0];
    S[serial] = v;
    if (serial === lib.PURIFIER) console.log(`[house] PURIFIER <- speed ${Math.round(v * 100)}%`);
    else console.log(`[house] ${serial} <- cooling ${Math.round(v * 100)}%`);
    pubActuator(serial);
  }
}

// Create one MQTT client for a device, using that device's certificate.
function connect(serial, kind) {
  const c = mqtt.connect(lib.brokerUrl(), lib.clientOptions(serial, kind === 'sensor' ? 'sns' : 'act'));
  clients[serial] = c;
  c.on('connect', () => {
    console.log(`[house] ${serial} connected`);
    if (kind === 'actuator') c.subscribe(lib.actuatorCmdTopic(serial, TYPE[serial]), { qos: 1 });
    if (kind === 'sensor') pubSensor(serial); else pubActuator(serial);
  });
  c.on('message', (topic, buf) => { if (kind === 'actuator') applyCmd(serial, buf); });
  c.on('error', (e) => console.error(`[house] ${serial} error:`, e.message));
  return c;
}

// ---------------------------------------------------------------------------
// Bring up every device on its own TLS connection.
// ---------------------------------------------------------------------------
for (const s of SENSORS) connect(s, 'sensor');
for (const a of ACTUATORS) connect(a, 'actuator');

// ---------------------------------------------------------------------------
// Periodic simulation
// ---------------------------------------------------------------------------
function updateFlow() {
  if (lowFlowTicks > 0) {
    S[lib.FLOW] = 1.0 + Math.random() * 1.5; // low
    lowFlowTicks--;
  } else if (Math.random() < 0.2) {
    lowFlowTicks = 4 + Math.floor(Math.random() * 3); // 4-6 ticks (~12-18 s) low
    S[lib.FLOW] = 1.0 + Math.random() * 1.5;
  } else {
    S[lib.FLOW] = 4 + Math.random() * 4; // normal
  }
}

setInterval(() => {
  const sw = lib.SWITCHES[Math.floor(Math.random() * lib.SWITCHES.length)];
  S[sw] = S[sw] ? 0 : 1;
  for (const t of lib.TEMPS) S[t] = clamp(S[t] + (Math.random() - 0.5) * 1.0, 22, 30);
  S[lib.AIR] = clamp(S[lib.AIR] + (Math.random() - 0.5) * 40, 20, 180);
  updateFlow();

  for (const s of SENSORS) pubSensor(s);
  for (const a of ACTUATORS) pubActuator(a);
}, 3000);
