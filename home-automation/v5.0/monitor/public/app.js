'use strict';

const status = document.getElementById('status');
const secTotal = document.getElementById('secTotal');
const secTls = document.getElementById('secTls');
const secAllTls = document.getElementById('secAllTls');
const msgTotal = document.getElementById('msgTotal');
const connBody = document.querySelector('#connTable tbody');
const msgBody = document.querySelector('#msgTable tbody');
const flowCounts = document.getElementById('flowCounts');
const impostorBtn = document.getElementById('impostorBtn');
const impostorOut = document.getElementById('impostorOut');

const MAX_ROWS = 80;
const FLOW_COLORS = {
  'dev-ctrl': '#0e7c3a',   // sensor -> controller
  'ctrl-dev': '#b45309',   // controller -> device
  'dev-ui':   '#1d4ed8',   // actuator -> ui
  'ui-ctrl':  '#7c3aed',   // ui -> controller
  'other':    '#6b7280',
};

function fmtPayload(p) {
  if (!p) return '';
  if (Array.isArray(p.digital_value)) return 'digital=' + JSON.stringify(p.digital_value);
  if (Array.isArray(p.analog_value)) return 'analog=' + JSON.stringify(p.analog_value);
  if (typeof p === 'object') return JSON.stringify(p);
  return String(p);
}

function esc(s) {
  return String(s == null ? '' : s).replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
}

function addMessage(rec) {
  const tr = document.createElement('tr');
  const time = (rec.t || '').slice(11, 23);
  tr.innerHTML =
    `<td class="mono">${esc(time)}</td>` +
    `<td><span class="pill" style="background:${FLOW_COLORS[rec.tag] || FLOW_COLORS.other}">${esc(rec.flow)}</span></td>` +
    `<td class="mono">${esc(rec.topic)}</td>` +
    `<td>${esc(rec.source)}</td>` +
    `<td>${esc(rec.class)}</td>` +
    `<td class="mono">${esc(fmtPayload(rec.payload))}</td>`;
  msgBody.prepend(tr);
  while (msgBody.children.length > MAX_ROWS) msgBody.removeChild(msgBody.lastChild);
}

function renderCounts(counts) {
  msgTotal.textContent = counts.total;
  const flows = Object.entries(counts.flows || {});
  if (!flows.length) { flowCounts.textContent = 'no messages yet'; return; }
  flowCounts.innerHTML = '';
  flows.forEach(([flow, n]) => {
    const s = document.createElement('span');
    s.className = 'flowchip';
    s.textContent = `${flow}: ${n}`;
    flowCounts.appendChild(s);
  });
}

function renderSecurity(sec) {
  secTotal.textContent = sec.total;
  secTls.textContent = sec.tls;
  secAllTls.textContent = sec.allTls ? 'YES' : 'NO';
  secAllTls.style.color = sec.allTls ? '#16a34a' : '#dc2626';
  connBody.innerHTML = '';
  sec.certs.forEach((c) => {
    const tr = document.createElement('tr');
    tr.innerHTML =
      `<td>${esc(c.protocol)}</td>` +
      `<td>${c.ssl ? '<span class="ok">● TLS</span>' : '<span class="bad">✗ plaintext</span>'}</td>` +
      `<td class="mono">${esc(c.cn)}</td>` +
      `<td class="mono">${esc(c.name)}</td>`;
    connBody.appendChild(tr);
  });
}

function refreshSecurity() {
  fetch('/api/security').then((r) => r.json()).then(renderSecurity).catch(() => {});
}

impostorBtn.addEventListener('click', () => {
  impostorBtn.disabled = true;
  impostorOut.textContent = 'running impostor test…';
  fetch('/api/impostor', { method: 'POST' })
    .then((r) => r.json())
    .then((d) => {
      if (!d.ok) { impostorOut.textContent = 'error: ' + (d.error || 'unknown'); return; }
      impostorOut.textContent = d.results.map((r) => `${r.attempt}\n   -> ${r.result}`).join('\n\n');
    })
    .catch((e) => { impostorOut.textContent = 'error: ' + e.message; })
    .finally(() => { impostorBtn.disabled = false; });
});

const es = new EventSource('/events');
es.onmessage = (ev) => {
  const d = JSON.parse(ev.data);
  if (d.type === 'status') status.textContent = d.connected ? 'monitor connected (MQTTS, monitor cert)' : 'disconnected';
  if (d.type === 'hello') {
    status.textContent = d.connected ? 'monitor connected (MQTTS, monitor cert)' : 'disconnected';
    renderCounts(d.counts);
    (d.messages || []).slice(0, MAX_ROWS).reverse().forEach(addMessage);
  }
  if (d.type === 'message') {
    addMessage(d.rec);
    renderCounts(d.counts);
  }
};

refreshSecurity();
setInterval(refreshSecurity, 5000);
