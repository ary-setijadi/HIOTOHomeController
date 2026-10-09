#!/usr/bin/env python3
"""HiotoDNSServer monitor HTTP server (:8081).

Endpoints:
  GET  /api/dns-clients   -> DNS query-per-client report (xref + category)
  GET  /api/events?limit  -> recent monitor events (JSONL tail)
  POST /api/event         -> append a monitor event
  GET  /                  -> help
"""
import os, sys, json, time
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

sys.path.insert(0, '/usr/local/bin')
import dns_report  # noqa: E402

PORT = 8081
BIND = '0.0.0.0'
EVENTS = '/srv/monitor/events.log'


def now_iso():
    return datetime.now(timezone.utc).isoformat()


def tail_events(limit):
    out = []
    if os.path.isfile(EVENTS):
        with open(EVENTS, 'r', encoding='utf-8') as f:
            lines = f.readlines()
        for ln in lines[-limit:]:
            ln = ln.strip()
            if ln:
                try:
                    out.append(json.loads(ln))
                except Exception:
                    pass
    return out


class H(BaseHTTPRequestHandler):
    server_version = 'HDSMonitor/1.0'

    def _json(self, code, obj):
        b = json.dumps(obj).encode('utf-8')
        self.send_response(code)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(b)))
        self.end_headers()
        self.wfile.write(b)

    def do_GET(self):
        parsed = urlparse(self.path)
        path = parsed.path
        qs = parse_qs(parsed.query)

        if path in ('/', '/index.html'):
            dash = '/usr/local/bin/dashboard.html'
            if os.path.isfile(dash):
                with open(dash, 'rb') as f:
                    body = f.read()
                self.send_response(200)
                self.send_header('Content-Type', 'text/html; charset=utf-8')
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            else:
                self._json(200, {'service': 'hds-monitor', 'endpoints': [
                    '/api/dns-clients', '/api/events?limit=100']})
            return

        if path == '/api/dns-clients':
            self._json(200, dns_report.build_report())
            return

        if path == '/api/events':
            try:
                limit = int((qs.get('limit') or ['100'])[0])
            except ValueError:
                limit = 100
            limit = max(1, min(limit, 1000))
            self._json(200, {'count': len(tail_events(limit)), 'events': tail_events(limit)})
            return

        self._json(404, {'error': 'not found'})

    def do_POST(self):
        path = urlparse(self.path).path
        if path == '/api/event':
            try:
                length = int(self.headers.get('Content-Length') or 0)
            except ValueError:
                length = 0
            data = {}
            if length > 0:
                try:
                    data = json.loads(self.rfile.read(length).decode('utf-8'))
                except Exception:
                    data = {}
            if not isinstance(data, dict):
                data = {}
            ev = {'time': now_iso(), 'ts': time.time(), 'ip': self.client_address[0]}
            ev.update(data)
            os.makedirs('/srv/monitor', exist_ok=True)
            with open(EVENTS, 'a', encoding='utf-8') as f:
                f.write(json.dumps(ev) + '\n')
            self._json(200, {'ok': True, 'event': ev})
            return
        self._json(404, {'error': 'not found'})

    def log_message(self, fmt, *args):
        print('[monitor] %s - %s' % (self.address_string(), fmt % args))


if __name__ == '__main__':
    os.makedirs('/srv/monitor', exist_ok=True)
    srv = ThreadingHTTPServer((BIND, PORT), H)
    print('monitor server listening on %s:%d' % (BIND, PORT))
    srv.serve_forever()
