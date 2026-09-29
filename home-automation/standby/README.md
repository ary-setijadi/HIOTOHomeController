# Stand-by Controller — Runbook

Turn a second board into a **warm stand-by** of the primary controller. It holds
the same software, certificates, and state, so if the primary dies (SD card,
board, power) you can fail over in minutes without re-registering the 135-device
fleet or re-importing rules.

```
PRIMARY  (192.168.1.22)  ◄── devices / tablet / datacenter agent
STAND-BY (idle, provisioned, controller stopped)
            └── on failover: takes 192.168.1.22, starts the controller
```

## What's in this directory

| File | Purpose |
|---|---|
| `backup.sh` | run on the **primary** — snapshots state into a tarball |
| `provision.sh` | run on the **stand-by** — installs + restores + configures |
| `rabbitmq.conf` | broker config (fallback reference) |
| `controller-v4m.service` | systemd unit (fallback reference) |

## Prerequisites

- Stand-by board: clean Armbian, **same architecture as the primary** (armv7 for
  an Orange Pi Zero). If it's 64-bit, see the note in §Notes.
- Root SSH access to both boards.
- The stand-by must eventually be able to **take over IP `192.168.1.22`** (set a
  static IP or DHCP reservation at failover time).

## Step 1 — Back up the primary

On the primary (as root):

```bash
bash backup.sh /root/standby-backup.tar.gz
# then copy it off:
scp /root/standby-backup.tar.gz root@<standby-ip>:/root/
```

The tarball contains: SQLite DB + telemetry, `/etc/rabbitmq/` (config + full
certs incl. `ca.key`), `/etc/stunnel/`, the controller binary, and the systemd
unit.

## Step 2 — Provision the stand-by

On the stand-by (as root):

```bash
bash provision.sh /root/standby-backup.tar.gz
```

`provision.sh` does, in order:

1. **Reset** — stops and wipes any existing RabbitMQ/homeautomation/stunnel state.
2. **Hostname** — sets `maincontroller` + `/etc/hosts` (avoids the Erlang
   hostname-resolution failure).
3. **Install** — `rabbitmq-server` + `stunnel4`.
4. **Restore** — unpacks the backup (config, certs, DB, binary).
5. **Broker** — enables `rabbitmq_mqtt` + `rabbitmq_management`, starts it, and
   recreates the vhost `/smarthome`, users `smarthome`/`agent`, and topic perms.
6. **stunnel** — MQTT TLS terminator on `8883` (mTLS).
7. **Controller** — installs the unit but keeps it **disabled** (stand-by mode).

Expected end state:

```
rabbitmq:   active
stunnel:    active
controller: disabled   (start on failover)
```

## Step 3 — Smoke test the stand-by (without failover)

On the stand-by, confirm it's ready but idle:

```bash
rabbitmqctl list_vhosts                       # should show / and /smarthome
rabbitmqctl list_users                        # should show smarthome, agent
ss -ltn | grep -E '1883|5671|5672|8883'       # listeners up
openssl s_client -connect 127.0.0.1:8883 -CAfile /etc/rabbitmq/certs/ca.crt \
  -cert /etc/rabbitmq/certs/agent.crt -key /etc/rabbitmq/certs/agent.key \
  -verify_return_error < /dev/null | grep 'Verify return code'
```

> Do **not** start the stand-by's controller during normal operation — it would
> connect to the primary and duplicate rule execution. It must stay disabled
> until failover.

## Step 4 — Fail over (when the primary is down)

1. Remove/shut down the primary (or it has already died).
2. Give the stand-by **`192.168.1.22`** (static IP, or move the DHCP reservation,
   then reboot the stand-by / restart RabbitMQ so it re-binds).
3. On the stand-by:
   ```bash
   systemctl enable --now controller-v4m
   ```
4. Verify: `systemctl is-active controller-v4m`, then open the dashboard at
   `http://192.168.1.22:8081/dashboard/`. The device gateway reconnects to
   `192.168.1.22` automatically (devices already auto-reconnect after broker
   restarts).

## Notes

- **Architecture**: if the stand-by is 64-bit (arm64), replace the restored
  `/root/controller-v4m` (armv7, from the backup) with an arm64 build:
  `GOOS=linux GOARCH=arm64 go build -o controller-v4m .` in `v4m/controller/`.
- **Certificates**: the stand-by reuses the primary's CA + server cert, so
  devices and the datacenter agent verify it identically. Keep `ca.key` private.
- **Telemetry**: the backup includes the telemetry rollover files; on a slow link
  you can exclude `/var/lib/homeautomation/telemetry/` (it's regenerable history).
- **Ongoing sync**: for a fresher stand-by, run `backup.sh` on the primary on a
  cron schedule and re-run `provision.sh` (or just `tar -xzf` the backup) on the
  stand-by.
