#!/usr/bin/env node
'use strict';

/* Publish a single AMQP message to a topic exchange. */

const { amqp, getArg, baseConfig, connectOpts } = require('./lib');

const args = process.argv.slice(2);
const key = getArg(args, '--key') || getArg(args, '--routing-key');
const message = getArg(args, '--message') || getArg(args, '-m') || '';
const exchange = getArg(args, '--exchange') || process.env.EXCHANGE || 'iot';

if (!key || args.includes('-h') || args.includes('--help')) {
  console.log(`
Usage: node publish.js --key <routing-key> --message <msg> [options]

Options:
  --key <k>          routing key to publish to (required)
  --message <msg>    message body
  --exchange <name>  topic exchange          [default: iot]
  --host <h>         [default: 192.168.137.44]
  --port <p>         [default: 5672]
  --user <u>         [default: admin]
  --password <p>     [default: 123456Aa!]

Example:
  node publish.js --key iot.sim-001.commands --message '{"command":"reboot"}'
`);
  process.exit(key ? 0 : 1);
}

(async () => {
  const cfg = baseConfig(args);
  const conn = await amqp.connect(connectOpts(cfg));
  const ch = await conn.createChannel();
  await ch.assertExchange(exchange, 'topic', { durable: true });
  ch.publish(exchange, key, Buffer.from(message), { persistent: true });
  console.log(`published to exchange "${exchange}" routing key "${key}": ${message}`);
  await ch.close();
  await conn.close();
})().catch((err) => {
  console.error('error:', err.message);
  process.exit(1);
});
