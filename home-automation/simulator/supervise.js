'use strict';

// Scriptable supervisory control (non-interactive counterpart to monitor.js).
//   node supervise.js override ACT-LMP-000X on|off
//   node supervise.js release  ACT-LMP-000X

const mqtt = require('mqtt');
const lib = require('./lib');

const [, , cmd, serial, onoff] = process.argv;
if (!cmd || !serial) {
  console.log('usage: node supervise.js override|release <ACT-LMP-000X> [on|off]');
  process.exit(1);
}

const client = mqtt.connect(lib.brokerUrl(), {
  username: lib.USER,
  password: lib.PASS,
  clientId: 'supervise-' + lib.uuid().slice(0, 8),
});

client.on('connect', () => {
  if (cmd === 'override') {
    const val = onoff === 'on' ? 1 : 0;
    client.publish(
      lib.actuatorOverrideTopic(serial),
      lib.envelope('monitor', 'event', { serial_number: serial, override: true }),
      { qos: 1, retain: true },
    );
    client.publish(
      lib.actuatorCmdTopic(serial),
      lib.envelope('monitor', 'cmd', { digital_value: [val] }),
      { qos: 1 },
    );
    console.log(`override ${serial} -> ${val ? 'ON' : 'OFF'}`);
  } else if (cmd === 'release') {
    client.publish(
      lib.actuatorOverrideTopic(serial),
      lib.envelope('monitor', 'event', { serial_number: serial, override: false }),
      { qos: 1, retain: true },
    );
    console.log(`release ${serial}`);
  } else {
    console.log('unknown command (use override|release)');
  }
  setTimeout(() => process.exit(0), 800);
});

client.on('error', (e) => { console.error('error:', e.message); process.exit(1); });
