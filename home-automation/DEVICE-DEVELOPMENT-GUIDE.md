# Device Development Guide — Plaintext & TLS

This guide is for developers building a **device** or a **datacenter agent** that
connects to the home-automation controller. It covers **both** connectivity
paths: the legacy **plaintext** path and the **TLS** path. The application
protocol (MQTT topics, payloads, registration) is identical for both — only the
transport and authentication differ.

> Controller internals: `v4m/controller/README.md`. System architecture & V5
> security roadmap: `TECHNICAL-DOCUMENTATION.md`.

---

## 1. Choosing a path

| | Plaintext | TLS |
|---|---|---|
| MQTT port | **1883** | **8883** |
| AMQP port | **5672** | **5671** |
| When to use | legacy fixed-firmware devices; trusted LAN; fast prototyping | new devices; the WAN-facing **agent**; production |
| Authentication | username/password only | username/password **and/or** client certificate (mTLS) |
| Data | not encrypted | encrypted |

**Rule of thumb:** anything that stays on the local LAN may use plaintext;
anything that crosses a WAN (e.g. the datacenter agent) **must** use TLS.

> **Deployment status (2026-09-23):** AMQP TLS on **5671** is **live**. MQTT TLS
> on **8883** is **NOT enabled** — on RabbitMQ 3.8.3 a TLS MQTT client triggers
> `rabbit_mqtt_processor:initial_state {error,einval}` and disrupts the MQTT
> plugin, so legacy devices stay on plaintext **1883**. See
> `VERSION.md` → *Operational notes*. The datacenter agent (AMQP) is the current
> TLS requirement, so only 5671 is exposed.

```
Device (MQTT)  ──1883 (plaintext)──┐
                                   ├──► RabbitMQ (home.automation exchange)
Agent  (AMQP)  ──5671 (TLS)────────┘        ▲
Controller      ──5672 (AMQP, localhost)
```

---

## 2. The protocol (identical for both paths)

### 2.1 MQTT topics

| Direction | Topic | Payload | Notes |
|---|---|---|---|
| Publish | `Sensor` | `guid#value` | switch / DI state |
| Publish | `Status` | `guid#1` | heartbeat |
| Publish | `sensor_suhu/<guid>` | JSON | temperature/humidity |
| Publish | `sensor_water_tank/<guid>` | JSON | water level |
| Publish | `sensor_gas_detector/<guid>` | JSON | gas |
| Publish | `sensor_weather/<guid>` | JSON | weather |
| Publish | `Log/<guid>` | JSON | smart plug/relay power |
| Publish | `smart_bell/<guid>` | (event) | doorbell |
| Subscribe | `Aktuator` | `guid#value` | **actuator commands** |
| Subscribe | `<own guid>` | `guid#value` | direct per-device commands (some devices) |

AMQP routing keys are the same strings with `.` instead of `/`
(`sensor_suhu.<guid>`), on the **topic** exchange `home.automation`.

### 2.2 Wire format

- **Actuator command / switch state**: plain `guid#value`, e.g. `5a119b76-…#0`.
- **Temperature/humidity**:
  ```json
  {"guid":"c2d78875-…","deviceName":"HIOTO-WEATHERSTATION-DHT11-01",
   "value":{"temperature":30.2,"humidity":39},
   "unit":{"temperature":"C","humidity":"%"}}
  ```
- **Smart plug/relay power**:
  ```json
  {"guid":"d66d2421-…","mac":"C4:5B:BE:73:E1:C9","deviceName":"HIOTO-SMARTRELAY-01",
   "status":1,"condition":"mati",
   "value":{"voltage":181.2,"current":1.3,"power":228.8,"energy":150.8,"frequency":50,"pf":100}}
  ```

### 2.3 Two conventions (critical)

- **Lamps/relays are ACTIVE-LOW**: command `0` = ON, `1` = OFF.
- **2-bit switches** publish `00/01/10/11` → parsed as **binary 0/1/2/3**.

---

## 3. Registration & onboarding

Register the device in the controller (not auto-discovered):

```http
POST /api/register            (or POST :8000/api/device)
Content-Type: application/json
```
```json
{"guid":"my-device-001","mac":"AA:BB:CC:DD:EE:FF","type":"SENSOR",
 "name":"My Device","version":"1.0","minor":"0","quantity":1,
 "room_id":6,"floor_id":3,"x_position":0,"y_position":0,"status_device":"0"}
```

Then issue the credentials (see §4) and connect. The dashboard **Devices** tab
also supports registration with a QR scan.

---

## 4. Authentication

### 4.1 Plaintext (1883 / 5672)

One username/password. Legacy devices share `smarthome`; new devices should get a
**per-device user** (better isolation even without TLS):

```bash
rabbitmqctl add_user device-001 'StrongPass!'
rabbitmqctl set_permissions -p /smarthome device-001 '.*' '.*' '.*'
rabbitmqctl set_topic_permissions -p /smarthome device-001 \
  '^(Sensor|Status|sensor_.*|Log\..*|smart_bell.*)$' '^Aktuator(\..*)?$'
```

### 4.2 TLS (AMQP 5671 — MQTT 8883 deferred)

Same as plaintext, **plus** transport security:

> **Note:** this section documents the *intended* mTLS design. On the current
> broker (RabbitMQ 3.8.3) only the **AMQP 5671** TLS listener is usable; the MQTT
> **8883** listener crashes on client connect (`{error,einval}`) and is disabled.
> Use 5671 for the datacenter agent; keep legacy devices on 1883 until a broker
> upgrade fixes MQTT TLS.

