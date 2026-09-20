#!/usr/bin/env node
'use strict';

/* Subscribe to a topic exchange: bind a queue to a routing key and print messages. */

const { amqp, getArg, baseConfig, connectOpts } = require('./lib');

const args = process.argv.slice(2);
const bindingKey = getArg(args, '--key') || getArg(args, '--binding-key');
const exchange = getArg(args, '--exchange') || process.env.EXCHANGE || 'iot';
const queue = getArg(args, '--queue'); // optional named queue (default: auto-generated exclusive)

if (!bindingKey || args.includes('-h') || args.includes('--help')) {
  console.log(`
Usage: node subscribe.js --key <binding-key> [options]

Options:
  --key <k>          binding key, supports * and # wildcards (required)
  --exchange <name>  topic exchange          [default: iot]
  --queue <name>     queue name              [default: auto-generated]
  --host <h>         [default: 192.168.137.44]
  --port <p>         [default: 5672]
  --user <u>         [default: admin]
  --password <p>     [default: 123456Aa!]

Examples:
  node subscribe.js --key 'iot.#'
  node subscribe.js --key 'iot.*.telemetry'
  node subscribe.js --key 'iot.sim-001.commands' --queue cmd.sim-001
`);
  process.exit(bindingKey ? 0 : 1);
}

(async () => {
  const cfg = baseConfig(args);
  for (;;) {
    let conn;
    try {
      conn = await amqp.connect(connectOpts(cfg));
      const ch = await conn.createChannel();
      await ch.assertExchange(exchange, 'topic', { durable: true });
      const q = await ch.assertQueue(queue || '', { exclusive: !queue, durable: !!queue });
      await ch.bindQueue(q.queue, exchange, bindingKey);
      console.log(`subscribed: queue "${q.queue}" bound to "${exchange}" with key "${bindingKey}" (Ctrl+C to stop)`);
      await ch.consume(q.queue, (msg) => {
        if (msg) {
          console.log(`<<< (${msg.fields.routingKey}): ${msg.content.toString()}`);
          ch.ack(msg);
        }
      });
      await new Promise((resolve) => {
        conn.on('error', (e) => console.error(`connection error: ${e.message}`));
        conn.on('close', resolve);
      });
    } catch (err) {
      console.error(`error: ${err.message}; reconnecting in 3s...`);
      if (conn) { try { conn.close(); } catch (_) { /* noop */ } }
      await new Promise((r) => setTimeout(r, 3000));
    }
  }
})().catch((err) => {
  console.error('error:', err.message);
  process.exit(1);
});

process.on('SIGINT', () => {
  console.log('\nStopping...');
  process.exit(0);
});
