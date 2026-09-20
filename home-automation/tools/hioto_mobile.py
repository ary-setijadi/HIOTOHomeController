import sqlite3, json

path = "/home/orangepi/hioto/AppData.db"
db = sqlite3.connect("file:%s?mode=ro" % path, uri=True)
db.row_factory = sqlite3.Row
cur = db.cursor()

for t in ["fcm_tokens", "sync_states"]:
    cur.execute('SELECT * FROM "%s"' % t)
    rows = [dict(r) for r in cur.fetchall()]
    print("== %s (%d) ==" % (t, len(rows)))
    for r in rows:
        # redact the fcm token itself
        d = dict(r)
        if "token" in d and d["token"]:
            d["token"] = d["token"][:12] + "..."
        print(json.dumps(d, default=str))
db.close()
