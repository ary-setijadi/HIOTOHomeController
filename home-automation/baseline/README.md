# Baseline Orange Pi Zero — flash + RabbitMQ

A **standalone** Orange Pi Zero: Armbian trixie on microSD with RabbitMQ only.
No TLS/stunnel, no controller, no coupling to the primary (`192.168.1.22`) or the
stand-by (`192.168.1.23`) — assign a role later.

Built 2026-10-14.

## As-built state

| | |
|---|---|
| Board | Xunlong Orange Pi Zero (H2+/H3, armv7l, 484 MB RAM) |
| OS | Armbian community 26.11.0-trunk.83 **trixie**, kernel 6.18.55-current-sunxi |
| Image | `Armbian_community_26.11.0-trunk.83_Orangepizero_trixie_current_6.18.55_minimal.img` |
| **Address** | **`192.168.137.200`** (static, survives reboot) + a DHCP lease as secondary |
| mDNS | `orangepizero.local` (systemd-resolved) |
| Broker | **RabbitMQ 4.0.5** on Erlang/OTP 27.3.4.1 — `active`, `enabled` |
| Listeners | MQTT **1883**, AMQP **5672**, management **15672**, dist **25672** |
| Vhost | `/smarthome` |
| Users | `smarthome`, `agent` (+ local-only `guest`); passwords defined in `provision_rmq_baseline.sh` |
| MQTT mapping | `mqtt.exchange = home.automation` (matches the fleet convention) |
| Tuning | `vm_memory_high_watermark.relative=0.5`, `default_queue_type=classic`, `+S 2:2 +A 4` |

Verified after a full reboot: service active, all listeners up, 0 failed units.

> Note the broker version differs from the fleet: primary runs **3.8.3**, stand-by
> **3.10.8**, this board **4.0.5**. Armbian no longer ships a bookworm build for
> this SoC, and trixie is the only current option, so 4.x was unavoidable.

> **Address is now stable.** The board holds a **static `192.168.137.200/24`** that
> survives reboots, plus a DHCP lease as a *secondary* address, so it still works if
> moved to another network. Reachable at that address and as `orangepizero.local`.
> (With DHCP alone the address moved `.149 → .21 → .11 → .166 → .98 → .3` within one
> session — that churn is why the static address exists.)

## Scripts in this directory

| File | Purpose |
|---|---|
| `flash_sd.ps1` | Raw-write the image to the card + **two** full read-back verifications |
| `check_card.ps1` | Counterfeit/addressability test across the whole reported capacity |
| `diagnose_flash.py` | Read-only triage when verification fails: stability + diff vs image |
| `recheck_card.py` | Repeated read passes to separate a flaky read path from bad writes |
| `repair_chunks.ps1` | Rewrites only the mismatching 1 MiB chunks, then re-verifies |
| `provision_rmq_baseline.sh` | Runs **on the board**: installs + configures RabbitMQ |
| `deploy_rmq.py` | SSH bootstrap (default creds → known password) + upload/run provisioning |
| `run_provision.py` | Runs provisioning **detached** (`setsid nohup`) and follows the log |
| `find_board.py` | Scans subnets for SSH; identifies the fresh board via default login |

## Procedure

```powershell
# 1. check the card is genuine (before it holds anything you care about)
Set-ExecutionPolicy -Scope Process Bypass -Force
.\check_card.ps1 -DiskNumber 1

# 2. flash + verify twice
.\flash_sd.ps1 -ImagePath <armbian>.img -DiskNumber 1

# 3. card -> board, cable board Ethernet to the PC, ENABLE ICS, power on
#    (see "Gotchas" 4 and 2)

# 4. find it
python find_board.py --subnets 192.168.137.0/24 --probe-default-login

# 5. bootstrap + provision
python run_provision.py <ip>          # detached; survives disconnects
```

## Gotchas — each of these cost real time

### 1. Windows silently overwrote the flashed image
The card still carried a **live, whole-disk FAT32 volume** from a Windows format.
On removable media Windows refuses **both** `Set-Disk -IsOffline`
(*"Removable media cannot be set to offline"*) **and** `Remove-Partition`
(*"Not supported"*), so the volume stayed mounted. The raw write appeared to
succeed, then Windows flushed its dirty FAT32 metadata back over the start of the
card — boot sector at 0 MiB plus FAT tables at 2/3/4/5 MiB. Read-back showed
exactly those 5 chunks mismatching, and further raw writes were **denied outright**
(*"Access to the path is denied"*).

