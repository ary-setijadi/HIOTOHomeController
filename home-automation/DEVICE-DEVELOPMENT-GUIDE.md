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

> **Deployment status (2026-09-26):** AMQP TLS on **5671** is **live** (datacenter
> agent). MQTT TLS on **8883** is served by a **stunnel TLS terminator** that
> fronts RabbitMQ 3.8.3 — its own MQTT-TLS listener crashes on connect — so
> TLS-capable devices (ESP32) can migrate gradually while legacy devices stay on
> plaintext **1883**. See §4.2 and `v4m/controller/README.md` → *TLS terminator*.

```
Device (MQTT)  ──1883 (plaintext)──────────────────┐
Device (MQTT)  ──8883 (TLS)──► stunnel ──1883 ─────┤► RabbitMQ (home.automation)
Agent  (AMQP)  ──5671 (TLS)────────────────────────┘
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

> **MQTT username = `vhost:user`** (RabbitMQ 3.8.3 quirk — the *vhost comes first*,
> then the user). To reach vhost `/smarthome` as user `smarthome`, set the MQTT
> username to **`/smarthome:smarthome`**. (AMQP uses the normal `user`/`vhost`
> fields, not this form.)

```bash
rabbitmqctl add_user device-001 'StrongPass!'
rabbitmqctl set_permissions -p /smarthome device-001 '.*' '.*' '.*'
rabbitmqctl set_topic_permissions -p /smarthome device-001 \
  '^(Sensor|Status|sensor_.*|Log\..*|smart_bell.*)$' '^Aktuator(\..*)?$'
```

### 4.2 TLS (AMQP 5671 + MQTT 8883 via stunnel)

Two TLS paths, both from the same CA (`HomeAutomation-CA`):

| Path | Port | Served by | For |
|---|---|---|---|
| AMQP | **5671** | RabbitMQ native TLS | datacenter agent |
| MQTT | **8883** | **stunnel** → `127.0.0.1:1883` | ESP32 / new devices (mTLS) |

RabbitMQ 3.8.3's own MQTT-TLS listener crashes on connect, so MQTT TLS is
terminated by **stunnel** (a TLS wrapper) in front of the broker. stunnel
decrypts the MQTT stream and forwards it to the broker's plaintext `1883` on
loopback. Old devices keep talking to `1883` directly — the two coexist, so you
can migrate one device at a time.

**stunnel config** (`/etc/stunnel/mqtt-tls.conf`):

```ini
[mqtt-tls]
client  = no
accept  = 8883
connect = 127.0.0.1:1883
cert    = /etc/stunnel/certs/server.crt      # copy of rabbitmq-server.crt
key     = /etc/stunnel/certs/server.key
CAfile  = /etc/stunnel/certs/ca.crt
verify  = 2   # 2 = require client cert (mTLS); 0 = server-only TLS
```

**Connection parameters** (identical for plaintext and TLS, except the port):

- Broker host = `192.168.1.22` (single-board); for large/multi-subnet deployments
  use the DNS name **`mqtt.home.arpa`** — see `TECHNICAL-DOCUMENTATION.md` §12.5.
- MQTT username = **`/smarthome:smarthome`** (`vhost:user` — see §4.1).
- MQTT password = `Ssm4rt2!`.
- TLS: trust `ca.crt` and verify `CN=maincontroller` (SAN includes `192.168.1.22`);
  with `verify=2`, also present the device client cert.

**Issue a client cert** for each device (script in repo at `tools/gen-device-cert.sh`):

```bash
/usr/local/bin/gen-device-cert.sh ESP32-KITCHEN-01   # → ESP32-KITCHEN-01.crt/.key
```

Ship **`<name>.crt` + `<name>.key`** (private) + **`ca.crt`** (public) to the device.
Never share `ca.key`.

| Identity | topic write | topic read | Notes |
|---|---|---|---|
| `smarthome` | — | — | legacy shared user (vhost `/smarthome`) |
| `agent` | `^Aktuator(\..*)?$` | `.*` | datacenter agent (AMQP 5671) |
| `device-<id>` | `^(Sensor|Status|sensor_.*|Log\..*|smart_bell.*)$` | `^Aktuator(\..*)?$` | per-device (recommended) |

---

## 5. MQTT code examples

### 5.1 Plaintext (Python, paho-mqtt)

```python
import paho.mqtt.client as mqtt
GUID, BROKER = "my-device-001", "192.168.1.22"

