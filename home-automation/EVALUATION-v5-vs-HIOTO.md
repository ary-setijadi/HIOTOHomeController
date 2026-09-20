# HomeAutomation v5.0 vs HIOTO — Evaluation

**Date:** 2026-09-19
**Subject:** HomeAutomation v5.0 (built on Orange Pi Zero, RabbitMQ 3.8.3, Go controller, Node simulator/UI)
**Reference:** HIOTO edge gateway design document (reconstructed)

---

## 0. Framing — two different maturity levels

| | HIOTO | HomeAutomation v5.0 |
|---|---|---|
| Purpose | Production edge gateway → cloud | Security-focused local smart-home demo |
| Devices | 46 physical ESP8266/32 (8 categories) | 26 simulated (2 categories) |
| Registry | 138 rows in SQLite | JSON registry (device-manager) |
| Persistence | 573 MB SQLite, 18 tables | JSON + rolling NDJSON file (no relational DB) |
| Cloud | AMQP/MQTT upstream + sync cursors | None (local only) |

"Evaluate against" therefore means: **how does v5.0 cover HIOTO's responsibilities, where does it exceed it, and what is missing to reach parity.**

---

## 1. Responsibility coverage matrix

| HIOTO responsibility | v5.0 status | Assessment |
|---|---|---|
| Device registration & lifecycle | ✅ **Strong** | `device-manager`: register → issue cert (CN=serial) → QR → enroll → revoke. Better onboarding UX than HIOTO (which registers via unauthenticated `Register_request.<MAC>` MQTT). |
| Local automation rule execution | ✅ | `controller-v5`: level + trigger rules, priority, hysteresis, debounce, time windows. Comparable to `rule_devices`, but **no separate alert rules** (§4 gap 5). |
| Sensor telemetry ingestion | ✅ | sensors → MQTTS → controller + traffic-logger. |
| Telemetry storage | ⚠️ **Partial** | `traffic-logger` rolling 100 MB file. **No structured `log_*` tables, no per-metric columns, no query/aggregation.** |
| Cloud sync | ❌ **Missing** | no cloud AMQP/MQTT, no `sync_states` cursors. |
| Device control | ✅ | controller → `cmd` (digital/analog), manual override. |
| Security/safety (camera, lock, siren, gas) | ❌ **Missing** | no camera/lock/siren/gas device classes, no camera-capture pipeline. |
| Push notifications (bell → FCM) | ❌ **Missing** | no Firebase/FCM. |
| Management API + WebSocket | ⚠️ **Partial** | REST-ish APIs (Node + Go), **SSE instead of WebSocket**, **mTLS instead of JWT**, no floors/rooms. |

**Scorecard: 4 of 8 fully covered, 2 partial, 2 missing.**

---

## 2. Component mapping

| HIOTO component | v5.0 equivalent | Parity |
|---|---|---|
| `hioto` main (Go, Fiber, JWT, WS) | `controller-v5` (Go, `net/http`) + `server.js` (Node dashboard) | ⚠️ |
| `hioto-logger` (cloud sync) | `traffic-logger` (local file + query API) | ⚠️ (local only) |
| RabbitMQ 3.5.7 | RabbitMQ 3.8.3 | ✅ (newer) |
| SQLite `AppData.db` (18 tables) | JSON registry + NDJSON log | ❌ (no relational DB) |
| *(none — no security layer)* | **mTLS + per-device certs + device-manager + registration app** | ✅ **v5.0 extra** |

---

## 3. Security — where v5.0 exceeds HIOTO

This is the decisive differentiator. HIOTO's §9 documents its own weaknesses; v5.0 was built specifically to fix exactly that class of problem.

| Area | HIOTO (as-built) | HomeAutomation v5.0 |
|---|---|---|
| Transport | plaintext MQTT:1883 + AMQP:5672 | **mTLS** AMQPS:5671 + MQTTS:8883; plaintext loopback-only |
| Client auth | username/password in `.env` | **per-device client cert (CN=serial)** + `verify_peer` + `fail_if_no_peer_cert` + password as 2nd factor |
| API auth | JWT RS256, but GET list endpoints **unauthenticated** | mTLS on data plane; dashboard on localhost; enroll **token-gated** |
| Onboarding | MQTT `Register_request.<MAC>` (no strong auth) | **QR + token + CA-signed cert issuance** |
| Demonstrable proof | — | **impostor test** shows no-cert/forged-cert rejection |

