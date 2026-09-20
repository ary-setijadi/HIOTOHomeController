# Go Controller

A Go service that bridges MQTT (IoT) and AMQP devices, aggregates their
telemetry every **T** seconds, and sends derived results back to all devices.

## Behavior (every T seconds)

1. **Collect** the latest telemetry from all devices over both protocols:
   - MQTT: subscribes to `iot/+/telemetry`
   - AMQP: consumes a queue bound to `iot.*.telemetry` on the topic exchange `iot`
2. **Process** it into an aggregate `Summary`:
   `{ deviceCount, avgTemperature, avgHumidity, minBattery, maxBattery, devices[] }`
3. **Send** results to all devices:
   - broadcast summary → `iot/broadcast` (MQTT) and `iot.broadcast` (AMQP)
   - per-device note → `iot/{id}/commands` (MQTT) and `iot.{id}.commands` (AMQP)

## Build

```powershell
cd controller
go build -o controller.exe .
```

## Run

```powershell
.\controller.exe --period 10
```

### Options (env-var fallback in parentheses)

| Flag | Env | Default | Description |
|---|---|---|---|
| `--period` | `PERIOD` | `10` | Processing period T (seconds) |
| `--mqtt-host` | `MQTT_HOST` | `192.168.137.44` | MQTT host |
| `--mqtt-port` | `MQTT_PORT` | `1883` | MQTT port |
| `--amqp-host` | `AMQP_HOST` | `192.168.137.44` | AMQP host |
| `--amqp-port` | `AMQP_PORT` | `5672` | AMQP port |
| `--vhost` | `AMQP_VHOST` | `/` | AMQP vhost |
| `--user` | `AMQP_USER` | `admin` | Broker username |
| `--password` | `AMQP_PASS` | `123456Aa!` | Broker password |
| `--exchange` | `EXCHANGE` | `iot` | AMQP topic exchange |
| `--mqtt-telemetry-topic` | `MQTT_TELEMETRY_TOPIC` | `iot/+/telemetry` | MQTT subscription |
| `--amqp-binding-key` | `AMQP_BINDING_KEY` | `iot.*.telemetry` | AMQP binding key |
| `--broadcast` | — | `true` | Publish aggregate summary |
| `--broadcast-mqtt-topic` | `BROADCAST_MQTT_TOPIC` | `iot/broadcast` | MQTT broadcast topic |
| `--broadcast-amqp-key` | `BROADCAST_AMQP_KEY` | `iot.broadcast` | AMQP broadcast key |
| `--perdevice` | — | `true` | Send per-device notes |
| `--mqtt-command-topic` | `MQTT_COMMAND_TOPIC` | `iot/{id}/commands` | MQTT command template |
| `--amqp-command-key` | `AMQP_COMMAND_KEY` | `iot.{id}.commands` | AMQP command template |

Example:

```powershell
.\controller.exe --period 5 --perdevice --broadcast
```

## Files

- `main.go` — config, wiring, ticker loop, process logic
- `store.go` — thread-safe per-device telemetry store
- `process.go` — aggregate `Summary` + per-device `DeviceNote` computation
- `broker.go` — MQTT (paho) and AMQP (amqp091-go) connect/subscribe/publish
