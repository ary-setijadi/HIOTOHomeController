'use strict';

const video = document.getElementById('video');
const canvas = document.getElementById('canvas');
const result = document.getElementById('result');
const rescanBtn = document.getElementById('btn-rescan');
const ctx = canvas.getContext('2d', { willReadFrequently: true });

let scanning = true;
let lastData = '';

function setResult(text, cls) {
  result.textContent = text;
  result.className = cls || '';
}

async function start() {
  try {
    const stream = await navigator.mediaDevices.getUserMedia({ video: { facingMode: 'environment' } });
    video.srcObject = stream;
    await video.play();
    requestAnimationFrame(tick);
  } catch (e) {
    setResult('Camera error: ' + e.message + '\n\nAllow camera access and serve this page over https or localhost.', 'err');
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
  if (!info.enroll || !info.serial) { setResult('QR is missing enroll/serial fields:\n' + text, 'err'); return; }
  setResult('Detected device "' + info.serial + '". Enrolling…');
  scanning = false;
  try {
    const r = await fetch('/api/enroll', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ enroll: info.enroll, serial: info.serial }),
    });
    const d = await r.json();
    if (d.ok) {
      setResult('✓ Registered device "' + d.serial + '"\n  broker: ' + d.broker + '\n  certs saved to v5.0/certs/' + d.serial + '.{crt,key}', 'ok');
    } else {
      setResult('✗ Enrollment failed: ' + (d.error || 'unknown'), 'err');
    }
  } catch (e) {
    setResult('✗ Enrollment failed: ' + e.message, 'err');
  }
  rescanBtn.style.display = 'inline-block';
}

rescanBtn.onclick = () => {
  lastData = '';
  scanning = true;
  rescanBtn.style.display = 'none';
  setResult('Point the camera at a device QR code…');
  requestAnimationFrame(tick);
};

start();