v5.0 directly closes HIOTO's top security findings (#2 unauthenticated enumeration, plaintext transport). HIOTO's remaining findings that v5.0 **also** still has are called out in §5.

---

## 4. Gaps vs HIOTO (prioritized)

1. **Structured persistence (SQLite).** HIOTO: floors/rooms/registrations/rules/`log_*`/`camera_captures`/`fcm_tokens`/`sync_states` (18 tables). v5.0: JSON registry + flat NDJSON. No relational querying, no aggregation over time, no historical analytics.
2. **Floors/rooms hierarchy + x/y positions.** v5.0 is a flat device list.
3. **Cloud sync.** HIOTO bridges to `hioto-rmq.pptik.id` with `sync_states` cursors. v5.0 is intentionally local-only.
4. **Device categories.** HIOTO: cameras, door locks, sirens, gas detectors, smart bells, master relays (power metering), water tanks, weather. v5.0: only sensor/actuator × digital/analog.
5. **Alert rules.** HIOTO `alert_rules`: threshold + operator + cooldown + message + `last_triggered_at`. v5.0 folds thresholds into the rule engine — no alerting, no cooldown, no notification channel.
6. **Heartbeat/offline detection.** HIOTO updates `registrations.last_seen` + `status_device` on `Status`. v5.0 has no liveness/offline tracking.
7. **WebSocket + JWT API.** HIOTO uses Fiber + JWT(RS256) + WebSocket `/wrapper`. v5.0 uses SSE (fine for one-way dashboards) + mTLS + basic auth; no JWT, no bidirectional WS.

---

## 5. Shared weaknesses (both systems)

1. **No AMQP reconnect.** HIOTO §11.1 explicitly lists this; v5.0's `controller-v5` has the same pattern (`amqp.DialTLS` + `log.Fatalf`, recovered via `systemd Restart=always`). Both should add an in-process reconnect loop.
2. **Plaintext credentials.** HIOTO `.env` (world-readable); v5.0 `admin`/`123456Aa!` hardcoded in `lib.js`/controller flags. v5.0 mitigates via certs, but the shared password is still a weak second factor.
3. **Single point of failure.** HIOTO: one Xiaomi AP for 46 devices. v5.0: broker + controller + logger + device-manager all on one 512 MB Pi — which already showed overload / reconnect-storm fragility during this build.

---

## 6. v5.0 strengths to retain

1. **mTLS with per-device certificates** — the core thesis, cleanly implemented and observable.
2. **Hierarchical topic convention** `home/<kind>/<type>/<serial>/<class>` — more structured than HIOTO's flat `Sensor`/`Aktuator`/`Log.#` topics.
3. **Uniform message envelope** `{msg_id, ts, source, message_class, payload}` — HIOTO payloads are less consistent.
4. **Observability tooling** — traffic feed, security/connection table, impostor test, history-from-file. HIOTO has no equivalent self-inspection.
5. **Provisioning UX** — QR + camera-scan enrollment (HIOTO has none).

---

## 7. Roadmap to close the gap (in order)

1. Add **SQLite** persistence (registrations, rules, structured `log_*` tables with per-metric columns).
2. Add **floors/rooms** model.
3. Add **device classes**: camera, door lock, siren, gas detector, smart bell, master relay (power metering).
4. Add **alert rules** (threshold + cooldown + notification).
5. Add **heartbeat / offline detection** (`last_seen`).
6. Add **cloud sync** (upstream AMQP/MQTT + `sync_states` cursors) — only if cloud integration is wanted.
7. Add **AMQP reconnect** to controller / traffic-logger / device-manager.
8. (Optional) **WebSocket + JWT** management API; **FCM** push.

---

## Verdict

HomeAutomation v5.0 is a **smaller, local-only system that is decisively more secure than HIOTO** — it nails mutual-TLS device authentication, which HIOTO entirely lacks — and covers the *core* loop (device lifecycle + rule engine + telemetry ingest + control) well. It is **not yet a parity replacement for HIOTO**: the main functional gaps are structured persistence (SQLite/floors-rooms), the richer device catalog (cameras/locks/sirens/gas/bell), alerting/notification, cloud sync, and operational hardening (AMQP reconnect, heartbeat).
