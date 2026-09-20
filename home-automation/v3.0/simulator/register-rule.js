'use strict';

// Register / remove a rule at runtime (v3.0).
//   node register-rule.js add '<rule-json>'
//   node register-rule.js remove <rule-name>
//
// Example:
//   node register-rule.js add '{"name":"night-light","when":{"sensor":"SNS-SW-003","op":"==","threshold":1},"then":[{"actuator":"ACT-LMP-003","value":1}],"else":[{"actuator":"ACT-LMP-003","value":0}]}'

const mqtt = require('mqtt');
const lib = require('./lib');

const [, , action, arg] = process.argv;
if (!action || !arg) {
  console.log('usage: node register-rule.js add <rule-json> | remove <name>');
  process.exit(1);
}

const client = mqtt.connect(lib.brokerUrl(), { username: lib.USER, password: lib.PASS, clientId: 'rule-' + lib.uuid().slice(0, 6) });

client.on('connect', () => {
  let payload;
  if (action === 'remove') {
    payload = { action: 'remove', name: arg };
  } else {
    payload = { action: 'add', rule: JSON.parse(arg) };
  }
  client.publish(lib.rulesTopic(), lib.envelope('ui', 'event', payload), { qos: 1 });
  console.log(`rule ${action}: ${JSON.stringify(payload)}`);
  setTimeout(() => process.exit(0), 800);
});
client.on('error', (e) => { console.error('error:', e.message); process.exit(1); });
