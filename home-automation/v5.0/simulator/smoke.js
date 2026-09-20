'use strict';
// One-shot MQTTS smoke test: connect as one device with its own cert and
// publish a retained state. Exits 0 on success, 1 on failure/timeout.
const mqtt = require('mqtt');
const lib = require('./lib');

const serial = process.argv[2] || 'SNS-TMP-001';
const c = mqtt.connect(lib.brokerUrl(), lib.clientOptions(serial, 'smoke'));

const timer = setTimeout(() => { console.error('SMOKE timeout'); process.exit(1); }, 8000);

c.on('connect', () => {
  console.log(`SMOKE: connected as ${serial} over ${lib.brokerUrl()}`);
  const topic = lib.sensorStateTopic(serial, 2);
  const payload = { serial_number: serial, device_kind: 'sensor', device_type: 2, analog_value: [25.5] };
  c.publish(topic, lib.envelope(serial, 'state', payload), { qos: 1, retain: true });
  setTimeout(() => { clearTimeout(timer); console.log('SMOKE: done'); process.exit(0); }, 1500);
});
c.on('error', (e) => { clearTimeout(timer); console.error('SMOKE error:', e.message); process.exit(1); });
