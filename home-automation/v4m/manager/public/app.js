'use strict';

const $ = (s) => document.querySelector(s);
const TYPES = ['AKTUATOR', 'SENSOR', 'SENSOR_CAMERA', 'SENSOR_SUHU', 'SENSOR_GAS_DETECTOR', 'SENSOR_WATER_TANK', 'SENSOR_WEATHER', 'SENSOR_BELL', 'SENSOR_SMART_RELAY', 'SENSOR_SMART_PLUG', 'DI_DO', 'DI/DO'];

let devices = [];
let deviceMap = {}; // guid -> device
let editingGuid = null;

async function api(method, path, body) {
  const opts = { method, headers: {} };
  if (body !== undefined) { opts.headers['Content-Type'] = 'application/json'; opts.body = JSON.stringify(body); }
  const r = await fetch(path, opts);
  const j = await r.json().catch(() => ({}));
  if (!r.ok) throw new Error(j.error || `${r.status}`);
  return j;
}

function esc(s) { return String(s == null ? '' : s).replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c])); }
function nameOf(guid) { return (deviceMap[guid] && deviceMap[guid].name) || guid; }
function setStatus(ok, text) { $('#dot').className = 'dot ' + (ok ? 'on' : 'off'); $('#status-text').textContent = text || (ok ? 'connected' : 'error'); }

function switchTab(name) {
  document.querySelectorAll('.tab').forEach((b) => b.classList.toggle('active', b.dataset.tab === name));
  document.querySelectorAll('.panel').forEach((p) => p.classList.toggle('active', p.id === 'panel-' + name));
}
document.querySelectorAll('.tab').forEach((b) => b.addEventListener('click', () => switchTab(b.dataset.tab)));

// ---- Devices ----
async function loadDevices() {
  try {
    devices = await api('GET', '/api/devices');
    deviceMap = {}; for (const d of devices) deviceMap[d.guid] = d;
    renderDevices(); renderDeviceSelects();
    setStatus(true, `${devices.length} devices`);
  } catch (e) { setStatus(false, e.message); }
}

function renderDevices() {
  const q = $('#dev-search').value.toLowerCase();
  const body = document.querySelector('#deviceTable tbody'); body.innerHTML = '';
  const list = devices.filter((d) => !q || (d.name || '').toLowerCase().includes(q) || (d.guid || '').toLowerCase().includes(q) || (d.type || '').toLowerCase().includes(q));
  for (const d of list) {
    const tr = document.createElement('tr');
    tr.innerHTML =
      `<td class="name-cell" title="${esc(d.guid)}">${esc(d.name || '')}<span class="sub">${esc(d.guid)}</span></td>` +
      `<td class="mono">${esc(d.guid)}</td>` +
      `<td>${esc(d.type)}</td>` +
      `<td>${esc(d.kind)}</td>` +
      `<td class="mono">${esc((d.last_seen || '').slice(0, 19))}</td>` +
      `<td class="actions"><button class="ghost" data-edit="${esc(d.guid)}">Edit</button><button class="danger" data-del="${esc(d.guid)}">Delete</button></td>`;
    body.appendChild(tr);
  }
  body.querySelectorAll('[data-edit]').forEach((b) => b.onclick = () => beginEdit(b.dataset.edit));
  body.querySelectorAll('[data-del]').forEach((b) => b.onclick = async () => { if (confirm(`Delete device ${b.dataset.del}?`)) { await api('DELETE', '/api/devices/' + encodeURIComponent(b.dataset.del)); loadDevices(); } });
}

function beginEdit(guid) {
  const d = deviceMap[guid]; if (!d) return;
  editingGuid = guid;
  $('#dev-guid').value = d.guid; $('#dev-guid').disabled = true;
  $('#dev-mac').value = d.mac || '';
  $('#dev-type').value = d.type || 'SENSOR';
  $('#dev-name').value = d.name || '';
  $('#btn-add-device').textContent = 'Update';
}

function resetDeviceForm() {
  editingGuid = null;
  $('#dev-guid').value = ''; $('#dev-guid').disabled = false;
  $('#dev-mac').value = ''; $('#dev-name').value = '';
  $('#btn-add-device').textContent = 'Add';
}

