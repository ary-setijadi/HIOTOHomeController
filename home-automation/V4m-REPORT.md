# V4m — Home Automation Controller (plaintext, local-only)

**Date:** 2026-09-17 · **Status:** Running & stable

---

## 1. Overview

V4m ("V4 minimal") is the plaintext, local-only variant of the home-automation
system, built to replace the HIOTO production stack. It runs the rule engine and
device management on the Orange Pi Zero, with device **simulators** and the
**dashboard** running on the PC.

- No TLS/mTLS — plaintext MQTT (1883) + AMQP (5672).
- Message broker: RabbitMQ 3.8.3 with the MQTT plugin.
- MQTT topic `/` ⇄ AMQP routing key `.` (the plugin maps the `home.automation`
  topic exchange).

---

## 2. Where things run

| Component | Host | Location | URL / port |
|---|---|---|---|
| RabbitMQ broker | Orange Pi Zero | system service | MQTT `1883`, AMQP `5672`, mgmt `15672` |
| Controller (Go rule engine) | Orange Pi Zero | `/root/controller-v4m`, service `controller-v4m` | HTTP `:8081` |
| SQLite database | Orange Pi Zero | `/var/lib/homeautomation/v4m.db` | — |
| **V4 whole-house simulator** | **PC** | `home-automation/v4.0/simulator/house.js` | MQTT client |
| **HIOTO simulator** | **PC** | `home-automation/v4m/simulator/hioto-house.js` | MQTT client |
| Dashboard (UI) | PC | `home-automation/v4m/simulator/server.js` | http://127.0.0.1:3005 |
| Registration + QR scanner | PC | `home-automation/v4m/registration/server.js` | http://127.0.0.1:3007 |

Orange Pi Zero: Armbian, hostname `maincontroller`, IP `192.168.137.44`, 512 MB RAM.
RabbitMQ credentials `admin` / `123456Aa!`.

### Simulators (answer: "where is the simulator?")

The simulators are **Node.js processes running on the PC** (not the Pi). They
simulate physical devices by publishing MQTT messages over the network to the
broker on the Pi.

- **V4 whole-house simulator** — `home-automation/v4.0/simulator/house.js`
  (`node house.js`) — simulates the full V4 device set (26 devices).
- **HIOTO simulator** — `home-automation/v4m/simulator/hioto-house.js`
  (`node hioto-house.js`) — one device per HIOTO category (11 devices).
- **Dashboard** — `home-automation/v4m/simulator/server.js` (`node server.js`).

Shared MQTT helper for V4m: `home-automation/v4m/simulator/lib.js`.

---

## 3. Source layout

```
home-automation/
├── VERSION.md
├── EVALUATION-v5-vs-HIOTO.md
├── V4m-REPORT.md                     (this file)
├── v4m/
│   ├── rabbitmq-plaintext.conf       (mqtt.exchange = home.automation)
│   ├── controller/
│   │   ├── main.go                   (AMQP consume + rule engine + publish)
│   │   ├── db.go                     (SQLite open/migrate + throttled writes)
│   │   ├── device.go                 (11 HIOTO categories, kind/value typing)
│   │   ├── hioto.go                  (HIOTO topic adapter + seed devices)
│   │   ├── http.go                   (device-management API :8081)
│   │   ├── controller-v4m.service    (systemd unit)
│   │   ├── go.mod / go.sum
│   │   └── controller-v4m            (cross-compiled ARM binary)
│   ├── simulator/
│   │   ├── server.js                 (dashboard :3005)
│   │   ├── hioto-house.js            (HIOTO simulator)
│   │   ├── lib.js                    (broker URL + envelope + topic helpers)
│   │   └── public/{index.html,app.js,style.css}
│   └── registration/
│       ├── server.js                 (registration + QR :3007)
│       └── public/{index.html,scan.js}
└── v4.0/
    └── simulator/
        ├── house.js                  (V4 whole-house simulator, 26 devices)
        └── lib.js
```

---

## 4. Message conventions

### V4 topics (the controller's native protocol)

```
home/<kind>/<type>/<serial>/<class>
```

- `kind` = `sensor` | `actuator`
- `type` = `1` (digital) | `2` (analog)
- `class` = `state` | `cmd` | `override` | `event`

Examples:
- `home/sensor/1/SNS-SW-001/state`
- `home/actuator/1/ACT-LMP-001/cmd`
- `home/actuator/1/ACT-LMP-001/override`

### Envelope (V4 messages)

```json
{ "msg_id":"…", "ts":"…", "source":"…",
  "message_class":"state|cmd|event", "payload":{…} }
```

State payload (sensor/actuator):
```json
{ "serial_number":"SNS-SW-001", "device_kind":"sensor", "device_type":1,
  "digital_value":[1] }
```
(analog devices use `"analog_value":[26.5]`).

### Manual sensor override

- Dashboard publishes `home/sensor/<type>/<serial>/state` with `source:"ui"`.
- Plus a **retained** `home/sensor/manual/<serial>` = `{"override":true,"value":v}`
  (release publishes `{"override":false}`).
- Simulators subscribe `home/sensor/manual/#` and skip auto-publishing any
  overridden sensor until released.

### HIOTO topics (flat, adapted by the controller)