Fix: `Clear-Disk` → `diskpart clean` → rescan → **assert no volume remains** →
prove write access with a probe, *then* write. `flash_sd.ps1` does all of this.

### 2. A naive write-then-read-back check CANNOT detect a fake card
A wrapped write and a wrapped read both land on the same real address, so they
agree with each other and the check passes. `check_card.ps1` therefore **writes
every sample block first, then reads them all back** — aliasing makes the later
write destroy an earlier block, which then fails.

### 3. Verify the whole region, and verify it TWICE
The original `flash_image.ps1` compared only the first and last 1 MiB. Hash the
entire written region instead — and repeat the pass after a rescan + delay, since
the corruption in gotcha 1 only appeared *after* the write finished.

### 4. The BootROM scans the card once, at power-on
The board sat in **Allwinner FEL mode** (`USB\VID_1F3A&PID_EFE8`) — the BootROM's
last-resort USB recovery state — purely because the microSD went in *after* it was
powered. Nothing was wrong with the card or the image. **Insert the card, then
power on.** A 5 V/2 A supply is safer than PC USB power (weak power causes
brown-out boot failures on these boards).

### 5. Python cannot write to a raw Windows device
`open(r'\\.\PhysicalDrive1','r+b')` fails with `OSError: [Errno 9] Bad file
descriptor` on write, though reading works fine. Use .NET `FileStream`
(`flash_sd.ps1`, `repair_chunks.ps1`) for writing.

### 6. PowerShell: never `Write-Output` inside a function that returns a value
Functions return *everything* written to the pipeline, so logging with
`Write-Output` polluted a returned hash and made a **passing** verification look
like a failure. Log with `[Console]::WriteLine` instead.

### 7. Streamed remote commands die with the client
Streaming a long `exec_command` means a client crash kills the remote script
mid-`dpkg`. That happened here and left `rabbitmq-server` half-configured. Use
`setsid nohup ... > log 2>&1` and poll the log (`run_provision.py`). Also force
UTF-8 on the client: remote output contains characters cp1252 cannot encode, and
a `UnicodeEncodeError` will kill a provision run.

### 8. Armbian's `log2ram` wipes `/var/log/rabbitmq` on every boot
The systemd unit's `StandardOutput=append:/var/log/rabbitmq/...` then fails and
the service crash-loops. `provision_rmq_baseline.sh` installs a
`/etc/tmpfiles.d/rabbitmq-server.conf` to recreate the directory at boot —
**validated by an actual reboot.**

### 9. MQTT returned "not authorized" even with the right password
MQTT CONNECT gave **rc=5** (not rc=4, which would mean a bad password): authentication
succeeded but authorization failed. The MQTT plugin authenticates against a **vhost**
(default `/`), and our users are only granted on `/smarthome`.

The 3.x setting name `mqtt.default_vhost` is **invalid** in RabbitMQ 4.x and makes the
broker fail to start (`failed_to_prepare_configuration`, crash-restart loop). The correct
key is **`mqtt.vhost`**. Verify setting names against the installed plugin schema rather
than guessing:
`/usr/lib/rabbitmq/lib/rabbitmq_server-*/plugins/rabbitmq_mqtt-*/priv/schema/rabbitmq_mqtt.schema`

Also note RabbitMQ 4.x MQTT takes the **plain username** (`smarthome`); the 3.8
`vhost:user` form documented for the fleet is rejected here.

### 10. netplan renumbers its generated files, so a `90-*.yaml` lost
A dedicated `/etc/netplan/90-end0-static-address.yaml` was **silently ignored**. netplan
generates `10-netplan-*.network` regardless of the source filename's number, and
systemd-networkd applied only the wildcard — `journalctl -u systemd-networkd` showed
*"end0: Configuring with /run/systemd/network/10-netplan-all-eth-interfaces.network"*.

Fix: put the override in the wildcard file (`10-dhcp-all-interfaces.yaml`) itself, and
apply the address live with `ip addr replace` rather than `netplan apply` — `netplan
apply` re-runs DHCP, which moved the board to a new lease mid-verification.

## Cross-workspace reference

Credentials, endpoints, ready-to-paste connection strings and caveats are published for
other workspaces at **`D:\DSHWork\ORANGEPI-BROKER.md`**.

## Deferred (not installed here)

MQTT-TLS `8883`, AMQP-TLS `5671`, client certificates, and `controller-v4m`.
Re-run `home-automation/standby/provision.sh` once this board's role is decided.
The management UI's `guest` account is localhost-only; create a real admin before
exposing `15672`.
