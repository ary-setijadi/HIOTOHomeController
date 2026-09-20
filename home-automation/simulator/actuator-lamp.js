'use strict';

// 5 lamp actuators (device_type 1, digital). Each subscribes to
// home/actuator/1/<serial>/cmd, executes the command (sets lamp on/off), and
// publishes retained state on home/actuator/1/<serial>/state, echoing the
// command's msg_id as correlation_id.

const mqtt = require('mqtt');
const lib = require('./lib');

const client = mqtt.connect(lib.brokerUrl(), {
  username: lib.USER,
  password: lib.PASS,
  clientId: 'sim-lamps-' + lib.uuid().slice(0, 8),
  reconnectPeriod: 3000,
});

const lampState = {};
for (const a of lib.ACTUATORS) lampState[a] = 0;

function publishState(serial, correlationId) {
  const payload = {
    serial_number: serial,
    device_kind: 'actuator',
    device_type: 1,
    digital_value: [lampState[serial]],
  };
  const msg = lib.envelope(serial, 'state', payload, correlationId);
  client.publish(lib.actuatorStateTopic(serial), msg, { qos: 1, retain: true });
  console.log(`[lamp] ${serial} -> ${lampState[serial] ? 'ON ' : 'OFF'}  (${lib.actuatorStateTopic(serial)})`);
}

client.on('connect', () => {
  console.log(`[lamp] connected to ${lib.brokerUrl()}; subscribing to 5 lamp cmd topics`);
  for (const a of lib.ACTUATORS) {
    client.subscribe(lib.actuatorCmdTopic(a), { qos: 1 });
    publishState(a); // initial state, retained
  }
});

client.on('message', (topic, buf) => {
  const parts = topic.split('/'); // home/actuator/1/<serial>/cmd
  const serial = parts[3];
  if (!serial || !lib.ACTUATORS.includes(serial)) return;

  let env;
  try { env = JSON.parse(buf.toString()); } catch {
    console.log(`[lamp] bad message on ${topic}`);
    return;
  }
  const payload = env.payload || {};
  const dv = payload.digital_value;
  if (Array.isArray(dv) && dv.length > 0) {
    lampState[serial] = dv[0] ? 1 : 0;
  }
  const from = env.source || 'unknown';
  const override = env.message_class === 'cmd' && env.source === 'monitor' ? ' (override)' : '';
  console.log(`[lamp] ${serial} <- cmd ${lampState[serial] ? 'ON' : 'OFF'} from ${from}${override}`);
  publishState(serial, env.msg_id);
});

client.on('error', (e) => console.error('[lamp] error:', e.message));
