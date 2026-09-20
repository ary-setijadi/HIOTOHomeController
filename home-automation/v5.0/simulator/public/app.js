'use strict';

const $ = (s) => document.querySelector(s);
const PAGE_SIZE = 8;       // devices/rules per page
const MSG_PAGE_SIZE = 15;  // traffic rows per page

let sensors = [];
let actuators = [];
let rules = [];
let messages = [];
let counts = { total: 0, flows: {} };
let historyMessages = null; // null = live view; array = history view
const state = {
  tab: 'sensors',
  sensorFilter: 'all',
  actuatorFilter: 'all',
  sensorPage: 0,
  actuatorPage: 0,
  rulePage: 0,
  msgPage: 0,
};

const FLOW_COLORS = {
  'dev-ctrl': '#0e7c3a',
  'ctrl-dev': '#b45309',
  'dev-ui':   '#1d4ed8',
  'ui-ctrl':  '#7c3aed',
  'other':    '#6b7280',
};

function post(url, body) {
  return fetch(url, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body || {}) }).then((r) => r.json());
}

function badge(value, type) {
  const b = document.createElement('span');
  if (type === 2) {
    b.className = 'state analog';
    b.textContent = value === undefined ? '—' : Number(value).toFixed(2);
  } else {
    b.className = 'state ' + (value === 1 ? 'on' : 'off');
    b.textContent = value === 1 ? 'ON' : 'OFF';
  }
  return b;
}

// --- pagination helpers ---
function filtered(list, filter) { return filter === 'all' ? list : list.filter((x) => x.type === Number(filter)); }
function pageCount(n, size) { return Math.max(1, Math.ceil(n / size)); }
function clampPage(page, pc) { return Math.max(0, Math.min(page, pc - 1)); }
function slicePage(list, page, size) { return list.slice(page * size, page * size + size); }
function updatePager(prevEl, nextEl, infoEl, page, pc) {
  infoEl.textContent = `${page + 1} / ${pc}`;
  prevEl.disabled = page === 0;
  nextEl.disabled = page >= pc - 1;
}

function renderSensors() {
  const f = filtered(sensors, state.sensorFilter);
  const pc = pageCount(f.length, PAGE_SIZE);
  state.sensorPage = clampPage(state.sensorPage, pc);
  const c = $('#sensors');
  c.innerHTML = '';
  for (const s of slicePage(f, state.sensorPage, PAGE_SIZE)) {
    const row = document.createElement('div'); row.className = 'dev-row';
    const serial = document.createElement('span'); serial.className = 'serial'; serial.textContent = s.serial;
    const t = document.createElement('span'); t.className = 'type'; t.textContent = s.type === 2 ? 'analog' : 'digital';
    row.append(serial, t, badge(s.value, s.type));
    c.appendChild(row);
  }
  updatePager($('#sensor-prev'), $('#sensor-next'), $('#sensor-page'), state.sensorPage, pc);
}

function renderActuators() {
  const f = filtered(actuators, state.actuatorFilter);
  const pc = pageCount(f.length, PAGE_SIZE);
  state.actuatorPage = clampPage(state.actuatorPage, pc);
  const c = $('#actuators');
  c.innerHTML = '';
  for (const a of slicePage(f, state.actuatorPage, PAGE_SIZE)) {
    const row = document.createElement('div'); row.className = 'dev-row';
    const serial = document.createElement('span'); serial.className = 'serial'; serial.textContent = a.serial;
    row.append(serial, badge(a.value, a.type));
    if (a.override) { const ov = document.createElement('span'); ov.className = 'ov-badge'; ov.textContent = 'OVERRIDE'; row.appendChild(ov); }
    const controls = document.createElement('div'); controls.className = 'controls';
    if (a.type === 2) {
      const input = document.createElement('input'); input.type = 'number'; input.step = '0.1'; input.min = '0'; input.max = '1'; input.value = a.value === undefined ? 0 : a.value;
      const set = document.createElement('button'); set.textContent = 'Set'; set.className = 'primary'; set.onclick = () => post('/api/override', { serial: a.serial, value: Number(input.value) });
      const rel = document.createElement('button'); rel.textContent = 'Release'; rel.onclick = () => post('/api/release', { serial: a.serial });
      controls.append(input, set, rel);
    } else {
      const on = document.createElement('button'); on.textContent = 'ON'; on.className = 'primary'; on.onclick = () => post('/api/override', { serial: a.serial, value: 1 });
      const off = document.createElement('button'); off.textContent = 'OFF'; off.className = 'danger'; off.onclick = () => post('/api/override', { serial: a.serial, value: 0 });
      const rel = document.createElement('button'); rel.textContent = 'Release'; rel.onclick = () => post('/api/release', { serial: a.serial });
      controls.append(on, off, rel);
    }
    row.appendChild(controls);
    c.appendChild(row);
  }
  updatePager($('#actuator-prev'), $('#actuator-next'), $('#actuator-page'), state.actuatorPage, pc);
}

