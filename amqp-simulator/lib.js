'use strict';

const amqp = require('amqplib');

function getArg(args, name) {
  const i = args.indexOf(name);
  return i !== -1 ? args[i + 1] : undefined;
}

// Shared connection settings (CLI arg -> env var -> default).
function baseConfig(args) {
  return {
    host: getArg(args, '--host') || process.env.AMQP_HOST || '192.168.137.44',
    port: Number(getArg(args, '--port') || process.env.AMQP_PORT || '5672'),
    vhost: getArg(args, '--vhost') || process.env.AMQP_VHOST || '/',
    user: getArg(args, '--user') || process.env.AMQP_USER || 'admin',
    password: getArg(args, '--password') || process.env.AMQP_PASS || '123456Aa!',
    exchange: getArg(args, '--exchange') || process.env.EXCHANGE || 'iot',
  };
}

// amqplib connect options object (avoids URL-encoding issues with special chars in the password).
function connectOpts(cfg) {
  return {
    protocol: 'amqp',
    hostname: cfg.host,
    port: cfg.port,
    vhost: cfg.vhost,
    username: cfg.user,
    password: cfg.password,
    heartbeat: 30,
  };
}

module.exports = { amqp, getArg, baseConfig, connectOpts };
