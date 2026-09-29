#!/usr/bin/env python3
"""OTA firmware server v4 — device-type aware + GUID + query/download/report logging.

Device identification:
  - device type : query 'device' (e.g. ?device=esp32) or JSON body field
  - device GUID : query 'guid' / header 'X-Device-GUID' / JSON body field

Endpoints:
  GET  /api/devices                          -> list device types
  GET  /api/latest?device=<type>&guid=<id>   -> latest manifest (logged)
  GET  /api/<type>/latest?guid=<id>          -> same, path form
  GET  /firmware/<type>/<file>?guid=<id>     -> firmware download (logged)
  POST /api/report                           -> device reports post-flash result (logged)
  GET  /api/log?limit=100                    -> recent events
"""
import os, json, time
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

ROOT = '/srv/ota'
PORT = 8080
BIND = '0.0.0.0'
LOG_FILE = os.path.join(ROOT, 'ota.log')


def now_iso():
    return datetime.now(timezone.utc).isoformat()


def log_event(**kw):
    ev = {'time': now_iso(), 'ts': time.time()}
    ev.update(kw)
    try:
        with open(LOG_FILE, 'a', encoding='utf-8') as f:
            f.write(json.dumps(ev) + '\n')
    except Exception as e:
        print('[ota] log error:', e)
    return ev


def get_guid(qs, headers):
    for key in ('guid', 'id', 'device_guid', 'device_id'):
        v = qs.get(key)
        if v and v[0].strip():
            return v[0].strip()[:128]
    for hkey in ('X-Device-GUID', 'X-Device-Id', 'X-Device-ID'):
        v = headers.get(hkey)
        if v and v.strip():
            return v.strip()[:128]
    return None


def read_manifest(device):
    p = os.path.join(ROOT, device, 'latest.json')
    if os.path.isfile(p):
        with open(p, 'r', encoding='utf-8') as f:
            return f.read()
    return json.dumps({'latest_version': None, 'filename': None,
                       'message': 'No firmware published for device type %r' % device})


def list_devices():
    out = []
    if os.path.isdir(ROOT):
        for d in sorted(os.listdir(ROOT)):
            if os.path.isfile(os.path.join(ROOT, d, 'latest.json')):
                out.append(d)
    return out


def tail_log(limit):
    entries = []
    if os.path.isfile(LOG_FILE):
        with open(LOG_FILE, 'r', encoding='utf-8') as f:
            lines = f.readlines()
        for line in lines[-limit:]:
            line = line.strip()
            if line:
                try:
                    entries.append(json.loads(line))
                except Exception:
                    pass
    return entries