function whenText(w) {
  if (!w) return '?';
  if (w.type === 'time') {
    if (w.at) return `⏱ at ${w.at}`;
    if (w.from || w.to) return `⏱ ${w.from || '00:00'}–${w.to || '23:59'}`;
    return '⏱ time';
  }
  let s = `${w.sensor} ${w.op} ${w.threshold}`;
  if (w.hysteresis) s += ` (±${w.hysteresis})`;
  return s;
}

function renderRules() {
  const pc = pageCount(rules.length, PAGE_SIZE);
  state.rulePage = clampPage(state.rulePage, pc);
  const c = $('#rules');
  c.innerHTML = '';
  for (const r of slicePage(rules, state.rulePage, PAGE_SIZE)) {
    const row = document.createElement('div'); row.className = 'rule-row';
    const txt = document.createElement('span'); txt.className = 'rule-text';
    const thens = (r.then || []).map((a) => `${a.actuator}=${a.value}`).join(', ');
    const elses = (r.else || []).map((a) => `${a.actuator}=${a.value}`).join(', ');
    const modeTag = r.mode === 'trigger' ? ' ⚡trigger' : '';
    txt.textContent = `${r.name} [P${r.priority || 0}]${modeTag}: IF ${whenText(r.when)} THEN ${thens || '—'}` + (elses ? ` ELSE ${elses}` : '');
    const rm = document.createElement('button'); rm.textContent = 'Remove'; rm.onclick = () => post('/api/rule', { action: 'remove', name: r.name });
    row.append(txt, rm);
    c.appendChild(row);
  }
  updatePager($('#rule-prev'), $('#rule-next'), $('#rule-page'), state.rulePage, pc);
}

// --- traffic tab ---
function esc(s) {
  return String(s == null ? '' : s).replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
}
function fmtPayload(p) {
  if (!p) return '';
  if (Array.isArray(p.digital_value)) return 'digital=' + JSON.stringify(p.digital_value);
  if (Array.isArray(p.analog_value)) return 'analog=' + JSON.stringify(p.analog_value);
  if (typeof p === 'object') return JSON.stringify(p);
  return String(p);
}
function currentMessages() { return historyMessages || messages; }

function renderTraffic() {
  const hist = !!historyMessages;
  const src = currentMessages();
  const pc = pageCount(src.length, MSG_PAGE_SIZE);
  state.msgPage = clampPage(state.msgPage, pc);
  const body = document.querySelector('#msgTable tbody');
  body.innerHTML = '';
  for (const rec of slicePage(src, state.msgPage, MSG_PAGE_SIZE)) {
    const tr = document.createElement('tr');
    const time = (rec.t || '').slice(11, 23);
    tr.innerHTML =
      `<td class="mono">${esc(time)}</td>` +
      `<td><span class="pill" style="background:${FLOW_COLORS[rec.tag] || FLOW_COLORS.other}">${esc(rec.flow)}</span></td>` +
      `<td class="mono">${esc(rec.topic)}</td>` +
      `<td>${esc(rec.source)}</td>` +
      `<td>${esc(rec.class)}</td>` +
      `<td class="mono">${esc(fmtPayload(rec.payload))}</td>`;
    body.appendChild(tr);
  }
  updatePager($('#msg-prev'), $('#msg-next'), $('#msg-page'), state.msgPage, pc);
  $('#traffic-title').textContent = hist ? 'History (from Pi file)' : 'Live message flow';
  $('#msgTotal').textContent = hist ? src.length : counts.total;
  const fc = $('#flowCounts');
  fc.innerHTML = '';
  if (hist) {
    fc.textContent = `${src.length} messages loaded from Pi storage`;
  } else {
    const flows = Object.entries(counts.flows || {});
    if (!flows.length) { fc.textContent = 'no messages yet'; }
    flows.forEach(([flow, n]) => {
      const s = document.createElement('span'); s.className = 'flowchip'; s.textContent = `${flow}: ${n}`; fc.appendChild(s);
    });
  }
}

