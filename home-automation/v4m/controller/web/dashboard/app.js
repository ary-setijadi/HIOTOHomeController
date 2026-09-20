'use strict';

const $ = (s) => document.querySelector(s);
let sensors = [];
let actuators = [];
let editing = false; // set while a device name is being edited inline
let floorsMap = {}; // id -> name
let roomsMap = {};  // id -> name

const FLOOR_ORDER = ['Lantai 1', 'Lantai 2', 'Lantai 3', 'Lantai 4', 'Lainnya'];
const flKey = (fl) => fl.toLowerCase().replace(/[^a-z0-9]+/g, '-');

function post(url, body) {
  return fetch(url, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body || {}) }).then((r) => r.json());
}

async function refreshState() {
  try { setSnapshot(await fetch('/api/state').then((r) => r.json())); } catch {}
}

function makeName(name, guid) {
  const span = document.createElement('span');
  span.className = 'serial';
  span.textContent = name || guid;
  if (name) span.title = guid;
  span.style.cursor = 'pointer';
  span.onclick = () => inlineRename(span, guid, name || '');
  return span;
}

function inlineRename(el, guid, currentName) {
  const input = document.createElement('input');
  input.type = 'text';
  input.value = currentName || '';
  input.className = 'serial inline-edit';
  input.style.width = Math.max(8, (currentName || guid || '').length + 2) + 'ch';
  editing = true;
  el.replaceWith(input);
  input.focus();
  input.select();
  let done = false;
  const finish = async (save) => {
    if (done) return;
    done = true;
    const name = input.value.trim();
    const span = makeName(name || guid, guid);
    input.replaceWith(span);
    editing = false;
    if (save && name && name !== currentName) {
      await fetch('/api/devices/' + encodeURIComponent(guid), {
        method: 'PUT', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ name }),
      });
      refreshState(); // re-fetch (sorted) and re-render with the new name
    } else {
      renderFloors(); // catch up on any live updates that arrived during the edit
    }
  };
  input.onblur = () => finish(true);
  input.onkeydown = (e) => {
    if (e.key === 'Enter') input.blur();
    else if (e.key === 'Escape') finish(false);
  };
}

function badge(value, type, kind) {
  const b = document.createElement('span');
  if (type === 2) { b.className = 'state analog'; b.textContent = value === undefined ? '—' : Number(value).toFixed(2); }
  else {
    // lamps/actuators are active-low: 0 = ON, 1 = OFF
    const on = kind === 'actuator' ? (value === 0) : (value === 1);
    b.className = 'state ' + (on ? 'on' : 'off'); b.textContent = on ? 'ON' : 'OFF';
  }
  return b;
}

function deviceRow(item, kind) {
  const row = document.createElement('div'); row.className = 'dev-row';
  const serial = makeName(item.name, item.serial);
  const t = document.createElement('span'); t.className = 'type'; t.textContent = item.category || (item.type === 2 ? 'analog' : 'digital');
  row.append(serial, t, badge(item.value, item.type, kind));
  if (kind === 'actuator' && item.override) { const ov = document.createElement('span'); ov.className = 'ov-badge'; ov.textContent = 'OVERRIDE'; row.appendChild(ov); }
  const controls = document.createElement('div'); controls.className = 'controls';
  if (item.type === 2) {
    const input = document.createElement('input'); input.type = 'number'; input.step = '0.1'; input.value = item.value === undefined ? 0 : item.value;
    if (kind === 'actuator') { input.min = '0'; input.max = '1'; }
    const set = document.createElement('button'); set.textContent = 'Set'; set.className = 'primary';
    set.onclick = () => post(kind === 'actuator' ? '/api/override' : '/api/sensor', { serial: item.serial, value: Number(input.value) });
    const rel = document.createElement('button'); rel.textContent = 'Release';
    rel.onclick = () => post(kind === 'actuator' ? '/api/release' : '/api/sensor-release', { serial: item.serial });
    controls.append(input, set, rel);
  } else if (kind === 'actuator') {
    const on = document.createElement('button'); on.textContent = 'ON'; on.className = 'primary'; on.onclick = () => post('/api/override', { serial: item.serial, value: 0 });
    const off = document.createElement('button'); off.textContent = 'OFF'; off.className = 'danger'; off.onclick = () => post('/api/override', { serial: item.serial, value: 1 });
    const rel = document.createElement('button'); rel.textContent = 'Release'; rel.onclick = () => post('/api/release', { serial: item.serial });
    controls.append(on, off, rel);
  } else {
    const on = document.createElement('button'); on.textContent = 'ON'; on.className = 'primary'; on.onclick = () => post('/api/sensor', { serial: item.serial, value: 1 });
    const off = document.createElement('button'); off.textContent = 'OFF'; off.onclick = () => post('/api/sensor', { serial: item.serial, value: 0 });
    const rel = document.createElement('button'); rel.textContent = 'Release'; rel.onclick = () => post('/api/sensor-release', { serial: item.serial });
    controls.append(on, off, rel);
  }
  row.appendChild(controls);
  return row;
}