class Handler(BaseHTTPRequestHandler):
    server_version = 'OTAServer/4.0'

    def _json(self, code, obj):
        body = json.dumps(obj).encode('utf-8')
        self.send_response(code)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _text(self, code, s):
        body = s.encode('utf-8')
        self.send_response(code)
        self.send_header('Content-Type', 'text/plain')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _manifest(self, device):
        body = read_manifest(device).encode('utf-8')
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Cache-Control', 'no-cache')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _serve_file(self, fpath):
        size = os.path.getsize(fpath)
        self.send_response(200)
        self.send_header('Content-Type', 'application/octet-stream')
        self.send_header('Content-Length', str(size))
        self.end_headers()
        with open(fpath, 'rb') as f:
            while True:
                chunk = f.read(65536)
                if not chunk:
                    break
                self.wfile.write(chunk)

    def do_GET(self):
        parsed = urlparse(self.path)
        path = parsed.path
        qs = parse_qs(parsed.query)
        ip = self.client_address[0]
        ua = self.headers.get('User-Agent', '')
        guid = get_guid(qs, self.headers)

        if path in ('/', '/index.html'):
            self._text(200, 'OTA firmware server v4.\n'
                            '  GET  /api/devices\n'
                            '  GET  /api/latest?device=<type>&guid=<id>\n'
                            '  GET  /api/<type>/latest?guid=<id>\n'
                            '  GET  /firmware/<type>/<file>?guid=<id>\n'
                            '  POST /api/report   (JSON: device_type, guid, status, from_version, to_version, message)\n'
                            '  GET  /api/log?limit=100\n')
            return

        if path == '/api/devices':
            self._json(200, {'devices': list_devices()})
            return

        if path == '/api/log':
            try:
                limit = int((qs.get('limit') or ['100'])[0])
            except ValueError:
                limit = 100
            limit = max(1, min(limit, 1000))
            self._json(200, {'count': len(tail_log(limit)), 'events': tail_log(limit)})
            return

        if path == '/api/latest':
            device = (qs.get('device') or [''])[0].strip()
            if not device:
                log_event(event='query', device_type=None, guid=guid, ip=ip, user_agent=ua, action='list_devices')
                self._json(200, {'devices': list_devices(), 'hint': 'use /api/latest?device=<type>&guid=<id>'})
                return
            log_event(event='query', device_type=device, guid=guid, ip=ip, user_agent=ua, action='latest')
            self._manifest(device)
            return

        if path.startswith('/api/') and path.endswith('/latest'):
            device = path[len('/api/'):-len('/latest')]
            if device and '/' not in device:
                log_event(event='query', device_type=device, guid=guid, ip=ip, user_agent=ua, action='latest')
                self._manifest(device)
                return

        if path.startswith('/firmware/'):
            parts = [p for p in path.split('/') if p]
            if len(parts) == 3:
                device, name = parts[1], parts[2]
                fpath = os.path.join(ROOT, device, 'firmware', name)
                if os.path.isfile(fpath):
                    size = os.path.getsize(fpath)
                    log_event(event='download', device_type=device, guid=guid, ip=ip, user_agent=ua,
                              filename=name, size=size, action='ota_execution')
                    self._serve_file(fpath)
                else:
                    self._json(404, {'error': 'firmware not found'})
                return
            self._json(400, {'error': 'bad firmware path'})
            return

        self._json(404, {'error': 'not found'})

    def do_POST(self):
        parsed = urlparse(self.path)
        path = parsed.path
        qs = parse_qs(parsed.query)
        ip = self.client_address[0]
        ua = self.headers.get('User-Agent', '')

        if path != '/api/report':
            self._json(404, {'error': 'not found'})
            return

        # Read body (JSON expected, but tolerate empty + query params)
        data = {}
        try:
            length = int(self.headers.get('Content-Length') or 0)
        except ValueError:
            length = 0
        if length > 0:
            raw = self.rfile.read(length)
            try:
                data = json.loads(raw.decode('utf-8'))
            except Exception:
                data = {}
        if not isinstance(data, dict):
            data = {}

        def pick(*keys):
            for k in keys:
                if k in data and data[k] not in (None, ''):
                    return data[k]
            for k in keys:
                v = qs.get(k)
                if v and v[0].strip():
                    return v[0].strip()
            return None

        device_type = pick('device_type', 'device')
        guid = pick('guid', 'device_guid', 'id') or get_guid(qs, self.headers)
        status_raw = str(pick('status', 'result') or '').strip().lower()
        if status_raw in ('success', 'ok', 'okay', '1', 'true', 'passed'):
            status = 'success'
        elif status_raw in ('fail', 'failed', 'failure', 'error', '0', 'false'):
            status = 'failed'
        else:
            status = status_raw or 'unknown'

        from_version = pick('from_version', 'from')
        to_version = pick('to_version', 'to', 'version')
        message = pick('message', 'note', 'detail')

        log_event(event='report', device_type=(device_type or None), guid=guid, ip=ip, user_agent=ua,
                  status=status, from_version=from_version, to_version=to_version,
                  message=message, action='ota_report')

        self._json(200, {'ok': True, 'logged': {
            'device_type': device_type, 'guid': guid, 'status': status,
            'from_version': from_version, 'to_version': to_version,
        }})

    def log_message(self, fmt, *args):
        print('[ota] %s - %s' % (self.address_string(), fmt % args))


if __name__ == '__main__':
    os.makedirs(ROOT, exist_ok=True)
    srv = ThreadingHTTPServer((BIND, PORT), Handler)
    print('OTA server v4 listening on %s:%d' % (BIND, PORT))
    srv.serve_forever()