// --- security tab ---
function renderSecurity(sec) {
  $('#secTotal').textContent = sec.total;
  $('#secTls').textContent = sec.tls;
  $('#secAllTls').textContent = sec.allTls ? 'YES' : 'NO';
  $('#secAllTls').style.color = sec.allTls ? '#16a34a' : '#dc2626';
  const body = document.querySelector('#connTable tbody');
  body.innerHTML = '';
  sec.certs.forEach((c) => {
    const tr = document.createElement('tr');
    tr.innerHTML =
      `<td>${esc(c.protocol)}</td>` +
      `<td>${c.ssl ? '<span class="ok">● TLS</span>' : '<span class="bad">✗ plaintext</span>'}</td>` +
      `<td class="mono">${esc(c.cn)}</td>` +
      `<td class="mono">${esc(c.name)}</td>`;
    body.appendChild(tr);
  });
}
function refreshSecurity() { fetch('/api/security').then((r) => r.json()).then(renderSecurity).catch(() => {}); }

const impostorBtn = $('#impostorBtn');
const impostorOut = $('#impostorOut');
function runImpostor() {
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
}
impostorBtn.addEventListener('click', runImpostor);

function setStatus(on) { $('#dot').className = 'dot ' + (on ? 'on' : 'off'); $('#status-text').textContent = on ? 'connected' : 'disconnected'; }
function setSnapshot(s) {
  sensors = s.sensors; actuators = s.actuators; rules = s.rules;
  messages = (s.messages || []).slice(0, 2000); counts = s.counts || { total: 0, flows: {} };
  setStatus(s.connected);
  renderSensors(); renderActuators(); renderRules(); renderTraffic();
}

// --- tab wiring ---
function switchTab(name) {
  state.tab = name;
  document.querySelectorAll('.tab').forEach((b) => b.classList.toggle('active', b.dataset.tab === name));
  document.querySelectorAll('.panel').forEach((p) => p.classList.toggle('active', p.id === 'panel-' + name));
}
document.querySelectorAll('.tab').forEach((b) => b.addEventListener('click', () => switchTab(b.dataset.tab)));

document.querySelectorAll('.filter').forEach((f) => {
  f.querySelectorAll('.fbtn').forEach((btn) => {
    btn.addEventListener('click', () => {
      f.querySelectorAll('.fbtn').forEach((x) => x.classList.remove('active'));
      btn.classList.add('active');
      const which = f.dataset.for;
      if (which === 'sensor') { state.sensorFilter = btn.dataset.filter; state.sensorPage = 0; renderSensors(); }
      else { state.actuatorFilter = btn.dataset.filter; state.actuatorPage = 0; renderActuators(); }
    });
  });
});

$('#sensor-prev').onclick = () => { state.sensorPage--; renderSensors(); };
$('#sensor-next').onclick = () => { state.sensorPage++; renderSensors(); };
$('#actuator-prev').onclick = () => { state.actuatorPage--; renderActuators(); };
$('#actuator-next').onclick = () => { state.actuatorPage++; renderActuators(); };
$('#rule-prev').onclick = () => { state.rulePage--; renderRules(); };
$('#rule-next').onclick = () => { state.rulePage++; renderRules(); };
$('#msg-prev').onclick = () => { state.msgPage--; renderTraffic(); };
$('#msg-next').onclick = () => { state.msgPage++; renderTraffic(); };
$('#btn-history').onclick = async () => {
  const btn = $('#btn-history');
  btn.disabled = true;
  btn.textContent = 'Loading…';
  try {
    const d = await fetch('/api/history?bytes=10485760').then((r) => r.json());
    historyMessages = d.messages || [];
    state.msgPage = 0;
    renderTraffic();
  } catch { alert('failed to load history'); }
  finally { btn.disabled = false; btn.textContent = 'Load history (Pi file)'; }
};
$('#btn-live').onclick = () => { historyMessages = null; state.msgPage = 0; renderTraffic(); };

