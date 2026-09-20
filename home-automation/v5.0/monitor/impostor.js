'use strict';

// Impostor device demo — proves mTLS enforcement by attempting to connect to
// the MQTTS broker (8883) three different ways:
//   1. no client certificate        -> rejected (fail_if_no_peer_cert = true)
//   2. forged self-signed cert      -> rejected (verify_peer: unknown CA)
//   3. valid device cert (control)  -> accepted
//
//   node impostor.js
// Prints a JSON array of { attempt, result } and exits 0.

const mqtt = require('mqtt');
const fs = require('fs');
const path = require('path');
const lib = require('../simulator/lib');

const CA = path.join(lib.CERTS_DIR, 'ca.crt');
const IMPOSTOR_DIR = __dirname;

function attempt(name, opts) {
  return new Promise((resolve) => {
    const c = mqtt.connect(lib.brokerUrl(), Object.assign({
      reconnectPeriod: 0,      // do not auto-retry a rejected attempt
      connectTimeout: 8000,
    }, opts));
    const timer = setTimeout(() => { try { c.end(true); } catch {} resolve({ attempt: name, result: 'TIMEOUT (no event)' }); }, 12000);
    c.on('connect', () => { clearTimeout(timer); c.end(true); resolve({ attempt: name, result: 'ACCEPTED — connected successfully' }); });
    c.on('error', (e) => { clearTimeout(timer); resolve({ attempt: name, result: 'REJECTED — ' + e.message }); });
  });
}

(async () => {
  const results = [];
  results.push(await attempt('1) no client certificate', {
    ca: fs.readFileSync(CA),
    clientId: 'impostor-none',
  }));
  results.push(await attempt('2) forged self-signed certificate', {
    ca: fs.readFileSync(CA),
    cert: fs.readFileSync(path.join(IMPOSTOR_DIR, 'impostor.crt')),
    key: fs.readFileSync(path.join(IMPOSTOR_DIR, 'impostor.key')),
    clientId: 'impostor-forged',
  }));
  results.push(await attempt('3) stolen valid cert, but NO username/password', {
    ca: fs.readFileSync(CA),
    cert: fs.readFileSync(path.join(lib.CERTS_DIR, 'SNS-SW-001.crt')),
    key: fs.readFileSync(path.join(lib.CERTS_DIR, 'SNS-SW-001.key')),
    clientId: 'impostor-stolen',
  }));
  results.push(await attempt('4) legitimate device: valid cert + credentials', {
    ca: fs.readFileSync(CA),
    cert: fs.readFileSync(path.join(lib.CERTS_DIR, 'SNS-SW-001.crt')),
    key: fs.readFileSync(path.join(lib.CERTS_DIR, 'SNS-SW-001.key')),
    username: lib.USER,
    password: lib.PASS,
    clientId: 'impostor-legit',
  }));
  console.log(JSON.stringify(results, null, 2));
  process.exit(0);
})();
