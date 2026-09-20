'use strict';

const $ = (s) => document.querySelector(s);
let sensors = [];
let actuators = [];

function post(url, body) {
  return fetch(url, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body || {}) }).then((r) => r.json());
}

function badge(value, type) {
  const b = document.createElement('span');
  if (type === 2) { b.className = 'state analog'; b.textContent = value === undefined ? '—' : Number(value).toFixed(2); }
  else { b.className = 'state ' + (value === 1 ? 'on' : 'off'); b.textContent = value === 1 ? 'ON' : 'OFF'; }
  return b;
}

function renderSensors() {
  const c = $('#sensors'); c.innerHTML = '';
  for (const s of sensors) {
    const row = document.createElement('div'); row.className = 'dev-row';
    const serial = document.createElement('span'); serial.className = 'serial'; serial.textContent = s.name || s.serial; if (s.name) serial.title = s.serial;
    const t = document.createElement('span'); t.className = 'type'; t.textContent = s.category || (s.type === 2 ? 'analog' : 'digital');
    row.append(serial, t, badge(s.value, s.type));
    const controls = document.createElement('div'); controls.className = 'controls';
    if (s.type === 2) {
      const input = document.createElement('input'); input.type = 'number'; input.step = '0.1'; input.value = s.value === undefined ? 0 : s.value;
      const set = document.createElement('button'); set.textContent = 'Set'; set.className = 'primary'; set.onclick = () => post('/api/sensor', { serial: s.serial, value: Number(input.value) });
      const rel = document.createElement('button'); rel.textContent = 'Release'; rel.onclick = () => post('/api/sensor-release', { serial: s.serial });
      controls.append(input, set, rel);
    } else {
      const on = document.createElement('button'); on.textContent = 'ON'; on.className = 'primary'; on.onclick = () => post('/api/sensor', { serial: s.serial, value: 1 });
      const off = document.createElement('button'); off.textContent = 'OFF'; off.onclick = () => post('/api/sensor', { serial: s.serial, value: 0 });
      const rel = document.createElement('button'); rel.textContent = 'Release'; rel.onclick = () => post('/api/sensor-release', { serial: s.serial });
      controls.append(on, off, rel);
    }
    row.appendChild(controls);
    c.appendChild(row);
  }
}
function renderActuators() {
  const c = $('#actuators'); c.innerHTML = '';
  for (const a of actuators) {
    const row = document.createElement('div'); row.className = 'dev-row';
    const serial = document.createElement('span'); serial.className = 'serial'; serial.textContent = a.name || a.serial; if (a.name) serial.title = a.serial;
    const t = document.createElement('span'); t.className = 'type'; t.textContent = a.category || (a.type === 2 ? 'analog' : 'digital');
    row.append(serial, t, badge(a.value, a.type));
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
}

function whenText(w) {
  if (!w) return '?';
  if (w.type === 'time') return `⏱ ${w.at || (w.from || '00:00') + '–' + (w.to || '23:59')}`;
  let s = `${w.sensor} ${w.op} ${w.threshold}`;
  if (w.hysteresis) s += ` (±${w.hysteresis})`;
  return s;
}
function renderRules(rules) {
  const c = $('#rules'); c.innerHTML = '';
  const rd = (rules.rule_devices || []).map((r) => ({ name: `rd: ${r.input_guid} = ${r.input_value} → ${r.output_guid} = ${r.output_value}`, isRd: true }));
  const adv = (rules.advanced || []).map((r) => ({
    name: `${r.name} [adv]: IF ${whenText(r.when)} THEN ${(r.then || []).map((a) => `${a.actuator}=${a.value}`).join(', ')}${(r.else || []).length ? ' ELSE ' + r.else.map((a) => `${a.actuator}=${a.value}`).join(', ') : ''}`,
    isRd: false,
  }));
  for (const r of rd.concat(adv)) {
    const row = document.createElement('div'); row.className = 'rule-row';
    const txt = document.createElement('span'); txt.className = 'rule-text'; txt.textContent = r.name;
    if (r.isRd) { const tag = document.createElement('span'); tag.className = 'ov-badge'; tag.textContent = 'rule_devices'; row.appendChild(tag); }
    row.appendChild(txt);
    c.appendChild(row);
  }
  if (!rd.length && !adv.length) { const row = document.createElement('div'); row.className = 'rule-row'; row.textContent = 'no rules'; c.appendChild(row); }
}

function esc(s) { return String(s == null ? '' : s).replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c])); }

