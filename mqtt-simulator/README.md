# MQTT IoT Device Simulator

A Node.js simulator for IoT devices that connects to an MQTT broker (RabbitMQ's
MQTT plugin at `192.168.137.44:1883` by default), **publishes periodic telemetry**
and **subscribes to command topics**.

## Web UI (dashboard)

```powershell
npm.cmd run ui        # or: node server.js
```

Then open **http://127.0.0.1:3000** in your browser.

The dashboard lets you:

- connect to the broker (with credentials)
- subscribe to topics and watch messages stream in live
- publish to any topic
- start / stop the virtual-device simulator (count, interval, topic templates)

The server bridges the browser to MQTT over Server-Sent Events and auto-connects
on startup using the `BROKER` / `MQTT_USER` / `MQTT_PASS` env vars
(defaults: `mqtt://192.168.137.44:1883`, `admin`, `123456Aa!`).

## 1. Install

```powershell
cd mqtt-simulator
npm.cmd install
```

## 2. Run the simulator

```powershell
node simulator.js
```

It will publish a JSON telemetry reading every 5 s to `iot/sim-001/telemetry`
and subscribe to `iot/sim-001/commands`.

### Options

| Flag | Env var | Default | Description |
|---|---|---|---|
| `--broker <url>` | `BROKER` | `mqtt://192.168.137.44:1883` | Broker URL |
| `--device <id>` | `DEVICE_ID` | `sim-001` | Device ID |
| `--count <n>` | `COUNT` | `1` | Number of devices to simulate |
| `--telemetry <topic>` | `TELEMETRY` | `iot/{id}/telemetry` | Telemetry topic template |
| `--command <topic>` | `COMMAND` | `iot/{id}/commands` | Command topic template |
| `--status <topic>` | `STATUS` | `iot/{id}/status` | Status (LWT) topic |
| `--interval <sec>` | `INTERVAL` | `5` | Publish interval |
| `--username <u>` | `MQTT_USER` | `admin` | MQTT username |
| `--password <p>` | `MQTT_PASS` | `123456Aa!` | MQTT password |

Examples:

```powershell
node simulator.js --device pump-01 --interval 2
node simulator.js --count 5
node simulator.js --telemetry 'factory/{id}/sensors' --command 'factory/{id}/cmd'
node simulator.js --username admin --password 123456Aa!
```

## 3. Topics

- `iot/<deviceId>/telemetry` — device → broker (sensor readings)
- `iot/<deviceId>/status` — device → broker (online/offline, retained)
- `iot/<deviceId>/commands` — broker → device (incoming commands)

## 4. Manually publish / subscribe (bundled `mqtt` CLI)

The `mqtt` package ships a CLI. From this folder:

```powershell
# Subscribe to all devices' telemetry
npx mqtt sub -h 192.168.137.44 -p 1883 -u admin -P '123456Aa!' -t 'iot/+/telemetry' -v

# Publish a command to a device
npx mqtt pub -h 192.168.137.44 -p 1883 -u admin -P '123456Aa!' -t 'iot/sim-001/commands' -m '{"command":"reboot"}'
```

Run `npx mqtt --help` for the full CLI reference.

> **Note:** remote MQTT clients must authenticate. Anonymous connections map to
> RabbitMQ's `guest` user, which is localhost-only, so they are rejected with
> "Not authorized" unless you configure `mqtt.allow_anonymous`/`mqtt.default_user`.

