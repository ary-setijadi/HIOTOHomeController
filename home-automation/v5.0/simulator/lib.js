'use strict';

// Shared config + envelope + topic helpers + device inventory (v5.0 mTLS).
// Each device/service connects with its own client certificate (CN = serial).

const path = require('path');
const fs = require('fs');

const BROKER = process.env.MQTT_HOST || '192.168.137.44';
const PORT = Number(process.env.MQTT_PORT || 8883); // MQTTS
const USER = process.env.MQTT_USER || 'admin';
const PASS = process.env.MQTT_PASS || '123456Aa!';
const CERTS_DIR = process.env.CERTS_DIR || path.join(__dirname, '..', 'certs');

// Inventory — house of 2 parents + 4 children.
const SWITCHES = ['SNS-SW-001', 'SNS-SW-002', 'SNS-SW-003', 'SNS-SW-004', 'SNS-SW-005', 'SNS-SW-006', 'SNS-SW-007']; // 007 = master
const LAMPS = ['ACT-LMP-001', 'ACT-LMP-002', 'ACT-LMP-003', 'ACT-LMP-004', 'ACT-LMP-005', 'ACT-LMP-006'];
const PUMP = 'ACT-PMP-001';
const PUMP_FB = 'SNS-PMP-001';
const TEMPS = ['SNS-TMP-001', 'SNS-TMP-002', 'SNS-TMP-003', 'SNS-TMP-004'];
const AIR = 'SNS-AIR-001';
const FLOW = 'SNS-FLW-001';
const ACS = ['ACT-AC-001', 'ACT-AC-002', 'ACT-AC-003', 'ACT-AC-004'];
const PURIFIER = 'ACT-APR-001';

function brokerUrl() { return `mqtts://${BROKER}:${PORT}`; }

// TLS options for a device/service identity (serial == certificate CN).
function tlsOptions(serial) {
  return {
    ca: fs.readFileSync(path.join(CERTS_DIR, 'ca.crt')),
    cert: fs.readFileSync(path.join(CERTS_DIR, `${serial}.crt`)),
    key: fs.readFileSync(path.join(CERTS_DIR, `${serial}.key`)),
    rejectUnauthorized: true,
  };
}

// Full MQTT connect options: per-device cert + username/password (2nd factor).
// reconnectPeriod is jittered so many devices don't reconnect in a synchronized
// storm (26 simultaneous TLS handshakes can overwhelm the 512 MB Pi).
function clientOptions(serial, prefix) {
  return Object.assign({}, tlsOptions(serial), {
    username: USER,
    password: PASS,
    clientId: (prefix || 'dev') + '-' + serial,
    reconnectPeriod: 3000 + Math.floor(Math.random() * 5000),
    connectTimeout: 15000,
  });
}

function uuid() {
  return 'xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx'.replace(/[xy]/g, (c) => {
    const r = (Math.random() * 16) | 0;
    const v = c === 'x' ? r : (r & 0x3) | 0x8;
    return v.toString(16);
  });
}

function envelope(source, messageClass, payload, correlationId) {
  const e = { msg_id: uuid(), ts: new Date().toISOString(), source, message_class: messageClass, payload };
  if (correlationId) e.correlation_id = correlationId;
  return JSON.stringify(e);
}

// Topic helpers — type 1 = digital (DI/DO), type 2 = analog (AI/AO).
function sensorStateTopic(serial, type) { return `home/sensor/${type}/${serial}/state`; }
function actuatorCmdTopic(serial, type) { return `home/actuator/${type}/${serial}/cmd`; }
function actuatorStateTopic(serial, type) { return `home/actuator/${type}/${serial}/state`; }
function actuatorOverrideTopic(serial, type) { return `home/actuator/${type}/${serial}/override`; }
function rulesTopic() { return 'home/config/rules'; }

module.exports = {
  BROKER, PORT, USER, PASS, CERTS_DIR,
  SWITCHES, LAMPS, PUMP, PUMP_FB, TEMPS, AIR, FLOW, ACS, PURIFIER,
  brokerUrl, tlsOptions, clientOptions, uuid, envelope,
  sensorStateTopic, actuatorCmdTopic, actuatorStateTopic, actuatorOverrideTopic, rulesTopic,
};
