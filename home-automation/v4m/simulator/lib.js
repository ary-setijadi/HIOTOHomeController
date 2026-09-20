'use strict';

// Shared config + helpers for V4m (plaintext, local-only).
// Devices connect over plaintext MQTT (no mTLS).

const path = require('path');

const BROKER = process.env.MQTT_HOST || '192.168.137.44';
const PORT = Number(process.env.MQTT_PORT || 1883);
const USER = process.env.MQTT_USER || 'admin';
const PASS = process.env.MQTT_PASS || '123456Aa!';

function brokerUrl() { return `mqtt://${BROKER}:${PORT}`; }

// Plaintext MQTT connect options (no TLS).
function clientOptions(prefix) {
  return {
    username: USER,
    password: PASS,
    clientId: (prefix || 'dev') + '-' + Math.random().toString(16).slice(2, 10),
    reconnectPeriod: 3000,
    connectTimeout: 10000,
  };
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

function sensorStateTopic(serial, type) { return `home/sensor/${type}/${serial}/state`; }
function actuatorCmdTopic(serial, type) { return `home/actuator/${type}/${serial}/cmd`; }
function actuatorStateTopic(serial, type) { return `home/actuator/${type}/${serial}/state`; }
function actuatorOverrideTopic(serial, type) { return `home/actuator/${type}/${serial}/override`; }
function rulesTopic() { return 'home/config/rules'; }

module.exports = {
  BROKER, PORT, USER, PASS, brokerUrl, clientOptions, uuid, envelope,
  sensorStateTopic, actuatorCmdTopic, actuatorStateTopic, actuatorOverrideTopic, rulesTopic,
};
