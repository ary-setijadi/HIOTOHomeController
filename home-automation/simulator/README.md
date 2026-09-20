# Home Automation Simulator (5 switches, 5 lamps, controller, monitor)

A runnable simulation of the `home.automation` design:

- **5 lamp-switch sensors** (MQTT, `home/sensor/1/SNS-SW-000X/state`, retained)
- **5 lamp actuators** (MQTT, subscribe `.../cmd`, publish `.../state`, retained)
- **controller** (Go, AMQP, runs on the Orange Pi) — `switch-lamp` algorithm
- **monitor & supervisory control** (Node.js) — watches all states and can
  force/release lamps, superseding the controller

## Layout

```
simulator/            Node.js (PC) — MQTT edge + monitor
  lib.js              shared config + envelope + topic helpers
  sensor-switch.js    5 lamp-switch sensors
  actuator-lamp.js    5 lamp actuators
  monitor.js          monitor & supervisory control (interactive CLI)
controller/           Go (Orange Pi) — AMQP controller
```

## Run

### 1. Controller (on the Orange Pi)

```bash
# build for the Pi (linux/arm), then copy and run:
GOOS=linux GOARCH=arm GOARM=7 go build -o controller .
scp controller root@192.168.137.44:/root/
ssh root@192.168.137.44 '/root/controller -period-ms 1000'
```

### 2. Sensors + actuators + monitor (on the PC)

```bash
cd simulator
npm.cmd install --cache "C:\Users\T495\Documents\MEGA\_OrangePiBasic\.npm-cache"

node sensor-switch.js     # terminal 1 — 5 switches toggling
node actuator-lamp.js     # terminal 2 — 5 lamps
node monitor.js           # terminal 3 — monitor + override CLI
```

### 3. Monitor commands

```
list                         show all switches and lamps
override ACT-LMP-0003 on     force lamp 3 ON (controller will skip it)
override ACT-LMP-0003 off    force lamp 3 OFF
release ACT-LMP-0003         resume automatic control
```

## Loop

1. switch sensor publishes retained `state` (ON/OFF).
2. controller (AMQP) caches it; every `period-ms` it commands each lamp
   `lamp = switch` (skipping any lamp under manual override).
3. lamp actuator executes and publishes its retained `state` (echoing
   `correlation_id`).
4. monitor displays every `state` update in real time.

## Override semantics

`override <serial> on|off` sets a retained flag on
`home/actuator/1/<serial>/override` **and** sends the command directly. The
controller caches that flag (via AMQP binding) and stops commanding that lamp
until `release <serial>` clears it.
