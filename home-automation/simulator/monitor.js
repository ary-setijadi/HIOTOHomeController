'use strict';

// Monitor & supervisory control (Node.js).
//
// - Subscribes to all device state (home/+/+/+/state) and prints live updates.
// - "list" shows the current state of every switch and lamp.
// - "override <ACT-LMP-000X> on|off" forces a lamp and sets a retained override
//   flag that the controller respects (it skips automatic control).
// - "release <ACT-LMP-000X>" clears the override so the controller resumes.

const mqtt = require('mqtt');
const readline = require('readline');
const lib = require('./lib');

const client = mqtt.connect(lib.brokerUrl(), {
  username: lib.USER,
  password: lib.PASS,
  clientId: 'monitor-' + lib.uuid().slice(0, 8),
  reconnectPeriod: 3000,
});

const states = {}; // serial -> { kind, value }

client.on('connect', () => {
  console.log(`[monitor] connected to ${lib.brokerUrl()}`);
  client.subscribe('home/+/+/+/state', { qos: 1 });
  console.log('[monitor] subscribed to home/+/+/+/state');
});

client.on('message', (topic, buf) => {
  const parts = topic.split('/'); // home/<kind>/<type>/<serial>/state
  const kind = parts[1];
  const serial = parts[3];
  let env;
  try { env = JSON.parse(buf.toString()); } catch { return; }
  const p = env.payload || {};
  const val = Array.isArray(p.digital_value)
    ? p.digital_value[0]
    : Array.isArray(p.analog_value) ? p.analog_value[0] : undefined;
  states[serial] = { kind, value: val };
  console.log(`[monitor] ${serial} (${kind}) = ${val === 1 ? 'ON' : val === 0 ? 'OFF' : val}`);
});

function list() {
  console.log('\n=== switches ===');
  for (const s of lib.SENSORS) {
    const v = states[s] ? states[s].value : '?';
    console.log(`  ${s} : ${v === 1 ? 'ON ' : v === 0 ? 'OFF' : '?  '}`);
  }
  console.log('=== lamps ===');
  for (const a of lib.ACTUATORS) {
    const v = states[a] ? states[a].value : '?';
    console.log(`  ${a} : ${v === 1 ? 'ON ' : v === 0 ? 'OFF' : '?  '}`);
  }
  console.log('');
}

function override(serial, onoff) {
  if (!serial || !lib.ACTUATORS.includes(serial) || !['on', 'off'].includes(onoff)) {
    console.log('usage: override <ACT-LMP-000X> on|off');
    return;
  }
  const val = onoff === 'on' ? 1 : 0;

  // 1) retained override flag so the controller stays hands-off.
  client.publish(
    lib.actuatorOverrideTopic(serial),
    lib.envelope('monitor', 'event', { serial_number: serial, override: true }),
    { qos: 1, retain: true },
  );
  // 2) command the actuator directly.
  client.publish(
    lib.actuatorCmdTopic(serial),
    lib.envelope('monitor', 'cmd', { digital_value: [val] }),
    { qos: 1 },
  );
  console.log(`[override] ${serial} forced ${val ? 'ON' : 'OFF'} (controller will skip this lamp)`);
}

function release(serial) {
  if (!serial || !lib.ACTUATORS.includes(serial)) {
    console.log('usage: release <ACT-LMP-000X>');
    return;
  }
  client.publish(
    lib.actuatorOverrideTopic(serial),
    lib.envelope('monitor', 'event', { serial_number: serial, override: false }),
    { qos: 1, retain: true },
  );
  console.log(`[release] ${serial} released (controller resumes automatic control)`);
}

function help() {
  console.log(`
Commands:
  list                              show all device states
  override <ACT-LMP-000X> on|off    force a lamp (supersedes controller)
  release <ACT-LMP-000X>            release manual override
  help                              show this help
  quit / exit                       exit
`);
}

const rl = readline.createInterface({ input: process.stdin, output: process.stdout });
rl.on('line', (line) => {
  const [cmd, a1, a2] = line.trim().split(/\s+/);
  switch (cmd) {
    case 'list': list(); break;
    case 'override': override(a1, a2); break;
    case 'release': release(a1); break;
    case 'help': help(); break;
    case 'quit': case 'exit': process.exit(0); break;
    default: if (cmd) console.log(`unknown command "${cmd}" — type 'help'`); break;
  }
});

help();
console.log('[monitor] ready. Type "list" to see current states.');
