'use strict';

// Measure end-to-end control-loop latency: switch state publish -> lamp state change.
const mqtt = require('mqtt');
const lib = require('./lib');

const SW = 'SNS-SW-006';
const LAMP = 'ACT-LMP-006';
const lampTopic = lib.actuatorStateTopic(LAMP, 1);
const N = Number(process.argv[2]) || 5;

const client = mqtt.connect(lib.brokerUrl(), { username: lib.USER, password: lib.PASS, clientId: 'measure-' + lib.uuid().slice(0, 6) });

let current = null;
let pending = null;
let results = [];
let flips = 0;

client.on('connect', () => {
  client.subscribe(lampTopic, { qos: 1 });
  console.log('waiting for retained lamp state...');
});

client.on('message', (topic, buf) => {
  if (topic !== lampTopic) return;
  let env; try { env = JSON.parse(buf.toString()); } catch { return; }
  const v = (env.payload && Array.isArray(env.payload.digital_value)) ? env.payload.digital_value[0] : 0;

  if (current === null) { current = v; startFlip(); return; }
  if (pending && v !== current) {
    results.push(Date.now() - pending.start);
    current = v; pending = null; flips++;
    console.log(`  flip ${flips}: ${results[results.length - 1]} ms`);
    if (flips < N) setTimeout(startFlip, 400); else finish();
  }
});

function startFlip() {
  const target = current ? 0 : 1;
  pending = { start: Date.now() };
  const payload = { serial_number: SW, device_kind: 'sensor', device_type: 1, digital_value: [target] };
  client.publish(lib.sensorStateTopic(SW, 1), lib.envelope(SW, 'state', payload), { qos: 1, retain: true });
}

function finish() {
  const avg = Math.round(results.reduce((a, b) => a + b, 0) / results.length);
  console.log(`\nend-to-end latency (switch -> controller -> lamp):`);
  console.log(`  min=${Math.min(...results)} ms, avg=${avg} ms, max=${Math.max(...results)} ms (${N} flips)`);
  process.exit(0);
}

setTimeout(() => { console.log('timeout'); process.exit(1); }, 20000);
