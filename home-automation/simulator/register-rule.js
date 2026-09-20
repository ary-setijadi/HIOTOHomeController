'use strict';

// Register or remove a control rule at runtime (v2.0 dynamic rules).
//   node register-rule.js add <SWITCH> <ACTUATOR:on:off> [<ACTUATOR:on:off> ...]
//   node register-rule.js remove <SWITCH>
//
// Example (switch 6 drives lamp 2 normally and lamp 4 inverted):
//   node register-rule.js add SNS-SW-0006 ACT-LMP-0002:1:0 ACT-LMP-0004:0:1

const mqtt = require('mqtt');
const lib = require('./lib');

const [, , action, sw, ...mappingStrs] = process.argv;
if (!action || !sw) {
  console.log('usage: node register-rule.js add|remove <SWITCH> [ACTUATOR:on:off ...]');
  process.exit(1);
}

const mappings = mappingStrs.map((s) => {
  const [actuator, on, off] = s.split(':');
  return { actuator, on: Number(on), off: Number(off) };
});

const client = mqtt.connect(lib.brokerUrl(), {
  username: lib.USER,
  password: lib.PASS,
  clientId: 'rule-' + lib.uuid().slice(0, 6),
});

client.on('connect', () => {
  const payload = { action, switch: sw, mappings };
  client.publish(lib.rulesTopic(), lib.envelope('ui', 'event', payload), { qos: 1 });
  console.log(`rule ${action} for ${sw}: ${JSON.stringify(payload)}`);
  setTimeout(() => process.exit(0), 800);
});

client.on('error', (e) => { console.error('error:', e.message); process.exit(1); });
