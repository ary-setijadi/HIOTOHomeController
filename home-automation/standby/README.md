# Stand-by Controller — Runbook

Turn a second board into a **warm stand-by** of the primary controller. It holds
the same software, certificates, and state, so if the primary dies (SD card,
board, power) you can fail over in minutes without re-registering the 135-device
fleet or re-importing rules.

```
PRIMARY  192.168.1.22   ◄── devices / tablet / datacenter agent
STAND-BY 192.168.1.23   (idle, provisioned, controller stopped)
             └── on failover: takes 192.168.1.22, starts the controller
```

## What's in this directory

| File | Purpose |
|---|---|
| `backup.sh` | run on the **primary** — snapshots state into a tarball |
| `provision.sh` | run on the **stand-by** — cleans + installs + configures |
| `rabbitmq.conf` | broker config (fallback reference) |
| `controller-v4m.service` | systemd unit (fallback reference) |

## Prerequisites

- Stand-by board: clean Armbian, **same architecture as the primary** (armv7 for
  an Orange Pi Zero). If 64-bit, see the note in §Notes.
- Root SSH access to the board.
- **Controller binary**: copy `v4m/controller/controller-v4m-armv7` (or `-arm64`
  for a 64-bit board) to `/root/controller-v4m` on the stand-by — unless you are
  restoring it from the primary's backup tarball (which already contains it).

## Provisioning (two modes)

### Mode A — baseline, no primary backup (fresh certs)

Run on the stand-by (as root) with **no argument**:

```bash
bash provision.sh
```

This **cleans the board**, installs RabbitMQ + stunnel, generates a **fresh
`HomeAutomation-CA`** + server/agent certs, creates the vhost/users, and installs
the controller (disabled). The board is fully functional standalone. This is the
"get it running now" path when the primary isn't reachable.

> Note: with fresh certs the stand-by has a *different* CA than the primary. To
> become a true clone, re-run Mode B below once the primary is reachable.

### Mode B — clone the primary (same CA + state)

1. On the **primary**:
   ```bash
   bash backup.sh /root/standby-backup.tar.gz
   scp /root/standby-backup.tar.gz root@192.168.1.23:/root/
   ```
2. On the **stand-by**:
   ```bash
   bash provision.sh /root/standby-backup.tar.gz
   ```

The tarball carries: SQLite DB + telemetry, `/etc/rabbitmq/` (config + full certs
incl. `ca.key`), `/etc/stunnel/`, the controller binary, and the systemd unit.
Re-running `provision.sh` with the tarball replaces the fresh certs with the
primary's, so devices and the datacenter agent verify the stand-by identically.

## What `provision.sh` does (in order)

1. **Reset** — stops and wipes any existing RabbitMQ/homeautomation/stunnel state.
2. **Hostname** — sets `maincontroller` + `/etc/hosts`.
3. **Install** — `rabbitmq-server` + `stunnel4`.
4. **Certs/state** — restores the backup, or generates a fresh CA/certs.
5. **Broker** — enables `rabbitmq_mqtt` + `rabbitmq_management`, starts it,
   recreates vhost `/smarthome`, users `smarthome`/`agent`, and topic perms.
6. **stunnel** — MQTT TLS terminator on `8883` (mTLS).
7. **Controller** — installs the unit but keeps it **disabled** (stand-by mode).

Expected end state:

```
rabbitmq:   active
stunnel:    active
controller: disabled   (start on failover)
```

## Smoke test (stand-by alone, without failover)

```bash
rabbitmqctl list_vhosts                       # should show / and /smarthome
rabbitmqctl list_users                        # should show smarthome, agent
ss -ltn | grep -E '1883|5671|5672|8883'       # listeners up
openssl s_client -connect 127.0.0.1:8883 -CAfile /etc/rabbitmq/certs/ca.crt \
  -cert /etc/rabbitmq/certs/agent.crt -key /etc/rabbitmq/certs/agent.key \
  -verify_return_error < /dev/null | grep 'Verify return code'
```

> Do **not** start the stand-by's controller during normal operation — it would
> connect to the primary and duplicate rule execution.

## Static IP 192.168.1.23

Set the stand-by to a fixed `192.168.1.23` (or a DHCP reservation). On Armbian
with NetworkManager, e.g.:

```bash
nmcli con mod <connection-name> ipv4.method manual \
  ipv4.addresses 192.168.1.23/24 ipv4.gateway 192.168.1.1
systemctl restart NetworkManager
```

(Adjust `<connection-name>` and the gateway `192.168.1.1` to your router.)

## Fail over (when the primary is down)

1. Remove/shut down the primary (or it has already died).
2. Give the stand-by **`192.168.1.22`** (change its static IP, or move the DHCP
   reservation, then reboot / restart RabbitMQ so it re-binds).
3. On the stand-by:
   ```bash
   systemctl enable --now controller-v4m
   ```
4. Verify: `systemctl is-active controller-v4m`, then open the dashboard at
   `http://192.168.1.22:8081/dashboard/`. The device gateway reconnects to
   `192.168.1.22` automatically (devices already auto-reconnect after broker
   restarts).

## Notes

- **Architecture**: if the stand-by is 64-bit, use `controller-v4m-arm64`
  (build: `GOOS=linux GOARCH=arm64 go build -o controller-v4m .` in
  `v4m/controller/`).
- **Certificates**: the stand-by reuses the primary's CA + server cert (SAN
  `maincontroller`, `192.168.1.22`, `192.168.1.23`, `127.0.0.1`), so devices and
  the agent verify it identically. Keep `ca.key` private.
- **Telemetry**: the backup includes telemetry rollover files; on a slow link you
  can exclude `/var/lib/homeautomation/telemetry/` (it's regenerable history).
- **Ongoing sync**: run `backup.sh` on the primary on a cron schedule and re-run
  `provision.sh <tarball>` on the stand-by to keep it fresh.