function groupByFloor() {
  const g = {};
  for (const fl of FLOOR_ORDER) g[fl] = { sensors: [], actuators: [] };
  for (const s of sensors) { (g[s.floor || 'Lainnya'] = g[s.floor || 'Lainnya'] || { sensors: [], actuators: [] }).sensors.push(s); }
  for (const a of actuators) { (g[a.floor || 'Lainnya'] = g[a.floor || 'Lainnya'] || { sensors: [], actuators: [] }).actuators.push(a); }
  const byName = (x, y) => ((x.name || x.serial) + '').localeCompare((y.name || y.serial) + '');
  for (const fl of Object.keys(g)) { g[fl].sensors.sort(byName); g[fl].actuators.sort(byName); }
  return g;
}

function renderFloors() {
  if (editing) return; // don't rebuild the DOM and clobber an in-progress rename
  const g = groupByFloor();
  for (const fl of FLOOR_ORDER) {
    const key = flKey(fl);
    const sc = document.getElementById('sensors-' + key);
    const ac = document.getElementById('actuators-' + key);
    if (!sc || !ac) continue;
    sc.innerHTML = ''; ac.innerHTML = '';
    if (!g[fl].sensors.length) sc.textContent = 'no sensors';
    if (!g[fl].actuators.length) ac.textContent = 'no actuators';
    for (const s of g[fl].sensors) sc.appendChild(deviceRow(s, 'sensor'));
    for (const a of g[fl].actuators) ac.appendChild(deviceRow(a, 'actuator'));
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

async function loadFloorsRooms() {
  try {
    const [fl, rm] = await Promise.all([
      fetch('/api/floors').then((r) => r.json()),
      fetch('/api/rooms').then((r) => r.json()),
    ]);
    const fsel = $('#dev-floor'), rsel = $('#dev-room');
    fsel.innerHTML = '<option value="">— none —</option>';
    rsel.innerHTML = '<option value="">— none —</option>';
    for (const f of fl) { floorsMap[f.id] = f.name; const o = document.createElement('option'); o.value = f.id; o.textContent = f.name; fsel.appendChild(o); }
    for (const r of rm) { roomsMap[r.id] = r.name; const o = document.createElement('option'); o.value = r.id; o.textContent = r.name; rsel.appendChild(o); }
  } catch {}
}

function renderDevices(devs) {
  const body = document.querySelector('#deviceTable tbody'); body.innerHTML = '';
  for (const d of devs) {
    const tr = document.createElement('tr');
    const ver = (d.version || '') + (d.minor && d.minor !== '0' ? '.' + d.minor : '');
    const room = d.room_id ? (roomsMap[d.room_id] || '#' + d.room_id) : '';
    const floor = d.floor_id ? (floorsMap[d.floor_id] || '#' + d.floor_id) : '';
    tr.innerHTML =
      `<td class="mono" title="${esc(d.guid)}">${esc(d.guid)}</td>` +
      `<td>${esc(d.name)}</td>` +
      `<td>${esc(d.type)}</td>` +
      `<td class="mono">${esc(d.mac || '')}</td>` +
      `<td>${esc(ver)}</td>` +
      `<td>${esc(d.quantity)}</td>` +
      `<td>${esc(room)}</td>` +
      `<td>${esc(floor)}</td>` +
      `<td>${esc(d.status || '')}</td>` +
      `<td class="mono">${esc((d.last_seen || '').slice(0, 19))}</td>` +
      `<td><button class="danger" data-revoke="${esc(d.guid)}">Revoke</button></td>`;
    body.appendChild(tr);
  }
  body.querySelectorAll('[data-revoke]').forEach((b) => {
    b.onclick = async () => { await fetch('/api/revoke-device/' + b.dataset.revoke, { method: 'DELETE' }); refreshDevices(); };
  });
}
async function refreshDevices() { try { renderDevices(await fetch('/api/devices').then((r) => r.json())); } catch {} }
async function refreshRules() { try { renderRules(await fetch('/api/rules').then((r) => r.json())); } catch {} }

// ---- timer rules ----
function timerName(guid) {
  const d = sensors.concat(actuators).find((x) => x.serial === guid);
  return d ? (d.name || d.serial) : guid;
}
function timerActionsText(acts) {
  if (!acts || !acts.length) return '—';
  return acts.map((a) => `${timerName(a.actuator)} = ${a.value === 0 ? 'ON' : a.value === 1 ? 'OFF' : a.value}`).join(', ');
}
function timerScheduleText(tr) {
  if (tr.from || tr.to) return `⏱ ${tr.from || '00:00'} – ${tr.to || '23:59'}`;
  if (tr.for_minutes > 0) return `⏱ ${tr.at ? 'at ' + tr.at + ', ' : 'now, '}for ${tr.for_minutes} min`;
  if (tr.at) return `⏱ at ${tr.at}`;
  return '—';
}
function renderTimerRules(rules) {
  const c = $('#timer-rules'); c.innerHTML = '';
  if (!rules || !rules.length) { const row = document.createElement('div'); row.className = 'rule-row'; row.textContent = 'no timer rules'; c.appendChild(row); return; }
  for (const tr of rules) {
    const row = document.createElement('div'); row.className = 'rule-row';
    const tag = document.createElement('span'); tag.className = 'ov-badge'; tag.textContent = tr.enabled ? 'on' : 'off';
    const txt = document.createElement('span'); txt.className = 'rule-text';
    txt.textContent = `${tr.name} — ${timerScheduleText(tr)} → ${timerActionsText(tr.then)}${tr.else && tr.else.length ? ' ELSE ' + timerActionsText(tr.else) : ''}`;
    const tog = document.createElement('button'); tog.textContent = tr.enabled ? 'Disable' : 'Enable';
    tog.onclick = async () => { await post('/api/timer-rules/' + tr.id + '/toggle', {}); loadTimerRules(); };
    const del = document.createElement('button'); del.textContent = 'Delete'; del.className = 'danger';
    del.onclick = async () => { await fetch('/api/timer-rules/' + tr.id, { method: 'DELETE' }); loadTimerRules(); };
    row.append(tag, txt, tog, del);
    c.appendChild(row);
  }
}
async function loadTimerRules() { try { renderTimerRules(await fetch('/api/timer-rules').then((r) => r.json())); } catch {} }

function populateTimerActuators() {
  const sel = $('#tr-actuator'), sel2 = $('#tr-else-actuator');
  if (!sel || !sel2) return;
  const cur = sel.value, cur2 = sel2.value;
  sel.innerHTML = ''; sel2.innerHTML = '';
  for (const a of actuators) {
    const o = document.createElement('option'); o.value = a.serial; o.textContent = a.name || a.serial; sel.appendChild(o);
    const o2 = document.createElement('option'); o2.value = a.serial; o2.textContent = a.name || a.serial; sel2.appendChild(o2);
  }
  if (cur) sel.value = cur;
  if (cur2) sel2.value = cur2;
}

function timerKindUI() {
  const k = $('#tr-kind').value;
  $('#tr-at-lbl').style.display = k === 'at' ? 'flex' : 'none';
  $('#tr-from-lbl').style.display = k === 'window' ? 'flex' : 'none';
  $('#tr-to-lbl').style.display = k === 'window' ? 'flex' : 'none';
  $('#tr-dat-lbl').style.display = k === 'duration' ? 'flex' : 'none';
  $('#tr-min-lbl').style.display = k === 'duration' ? 'flex' : 'none';
  const showElse = k === 'window' || k === 'duration';
  $('#tr-else-lbl').style.display = showElse ? 'flex' : 'none';
  $('#tr-else-state-lbl').style.display = showElse ? 'flex' : 'none';
}

$('#tr-kind').onchange = timerKindUI;
$('#btn-add-timer').onclick = async () => {
  const name = $('#tr-name').value.trim();
  if (!name) { alert('Name is required'); return; }
  const kind = $('#tr-kind').value;
  const thenArr = [{ actuator: $('#tr-actuator').value, value: Number($('#tr-then-state').value) }];
  const body = { name, enabled: true, then: thenArr };
  if (kind === 'at') {
    body.at = $('#tr-at').value;
  } else if (kind === 'window') {
    body.from = $('#tr-from').value; body.to = $('#tr-to').value;
    body.else = [{ actuator: $('#tr-else-actuator').value, value: Number($('#tr-else-state').value) }];
  } else if (kind === 'duration') {
    body.at = $('#tr-dat').value || '';
    body.for_minutes = parseInt($('#tr-minutes').value, 10) || 0;
    body.else = [{ actuator: $('#tr-else-actuator').value, value: Number($('#tr-else-state').value) }];
  }
  const r = await post('/api/timer-rules', body);
  if (r && r.id) { $('#tr-name').value = ''; loadTimerRules(); }
};

function setStatus(on) { $('#dot').className = 'dot ' + (on ? 'on' : 'off'); $('#status-text').textContent = on ? 'connected' : 'disconnected'; }
function setSnapshot(s) { sensors = s.sensors || []; actuators = s.actuators || []; setStatus(s.connected); renderFloors(); populateHistDevices(); populateTimerActuators(); }

// ---- history tab ----
function populateHistDevices() {
  const sel = $('#hist-device'); if (!sel) return;
  const cur = sel.value;
  sel.innerHTML = '<option value="">— select device —</option>';
  const all = sensors.concat(actuators).sort((x, y) => ((x.name || x.serial) + '').localeCompare((y.name || y.serial) + ''));
  for (const d of all) {
    const o = document.createElement('option');
    o.value = d.serial; o.textContent = (d.name || d.serial) + (d.floor && d.floor !== 'Lainnya' ? ' · ' + d.floor : '');
    sel.appendChild(o);
  }
  if (cur) sel.value = cur;
}
async function loadHistory() {
  const guid = $('#hist-device').value;
  const metric = $('#hist-metric').value;
  const res = await fetch('/api/telemetry?guid=' + encodeURIComponent(guid) + '&metric=' + encodeURIComponent(metric) + '&limit=500').then((r) => r.json());
  const list = $('#history'); list.innerHTML = '';
  $('#hist-stats').textContent = `memory ${(res.mem_bytes / 1048576).toFixed(2)} MB · disk ${(res.disk_bytes / 1048576).toFixed(2)} MB · ${res.files} file(s)`;
  if (!res.points || !res.points.length) { list.textContent = 'no data'; return; }
  for (const p of res.points) {
    const row = document.createElement('div'); row.className = 'dev-row';
    const ts = document.createElement('span'); ts.className = 'mono'; ts.textContent = (p.ts || '').slice(0, 19);
    const n = document.createElement('span'); n.className = 'serial'; n.textContent = p.name || p.guid;
    const m = document.createElement('span'); m.className = 'type'; m.textContent = p.metric;
    const v = document.createElement('span'); v.className = 'state analog'; v.textContent = Number(p.value).toFixed(2);
    row.append(ts, n, m, v);
    list.appendChild(row);
  }
}

// ---- tab wiring ----
function switchTab(name) {
  document.querySelectorAll('#top-tabs .tab').forEach((b) => b.classList.toggle('active', b.dataset.tab === name));
  document.querySelectorAll('.panel').forEach((p) => p.classList.toggle('active', p.id === 'panel-' + name));
}

function buildLayout() {
  const nav = $('#top-tabs');
  const main = $('#content');
  const anchor = $('#panel-history');
  for (const fl of FLOOR_ORDER) {
    const key = flKey(fl);
    const btn = document.createElement('button');
    btn.className = 'tab'; btn.dataset.tab = 'floor-' + key; btn.textContent = fl;
    nav.insertBefore(btn, anchor ? nav.querySelector('[data-tab="history"]') : null);
    const sec = document.createElement('section');
    sec.id = 'panel-floor-' + key; sec.className = 'panel';
    sec.innerHTML =
      `<div class="panel-head"><h2>${esc(fl)}</h2></div>` +
      `<div class="subtabs">` +
      `<button class="subtab active" data-sub="sensors-${key}">Sensors</button>` +
      `<button class="subtab" data-sub="actuators-${key}">Actuators</button>` +
      `</div>` +
      `<div id="sensors-${key}" class="list subpanel active"></div>` +
      `<div id="actuators-${key}" class="list subpanel"></div>`;
    main.insertBefore(sec, anchor);
  }
  // delegated tab clicks (top + sub)
  nav.addEventListener('click', (e) => { const b = e.target.closest('.tab'); if (b) switchTab(b.dataset.tab); });
  main.addEventListener('click', (e) => {
    const b = e.target.closest('.subtab'); if (!b) return;
    const panel = b.closest('.panel');
    panel.querySelectorAll('.subtab').forEach((x) => x.classList.toggle('active', x === b));
    panel.querySelectorAll('.subpanel').forEach((x) => x.classList.toggle('active', x.id === b.dataset.sub));
  });
}

// ---- registration (manual + QR scan) ----
function fillForm(p) {
  $('#dev-guid').value = p.guid || '';
  $('#dev-name').value = p.name || '';
  if (p.type) $('#dev-type').value = p.type;
  $('#dev-mac').value = p.mac || '';
  $('#dev-version').value = p.version || '';
  $('#dev-minor').value = p.minor || '';
  $('#dev-qty').value = (p.quantity == null ? 1 : p.quantity);
  if (p.room_id) $('#dev-room').value = String(p.room_id);
  if (p.floor_id) $('#dev-floor').value = String(p.floor_id);
  $('#dev-x').value = (p.x_position == null ? 0 : p.x_position);
  $('#dev-y').value = (p.y_position == null ? 0 : p.y_position);
  $('#dev-status-device').value = p.status_device || '';
}

function normalizeRegistration(p) {
  return {
    guid: (p.guid || '').trim(),
    name: p.name || '',
    type: p.type || 'SENSOR',
    mac: p.mac || '',
    version: p.version || '',
    minor: p.minor || '',
    quantity: Number(p.quantity) || 0,
    room_id: Number(p.room_id) || 0,
    floor_id: Number(p.floor_id) || 0,
    x_position: Number(p.x_position) || 0,
    y_position: Number(p.y_position) || 0,
    status_device: p.status_device || '',
  };
}

async function registerDevice(body) {
  const res = $('#reg-result');
  try {
    const d = await post('/api/register-device', body);
    if (d && d.guid) {
      res.innerHTML = `Registered <b>${esc(d.guid)}</b> (${esc(d.type)})`;
      refreshDevices();
      return d;
    }
    res.textContent = 'Registration failed (no guid returned)';
  } catch (e) {
    res.textContent = 'Registration error: ' + (e && e.message ? e.message : e);
  }
  return null;
}

$('#btn-register-device').onclick = async () => {
  const guid = $('#dev-guid').value.trim();
  if (!guid) { $('#reg-result').textContent = 'GUID is required.'; return; }
  await registerDevice(normalizeRegistration({
    guid, name: $('#dev-name').value.trim(), type: $('#dev-type').value,
    mac: $('#dev-mac').value.trim(), version: $('#dev-version').value.trim(),
    minor: $('#dev-minor').value.trim(), quantity: $('#dev-qty').value,
    room_id: $('#dev-room').value, floor_id: $('#dev-floor').value,
    x_position: $('#dev-x').value, y_position: $('#dev-y').value,
    status_device: $('#dev-status-device').value.trim(),
  }));
};
$('#btn-reg-clear').onclick = () => {
  ['dev-guid', 'dev-name', 'dev-mac', 'dev-version', 'dev-minor', 'dev-status-device'].forEach((id) => { const el = $('#' + id); if (el) el.value = ''; });
  $('#dev-type').value = 'AKTUATOR';
  $('#dev-qty').value = '1';
  $('#dev-x').value = '0';
  $('#dev-y').value = '0';
  $('#dev-room').value = '';
  $('#dev-floor').value = '';
  $('#reg-result').textContent = '';
};

// ---- webcam QR scanner ----
let camStream = null;
let scanRaf = null;
let lastScan = { text: '', ts: 0 };

async function startCamera() {
  const status = $('#scan-status');
  if (!navigator.mediaDevices || !navigator.mediaDevices.getUserMedia) {
    status.textContent = 'Camera unavailable — the webcam needs a secure context (https:// or localhost).';
    return;
  }
  try {
    camStream = await navigator.mediaDevices.getUserMedia({ video: { facingMode: 'environment' }, audio: false });
    const video = $('#qr-video');
    video.srcObject = camStream;
    await video.play();
    $('#btn-cam-start').disabled = true;
    status.textContent = 'Scanning… point the camera at a registration QR.';
    scanLoop();
  } catch (e) {
    status.textContent = 'Camera error: ' + (e && e.message ? e.message : e);
  }
}

function stopCamera() {
  if (scanRaf) { cancelAnimationFrame(scanRaf); scanRaf = null; }
  if (camStream) { camStream.getTracks().forEach((t) => t.stop()); camStream = null; }
  const video = $('#qr-video');
  if (video) video.srcObject = null;
  $('#btn-cam-start').disabled = false;
  $('#scan-status').textContent = 'Camera off.';
}

function scanLoop() {
  const video = $('#qr-video');
  const canvas = $('#qr-canvas');
  if (video && video.readyState === video.HAVE_ENOUGH_DATA) {
    const w = video.videoWidth, h = video.videoHeight;
    if (w && h) {
      canvas.width = w; canvas.height = h;
      const ctx = canvas.getContext('2d', { willReadFrequently: true });
      ctx.drawImage(video, 0, 0, w, h);
      const img = ctx.getImageData(0, 0, w, h);
      if (window.jsQR) {
        const code = jsQR(img.data, w, h, { inversionAttempts: 'dontInvert' });
        if (code && code.data) {
          handleScannedPayload(code.data);
          return;
        }
      }
    }
  }
  scanRaf = requestAnimationFrame(scanLoop);
}

function handleScannedPayload(text) {
  const status = $('#scan-status');
  const now = Date.now();
  // debounce: same code re-decoded every frame shouldn't re-register
  if (text === lastScan.text && now - lastScan.ts < 4000) { scanRaf = requestAnimationFrame(scanLoop); return; }
  lastScan = { text, ts: now };

  let p = null;
  try { p = JSON.parse(text); } catch { p = null; }
  if (!p || typeof p !== 'object' || !p.guid) {
    // fall back: treat the raw text as a bare guid
    const g = (text || '').trim();
    if (g) p = { guid: g };
  }
  if (!p || !p.guid) {
    status.textContent = 'Scanned code has no "guid" — not a registration QR.';
    scanRaf = requestAnimationFrame(scanLoop);
    return;
  }
  const body = normalizeRegistration(p);
  fillForm(p);
  status.textContent = `Scanned ${body.guid} — registering…`;
  registerDevice(body).then((d) => {
    status.textContent = d ? `Registered ${d.guid}. Scan the next one or stop.` : 'Registration failed — see result below.';
  });
  scanRaf = requestAnimationFrame(scanLoop);
}

$('#btn-cam-start').onclick = startCamera;
$('#btn-cam-stop').onclick = stopCamera;

// ---- CSV import / export ----
$('#btn-export-csv').onclick = () => { window.location.href = '/api/devices/export.csv'; };

$('#btn-import-csv').onclick = async () => {
  const status = $('#import-status');
  const file = $('#csv-file').files[0];
  if (!file) { status.textContent = 'Choose a CSV file first.'; return; }
  try {
    const text = await file.text();
    const r = await fetch('/api/devices/import.csv', { method: 'POST', headers: { 'Content-Type': 'text/csv' }, body: text });
    const j = await r.json();
    if (r.ok) {
      status.textContent = `Imported ${j.imported} device(s).`;
      $('#csv-file').value = '';
      refreshDevices();
    } else {
      status.textContent = 'Import error: ' + (j.error || ('HTTP ' + r.status));
    }
  } catch (e) {
    status.textContent = 'Import error: ' + (e && e.message ? e.message : e);
  }
};
$('#btn-import-rules').onclick = async () => {
  const text = $('#rules-json').value.trim();
  if (!text) return;
  try { const r = await post('/api/import-rules', { rules: JSON.parse(text) }); alert(`imported ${r.imported} rules`); $('#rules-json').value = ''; refreshRules(); }
  catch { alert('invalid JSON'); }
};
$('#btn-hist-refresh').onclick = loadHistory;
$('#hist-device').onchange = loadHistory;
$('#hist-metric').onchange = loadHistory;

const es = new EventSource('/events');
es.onmessage = (e) => {
  let m; try { m = JSON.parse(e.data); } catch { return; }
  switch (m.type) {
    case 'hello': setSnapshot(m); break;
    case 'state': {
      const s = sensors.find((x) => x.serial === m.serial); if (s) { s.value = m.value; s.type = m.dtype; s.name = m.name; s.category = m.category; s.floor = m.floor; }
      const a = actuators.find((x) => x.serial === m.serial); if (a) { a.value = m.value; a.type = m.dtype; a.name = m.name; a.category = m.category; a.floor = m.floor; }
      if (m.kind === 'sensor' && !s) sensors.push({ serial: m.serial, name: m.name, category: m.category, type: m.dtype, value: m.value, floor: m.floor });
      if (m.kind === 'actuator' && !a) actuators.push({ serial: m.serial, name: m.name, category: m.category, type: m.dtype, value: m.value, override: false, floor: m.floor });
      renderFloors();
      break;
    }
    case 'override': { const a = actuators.find((x) => x.serial === m.serial); if (a) a.override = m.override; renderFloors(); break; }
    case 'status': setStatus(m.connected); break;
  }
};

buildLayout();
switchTab('floor-' + flKey(FLOOR_ORDER[0]));
(async () => { try { setSnapshot(await fetch('/api/state').then((r) => r.json())); } catch {} })();
loadFloorsRooms().then(() => refreshDevices());
refreshRules();
loadTimerRules();
timerKindUI();
