'use strict';

const $ = (s) => document.querySelector(s);
let switches = [];
let lamps = [];
let rules = [];

function post(url, body) {
  return fetch(url, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body || {}) }).then((r) => r.json());
}

function badge(v) {
  const b = document.createElement('span');
  b.className = 'state ' + (v === 1 ? 'on' : 'off');
  b.textContent = v === 1 ? 'ON' : 'OFF';
  return b;
}

function renderSwitches() {
  const c = $('#switches');
  c.innerHTML = '';
  for (const s of switches) {
    const row = document.createElement('div');
    row.className = 'dev-row';
    const serial = document.createElement('span'); serial.className = 'serial'; serial.textContent = s.serial;
    row.append(serial, badge(s.value));
    c.appendChild(row);
  }
}

function renderLamps() {
  const c = $('#lamps');
  c.innerHTML = '';
  for (const l of lamps) {
    const row = document.createElement('div');
    row.className = 'dev-row';
    const serial = document.createElement('span'); serial.className = 'serial'; serial.textContent = l.serial;
    row.append(serial, badge(l.value));
    if (l.override) {
      const ov = document.createElement('span'); ov.className = 'ov-badge'; ov.textContent = 'OVERRIDE';
      row.appendChild(ov);
    }
    const controls = document.createElement('div'); controls.className = 'controls';
    const on = document.createElement('button'); on.textContent = 'ON'; on.className = 'primary'; on.onclick = () => post('/api/override', { serial: l.serial, value: 1 });
    const off = document.createElement('button'); off.textContent = 'OFF'; off.className = 'danger'; off.onclick = () => post('/api/override', { serial: l.serial, value: 0 });
    const rel = document.createElement('button'); rel.textContent = 'Release'; rel.onclick = () => post('/api/release', { serial: l.serial });
    controls.append(on, off, rel);
    row.appendChild(controls);
    c.appendChild(row);
  }
}

function renderRules() {
  const c = $('#rules');
  c.innerHTML = '';
  for (const r of rules) {
    const row = document.createElement('div');
    row.className = 'rule-row';
    const txt = document.createElement('span');
    txt.className = 'rule-text';
    const acts = r.mappings.map((m) => m.actuator + (m.on === 0 && m.off === 1 ? ' (inv)' : '')).join(', ');
    txt.textContent = `${r.switch}  →  ${acts}`;
    const rm = document.createElement('button');
    rm.textContent = 'Remove';
    rm.onclick = () => post('/api/rule', { action: 'remove', switch: r.switch });
    row.append(txt, rm);
    c.appendChild(row);
  }
}

function setStatus(on) {
  $('#dot').className = 'dot ' + (on ? 'on' : 'off');
  $('#status-text').textContent = on ? 'connected' : 'disconnected';
}

function setSnapshot(s) {
  switches = s.switches; lamps = s.lamps; rules = s.rules;
  setStatus(s.connected);
  renderSwitches(); renderLamps(); renderRules();
}

const es = new EventSource('/events');
es.onmessage = (e) => {
  let m; try { m = JSON.parse(e.data); } catch { return; }
  switch (m.type) {
    case 'hello': setSnapshot(m); break;
    case 'state': {
      const sw = switches.find((x) => x.serial === m.serial); if (sw) sw.value = m.value;
      const lp = lamps.find((x) => x.serial === m.serial); if (lp) lp.value = m.value;
      renderSwitches(); renderLamps();
      break;
    }
    case 'override': {
      const lp = lamps.find((x) => x.serial === m.serial); if (lp) lp.override = m.override;
      renderLamps();
      break;
    }
    case 'rule': {
      const i = rules.findIndex((x) => x.switch === m.rule.switch);
      if (i >= 0) rules[i] = m.rule; else rules.push(m.rule);
      renderRules();
      break;
    }
    case 'rule-removed': rules = rules.filter((x) => x.switch !== m.switch); renderRules(); break;
    case 'switch-created': {
      if (!switches.find((x) => x.serial === m.serial)) switches.push({ serial: m.serial, value: undefined });
      renderSwitches();
      break;
    }
    case 'status': setStatus(m.connected); break;
  }
};

$('#btn-create-switch').onclick = async () => {
  const serial = $('#new-switch').value.trim();
  if (!serial) return;
  await post('/api/switch', { serial });
  $('#new-switch').value = '';
};

$('#btn-add-rule').onclick = async () => {
  const sw = $('#rule-switch').value.trim();
  const mappingsText = $('#rule-mappings').value.trim();
  if (!sw || !mappingsText) return;
  const mappings = mappingsText.split(',').map((s) => {
    const [actuator, on, off] = s.trim().split(':');
    return { actuator: (actuator || '').trim(), on: Number(on), off: Number(off) };
  }).filter((m) => m.actuator);
  await post('/api/rule', { action: 'add', switch: sw, mappings });
  $('#rule-switch').value = '';
  $('#rule-mappings').value = '';
};

(async () => {
  try { setSnapshot(await fetch('/api/state').then((r) => r.json())); } catch (_) { /* ignore */ }
})();
