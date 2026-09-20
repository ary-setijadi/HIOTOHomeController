'use strict';

// HIOTO device simulator (V4m2): one representative device per HIOTO category,
// publishing over plaintext MQTT using HIOTO's actual topic convention
// (Sensor, Aktuator, Log/#, Status, sensor_suhu/#, sensor_water_tank/#,
// sensor_gas_detector/#, sensor_weather/#, smart_bell/#).
//   node hioto-house.js

const mqtt = require('mqtt');
const lib = require('./lib');

const client = mqtt.connect(lib.brokerUrl(), lib.clientOptions('hioto-house'));

// Rate limiter: publish at most RATE_LIMIT messages/second (default 3).
// Latest-wins per topic so no state is dropped forever (bounded by topic count).
const RATE_LIMIT = Number(process.env.RATE_LIMIT || 3);
const pending = new Map(); // topic -> { payload, opts }
setInterval(() => {
  const topic = pending.keys().next().value;
  if (topic === undefined) return;
  const item = pending.get(topic);
  pending.delete(topic);
  client.publish(topic, item.payload, item.opts);
}, Math.round(1000 / RATE_LIMIT));

function ratePublish(topic, payload, opts) {
  pending.set(topic, { payload, opts }); // latest value wins
}

// One device per HIOTO category.
const devices = [
  { guid: 'LAMP.001',                  type: 'AKTUATOR',              topic: 'Aktuator',                       v: 0,   extra: {} },
  { guid: 'SAKLAR.001',                type: 'SENSOR',                topic: 'Sensor',                        v: 0,   extra: {} },
  { guid: 'HIOTO-SECURITYCAMERA-001',  type: 'SENSOR_CAMERA',         topic: 'Sensor',                        v: 0,   extra: { filename: 'cam-001-0001.jpg' } },
  { guid: 'HIOTO-SUHU-001',            type: 'SENSOR_SUHU',           topic: 'sensor_suhu/HIOTO-SUHU-001',     v: 26.5, extra: { temperature: 26.5, humidity: 60 } },
  { guid: 'HIOTO-GAS-001',             type: 'SENSOR_GAS_DETECTOR',   topic: 'sensor_gas_detector/HIOTO-GAS-001', v: 80, extra: { condition: 'normal' } },
  { guid: 'LSKK-HIOTO-WATERLEVEL',     type: 'SENSOR_WATER_TANK',     topic: 'sensor_water_tank/LSKK-HIOTO-WATERLEVEL', v: 150, extra: { unit: 'cm' } },
  { guid: 'HIOTO-WEATHER-001',         type: 'SENSOR_WEATHER',        topic: 'sensor_weather/HIOTO-WEATHER-001', v: 5.5, extra: {} },
  { guid: 'HIOTO-SMARTBELL',           type: 'SENSOR_BELL',           topic: 'smart_bell/HIOTO-SMARTBELL',     v: 0,   extra: {} },
  { guid: 'HIOTO-SMARTRELAY',          type: 'SENSOR_SMART_RELAY',    topic: 'Log/HIOTO-SMARTRELAY',           v: 330, extra: { voltage: 220, current: 1.5, power: 330, energy: 1.2, frequency: 50, pf: 0.95 } },
  { guid: 'HIOTO-SMARTSTEKER',         type: 'SENSOR_SMART_PLUG',     topic: 'Log/HIOTO-SMARTSTEKER',          v: 120, extra: { voltage: 220, current: 0.5, power: 120, energy: 0.4, frequency: 50, pf: 0.9 } },
  { guid: 'HIOTO-DIDO-001',            type: 'DI_DO',                 topic: 'Sensor',                        v: 0,   extra: {} },
];

// Manually-overridden sensors (set via the dashboard) are skipped so the value
// persists until released.
const manualSensors = {};

function pub(dev) {
  if (manualSensors[dev.guid] !== undefined) return;
  const payload = Object.assign({ guid: dev.guid, value: dev.v }, dev.extra);
  ratePublish(dev.topic, JSON.stringify(payload), { qos: 0 });
  // also publish a Status heartbeat occasionally
  if (Math.random() < 0.3) ratePublish('Status', JSON.stringify({ guid: dev.guid }), { qos: 0 });
}

client.on('connect', () => {
  console.log(`[hioto] connected to ${lib.brokerUrl()} (${devices.length} HIOTO devices)`);
  client.subscribe('Aktuator', { qos: 0 });
  client.subscribe('home/sensor/manual/#', { qos: 0 });

  // initial publish + periodic simulation
  devices.forEach(pub);
  setInterval(() => {
    // toggle the switch (drives the switch->lamp rule)
    const sw = devices.find((d) => d.guid === 'SAKLAR.001');
    sw.v = sw.v ? 0 : 1;
    // random walk the analog sensors
    const suhu = devices.find((d) => d.guid === 'HIOTO-SUHU-001');
    suhu.v = Math.max(22, Math.min(30, suhu.v + (Math.random() - 0.5)));
    suhu.extra.temperature = suhu.v;
    const gas = devices.find((d) => d.guid === 'HIOTO-GAS-001');
    gas.v = Math.max(20, Math.min(180, gas.v + (Math.random() - 0.5) * 40));
    const water = devices.find((d) => d.guid === 'LSKK-HIOTO-WATERLEVEL');
    water.v = Math.max(50, Math.min(200, water.v + (Math.random() - 0.5) * 10));
    // occasionally ring the bell
    const bell = devices.find((d) => d.guid === 'HIOTO-SMARTBELL');
    bell.v = Math.random() < 0.15 ? 1 : 0;

    devices.forEach(pub);
  }, 3000);
});

client.on('message', (topic, buf) => {
  // Manual sensor override: home/sensor/manual/<guid>
  if (topic.startsWith('home/sensor/manual/')) {
    const guid = topic.split('/')[3];
    let m; try { m = JSON.parse(buf.toString()); } catch { return; }
    if (m.override) manualSensors[guid] = m.value;
    else delete manualSensors[guid];
    return;
  }
  if (topic !== 'Aktuator') return;
  let m; try { m = JSON.parse(buf.toString()); } catch { return; }
  const lamp = devices.find((d) => d.guid === 'LAMP.001');
  if (m.guid === 'LAMP.001' && typeof m.value === 'number') {
    lamp.v = m.value;
    console.log(`[hioto] LAMP.001 <- cmd ${m.value ? 'ON' : 'OFF'}`);
    ratePublish('Aktuator', JSON.stringify({ guid: 'LAMP.001', value: lamp.v }), { qos: 0 });
  }
});

client.on('error', (e) => console.error('[hioto] error:', e.message));
