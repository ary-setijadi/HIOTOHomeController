#!/usr/bin/env python3
"""Ask HIOTO switch panels to check the OTA server now.

Publishes {"ota":1} to iot/<device-id>/cmd on the HIOTO broker, which the
device picks up on its command channel — no cable, no BOOT button. Same effect
as `ota` on the serial console, so a rollout does not have to wait for the
15-minute automatic check.

Usage:
    python ota_trigger.py                     # whole registered fleet
    python ota_trigger.py hioto-genmon-3d42cc # one board
    python ota_trigger.py --list              # just show the default fleet
"""
import sys
import time

import paho.mqtt.client as mqtt

BROKER = "192.168.1.22"
PORT = 1883
USER = "/smarthome:smarthome"
PASS = "Ssm4rt2!"

# Device ids as recorded in firmware/PROVISIONED-UNITS.md
FLEET = [
    "hioto-genmon-3d42cc",  # unit 1 - 2.8"
    "hioto-genmon-70b134",  # unit 2 - 2.8"
    "hioto-genmon-3c4b08",  # unit 3 - 2.8"
    "hioto-genmon-3c4b98",  # unit 4 - 2.8"
    "hioto-genmon-3c3374",  # unit 5 - 2.8"
    "hioto-genmon-ec0940",  # 3.5" board
]


def main():
    args = [a for a in sys.argv[1:] if a != "--list"]
    if "--list" in sys.argv[1:]:
        print("\n".join(FLEET))
        return
    devices = args or FLEET
    payload = '{"ota":1}'

    done = {"rc": None}

    def on_connect(c, _u, _f, rc):
        done["rc"] = rc
        if rc != 0:
            return
        for dev in devices:
            info = c.publish(f"iot/{dev}/cmd", payload, qos=0)
            print(f"[mqtt] {payload} -> iot/{dev}/cmd rc={info.rc}")

    c = mqtt.Client(client_id="ota-trigger")
    c.username_pw_set(USER, PASS)
    c.on_connect = on_connect
    c.connect(BROKER, PORT, 10)
    c.loop_start()
    for _ in range(50):
        if done["rc"] is not None:
            break
        time.sleep(0.1)
    time.sleep(0.8)
    c.loop_stop()
    c.disconnect()
    print(f"[mqtt] connected rc={done['rc']}, published {len(devices)} request(s)")


if __name__ == "__main__":
    main()
