'use strict';

const video = document.getElementById('video');
const canvas = document.getElementById('canvas');
const result = document.getElementById('result');
const rescanBtn = document.getElementById('btn-rescan');
const ctx = canvas.getContext('2d', { willReadFrequently: true });

let scanning = true;
let lastData = '';

function setResult(text, cls) { result.textContent = text; result.className = cls || ''; }

async function start() {
  try {
    const stream = await navigator.mediaDevices.getUserMedia({ video: { facingMode: 'environment' } });
    video.srcObject = stream;
    await video.play();
    requestAnimationFrame(tick);
  } catch (e) {
    setResult('Camera error: ' + e.message, 'err');
  }
}

function tick() {
  if (video.readyState === video.HAVE_ENOUGH_DATA) {
    canvas.width = video.videoWidth;
    canvas.height = video.videoHeight;
    ctx.drawImage(video, 0, 0, canvas.width, canvas.height);
    const img = ctx.getImageData(0, 0, canvas.width, canvas.height);
    const qr = jsQR(img.data, img.width, img.height, { inversionAttempts: 'dontInvert' });
    if (qr && qr.data && scanning && qr.data !== lastData) {
      lastData = qr.data;
      onScan(qr.data);
    }
  }
  if (scanning) requestAnimationFrame(tick);
}

async function onScan(text) {
  let info;
  try { info = JSON.parse(text); } catch { setResult('Scanned QR is not valid JSON:\n' + text, 'err'); return; }
  if (!info.guid) { setResult('QR is missing a guid:\n' + text, 'err'); return; }
  setResult('Detected device "' + info.guid + '". Saving config…');
  scanning = false;
  try {
    const r = await fetch('/api/config', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(info) });
    const d = await r.json();
    if (d.ok) setResult('✓ Configured device "' + d.guid + '"\n  broker: ' + d.broker + '\n  config saved to v4m/simulator/devices/' + d.guid + '.json', 'ok');
    else setResult('✗ Failed: ' + (d.error || 'unknown'), 'err');
  } catch (e) { setResult('✗ Failed: ' + e.message, 'err'); }
  rescanBtn.style.display = 'inline-block';
}

rescanBtn.onclick = () => { lastData = ''; scanning = true; rescanBtn.style.display = 'none'; setResult('Point the camera at a device QR code…'); requestAnimationFrame(tick); };

start();
