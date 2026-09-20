'use strict';

// Shared config + envelope/topic helpers for the home.automation simulator.

const BROKER = process.env.MQTT_HOST || '192.168.137.44';
const PORT = Number(process.env.MQTT_PORT || 1883);
const USER = process.env.MQTT_USER || 'admin';
const PASS = process.env.MQTT_PASS || '123456Aa!';

const SENSORS = ['SNS-SW-0001', 'SNS-SW-0002', 'SNS-SW-0003', 'SNS-SW-0004', 'SNS-SW-0005'];
const ACTUATORS = ['ACT-LMP-0001', 'ACT-LMP-0002', 'ACT-LMP-0003', 'ACT-LMP-0004', 'ACT-LMP-0005'];

function brokerUrl() { return `mqtt://${BROKER}:${PORT}`; }

function uuid() {
  return 'xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx'.replace(/[xy]/g, (c) => {
    const r = (Math.random() * 16) | 0;
    const v = c === 'x' ? r : (r & 0x3) | 0x8;
    return v.toString(16);
  });
}

// Uniform envelope (DESIGN.md §6).
function envelope(source, messageClass, payload, correlationId) {
  const e = {
    msg_id: uuid(),
    ts: new Date().toISOString(),
    source,
    message_class: messageClass,
    payload,
  };
  if (correlationId) e.correlation_id = correlationId;
  return JSON.stringify(e);
}

// Topic helpers (DESIGN.md §5: home/<kind>/<type>/<serial>/<class>).
function sensorStateTopic(serial) { return `home/sensor/1/${serial}/state`; }
function actuatorCmdTopic(serial) { return `home/actuator/1/${serial}/cmd`; }
function actuatorStateTopic(serial) { return `home/actuator/1/${serial}/state`; }
function actuatorOverrideTopic(serial) { return `home/actuator/1/${serial}/override`; }
function rulesTopic() { return 'home/config/rules'; }

module.exports = {
  BROKER, PORT, USER, PASS, SENSORS, ACTUATORS,
  brokerUrl, uuid, envelope,
  sensorStateTopic, actuatorCmdTopic, actuatorStateTopic, actuatorOverrideTopic, rulesTopic,
};
