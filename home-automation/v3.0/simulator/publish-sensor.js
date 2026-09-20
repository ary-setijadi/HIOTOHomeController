'use strict';
// Publish a sensor state (for testing): node publish-sensor.js <serial> <value> [1|2]
const mqtt = require('mqtt');
const lib = require('./lib');

const [serial, valueStr, typeStr] = process.argv.slice(2);
const value = Number(valueStr);
const type = Number(typeStr) || 1;
if (!serial || isNaN(value)) { console.log('usage: node publish-sensor.js <serial> <value> [1|2]'); process.exit(1); }

const client = mqtt.connect(lib.brokerUrl(), { username: lib.USER, password: lib.PASS, clientId: 'pub-' + lib.uuid().slice(0, 6) });
client.on('connect', () => {
  const payload = { serial_number: serial, device_kind: 'sensor', device_type: type };
  if (type === 2) payload.analog_value = [value]; else payload.digital_value = [value];
  client.publish(lib.sensorStateTopic(serial, type), lib.envelope(serial, 'state', payload), { qos: 1, retain: true });
  console.log(`published ${serial} = ${value}`);
  setTimeout(() => process.exit(0), 600);
});
client.on('error', (e) => { console.error(e.message); process.exit(1); });
