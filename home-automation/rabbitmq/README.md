# RabbitMQ topology-as-code

`definitions.json` is a RabbitMQ definitions export covering the
`/home-site1` vhost, the `home.automation` topic exchange, the standing
controller/monitor queues with dead-lettering, and per-kind users with topic
permissions.

## Import

```bash
# one of:
rabbitmqadmin -H 192.168.137.44 -u admin -p '123456Aa!' import definitions.json
# or via the management API: PUT /api/definitions with the file body
```

> **Before importing:** replace every `CHANGE_ME` `password_hash` with a real
> hash, OR import and then set real passwords with:
>
> ```bash
> rabbitmqctl change_password sensor.SNS-SW-0001 '<secret>'
> rabbitmqctl change_password actuator.ACT-LMP-0002 '<secret>'
> rabbitmqctl change_password controller.ctrl-0001 '<secret>'
> rabbitmqctl change_password monitor.mon-0001 '<secret>'
> ```

## MQTT plugin configuration

Add to `rabbitmq.conf` (or the `advanced.config`/`rabbitmq.conf` used by the
broker) and restart:

```ini
mqtt.exchange          = home.automation
mqtt.allow_anonymous   = false
mqtt.vhost             = /home-site1
```

MQTT topic ↔ AMQP routing-key mapping: an MQTT topic `a/b/c` maps to routing
key `a.b.c` on `mqtt.exchange`; a `+` maps to `*` and `#` maps to `#`.

## Notes

- MQTT edge users (`sensor.*`, `actuator.*`) need no AMQP `configure/write/read`
  permissions — the MQTT plugin enforces the `topic_permissions` above.
- `permissions` and `topic_permissions` here are representative per-device
  examples; generate one user + permission pair per physical device.
