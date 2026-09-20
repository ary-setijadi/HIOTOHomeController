'use strict';

const $ = (sel) => document.querySelector(sel);
const MAX_LOG = 500;

const els = {
  statusDot: $('#status-dot'),
  statusText: $('#status-text'),
  connLabel: $('#conn-label'),
  simBadge: $('#sim-badge'),
  host: $('#host'),
  port: $('#port'),
  vhost: $('#vhost'),
  username: $('#username'),
  password: $('#password'),
  exchange: $('#exchange'),
  btnConnect: $('#btn-connect'),
  subKey: $('#sub-key'),
  subQueue: $('#sub-queue'),
  btnSubscribe: $('#btn-subscribe'),
  subsList: $('#subs-list'),
  pubKey: $('#pub-key'),
  pubMessage: $('#pub-message'),
  btnPublish: $('#btn-publish'),
  simDevice: $('#sim-device'),
  simCount: $('#sim-count'),
  simInterval: $('#sim-interval'),
  simTelemetry: $('#sim-telemetry'),
  simCommand: $('#sim-command'),
  btnSimStart: $('#btn-sim-start'),
  btnSimStop: $('#btn-sim-stop'),
  log: $('#log'),
  btnClear: $('#btn-clear'),
};

let connected = false;

async function api(url, body) {
  const res = await fetch(url, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body || {}),
  });
  return res.json();
}

function setStatus(on, host) {
  connected = on;
  els.statusDot.className = 'dot ' + (on ? 'on' : 'off');
  els.statusText.textContent = on ? 'connected' : 'disconnected';
  els.connLabel.textContent = host || '';
}

function setSim(running) {
  els.simBadge.classList.toggle('hidden', !running);
}

function addLog(key, payload, cls) {
  const div = document.createElement('div');
  div.className = 'msg' + (cls ? ' ' + cls : '');
  const ts = document.createElement('span');
  ts.className = 'ts';
  ts.textContent = new Date().toLocaleTimeString();
  const k = document.createElement('span');
  k.className = 'topic';
  k.textContent = key;
  const p = document.createElement('span');
  p.className = 'payload';
  p.textContent = payload;
  div.append(ts, k, p);
  els.log.appendChild(div);
  while (els.log.children.length > MAX_LOG) els.log.removeChild(els.log.firstChild);
  els.log.scrollTop = els.log.scrollHeight;
}

function renderSubs(list) {
  els.subsList.innerHTML = '';
  for (const key of list) {
    const li = document.createElement('li');
    const span = document.createElement('span');
    span.textContent = key;
    const btn = document.createElement('button');
    btn.textContent = '✕';
    btn.title = 'unsubscribe';
    btn.onclick = async () => { await api('/api/unsubscribe', { bindingKey: key }); };
    li.append(span, btn);
    els.subsList.appendChild(li);
  }
}

const es = new EventSource('/events');
es.onmessage = (e) => {
  let m;
  try { m = JSON.parse(e.data); } catch (_) { return; }

  switch (m.type) {
    case 'hello':
      setStatus(m.connected, m.host);
      setSim(m.simRunning);
      renderSubs(m.subscriptions || []);
      break;
    case 'status':
      setStatus(m.connected, m.host);
      break;
    case 'message':
      addLog(m.routingKey, m.payload, m.source === 'simulator' ? 'sim' : (m.source === 'command' ? 'cmd' : ''));
      break;
    case 'error':
      addLog('(error)', m.message, 'err');
      break;
    case 'sim':
      setSim(m.running);
      break;
    case 'subscriptions':
      renderSubs(m.list || []);
      break;
  }
};

els.btnConnect.onclick = async () => {
  els.btnConnect.disabled = true;
  setStatus(false);
  await api('/api/connect', {
    host: els.host.value,
    port: Number(els.port.value) || 5672,
    vhost: els.vhost.value,
    user: els.username.value,
    password: els.password.value,
    exchange: els.exchange.value,
  });
  els.btnConnect.disabled = false;
};

els.btnSubscribe.onclick = async () => {
  const key = els.subKey.value.trim();
  if (!key) return;
  await api('/api/subscribe', { bindingKey: key, queue: els.subQueue.value.trim() });
  els.subKey.value = '';
};

els.btnPublish.onclick = async () => {
  const key = els.pubKey.value.trim();
  if (!key) return;
  await api('/api/publish', { routingKey: key, message: els.pubMessage.value });
  els.pubMessage.value = '';
};

els.btnSimStart.onclick = async () => {
  const r = await api('/api/sim/start', {
    count: Number(els.simCount.value) || 1,
    device: els.simDevice.value || 'sim',
    interval: Number(els.simInterval.value) || 2,
    telemetryKey: els.simTelemetry.value,
    commandKey: els.simCommand.value,
  });
  if (r && r.error) addLog('(sim)', r.error, 'err');
};

els.btnSimStop.onclick = async () => { await api('/api/sim/stop'); };

els.btnClear.onclick = () => { els.log.innerHTML = ''; };

(async () => {
  try {
    const s = await fetch('/api/state').then((r) => r.json());
    els.host.value = s.host;
    els.port.value = s.port;
    els.vhost.value = s.vhost;
    els.username.value = s.user || '';
    els.exchange.value = s.exchange;
    setStatus(s.connected, s.host);
    setSim(s.simRunning);
    renderSubs(s.subscriptions || []);
  } catch (_) { /* ignore */ }
})();
