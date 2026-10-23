#!/usr/bin/env python3
"""Ask a HIOTO switch panel to check the OTA server now.

Publishes {"ota":1} to iot/<device-id>/cmd on the HIOTO broker, which the
device picks up on its command channel — no cable, no BOOT button.

Usage:
    python ota_trigger.py [device-id]
"""
import sys
import time

import paho.mqtt.client as mqtt

BROKER = "192.168.1.22"
PORT = 1883
USER = "/smarthome:smarthome"
PASS = "Ssm4rt2!"


def main():
    device = sys.argv[1] if len(sys.argv) > 1 else "hioto-genmon-ec0940"
    topic = f"iot/{device}/cmd"
    payload = '{"ota":1}'

    done = {"ok": False}

    def on_connect(c, _u, _f, rc):
        print(f"[mqtt] connected rc={rc}")
        c.publish(topic, payload, qos=0)
        done["ok"] = True

    c = mqtt.Client(client_id="ota-trigger")
    c.username_pw_set(USER, PASS)
    c.on_connect = on_connect
    c.connect(BROKER, PORT, 10)
    c.loop_start()
    for _ in range(50):
        if done["ok"]:
            break
        time.sleep(0.1)
    time.sleep(0.5)
    c.loop_stop()
    c.disconnect()
    print(f"[mqtt] published {payload} -> {topic}")


if __name__ == "__main__":
    main()
