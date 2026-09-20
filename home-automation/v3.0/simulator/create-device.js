'use strict';

// Create a new device at runtime (v3.0).
//   node create-device.js <SERIAL> <sensor|actuator> <1|2> [interval-ms]

const mqtt = require('mqtt');
const lib = require('./lib');

const [serial, kind, typeStr, intervalStr] = process.argv.slice(2);
const type = Number(typeStr) || 1;
const interval = Number(intervalStr) || 3000;
if (!serial || !['sensor', 'actuator'].includes(kind)) {
  console.log('usage: node create-device.js <SERIAL> <sensor|actuator> <1|2> [interval-ms]');
  process.exit(1);
}

const client = mqtt.connect(lib.brokerUrl(), { username: lib.USER, password: lib.PASS, clientId: 'dev-' + serial + '-' + lib.uuid().slice(0, 6) });
let value = type === 2 ? Math.random() : (Math.random() < 0.5 ? 1 : 0);

function publish() {
  const payload = { serial_number: serial, device_kind: kind, device_type: type };
  if (type === 2) payload.analog_value = [Math.round(value * 10) / 10];
  else payload.digital_value = [value];
  const topic = kind === 'sensor' ? lib.sensorStateTopic(serial, type) : lib.actuatorStateTopic(serial, type);
  client.publish(topic, lib.envelope(serial, 'state', payload), { qos: 1, retain: true });
  console.log(`[${kind}] ${serial} (type ${type}) -> ${JSON.stringify(type === 2 ? payload.analog_value : payload.digital_value)}`);
}

client.on('connect', () => {
  console.log(`[${kind}] created ${serial} (type ${type}); publishing every ~${interval}ms`);
  publish();
  setInterval(() => { value = type === 2 ? Math.random() : (value ? 0 : 1); publish(); }, interval);
});
client.on('error', (e) => console.error(`[${serial}] error:`, e.message));
