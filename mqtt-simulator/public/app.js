'use strict';

const $ = (sel) => document.querySelector(sel);
const MAX_LOG = 500;

const els = {
  statusDot: $('#status-dot'),
  statusText: $('#status-text'),
  brokerLabel: $('#broker-label'),
  simBadge: $('#sim-badge'),
  broker: $('#broker'),
  username: $('#username'),
  password: $('#password'),
  btnConnect: $('#btn-connect'),
  subTopic: $('#sub-topic'),
  btnSubscribe: $('#btn-subscribe'),
  subsList: $('#subs-list'),
  pubTopic: $('#pub-topic'),
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
let simRunning = false;

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------
async function api(url, body) {
  const res = await fetch(url, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body || {}),
  });
  return res.json();
}

function setStatus(on, broker) {
  connected = on;
  els.statusDot.className = 'dot ' + (on ? 'on' : 'off');
  els.statusText.textContent = on ? 'connected' : 'disconnected';
  els.brokerLabel.textContent = broker || '';
}

function setSim(running) {
  simRunning = running;
  els.simBadge.classList.toggle('hidden', !running);
}

function addLog(topic, payload, cls) {
  const div = document.createElement('div');
  div.className = 'msg' + (cls ? ' ' + cls : '');
  const ts = document.createElement('span');
  ts.className = 'ts';
  ts.textContent = new Date().toLocaleTimeString();
  const t = document.createElement('span');
  t.className = 'topic';
  t.textContent = topic;
  const p = document.createElement('span');
  p.className = 'payload';
  p.textContent = payload;
  div.append(ts, t, p);
  els.log.appendChild(div);
  while (els.log.children.length > MAX_LOG) els.log.removeChild(els.log.firstChild);
  els.log.scrollTop = els.log.scrollHeight;
}

function renderSubs(list) {
  els.subsList.innerHTML = '';
  for (const topic of list) {
    const li = document.createElement('li');
    const span = document.createElement('span');
    span.textContent = topic;
    const btn = document.createElement('button');
    btn.textContent = '✕';
    btn.title = 'unsubscribe';
    btn.onclick = async () => { await api('/api/unsubscribe', { topic }); };
    li.append(span, btn);
    els.subsList.appendChild(li);
  }
}

// ---------------------------------------------------------------------------
// SSE
// ---------------------------------------------------------------------------
const es = new EventSource('/events');
es.onmessage = (e) => {
  let msg;
  try { msg = JSON.parse(e.data); } catch (_) { return; }

  switch (msg.type) {
    case 'hello':
      setStatus(msg.connected, msg.broker);
      setSim(msg.simRunning);
      renderSubs(msg.subscriptions || []);
      break;
    case 'status':
      setStatus(msg.connected, msg.broker);
      break;
    case 'message':
      addLog(msg.topic, msg.payload);
      break;
    case 'error':
      addLog('(error)', msg.message, 'err');
      break;
    case 'sim':
      setSim(msg.running);
      break;
  }
};

// ---------------------------------------------------------------------------
// actions
// ---------------------------------------------------------------------------
els.btnConnect.onclick = async () => {
  els.btnConnect.disabled = true;
  setStatus(false, els.broker.value);
  await api('/api/connect', {
    broker: els.broker.value,
    username: els.username.value,
    password: els.password.value,
  });
  els.btnConnect.disabled = false;
};

els.btnSubscribe.onclick = async () => {
  const topic = els.subTopic.value.trim();
  if (!topic) return;
  await api('/api/subscribe', { topic, qos: 1 });
  els.subTopic.value = '';
};

els.btnPublish.onclick = async () => {
  const topic = els.pubTopic.value.trim();
  if (!topic) return;
  await api('/api/publish', { topic, message: els.pubMessage.value, qos: 1 });
  els.pubMessage.value = '';
};

els.btnSimStart.onclick = async () => {
  const r = await api('/api/sim/start', {
    count: Number(els.simCount.value) || 1,
    device: els.simDevice.value || 'sim',
    interval: Number(els.simInterval.value) || 2,
    telemetry: els.simTelemetry.value,
    command: els.simCommand.value,
  });
  if (r && r.error) addLog('(sim)', r.error, 'err');
};

els.btnSimStop.onclick = async () => { await api('/api/sim/stop'); };

els.btnClear.onclick = () => { els.log.innerHTML = ''; };

// initial state
(async () => {
  try {
    const s = await fetch('/api/state').then((r) => r.json());
    els.broker.value = s.broker;
    els.username.value = s.username || '';
    setStatus(s.connected, s.broker);
    setSim(s.simRunning);
    renderSubs(s.subscriptions || []);
  } catch (_) { /* ignore */ }
})();
