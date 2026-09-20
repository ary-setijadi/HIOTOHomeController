# Controller (V4m) vs HIOTO App — Comparison

**HIOTO app:** `main` (56 MB statically-linked ARM Go "Hioto Worker") @ `192.168.1.22:8000`, SQLite `AppData.db`
**V4m controller:** `controller-v4m` (14.8 MB ARM Go) @ `192.168.137.44:8081`, SQLite `v4m.db`

> Architectural framing: HIOTO `main` is an **edge worker inside a larger cloud-connected
> platform** (Backend Global + `hioto-rmq.pptik.id` + Firebase + mobile apps). The V4m
> controller is a **self-contained local replacement**.

---

## 1. Side-by-side

| Dimension | HIOTO app (192.168.1.22) | V4m controller (192.168.137.44) |
|---|---|---|
| **Role** | Edge worker + local API + cloud sync | Local rule engine + device mgmt + telemetry |
| **Language / size** | Go, 56 MB statically linked | Go, 14.8 MB (cross-compiled ARMv7) |
| **HTTP API** | :8000 — Fiber + **JWT (RS256)** + WebSocket `/wrapper` | :8081 — plaintext, no auth |
| **Broker** | RabbitMQ local (5672/1883) **+ cloud** `hioto-rmq.pptik.id` **+ SmartParking** `rmq2.pptik.id` | RabbitMQ local (5672/1883) only |
| **AMQP exchanges** | `amq.direct`, `amq.topic` | `home.automation` (topic) |
| **Routing keys** | `Control`, `Register_request/response`, `Update_device/response`, `Delete_device/response`, `Rules_response`, `Floor_sync`, `Monitoring` | (uses MQTT topics as routing keys; no dedicated control/register keys) |
| **MQTT topics** | `Sensor`, `Aktuator`, `Status`, `Log/#`, `sensor_suhu/#`, `sensor_water_tank/#`, `sensor_gas_detector/#`, `sensor_weather/#`, `smart_bell/#` | Same HIOTO topics **+** V4 `home/<kind>/<type>/<serial>/<class>` |
| **Device registration** | MQTT `Register_request.<MAC>` (unauthenticated) | HTTP `/api/register` + QR |
| **Rule engine** | `rule_devices` equality only (switch state → lamp) | equality **+ threshold/hysteresis/debounce + trigger** modes; multi-output merge |
| **Database** | `AppData.db` — **18 tables** | `v4m.db` — 4 tables |
| **Cloud sync** | ✅ (`sync_states` cursors) | ❌ local-only |
| **Push notifications** | ✅ Firebase FCM (bell) | ❌ |
| **Camera storage** | ✅ `camera_captures` (1.4 M rows) + image proxy | ❌ |
| **Floors/rooms** | ✅ 4 floors, 12 rooms + positions | ❌ (schema column exists, unused) |
| **Alerting** | ✅ `alert_rules` (threshold + cooldown + message) | schema only (0 rows) |
| **Typed telemetry logs** | ✅ per-category (`log_temperatures`, `log_water_tanks`, …) | single generic `logs` table |
| **Retention** | ✅ 60-day auto-purge | ❌ no purge |
| **Heartbeat/offline** | ✅ `last_seen` + `status_device` | partial (`touchDevice` on message) |

---

## 2. What HIOTO has that V4m does **not**

1. **Cloud bridge** — dual RMQ instances (`Hioto_Local`/`Hioto_Cloud`) with `sync_states`
   cursors; two-way registration/update/delete sync to `hioto-rmq.pptik.id`.
2. **Firebase FCM** — bell press pushes a notification to 15 registered `fcm_tokens` (the
   Firebase service account + `hioto-bell` project are configured).
3. **Firestore + Google Cloud Storage** — camera image metadata/objects (gRPC + googleapis
   are linked into the binary).
4. **JWT (RS256) API auth** — public key from the Backend Global; WebSocket `/wrapper`.
5. **Floors/rooms** — 4 floors + 12 rooms, with `x_position`/`y_position` per device.
6. **Per-category telemetry tables** — `log_temperatures`, `log_water_tanks`,
   `log_gas_detectors`, `log_master_relays`, `log_smart_plugs`, `log_aktuators`,
   `log_dispensers`, `monitoring_histories` (each with category-specific columns).
7. **Camera pipeline** — 1.4 M capture rows + `imageproxy.smartsystem.id` URL builder.
8. **Alert rules** — `alert_rules` (operator/threshold/cooldown/message/`last_triggered_at`).
9. **Log retention** — `LOG_RETENTION_DAYS=60` auto-purge.
10. **SmartParking** — a separate worker connection to `rmq2.pptik.id`.

## 3. What V4m does **better / differently**

1. **Local-only, no external dependency** — no cloud/Firebase/gRPC; survives total WAN
   outage (HIOTO's worker degrades when the cloud is unreachable).
2. **~4× smaller binary** (14.8 MB vs 56 MB) — no gRPC/Google SDK/Firebase baggage.
3. **QR onboarding** — scan-to-enroll (HIOTO enrolls via unauthenticated MQTT `Register_request`).
4. **Richer rule engine** — hysteresis, min-duration/debounce, and `trigger` (edge) rules,
   not just equality mappings.
5. **Multi-output rule merge** — one switch state → many lamps is one rule with many actions
   (fixed during the v4m2 import).
6. **2-bit switch support** — `00/01/10/11` parsed as binary 0–3 (matches the real HIOTO data).
7. **Structured V4 topic convention** — `home/<kind>/<type>/<serial>/<class>` alongside the
   flat HIOTO topics.
8. **Bulk import + clean-all** endpoints for migration.
9. **Operational hardening** — QoS 0 (no durable queues), transient command delivery,
   non-durable/exclusive queue, latest-wins rate limiter (stability fixes the 512 MB Pi needed).

## 4. Bottom line

V4m covers HIOTO's **core loop** (device registry + rule engine + telemetry ingest + control
+ local SQLite) faithfully — it even imports the real HIOTO device/rule data and matches its
2-bit switch semantics. It is deliberately **smaller and local-only**.

To reach **full HIOTO parity**, V4m still needs: cloud sync, FCM push, camera/image storage,
floors/rooms UI, per-category telemetry tables, alert rules + notifications, log retention,
and JWT/WebSocket API auth.
