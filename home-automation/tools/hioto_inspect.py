import sqlite3, json, sys

path = "/home/orangepi/hioto/AppData.db"
db = sqlite3.connect("file:%s?mode=ro" % path, uri=True)
cur = db.cursor()
cur.execute("SELECT name FROM sqlite_master WHERE type='table' ORDER BY name")
tables = [r[0] for r in cur.fetchall()]
print("TABLES:", json.dumps(tables))
for t in tables:
    try:
        cur.execute('SELECT COUNT(*) FROM "%s"' % t)
        n = cur.fetchone()[0]
    except Exception as e:
        n = "err:" + str(e)
    print("TABLE %s rows=%s" % (t, n))
    cur.execute("SELECT sql FROM sqlite_master WHERE name=?", (t,))
    row = cur.fetchone()
    if row and row[0]:
        print("SQL %s: %s" % (t, row[0]))
db.close()