c = mqtt.Client(client_id=GUID)
c.username_pw_set("/smarthome:device-001", "StrongPass!")   # MQTT username is vhost:user
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
c.username_pw_set("/smarthome:device-001", "StrongPass!")   # MQTT username is vhost:user
c.tls_set(ca_certs="/etc/devices/ca.crt",
          certfile="/etc/devices/my-device-001.crt",
          keyfile="/etc/devices/my-device-001.key",
          tls_version=ssl.PROTOCOL_TLS)
c.connect(BROKER, 8883, keepalive=60)
# ... same publish/subscribe as above
```

### 5.3 ESP32 (Arduino, WiFiClientSecure + PubSubClient)

```cpp
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>

const char* MQTT_HOST = "192.168.1.22";
const uint16_t MQTT_PORT = 8883;
const char* MQTT_USER = "/smarthome:smarthome";   // vhost:user (see §4.1)
const char* MQTT_PASS = "Ssm4rt2!";
const char* CLIENT_ID = "ESP32-DEMO-01";

// Embed the three PEMs emitted by gen-device-cert.sh.
static const char* CA_CERT = R"EOF(
-----BEGIN CERTIFICATE-----
... contents of ca.crt ...
-----END CERTIFICATE-----
)EOF";
static const char* CLIENT_CERT = R"EOF(
-----BEGIN CERTIFICATE-----
... contents of ESP32-DEMO-01.crt ...
-----END CERTIFICATE-----
)EOF";
static const char* CLIENT_KEY = R"EOF(
-----BEGIN PRIVATE KEY-----
... contents of ESP32-DEMO-01.key ...
-----END PRIVATE KEY-----
)EOF";

WiFiClientSecure net;
PubSubClient mqtt(net);

void connectMQTT() {
  net.setCACert(CA_CERT);              // verify broker against HomeAutomation-CA
  net.setCertificate(CLIENT_CERT);     // present device cert (mTLS)
  net.setPrivateKey(CLIENT_KEY);
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  while (!mqtt.connect(CLIENT_ID, MQTT_USER, MQTT_PASS)) {
    Serial.println("mqtt connect failed; retrying");
    delay(3000);
  }
  mqtt.subscribe("Aktuator");          // receive actuator commands
}

void publishState(int v) {
  char buf[64];
  snprintf(buf, sizeof buf, "%s#%d", CLIENT_ID, v);
  mqtt.publish("Sensor", buf);         // publish switch/sensor state
}
```

> **ESP8266 note:** same pattern, but verify by **fingerprint** instead of a full
> CA chain to save RAM.

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
# TLS handshake + cert check (mTLS)
openssl s_client -connect 192.168.1.22:8883 -CAfile ca.crt \
  -cert device.crt -key device.key -verify_return_error < /dev/null
openssl s_client -connect 192.168.1.22:5671 -CAfile ca.crt < /dev/null

# Plaintext connectivity
nc -zv 192.168.1.22 1883   # MQTT (legacy)
nc -zv 192.168.1.22 5672   # AMQP
```

End-to-end MQTT over TLS (publish via 8883 → receive on 1883):

```bash
# subscriber on the plaintext side
mosquitto_sub -h 127.0.0.1 -p 1883 -u '/smarthome:smarthome' -P 'Ssm4rt2!' -t 'test/tls' -C 1 &
sleep 1
# publisher through stunnel (TLS + mTLS)
mosquitto_pub -h 127.0.0.1 -p 8883 --cafile ca.crt --cert ESP32-DEMO-01.crt --key ESP32-DEMO-01.key \
  -u '/smarthome:smarthome' -P 'Ssm4rt2!' -t 'test/tls' -m 'hello'
```

Confirm in RabbitMQ (`rabbitmqctl list_connections`) and in the controller
(`GET /api/telemetry`).

---

*Target state: plaintext `1883`/`5672` (legacy) alongside TLS `8883`/`5671`
(new ESP32 devices + datacenter agent), CA `HomeAutomation-CA`, mTLS via the
stunnel terminator, scoped per-device/agent users.*
