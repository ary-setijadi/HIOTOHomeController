#!/usr/bin/env python3
"""Publish a firmware blob for a specific device type and mark it latest.

Usage: publish_firmware.py <file> --device <type> --version 1.2.3 [--base-url http://192.168.1.99:8080] [--note "..."]
"""
import os, json, hashlib, shutil, argparse
from datetime import datetime, timezone

ROOT = '/srv/ota'


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(65536), b''):
            h.update(chunk)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('file')
    ap.add_argument('--device', required=True)
    ap.add_argument('--version', required=True)
    ap.add_argument('--base-url', default='http://192.168.1.99:8080')
    ap.add_argument('--note', default='')
    args = ap.parse_args()

    device = args.device.strip().lower()
    firm_dir = os.path.join(ROOT, device, 'firmware')
    manifest_path = os.path.join(ROOT, device, 'latest.json')
    os.makedirs(firm_dir, exist_ok=True)

    src = args.file
    name = os.path.basename(src)
    dst = os.path.join(firm_dir, name)
    shutil.copyfile(src, dst)

    size = os.path.getsize(dst)
    digest = sha256_file(dst)
    manifest = {
        'device_type': device,
        'latest_version': args.version,
        'filename': name,
        'url': args.base_url.rstrip('/') + '/firmware/' + device + '/' + name,
        'size': size,
        'sha256': digest,
        'released_at': datetime.now(timezone.utc).isoformat(),
        'note': args.note,
    }
    with open(manifest_path, 'w', encoding='utf-8') as f:
        json.dump(manifest, f, indent=2)
    print('Published [%s] %s v%s (%d bytes, sha256 %s)' % (device, name, args.version, size, digest))
    print('Manifest ->', manifest_path)


if __name__ == '__main__':
    main()