async function submitDevice() {
  const guid = $('#dev-guid').value.trim();
  const type = $('#dev-type').value;
  const name = $('#dev-name').value.trim();
  const mac = $('#dev-mac').value.trim();
  try {
    if (editingGuid) { await api('PUT', '/api/devices/' + encodeURIComponent(editingGuid), { name, type, mac }); resetDeviceForm(); }
    else { if (!guid) return alert('guid required'); await api('POST', '/api/register', { guid, type, mac, name }); }
    loadDevices();
  } catch (e) { alert('Error: ' + e.message); }
}

function renderDeviceSelects() {
  const inSel = $('#rule-in'), outSel = $('#rule-out');
  const prevIn = inSel.value, prevOut = outSel.value;
  inSel.innerHTML = ''; outSel.innerHTML = '';
  const sorted = [...devices].sort((a, b) => (a.name || a.guid).localeCompare(b.name || b.guid));
  for (const d of sorted) {
    const label = `${d.name || d.guid} · ${d.type}`;
    for (const sel of [inSel, outSel]) {
      const o = document.createElement('option');
      o.value = d.guid; o.textContent = label; o.title = d.guid;
      sel.appendChild(o);
    }
  }
  if (prevIn) inSel.value = prevIn;
  if (prevOut) outSel.value = prevOut;
}

// ---- Rules ----
async function loadRules() {
  try {
    const r = await api('GET', '/api/rules');
    renderRules(r.rule_devices || []);
    setStatus(true, `${r.rule_devices.length} rules`);
  } catch (e) { setStatus(false, e.message); }
}

function renderRules(rows) {
  const q = $('#rule-search').value.toLowerCase();
  const body = document.querySelector('#ruleTable tbody'); body.innerHTML = '';
  const list = rows.filter((r) => !q || (r.input_guid || '').toLowerCase().includes(q) || (r.output_guid || '').toLowerCase().includes(q));
  for (const r of list) {
    const tr = document.createElement('tr');
    tr.innerHTML =
      `<td class="mono">${r.id}</td>` +
      `<td class="name-cell" title="${esc(r.input_guid)}">${esc(nameOf(r.input_guid))}<span class="sub">${esc(r.input_guid)}</span></td>` +
      `<td class="mono">${r.input_value}</td>` +
      `<td class="name-cell" title="${esc(r.output_guid)}">${esc(nameOf(r.output_guid))}<span class="sub">${esc(r.output_guid)}</span></td>` +
      `<td class="mono">${r.output_value}</td>` +
      `<td class="actions"><button class="danger" data-del="${r.id}">Delete</button></td>`;
    body.appendChild(tr);
  }
  body.querySelectorAll('[data-del]').forEach((b) => b.onclick = async () => { if (confirm(`Delete rule #${b.dataset.del}?`)) { await api('DELETE', '/api/rules/' + b.dataset.del); loadRules(); } });
}

async function submitRule() {
  const input_guid = $('#rule-in').value;
  const output_guid = $('#rule-out').value;
  const input_value = Number($('#rule-in-val').value);
  const output_value = Number($('#rule-out-val').value);
  if (!input_guid || !output_guid) return alert('Choose an input and an output device.');
  try { await api('POST', '/api/rule', { input_guid, input_value, output_guid, output_value }); loadRules(); }
  catch (e) { alert('Error: ' + e.message); }
}

// ---- wiring ----
(() => {
  const t = $('#dev-type');
  for (const x of TYPES) { const o = document.createElement('option'); o.textContent = x; t.appendChild(o); }

  $('#btn-refresh-devices').onclick = loadDevices;
  $('#btn-add-device').onclick = submitDevice;
  $('#btn-import-devices').onclick = async () => {
    try { const arr = JSON.parse($('#devices-json').value); const r = await api('POST', '/api/import-devices', { devices: arr }); alert(`imported ${r.imported} devices`); $('#devices-json').value = ''; loadDevices(); }
    catch (e) { alert('Error: ' + e.message); }
  };
  $('#btn-clean').onclick = async () => { if (confirm('Delete ALL devices and rules?')) { await api('POST', '/api/clean'); loadDevices(); loadRules(); } };

  $('#btn-refresh-rules').onclick = loadRules;
  $('#btn-add-rule').onclick = submitRule;
  $('#btn-import-rules').onclick = async () => {
    try { const arr = JSON.parse($('#rules-json').value); const r = await api('POST', '/api/import-rules', { rules: arr }); alert(`imported ${r.imported} rules`); $('#rules-json').value = ''; loadRules(); }
    catch (e) { alert('Error: ' + e.message); }
  };

  $('#dev-search').oninput = renderDevices;
  $('#rule-search').oninput = () => loadRules();

  loadDevices(); loadRules();
})();