1. Client **trusts the CA** (`ca.crt`) and verifies the broker hostname
   (`maincontroller` / `192.168.1.22`).
2. Client **presents a client cert** (mTLS) — optional; if omitted, fall back to
   username/password.

Issue a client cert on the broker:

```bash
cd /etc/rabbitmq/certs
NAME=my-device-001
openssl req -new -newkey rsa:2048 -nodes -keyout "$NAME.key" -out "$NAME.csr" -subj "/CN=$NAME"
printf 'extendedKeyUsage=clientAuth\n' > ext.cnf
openssl x509 -req -in "$NAME.csr" -CA ca.crt -CAkey ca.key -CAcreateserial -out "$NAME.crt" -days 3650 -extfile ext.cnf
```

Ship **`$NAME.crt` + `$NAME.key`** (private) + **`ca.crt`** (public) to the device.
Never share `ca.key`.

| Identity | topic write | topic read | Notes |
|---|---|---|---|
| `smarthome` | — | — | legacy shared user |
| `agent` | `^Aktuator(\..*)?$` | `.*` | datacenter agent |
| `device-<id>` | `^(Sensor|Status|sensor_.*|Log\..*|smart_bell.*)$` | `^Aktuator(\..*)?$` | per-device (recommended) |

---

## 5. MQTT code examples

### 5.1 Plaintext (Python, paho-mqtt)

```python
import paho.mqtt.client as mqtt
GUID, BROKER = "my-device-001", "192.168.1.22"

c = mqtt.Client(client_id=GUID)
c.username_pw_set("device-001", "StrongPass!")
c.connect(BROKER, 1883, keepalive=60)

c.publish("Sensor", f"{GUID}#1")          # publish state
c.subscribe("Aktuator")                    # receive commands
c.loop_forever()
```

### 5.2 TLS (Python, paho-mqtt + mTLS)

```python
import ssl, paho.mqtt.client as mqtt
GUID, BROKER = "my-device-001", "192.168.1.22"

c = mqtt.Client(client_id=GUID)
c.username_pw_set("device-001", "StrongPass!")
c.tls_set(ca_certs="/etc/devices/ca.crt",
          certfile="/etc/devices/my-device-001.crt",
          keyfile="/etc/devices/my-device-001.key",
          tls_version=ssl.PROTOCOL_TLS)
c.connect(BROKER, 8883, keepalive=60)
# ... same publish/subscribe as above
```

**ESP32/ESP8266 note:** use `WiFiClientSecure`/ESP-IDF TLS. ESP32 does full-chain
verification fine; ESP8266 should verify by **fingerprint** to save RAM.

---

## 6. AMQP agent examples (datacenter bridge)

Exchange `home.automation` (topic). Publish commands to routing key `Aktuator`;
consume telemetry by binding an exclusive queue to `Sensor`, `Status`, `sensor_*.#`,
`Log.#`, etc.

### 6.1 Plaintext (Go, amqp091-go)

```go
import amqp "github.com/rabbitmq/amqp091-go"

conn, _ := amqp.Dial("amqp://device-001:StrongPass%21@192.168.1.22:5672/%2Fsmarthome")
ch, _ := conn.Channel()
_ = ch.Publish("home.automation", "Aktuator", false, false,
    amqp.Publishing{ContentType: "text/plain", Body: []byte(guid + "#0")})
```

### 6.2 TLS (Go, amqp091-go + mTLS)

```go
import ("crypto/tls"; "crypto/x509"; "os"; amqp "github.com/rabbitmq/amqp091-go")

ca, _ := os.ReadFile("/etc/devices/ca.crt")
pool := x509.NewCertPool(); pool.AppendCertsFromPEM(ca)
cert, _ := tls.LoadX509KeyPair("/etc/devices/agent.crt", "/etc/devices/agent.key")
cfg := &tls.Config{RootCAs: pool, Certificates: []tls.Certificate{cert}, ServerName: "maincontroller"}

conn, _ := amqp.DialTLS("amqps://agent:Agent%2123@192.168.1.22:5671/%2Fsmarthome", cfg)
```

> `!` in a password must be percent-encoded (`%21`) in an AMQP URL.

---

## 7. Security checklist

**Baseline (both paths):**
- [ ] Unique RabbitMQ user (not the shared `smarthome`).
- [ ] Publishes only its own topics; subscribes only to `Aktuator` (+ own GUID).
- [ ] Reconnects with backoff (no tight loop).
- [ ] Secrets in secure storage, not hardcoded/source control.

**TLS path (additional):**
- [ ] Uses `8883`/`5671`, never plaintext for WAN.
- [ ] Loads `ca.crt` and verifies broker cert + hostname.
- [ ] (mTLS) presents its own client cert; key kept private.

**Plaintext path (mitigations):**
- [ ] Restrict to a trusted LAN/VLAN segment.
- [ ] Prefer a per-device user even without encryption.

---

## 8. Testing

```bash
# TLS handshake + cert check
openssl s_client -connect 192.168.1.22:8883 -CAfile ca.crt \
  -cert device.crt -key device.key -verify_return_error < /dev/null
openssl s_client -connect 192.168.1.22:5671 -CAfile ca.crt < /dev/null

# Plaintext connectivity
nc -zv 192.168.1.22 1883   # MQTT
nc -zv 192.168.1.22 5672   # AMQP
```

Confirm in RabbitMQ (`rabbitmqctl list_connections`) and in the controller
(`GET /api/telemetry`).

---

*Target state: plaintext `1883`/`5672` (legacy) alongside TLS `8883`/`5671`
(new devices + agent), CA `HomeAutomation-CA`, scoped per-device/agent users.*