`Sensor`, `Aktuator`, `Status`, `Log/#`, `sensor_suhu/#`, `sensor_water_tank/#`,
`sensor_gas_detector/#`, `sensor_weather/#`, `smart_bell/#`.

---

## 5. Devices (38 registered) and Rules (20)

### V4 whole-house simulator — 26 devices
- 7 digital switches: `SNS-SW-001` … `SNS-SW-007`
- 6 digital lamps: `ACT-LMP-001` … `ACT-LMP-006`
- 1 pump actuator `ACT-PMP-001` + pump feedback sensor `SNS-PMP-001`
- 4 analog temp sensors `SNS-TMP-001` … `SNS-TMP-004`
- 1 air-quality sensor `SNS-AIR-001`
- 1 water-flow sensor `SNS-FLW-001`
- 4 analog AC actuators `ACT-AC-001` … `ACT-AC-004`
- 1 analog air-purifier actuator `ACT-APR-001`

### HIOTO simulator — 11 devices (one per category)
`LAMP.001` (AKTUATOR), `SAKLAR.001` (SENSOR), `HIOTO-SECURITYCAMERA-001`,
`HIOTO-SUHU-001`, `HIOTO-GAS-001`, `LSKK-HIOTO-WATERLEVEL`, `HIOTO-WEATHER-001`,
`HIOTO-SMARTBELL`, `HIOTO-SMARTRELAY`, `HIOTO-SMARTSTEKER`, `HIOTO-DIDO-001`.

### Rules — 20 total
- **14 from `rule_devices`** (level rules):
  - 12 V4 switch→lamp rules (`SNS-SW-00X` value 0/1 → `ACT-LMP-00X` 0/1).
  - 2 HIOTO rules (`SAKLAR.001` → `LAMP.001`).
- **6 advanced threshold rules**:
  - `water-pump` (`SNS-FLW-001 < 2` → pump on, with hysteresis + 5 s debounce).
  - `ac-1` … `ac-4` (`SNS-TMP-00X > 26` → AC 0.8 / else 0.1).
  - `air-purifier` (`SNS-AIR-001 > 100` → 1.0 / else 0.3).

Rule engine: level rules (then/else, hysteresis, debounce) vs trigger rules;
priority ordering + first-write-wins per actuator per tick.

---

## 6. Data flow

1. Simulator publishes sensor state → MQTT → `home.automation` exchange →
   controller's AMQP queue (bound to `home.sensor.*.*.state` etc.).
2. Controller rule engine ticks every **1 s**, evaluates rules against the
   in-memory sensor map, and publishes actuator `cmd` messages.
3. Simulator subscribes to its `cmd` topic, applies the value, and publishes the
   new actuator `state`.
4. Dashboard subscribes to all `state` topics and renders sensors/actuators/rules.

---

## 7. Stability hardening (applied this session)

| Issue | Fix |
|---|---|
| Abandoned MQTT QoS-1 durable queues buffered ~165 K messages → memory pressure / Pi reboots | Purged queues; all publishes+subscriptions switched to **QoS 0** ("no consumer = lost") |
| Controller published every command `Persistent` (≈12 disk fsyncs/s to SD card) | Command delivery switched to **Transient** |
| Rate limiter FIFO (cap 200) dropped released-sensor state forever | Replaced with **latest-wins per topic** (bounded, ≤3 msg/s, no permanent drops) |
| `[v4sensor]` debug log wrote ~6 lines/s to journald | Removed |
| Controller queue buffered messages while down | Non-durable / exclusive / auto-delete queue + auto-ack |
| SQLite "database is locked" | WAL + `busy_timeout=5000`, write throttle 5 s per (guid,metric), in-memory device map |
| Controller rule loop | Clamped to ≥333 ms (runs at 1000 ms) |

Result after fixes: 4 × `qos0` queues at 0 messages, ~150 MB RAM used / ~320 MB
available, load < 0.5, no reboot.

---

## 8. Build & deploy

### Controller (Go → ARM)

```powershell
$env:GOOS='linux'; $env:GOARCH='arm'; $env:GOARM='7'
$env:GOPROXY='off'   # deps already cached; use proxy.golang.org for first download
go build -o controller-v4m .
```

Deploy (Pi):
1. `systemctl stop controller-v4m`
2. `pscp controller-v4m root@192.168.137.44:/root/controller-v4m.new`
3. `chmod +x /root/controller-v4m.new && mv /root/controller-v4m.new /root/controller-v4m`
4. `systemctl start controller-v4m`

### Simulators / UI (Node.js, on PC)

```powershell
node home-automation/v4.0/simulator/house.js      # V4 simulator
node home-automation/v4m/simulator/hioto-house.js # HIOTO simulator
node home-automation/v4m/simulator/server.js      # dashboard :3005
node home-automation/v4m/registration/server.js   # registration :3007
```

---

## 9. Verified behaviors

- Manual sensor override fires its rule in **both directions**:
  `SNS-SW-001 = 1 → ACT-LMP-001 ON`, `SNS-SW-001 = 0 → ACT-LMP-001 OFF`.
- Release restores simulator control (sensor resumes toggling).
- HIOTO `SAKLAR.001 → LAMP.001` rule alternates correctly.
- Unconsumed messages are dropped (QoS 0) — no durable backlog.
