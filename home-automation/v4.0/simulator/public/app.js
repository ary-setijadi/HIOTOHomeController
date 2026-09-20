'use strict';

const $ = (s) => document.querySelector(s);
let sensors = [];
let actuators = [];
let rules = [];

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

function renderSensors() {
  const c = $('#sensors');
  c.innerHTML = '';
  for (const s of sensors) {
    const row = document.createElement('div'); row.className = 'dev-row';
    const serial = document.createElement('span'); serial.className = 'serial'; serial.textContent = s.serial;
    const t = document.createElement('span'); t.className = 'type'; t.textContent = s.type === 2 ? 'analog' : 'digital';
    row.append(serial, t, badge(s.value, s.type));
    c.appendChild(row);
  }
}

function renderActuators() {
  const c = $('#actuators');
  c.innerHTML = '';
  for (const a of actuators) {
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
  const c = $('#rules');
  c.innerHTML = '';
  for (const r of rules) {
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
}

function setStatus(on) { $('#dot').className = 'dot ' + (on ? 'on' : 'off'); $('#status-text').textContent = on ? 'connected' : 'disconnected'; }
function setSnapshot(s) { sensors = s.sensors; actuators = s.actuators; rules = s.rules; setStatus(s.connected); renderSensors(); renderActuators(); renderRules(); }

const es = new EventSource('/events');
es.onmessage = (e) => {
  let m; try { m = JSON.parse(e.data); } catch { return; }
  switch (m.type) {
    case 'hello': setSnapshot(m); break;
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

(async () => { try { setSnapshot(await fetch('/api/state').then((r) => r.json())); } catch (_) {} })();
