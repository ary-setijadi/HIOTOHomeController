'use strict';

// Whole-house simulator (v3.0): simulates every device in one process.
//   node house.js
//
// DI/DO (type 1): 7 switches, 6 lamps, pump + pump feedback.
// Analog (type 2): 4 temperature sensors, air-quality sensor, water-flow sensor,
//                  4 ACs, air purifier.

const mqtt = require('mqtt');
const lib = require('./lib');

const client = mqtt.connect(lib.brokerUrl(), {
  username: lib.USER, password: lib.PASS,
  clientId: 'house-' + lib.uuid().slice(0, 8),
  reconnectPeriod: 3000,
});

// Device state
const switches = {}; for (const s of lib.SWITCHES) switches[s] = Math.random() < 0.5 ? 1 : 0;
const lamps = {}; for (const a of lib.LAMPS) lamps[a] = 0;
const pump = { on: 0 };
const temps = {}; for (const t of lib.TEMPS) temps[t] = 24 + Math.random() * 4; // °C
const air = { aqi: 60 + Math.random() * 60 };
const flow = { lpm: 6.0 };
let lowFlowTicks = 0;
const acs = {}; for (const a of lib.ACS) acs[a] = 0.1;
const purifier = { speed: 0.3 };

function pubState(serial, type, payload, correlationId) {
  client.publish(lib.sensorStateTopic(serial, type), lib.envelope(serial, 'state', payload, correlationId), { qos: 1, retain: true });
}
function pubAct(serial, type, payload, correlationId) {
  client.publish(lib.actuatorStateTopic(serial, type), lib.envelope(serial, 'state', payload, correlationId), { qos: 1, retain: true });
}

client.on('connect', () => {
  console.log(`[house] connected to ${lib.brokerUrl()}`);

  // Subscribe to all actuator command topics.
  for (const a of lib.LAMPS) client.subscribe(lib.actuatorCmdTopic(a, 1), { qos: 1 });
  client.subscribe(lib.actuatorCmdTopic(lib.PUMP, 1), { qos: 1 });
  for (const a of lib.ACS) client.subscribe(lib.actuatorCmdTopic(a, 2), { qos: 1 });
  client.subscribe(lib.actuatorCmdTopic(lib.PURIFIER, 2), { qos: 1 });

  // Initial states.
  publishAll();

  // Periodic simulation.
  setInterval(() => {
    // toggle a random switch
    const s = lib.SWITCHES[Math.floor(Math.random() * lib.SWITCHES.length)];
    switches[s] = switches[s] ? 0 : 1;
    // temp random walk
    for (const t of lib.TEMPS) temps[t] = clamp(temps[t] + (Math.random() - 0.5) * 1.0, 22, 30);
    // air quality random walk
    air.aqi = clamp(air.aqi + (Math.random() - 0.5) * 40, 20, 180);
    updateFlow();
    publishAll();
  }, 3000);
});

function clamp(v, lo, hi) { return Math.max(lo, Math.min(hi, v)); }

// Water flow: mostly normal, occasionally a persistent low-flow episode
// (lasting several ticks) so the pump's debounce rule can trigger.
function updateFlow() {
  if (lowFlowTicks > 0) {
    flow.lpm = 1.0 + Math.random() * 1.5; // low
    lowFlowTicks--;
  } else if (Math.random() < 0.2) {
    lowFlowTicks = 4 + Math.floor(Math.random() * 3); // 4-6 ticks (~12-18 s) low
    flow.lpm = 1.0 + Math.random() * 1.5;
  } else {
    flow.lpm = 4 + Math.random() * 4; // normal
  }
}

function publishAll() {
  for (const s of lib.SWITCHES) pubState(s, 1, { serial_number: s, device_kind: 'sensor', device_type: 1, digital_value: [switches[s]] });
  for (const a of lib.LAMPS) pubAct(a, 1, { serial_number: a, device_kind: 'actuator', device_type: 1, digital_value: [lamps[a]] });
  pubAct(lib.PUMP, 1, { serial_number: lib.PUMP, device_kind: 'actuator', device_type: 1, digital_value: [pump.on] });
  pubState(lib.PUMP_FB, 1, { serial_number: lib.PUMP_FB, device_kind: 'sensor', device_type: 1, digital_value: [pump.on] });
  for (const t of lib.TEMPS) pubState(t, 2, { serial_number: t, device_kind: 'sensor', device_type: 2, analog_value: [round1(temps[t])] });
  pubState(lib.AIR, 2, { serial_number: lib.AIR, device_kind: 'sensor', device_type: 2, analog_value: [Math.round(air.aqi)] });
  pubState(lib.FLOW, 2, { serial_number: lib.FLOW, device_kind: 'sensor', device_type: 2, analog_value: [round1(flow.lpm)] });
  for (const a of lib.ACS) pubAct(a, 2, { serial_number: a, device_kind: 'actuator', device_type: 2, analog_value: [acs[a]] });
  pubAct(lib.PURIFIER, 2, { serial_number: lib.PURIFIER, device_kind: 'actuator', device_type: 2, analog_value: [purifier.speed] });
}
function round1(v) { return Math.round(v * 10) / 10; }

client.on('message', (topic, buf) => {
  const parts = topic.split('/'); // home/actuator/<type>/<serial>/cmd
  const type = Number(parts[2]);
  const serial = parts[3];
  let env; try { env = JSON.parse(buf.toString()); } catch { return; }
  const p = env.payload || {};

  if (lib.LAMPS.includes(serial) && Array.isArray(p.digital_value)) {
    lamps[serial] = p.digital_value[0] ? 1 : 0;
    console.log(`[house] ${serial} <- cmd ${lamps[serial] ? 'ON' : 'OFF'}`);
    pubAct(serial, type, { serial_number: serial, device_kind: 'actuator', device_type: 1, digital_value: [lamps[serial]] }, env.msg_id);
  } else if (serial === lib.PUMP && Array.isArray(p.digital_value)) {
    pump.on = p.digital_value[0] ? 1 : 0;
    console.log(`[house] PUMP <- cmd ${pump.on ? 'ON' : 'OFF'}`);
    pubAct(serial, type, { serial_number: serial, device_kind: 'actuator', device_type: 1, digital_value: [pump.on] }, env.msg_id);
    pubState(lib.PUMP_FB, 1, { serial_number: lib.PUMP_FB, device_kind: 'sensor', device_type: 1, digital_value: [pump.on] }, env.msg_id);
  } else if (lib.ACS.includes(serial) && Array.isArray(p.analog_value)) {
    acs[serial] = p.analog_value[0];
    console.log(`[house] ${serial} <- cooling ${Math.round(acs[serial] * 100)}%`);
    pubAct(serial, type, { serial_number: serial, device_kind: 'actuator', device_type: 2, analog_value: [acs[serial]] }, env.msg_id);
  } else if (serial === lib.PURIFIER && Array.isArray(p.analog_value)) {
    purifier.speed = p.analog_value[0];
    console.log(`[house] PURIFIER <- speed ${Math.round(purifier.speed * 100)}%`);
    pubAct(serial, type, { serial_number: serial, device_kind: 'actuator', device_type: 2, analog_value: [purifier.speed] }, env.msg_id);
  }
});

client.on('error', (e) => console.error('[house] error:', e.message));