const es = new EventSource('/events');
es.onmessage = (e) => {
  let m; try { m = JSON.parse(e.data); } catch { return; }
  switch (m.type) {
    case 'hello': setSnapshot(m); break;
    case 'message': { messages.unshift(m.rec); if (messages.length > 2000) messages.pop(); counts = m.counts; if (!historyMessages) renderTraffic(); break; }
    case 'state': {
      const s = sensors.find((x) => x.serial === m.serial); if (s) { s.value = m.value; s.type = m.type; }
      const a = actuators.find((x) => x.serial === m.serial); if (a) { a.value = m.value; a.type = m.type; }
      if (m.kind === 'sensor' && !s) sensors.push({ serial: m.serial, type: m.type, value: m.value });
      if (m.kind === 'actuator' && !a) actuators.push({ serial: m.serial, type: m.type, value: m.value, override: false });
      renderSensors(); renderActuators();
      break;
    }
    case 'override': { const a = actuators.find((x) => x.serial === m.serial); if (a) a.override = m.override; renderActuators(); break; }
    case 'rule': { const i = rules.findIndex((x) => x.name === m.rule.name); if (i >= 0) rules[i] = m.rule; else rules.push(m.rule); renderRules(); break; }
    case 'rule-removed': rules = rules.filter((x) => x.name !== m.name); renderRules(); break;
    case 'status': setStatus(m.connected); break;
  }
};

$('#btn-create-device').onclick = async () => {
  const serial = $('#new-serial').value.trim();
  if (!serial) return;
  await post('/api/device', { serial, kind: $('#new-kind').value, type: Number($('#new-type').value) });
  $('#new-serial').value = '';
};
$('#btn-add-rule').onclick = async () => {
  const text = $('#rule-json').value.trim();
  if (!text) return;
  try { await post('/api/rule', { action: 'add', rule: JSON.parse(text) }); $('#rule-json').value = ''; }
  catch { alert('invalid JSON'); }
};

// --- devices tab (device-manager on the Pi) ---
const MANAGER = 'http://192.168.137.44:8081';
function renderDevices(devices) {
  const body = document.querySelector('#deviceTable tbody');
  body.innerHTML = '';
  (devices || []).forEach((d) => {
    const tr = document.createElement('tr');
    const qr = `${MANAGER}/api/devices/${encodeURIComponent(d.serial)}/qr.png`;
    tr.innerHTML =
      `<td class="mono">${esc(d.serial)}</td>` +
      `<td>${esc(d.kind)}</td>` +
      `<td>${d.type === 2 ? 'analog' : 'digital'}</td>` +
      `<td class="mono">${esc((d.created_at || '').slice(0, 19))}</td>` +
      `<td><img class="qr-thumb" src="${qr}" alt="QR" loading="lazy"></td>` +
      `<td><button class="danger" data-revoke="${esc(d.serial)}">Revoke</button></td>`;
    body.appendChild(tr);
  });
  body.querySelectorAll('[data-revoke]').forEach((b) => {
    b.onclick = async () => { await fetch('/api/revoke-device/' + b.dataset.revoke, { method: 'DELETE' }); refreshDevices(); };
  });
}
async function refreshDevices() { try { renderDevices(await fetch('/api/devices').then((r) => r.json())); } catch {} }
$('#btn-register-device').onclick = async () => {
  const serial = $('#dev-serial').value.trim();
  if (!serial) return;
  const d = await post('/api/register-device', { serial, kind: $('#dev-kind').value, type: Number($('#dev-type').value) });
  $('#dev-serial').value = '';
  if (d.serial) {
    $('#device-qr').innerHTML =
      `<p>Device <b>${esc(d.serial)}</b> registered — scan this QR with the registration app:</p>` +
      `<img class="qr-big" src="${MANAGER}/api/devices/${encodeURIComponent(d.serial)}/qr.png" alt="QR">`;
  }
  refreshDevices();
};
refreshDevices();

(async () => { try { setSnapshot(await fetch('/api/state').then((r) => r.json())); } catch (_) {} })();
refreshSecurity();
setInterval(refreshSecurity, 5000);
