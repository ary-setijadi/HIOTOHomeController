# AMQP Device Simulator

A Node.js simulator for IoT devices over **AMQP (RabbitMQ)** using `amqplib`.
It connects to a **topic exchange**, **publishes periodic telemetry**, and
**consumes commands** from a bound queue.

Default broker: `amqp://192.168.137.44:5672` (user `admin`).

## Web Dashboard

```powershell
npm.cmd run ui        # or: node server.js
```

Open **http://127.0.0.1:3001** in your browser.

The dashboard lets you connect to the broker, subscribe (bind queues to routing
keys), publish, and start/stop the simulator — with a live message log. It runs
on a separate port from the MQTT dashboard (3000), so both can run at once.

## 1. Install

```powershell
cd amqp-simulator
npm.cmd install --cache "C:\Users\T495\Documents\MEGA\_OrangePiBasic\.npm-cache"
```

## 2. Run the simulator

```powershell
node simulator.js
```

It publishes a JSON telemetry reading every 5 s with routing key
`iot.sim-001.telemetry` and consumes commands from queue `cmd.sim-001`
(bound to `iot.sim-001.commands`).

### Options

| Flag | Env var | Default | Description |
|---|---|---|---|
| `--host <h>` | `AMQP_HOST` | `192.168.137.44` | AMQP host |
| `--port <p>` | `AMQP_PORT` | `5672` | AMQP port |
| `--vhost <v>` | `AMQP_VHOST` | `/` | Virtual host |
| `--user <u>` | `AMQP_USER` | `admin` | Username |
| `--password <p>` | `AMQP_PASS` | `123456Aa!` | Password |
| `--exchange <name>` | `EXCHANGE` | `iot` | Topic exchange |
| `--device <id>` | `DEVICE_ID` | `sim-001` | Device ID |
| `--count <n>` | `COUNT` | `1` | Number of devices |
| `--telemetry-key <k>` | `TELEMETRY_KEY` | `iot.{id}.telemetry` | Telemetry routing key |
| `--command-key <k>` | `COMMAND_KEY` | `iot.{id}.commands` | Command routing key |
| `--interval <sec>` | `INTERVAL` | `5` | Publish interval |

Examples:

```powershell
node simulator.js --device pump-01 --interval 2
node simulator.js --count 5
node simulator.js --telemetry-key 'factory.{id}.sensors' --command-key 'factory.{id}.cmd'
```

## 3. Manually publish / subscribe

```powershell
# Subscribe to everything under iot
node subscribe.js --key 'iot.#'

# Subscribe to all telemetry
node subscribe.js --key 'iot.*.telemetry'

# Publish a command to a device
node publish.js --key 'iot.sim-001.commands' --message '{"command":"reboot"}'
```

## 4. Routing scheme (topic exchange `iot`)

- `iot.<device>.telemetry` — device → broker (telemetry)
- `iot.<device>.commands` — broker → device (commands, queued in `cmd.<device>`)

Topic binding wildcards: `*` matches one word, `#` matches zero or more words.
