#!/usr/bin/env node
'use strict';

/*
 * AMQP IoT Device Simulator
 *
 * Simulates one or more IoT devices over AMQP (RabbitMQ): each publishes
 * periodic telemetry to a topic exchange and consumes commands from a bound
 * queue.
 *
 * Run with:  node simulator.js [options]
 * Help:      node simulator.js --help
 */

const { amqp, getArg, baseConfig, connectOpts } = require('./lib');

const args = process.argv.slice(2);
const has = (f) => args.includes(f);

const HELP = `
AMQP IoT Device Simulator
=========================
Publishes periodic telemetry and consumes commands (topic exchange).

Usage:
  node simulator.js [options]

Options (env var fallback in parentheses):
  --host <h>            AMQP host          (AMQP_HOST)      [default: 192.168.137.44]
  --port <p>            AMQP port          (AMQP_PORT)      [default: 5672]
  --vhost <v>           Virtual host       (AMQP_VHOST)     [default: /]
  --user <u>            Username           (AMQP_USER)      [default: admin]
  --password <p>        Password           (AMQP_PASS)      [default: 123456Aa!]
  --exchange <name>     Topic exchange     (EXCHANGE)       [default: iot]
  --device <id>         Device ID          (DEVICE_ID)      [default: sim-001]
  --count <n>           Number of devices  (COUNT)          [default: 1]
  --telemetry-key <k>   Telemetry routing key  (TELEMETRY_KEY) [default: iot.{id}.telemetry]
  --command-key <k>     Command routing key    (COMMAND_KEY)   [default: iot.{id}.commands]
  --interval <sec>      Publish interval   (INTERVAL)       [default: 5]
  -h, --help            Show this help

Routing-key templates use {id} as a placeholder for the device ID.

Examples:
  node simulator.js
  node simulator.js --device pump-01 --interval 2
  node simulator.js --count 5
  node simulator.js --telemetry-key 'factory.{id}.sensors' --command-key 'factory.{id}.cmd'
`;

if (has('-h') || has('--help')) {
  console.log(HELP);
  process.exit(0);
}

const cfg = baseConfig(args);
const deviceId = getArg(args, '--device') || process.env.DEVICE_ID || 'sim-001';
const count = parseInt(getArg(args, '--count') || process.env.COUNT || '1', 10);
const telemetryKeyTpl = getArg(args, '--telemetry-key') || process.env.TELEMETRY_KEY || 'iot.{id}.telemetry';
const commandKeyTpl = getArg(args, '--command-key') || process.env.COMMAND_KEY || 'iot.{id}.commands';
const interval = Math.max(0.5, parseFloat(getArg(args, '--interval') || process.env.INTERVAL || '5'));
const fill = (tpl, id) => tpl.replace(/\{id\}/g, id);

async function runDevice(id) {
  const telemetryKey = fill(telemetryKeyTpl, id);
  const commandKey = fill(commandKeyTpl, id);
  const commandQueue = `cmd.${id}`;

  let temperature = 20 + Math.random() * 8;
  let humidity = 45 + Math.random() * 20;
  let battery = 100;

  for (;;) {
    let conn;
    try {
      conn = await amqp.connect(connectOpts(cfg));
      const ch = await conn.createChannel();

      await ch.assertExchange(cfg.exchange, 'topic', { durable: true });
      await ch.assertQueue(commandQueue, { durable: true });
      await ch.bindQueue(commandQueue, cfg.exchange, commandKey);

      await ch.consume(commandQueue, (msg) => {
        if (msg) {
          console.log(`[${id}] <<< command (${msg.fields.routingKey}): ${msg.content.toString()}`);
          ch.ack(msg);
        }
      });

      console.log(`[${id}] connected: publishing to "${telemetryKey}", consuming "${commandQueue}" (bound to "${commandKey}")`);

      const timer = setInterval(() => {
        temperature += (Math.random() - 0.5) * 0.6;
        humidity += (Math.random() - 0.5) * 1.2;
        battery = Math.max(0, battery - Math.random() * 0.1);
        const payload = JSON.stringify({
          device: id,
          ts: new Date().toISOString(),
          temperature: +temperature.toFixed(2),
          humidity: +humidity.toFixed(1),
          battery: +battery.toFixed(2),
        });
        ch.publish(cfg.exchange, telemetryKey, Buffer.from(payload), { persistent: true });
        console.log(`[${id}] >>> published to "${telemetryKey}": ${payload}`);
      }, interval * 1000);

      // wait until the connection closes, then reconnect
      await new Promise((resolve) => {
        conn.on('error', (e) => console.error(`[${id}] connection error: ${e.message}`));
        conn.on('close', resolve);
      });
      clearInterval(timer);
    } catch (err) {
      console.error(`[${id}] error: ${err.message}; reconnecting in 3s...`);
      if (conn) { try { conn.close(); } catch (_) { /* noop */ } }
      await new Promise((r) => setTimeout(r, 3000));
    }
  }
}

async function main() {
  console.log('AMQP IoT Device Simulator');
  console.log(`  broker:   amqp://${cfg.host}:${cfg.port}${cfg.vhost}`);
  console.log(`  exchange: ${cfg.exchange} (topic)`);
  console.log(`  devices:  ${count}`);
  console.log(`  telemetry key: ${fill(telemetryKeyTpl, deviceId)}${count > 1 ? '  (per device)' : ''}`);
  console.log(`  command key:   ${fill(commandKeyTpl, deviceId)}${count > 1 ? '  (per device)' : ''}`);
  console.log(`  interval:  ${interval}s`);
  console.log('  Press Ctrl+C to stop.\n');

  const ids = count === 1
    ? [deviceId]
    : Array.from({ length: count }, (_, i) => `${deviceId}-${String(i + 1).padStart(3, '0')}`);

  await Promise.all(ids.map((id) => runDevice(id)));
}

main().catch((err) => {
  console.error('fatal:', err.message);
  process.exit(1);
});

process.on('SIGINT', () => {
  console.log('\nShutting down...');
  process.exit(0);
});
