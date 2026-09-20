'use strict';

// Spawn a new lamp-switch sensor at runtime (v2.0 dynamic device creation).
//   node create-switch.js <SERIAL> [interval-ms]

const mqtt = require('mqtt');
const lib = require('./lib');

const serial = process.argv[2];
const interval = Number(process.argv[3]) || 3000;
if (!serial) {
  console.log('usage: node create-switch.js <SERIAL> [interval-ms]');
  process.exit(1);
}

const client = mqtt.connect(lib.brokerUrl(), {
  username: lib.USER,
  password: lib.PASS,
  clientId: 'sw-' + serial + '-' + lib.uuid().slice(0, 6),
  reconnectPeriod: 3000,
});

let value = Math.random() < 0.5 ? 1 : 0;

function publish() {
  const payload = { serial_number: serial, device_kind: 'sensor', device_type: 1, digital_value: [value] };
  client.publish(`home/sensor/1/${serial}/state`, lib.envelope(serial, 'state', payload), { qos: 1, retain: true });
  console.log(`[switch] ${serial} -> ${value ? 'ON' : 'OFF'}`);
}

client.on('connect', () => {
  console.log(`[switch] created ${serial}; toggling every ~${interval}ms`);
  publish();
  setInterval(() => { value = value ? 0 : 1; publish(); }, interval);
});

client.on('error', (e) => console.error(`[switch ${serial}] error:`, e.message));
