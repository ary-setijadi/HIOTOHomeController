import sqlite3, json, sys

path = "/home/orangepi/hioto/AppData.db"
db = sqlite3.connect("file:%s?mode=ro" % path, uri=True)
db.row_factory = sqlite3.Row
cur = db.cursor()

def dump(table, cols):
    cur.execute('SELECT %s FROM "%s"' % (",".join(cols), table))
    return [dict(r) for r in cur.fetchall()]

out = {
    "registrations": dump("registrations",
        ["guid","mac","type","quantity","name","version","minor","status",
         "status_device","last_seen","room_id","floor_id","x_position","y_position","category"]),
    "rule_devices": dump("rule_devices",
        ["input_guid","input_value","output_guid","output_value"]),
    "rooms": dump("rooms", ["id","name","floor_id"]),
    "floors": dump("floors", ["id","name"]),
}
db.close()

with open("/tmp/hioto_export.json", "w") as f:
    json.dump(out, f, indent=2, default=str)

print("registrations=%d rule_devices=%d rooms=%d floors=%d" % (
    len(out["registrations"]), len(out["rule_devices"]),
    len(out["rooms"]), len(out["floors"])))