const MANAGER = 'http://192.168.137.44:8081';
function renderDevices(devs) {
  const body = document.querySelector('#deviceTable tbody'); body.innerHTML = '';
  for (const d of devs) {
    const tr = document.createElement('tr');
    const qr = `${MANAGER}/api/devices/${encodeURIComponent(d.guid)}/qr.png`;
    tr.innerHTML =
      `<td class="mono">${esc(d.guid)}</td>` +
      `<td>${esc(d.type)}</td>` +
      `<td>${esc(d.kind)}</td>` +
      `<td>${esc(d.value_type)}</td>` +
      `<td class="mono">${esc((d.last_seen || '').slice(0, 19))}</td>` +
      `<td><img class="qr-thumb" src="${qr}" alt="QR" loading="lazy"></td>` +
      `<td><button class="danger" data-revoke="${esc(d.guid)}">Revoke</button></td>`;
    body.appendChild(tr);
  }
  body.querySelectorAll('[data-revoke]').forEach((b) => {
    b.onclick = async () => { await fetch('/api/revoke-device/' + b.dataset.revoke, { method: 'DELETE' }); refreshDevices(); };
  });
}
async function refreshDevices() { try { renderDevices(await fetch('/api/devices').then((r) => r.json())); } catch {} }
async function refreshRules() { try { renderRules(await fetch('/api/rules').then((r) => r.json())); } catch {} }

function setStatus(on) { $('#dot').className = 'dot ' + (on ? 'on' : 'off'); $('#status-text').textContent = on ? 'connected' : 'disconnected'; }
function setSnapshot(s) { sensors = s.sensors; actuators = s.actuators; setStatus(s.connected); renderSensors(); renderActuators(); }

function switchTab(name) {
  document.querySelectorAll('.tab').forEach((b) => b.classList.toggle('active', b.dataset.tab === name));
  document.querySelectorAll('.panel').forEach((p) => p.classList.toggle('active', p.id === 'panel-' + name));
}
document.querySelectorAll('.tab').forEach((b) => b.addEventListener('click', () => switchTab(b.dataset.tab)));

$('#btn-register-device').onclick = async () => {
  const guid = $('#dev-guid').value.trim();
  if (!guid) return;
  const d = await post('/api/register-device', { guid, type: $('#dev-type').value, mac: $('#dev-mac').value.trim() });
  $('#dev-guid').value = ''; $('#dev-mac').value = '';
  if (d.guid) {
    $('#device-qr').innerHTML =
      `<p>Device <b>${esc(d.guid)}</b> (${esc(d.type)}) registered — scan this QR:</p>` +
      `<img class="qr-big" src="${MANAGER}/api/devices/${encodeURIComponent(d.guid)}/qr.png" alt="QR">`;
  }
  refreshDevices();
};
$('#btn-import-rules').onclick = async () => {
  const text = $('#rules-json').value.trim();
  if (!text) return;
  try { const r = await post('/api/import-rules', { rules: JSON.parse(text) }); alert(`imported ${r.imported} rules`); $('#rules-json').value = ''; refreshRules(); }
  catch { alert('invalid JSON'); }
};

const es = new EventSource('/events');
es.onmessage = (e) => {
  let m; try { m = JSON.parse(e.data); } catch { return; }
  switch (m.type) {
    case 'hello': setSnapshot(m); break;
    case 'state': {
      const s = sensors.find((x) => x.serial === m.serial); if (s) { s.value = m.value; s.type = m.type; s.name = m.name; s.category = m.category; }
      const a = actuators.find((x) => x.serial === m.serial); if (a) { a.value = m.value; a.type = m.type; a.name = m.name; a.category = m.category; }
      if (m.kind === 'sensor' && !s) sensors.push({ serial: m.serial, name: m.name, category: m.category, type: m.type, value: m.value });
      if (m.kind === 'actuator' && !a) actuators.push({ serial: m.serial, name: m.name, category: m.category, type: m.type, value: m.value, override: false });
      renderSensors(); renderActuators();
      break;
    }
    case 'override': { const a = actuators.find((x) => x.serial === m.serial); if (a) a.override = m.override; renderActuators(); break; }
    case 'status': setStatus(m.connected); break;
  }
};

(async () => { try { setSnapshot(await fetch('/api/state').then((r) => r.json())); } catch {} })();
refreshDevices();
refreshRules();
