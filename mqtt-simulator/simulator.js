#!/usr/bin/env node
'use strict';

/*
 * MQTT IoT Device Simulator
 *
 * Simulates one or more IoT devices: each connects to an MQTT broker,
 * publishes periodic telemetry, and subscribes to a command topic.
 *
 * Run with:  node simulator.js [options]
 * Help:      node simulator.js --help
 */

const mqtt = require('mqtt');

// ---------------------------------------------------------------------------
// Configuration (CLI args, with environment-variable fallbacks)
// ---------------------------------------------------------------------------
const args = process.argv.slice(2);
const getArg = (name) => {
  const i = args.indexOf(name);
  return i !== -1 ? args[i + 1] : undefined;
};
const has = (f) => args.includes(f);

const HELP = `
MQTT IoT Device Simulator
=========================
Publishes periodic telemetry and subscribes to commands.

Usage:
  node simulator.js [options]

Options (env var fallback in parentheses):
  --broker <url>       Broker URL         (BROKER)      [default: mqtt://192.168.137.44:1883]
  --device <id>        Device ID          (DEVICE_ID)   [default: sim-001]
  --count <n>          Number of devices  (COUNT)       [default: 1]
  --telemetry <topic>  Telemetry topic    (TELEMETRY)   [default: iot/{id}/telemetry]
  --command <topic>    Command topic      (COMMAND)     [default: iot/{id}/commands]
  --status <topic>     Status topic       (STATUS)      [default: iot/{id}/status]
  --interval <sec>     Publish interval   (INTERVAL)    [default: 5]
  --username <u>       MQTT username      (MQTT_USER)   [default: admin]
  --password <p>       MQTT password      (MQTT_PASS)   [default: 123456Aa!]
  -h, --help           Show this help

Topic templates use {id} as a placeholder for the device ID.

Examples:
  node simulator.js
  node simulator.js --device pump-01 --interval 2
  node simulator.js --count 5 --telemetry 'factory/{id}/sensors'
  node simulator.js --broker mqtt://192.168.137.44:1883 --username admin --password 123456Aa!
`;

if (has('-h') || has('--help')) {
  console.log(HELP);
  process.exit(0);
}

const broker       = getArg('--broker')    || process.env.BROKER    || 'mqtt://192.168.137.44:1883';
const deviceId     = getArg('--device')    || process.env.DEVICE_ID || 'sim-001';
const count        = parseInt(getArg('--count')     || process.env.COUNT     || '1', 10);
const telemetryTpl = getArg('--telemetry') || process.env.TELEMETRY || 'iot/{id}/telemetry';
const commandTpl   = getArg('--command')   || process.env.COMMAND   || 'iot/{id}/commands';
const statusTpl    = getArg('--status')    || process.env.STATUS    || 'iot/{id}/status';
const interval     = Math.max(0.5, parseFloat(getArg('--interval') || process.env.INTERVAL || '5'));
const username     = getArg('--username')  || process.env.MQTT_USER || 'admin';
const password     = getArg('--password')  || process.env.MQTT_PASS || '123456Aa!';

const fillTopic = (tpl, id) => tpl.replace(/\{id\}/g, id);

// ---------------------------------------------------------------------------
// Device simulator
// ---------------------------------------------------------------------------
function startDevice(id) {
  const telemetryTopic = fillTopic(telemetryTpl, id);
  const commandTopic   = fillTopic(commandTpl, id);
  const statusTopic    = fillTopic(statusTpl, id);

  const options = {
    clientId: `${id}-${Math.random().toString(16).slice(2, 8)}`,
    clean: true,
    reconnectPeriod: 3000,
    // Last Will & Testament: mark the device offline if it disconnects ungracefully
    will: {
      topic: statusTopic,
      payload: JSON.stringify({ device: id, status: 'offline' }),
      qos: 1,
      retain: true,
    },
  };
  if (username) options.username = username;
  if (password) options.password = password;

  const client = mqtt.connect(broker, options);

  // Random-walk sensor state
  let temperature = 20 + Math.random() * 8;
  let humidity    = 45 + Math.random() * 20;
  let battery     = 100;

  client.on('connect', () => {
    console.log(`[${id}] connected to ${broker}`);
    client.publish(statusTopic, JSON.stringify({ device: id, status: 'online' }), { qos: 1, retain: true });
    client.subscribe(commandTopic, { qos: 1 }, (err) => {
      if (err) {
        console.error(`[${id}] subscribe error on ${commandTopic}:`, err.message);
      } else {
        console.log(`[${id}] subscribed to ${commandTopic}`);
      }
    });
  });

  client.on('message', (topic, payload) => {
    console.log(`[${id}] <<< command on "${topic}": ${payload.toString()}`);
  });

  client.on('error', (err) => {
    console.error(`[${id}] error:`, err.message);
  });

  client.on('close', () => {
    console.log(`[${id}] disconnected (will retry)`);
  });

  // Periodic telemetry publish
  setInterval(() => {
    temperature += (Math.random() - 0.5) * 0.6;
    humidity    += (Math.random() - 0.5) * 1.2;
    battery      = Math.max(0, battery - Math.random() * 0.1);

    const reading = {
      device: id,
      ts: new Date().toISOString(),
      temperature: +temperature.toFixed(2),
      humidity: +humidity.toFixed(1),
      battery: +battery.toFixed(2),
    };
    const payload = JSON.stringify(reading);
    client.publish(telemetryTopic, payload, { qos: 1 });
    console.log(`[${id}] >>> published to "${telemetryTopic}": ${payload}`);
  }, interval * 1000);
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
console.log('MQTT IoT Device Simulator');
console.log(`  broker:   ${broker}`);
console.log(`  devices:  ${count}`);
console.log(`  telemetry: ${fillTopic(telemetryTpl, deviceId)}${count > 1 ? '  (per device)' : ''}`);
console.log(`  commands:  ${fillTopic(commandTpl, deviceId)}${count > 1 ? '  (per device)' : ''}`);
console.log(`  interval:  ${interval}s`);
console.log('  Press Ctrl+C to stop.\n');

if (count === 1) {
  startDevice(deviceId);
} else {
  for (let i = 1; i <= count; i++) {
    startDevice(`${deviceId}-${String(i).padStart(3, '0')}`);
  }
}

process.on('SIGINT', () => {
  console.log('\nShutting down...');
  process.exit(0);
});
