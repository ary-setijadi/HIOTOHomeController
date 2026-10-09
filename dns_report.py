#!/usr/bin/env python3
"""DNS query-per-client report, cross-referenced against known devices.

Reads Pi-hole's FTL database (which already logs every query + client MAC/vendor),
then categorizes each client using /etc/hioto/devices.json.

Categories: trusted | infrastructure | controller | device | unknown

Usage:
  dns_report.py                  -> JSON report to stdout
  dns_report.py --text           -> human-readable summary
"""
import sqlite3, json, os, sys, time

FTL_DB = '/etc/pihole/pihole-FTL.db'
DEVICES = '/etc/hioto/devices.json'
ACTIVE_WINDOW_S = 15 * 60          # client is "active" if queried within this
DOMAIN_HOURS = 24                  # window for top-domain aggregation
TOP_N = 8


def load_devices():
    if os.path.isfile(DEVICES):
        with open(DEVICES, 'r', encoding='utf-8') as f:
            return json.load(f)
    return {}


def get_clients(db):
    cur = db.execute("""
        SELECT n.hwaddr, n.macVendor, n.numQueries, n.lastQuery, na.ip, na.name
        FROM network n
        LEFT JOIN network_addresses na ON na.network_id = n.id
        WHERE n.hwaddr NOT LIKE 'ip-%'
        ORDER BY n.numQueries DESC
    """)
    return cur.fetchall()


def top_domains(db, ip, limit=TOP_N, hours=DOMAIN_HOURS):
    cutoff = time.time() - hours * 3600
    cur = db.execute("""
        SELECT domain, COUNT(*) c FROM queries
        WHERE client = ? AND timestamp > ?
        GROUP BY domain ORDER BY c DESC LIMIT ?
    """, (ip, cutoff, limit))
    return [(d, c) for d, c in cur.fetchall()]


def build_report():
    db = sqlite3.connect('file:%s?mode=ro' % FTL_DB, uri=True)
    devices = load_devices()
    now = time.time()
    clients = []
    for hwaddr, vendor, numq, lastq, ip, name in get_clients(db):
        ip = ip or '?'
        entry = devices.get(hwaddr, {})
        category = entry.get('category', 'unknown')
        dname = entry.get('name') or name or vendor or hwaddr
        clients.append({
            'ip': ip,
            'mac': hwaddr,
            'vendor': vendor,
            'name': dname,
            'category': category,
            'type': entry.get('type', 'host'),
            'note': entry.get('note', ''),
            'num_queries': numq,
            'last_query': lastq,
            'active': bool(lastq and (now - lastq) < ACTIVE_WINDOW_S),
            'top_domains': top_domains(db, ip),
        })
    db.close()
    return {'generated_at': now, 'active_window_s': ACTIVE_WINDOW_S, 'clients': clients}


def main():
    report = build_report()
    if '--text' in sys.argv:
        print('%-16s %-20s %-14s %-10s %6s  %s' % ('IP', 'NAME', 'CATEGORY', 'ACTIVE', 'QUERIES', 'MAC'))
        for c in report['clients']:
            print('%-16s %-20s %-14s %-10s %6d  %s' % (
                c['ip'], c['name'][:20], c['category'], 'yes' if c['active'] else 'no',
                c['num_queries'], c['mac']))
        print('\n--- top domains per client (24h) ---')
        for c in report['clients']:
            print('== %s (%s) [%s]' % (c['ip'], c['name'], c['category']))
            for d, n in c['top_domains']:
                print('    %5d  %s' % (n, d))
    else:
        print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
