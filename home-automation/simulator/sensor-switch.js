'use strict';

// 5 lamp-switch sensors (device_type 1, digital). Each publishes retained
// state on home/sensor/1/<serial>/state, toggling a random switch every few
// seconds to exercise the controller loop.

const mqtt = require('mqtt');
const lib = require('./lib');

const client = mqtt.connect(lib.brokerUrl(), {
  username: lib.USER,
  password: lib.PASS,
  clientId: 'sim-switches-' + lib.uuid().slice(0, 8),
  reconnectPeriod: 3000,
});

const switchState = {};
for (const s of lib.SENSORS) switchState[s] = Math.random() < 0.5 ? 1 : 0;

function publish(serial) {
  const payload = {
    serial_number: serial,
    device_kind: 'sensor',
    device_type: 1,
    digital_value: [switchState[serial]],
  };
  const msg = lib.envelope(serial, 'state', payload);
  client.publish(lib.sensorStateTopic(serial), msg, { qos: 1, retain: true });
  console.log(`[switch] ${serial} -> ${switchState[serial] ? 'ON ' : 'OFF'}  (${lib.sensorStateTopic(serial)})`);
}

client.on('connect', () => {
  console.log(`[switch] connected to ${lib.brokerUrl()}; publishing 5 switch states`);
  for (const s of lib.SENSORS) publish(s);

  // Toggle one random switch every 2.5 s.
  setInterval(() => {
    const s = lib.SENSORS[Math.floor(Math.random() * lib.SENSORS.length)];
    switchState[s] = switchState[s] ? 0 : 1;
    publish(s);
  }, 2500);
});

client.on('error', (e) => console.error('[switch] error:', e.message));
