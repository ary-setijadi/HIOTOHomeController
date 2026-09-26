# Datacenter Agent (agent-v4m)

Off-site client for the home-automation broker. It connects to the **local**
broker over **AMQP + mutual TLS (5671)**, mirrors every message on the
`home.automation` exchange to a rolling JSONL store (off-site backup / remote
monitoring), and can publish actuator commands (remote control).

This is the "cloud aggregation" half of the architecture in
`../TECHNICAL-DOCUMENTATION.md` §12.5 — the broker stays local; the agent is a
remote *client*, not the broker.

## What it does

- Connects to `amqps://<broker>:5671` with a client cert (mTLS) + agent creds.
- Binds `#` on `home.automation` (full mirror), consumes, and appends each message
  to `messages-YYYY-MM-DD.jsonl` in the output dir.
- Retains the last N days (default 30), deletes older files on startup.
- Reconnects automatically (5 s retry) if the broker/network drops.
- One-shot remote control: `agent-v4m -publish Aktuator -publish-body "<guid>#0"`.

## Build

Cross-compile for the datacenter host (usually linux/amd64):

```powershell
cd home-automation/agent
$env:GOOS='linux'; $env:GOARCH='amd64'
go build -o agent-v4m .
```

(For a test on the Orange Pi itself: `$env:GOARCH='arm'; $env:GOARM='7'`.)

## Credentials & certs (copy from the home broker)

The agent needs three files from `/etc/rabbitmq/certs/` on the controller:

```
ca.crt       # HomeAutomation-CA (public)
agent.crt    # client cert, CN=agent (public)
agent.key    # client private key (KEEP PRIVATE)
```

Plus the RabbitMQ user `agent` / password `Agent!23` (vhost `/smarthome`,
topic write `Aktuator`, read `.*`).

## Run

```bash
./agent-v4m \
  -broker-host <HOME_IP_OR_HOST> -port 5671 -vhost /smarthome \
  -user agent -password 'Agent!23' \
  -ca ./ca.crt -cert ./agent.crt -key ./agent.key \
  -server-name maincontroller \
  -out-dir /var/lib/agent-v4m -retention-days 30
```

`-broker-host` is the address the *datacenter* uses to reach the home broker:
the home's public IP (5671 port-forwarded) or a VPN/WireGuard peer address.
`-server-name` must stay `maincontroller` (matches the broker cert SAN) even when
the TCP destination is a public IP.

## Remote control (one-shot)

```bash
# turn a lamp ON (active-low: 0 = ON)
./agent-v4m -broker-host ... -ca ... -cert ... -key ... \
  -publish Aktuator -publish-body '<guid>#0'
```

## systemd (datacenter)

See `agent-v4m.service`.

## Notes

- The mirror is the **event stream** (device messages). Full DB snapshots
  (registrations/rules/timers) are handled separately by the standby-board sync
  (rsync), not by this agent.
- Disk: daily JSONL files, bounded by `-retention-days`.
