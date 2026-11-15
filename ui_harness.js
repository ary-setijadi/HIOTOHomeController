#!/usr/bin/env node
/**
 * Headless DOM harness for the OTA console UI.
 *
 * Loads the real /app.js that the server hands to browsers, runs it against a
 * minimal DOM stub with the live API responses captured as fixtures, then
 * drives every view's loader and asserts the produced markup is sane.
 *
 * This is not a browser: it cannot check pixels. It does catch runtime
 * exceptions, missing functions, undefined/NaN leaking into the page, and
 * broken view wiring.
 */
'use strict';

const fs = require('fs');
const vm = require('vm');

const fixtures = JSON.parse(fs.readFileSync('_fixtures.json', 'utf8'));
const js = fs.readFileSync('_served_app.js', 'utf8');

let failures = 0;
function check(name, ok, detail) {
  if (!ok) failures++;
  console.log(`  ${ok ? 'PASS' : 'FAIL'}  ${name}` +
              (detail && !ok ? `  [${detail}]` : ''));
}

// ---------------------------------------------------------------- DOM stub
class El {
  constructor(sel) {
    this.sel = sel;
    this._html = '';
    this.textContent = '';
    this.value = '';
    this.disabled = false;
    this.checked = false;
    this.files = null;
    this.options = [];
    this.classList = {
      _s: new Set(),
      add(c) { this._s.add(c); }, remove(c) { this._s.delete(c); },
      toggle(c, on) { if (on === undefined) { this._s.has(c) ? this._s.delete(c) : this._s.add(c); }
                      else if (on) this._s.add(c); else this._s.delete(c); },
      contains(c) { return this._s.has(c); },
    };
  }
  get innerHTML() { return this._html; }
  set innerHTML(v) { this._html = String(v); }
  addEventListener() {}
  appendChild() {}
  remove() {}
  closest() { return null; }
  querySelector() { return null; }
}

const elements = new Map();
function el(sel) {
  if (!elements.has(sel)) elements.set(sel, new El(sel));
  return elements.get(sel);
}

const document = {
  querySelector: (sel) => el(sel),
  querySelectorAll: () => [],
  addEventListener: () => {},
  createElement: () => new El('div'),
};

const calls = [];
const sandbox = {
  console,
  document,
  location: { hash: '', href: '' },
  setTimeout: () => 0,
  clearTimeout: () => {},
  setInterval: () => 0,
  clearInterval: () => {},
  confirm: () => true,
  alert: () => {},
  FormData: class { constructor() { this._d = []; } append(k, v) { this._d.push([k, v]); } },
  URLSearchParams: class {
    constructor(o) { this._o = Object.assign({}, o); }
    set(k, v) { this._o[k] = v; }
    toString() { return Object.entries(this._o)
      .map(([k, v]) => `${k}=${encodeURIComponent(v)}`).join('&'); }
  },
  fetch: async (url) => {
    calls.push(url);
    const key = Object.keys(fixtures).find(k => url === k || url.startsWith(k));
    const body = key !== undefined ? fixtures[key] : { error: 'no fixture: ' + url };
    return {
      status: 200,
      ok: true,
      headers: { get: () => 'application/json' },
      json: async () => body,
      text: async () => JSON.stringify(body),
    };
  },
  Date, Math, JSON, Number, String, Object, Array, isFinite, isNaN, parseInt, parseFloat,
};
sandbox.window = sandbox;
sandbox.globalThis = sandbox;

const ctx = vm.createContext(sandbox);
try {
  vm.runInContext(js, ctx, { filename: 'app.js' });
  check('app.js evaluates without throwing', true);
} catch (e) {
  check('app.js evaluates without throwing', false, e.message);
  process.exit(1);
}

// ---------------------------------------------------------------- helpers
const BAD = ['undefined', 'NaN', '[object Object]', 'null%', '>null<'];

function inspect(label, html) {
  if (!html) { check(`${label} renders content`, false, 'empty markup'); return; }
  const bad = BAD.filter(b => html.includes(b));
  check(`${label} renders content`, html.length > 200, `${html.length} chars`);
  check(`${label} has no undefined/NaN leakage`, bad.length === 0, bad.join(','));
  const open = (html.match(/</g) || []).length;
  const close = (html.match(/>/g) || []).length;
  check(`${label} markup is balanced`, open === close, `${open} < vs ${close} >`);
}

async function main() {
  console.log('=== view renderers (real API fixtures) ===');

  // boot() already ran and called loadOverview()
  await new Promise(r => setTimeout(r, 60));
  inspect('overview', el('#view').innerHTML);

  check('overview shows the production channel',
        el('#view').innerHTML.includes('hioto-genmon-28-st7789'));
  check('overview reports the OTA self-test as healthy',
        el('#view').innerHTML.includes('OTA service healthy'));
  check('overview lists host services',
        el('#view').innerHTML.includes('ota-firmware'));

  // channels
  await ctx.loadChannels();
  await new Promise(r => setTimeout(r, 30));
  let h = el('#view').innerHTML;
  inspect('channels', h);
  check('channels renders version history rows', (h.match(/data-promote/g) || []).length > 0,
        'no promote buttons');
  check('channels marks the live blob', h.includes('live'));

  // fleet
  await ctx.loadFleet();
  await new Promise(r => setTimeout(r, 30));
  h = el('#view').innerHTML;
  inspect('fleet', h);
  check('fleet shows a device guid', h.includes('hioto-genmon-'));
  check('fleet shows an up-to-date/outdated state',
        h.includes('up to date') || h.includes('outdated') || h.includes('no report yet'));

  // events: shell then table
  ctx.setView('events');
  await new Promise(r => setTimeout(r, 120));
  const shell = el('#view').innerHTML;
  inspect('events shell', shell);
  check('events shell has the filter controls', shell.includes('id="f-event"'));
  check('events shell has the table host', shell.includes('id="events-table"'));
  const table = el('#events-table').innerHTML;
  inspect('events table', table);
  check('events table lists records', table.includes('query') || table.includes('report'));

  // publish
  await ctx.loadPublish();
  await new Promise(r => setTimeout(r, 40));
  h = el('#view').innerHTML;
  inspect('publish', h);
  check('publish form targets a channel select', h.includes('id="pub-device"'));
  check('publish warns about panel mismatch', h.includes('Panel safety'));
  check('publish lists existing channels', h.includes('hioto-genmon-35-st7796'));

  // system
  await ctx.loadSystem();
  await new Promise(r => setTimeout(r, 40));
  h = el('#view').innerHTML;
  inspect('system', h);
  check('system shows service states', h.includes('ota-firmware'));
  check('system exposes the credential form', h.includes('id="pw-form"'));

  console.log('\n=== navigation wiring ===');
  for (const v of ['overview', 'channels', 'fleet', 'events', 'publish', 'system']) {
    let threw = null;
    try {
      ctx.setView(v);
      await new Promise(r => setTimeout(r, 90));
    } catch (e) { threw = e; }
    check(`setView('${v}') renders without throwing`,
          !threw && el('#view').innerHTML.length > 100,
          threw ? threw.message : `${el('#view').innerHTML.length} chars`);
  }

  console.log('\n=== escaping (through the real render pipeline) ===');
  // poison a channel note + a guid, then confirm nothing executable survives
  const evil = '<script>alert(1)</script><img src=x onerror=alert(2)>';
  fixtures['/api/channels'].channels[0].note = evil;
  fixtures['/api/fleet'].devices[0].guid = evil;
  await ctx.loadChannels();
  await new Promise(r => setTimeout(r, 30));
  const chHtml = el('#view').innerHTML;
  check('channel note is HTML-escaped', !chHtml.includes('<script>alert(1)'),
        'raw script tag survived');
  check('escaped note is still visible as text', chHtml.includes('&lt;script&gt;'));
  await ctx.loadFleet();
  await new Promise(r => setTimeout(r, 30));
  const fleetHtml = el('#view').innerHTML;
  check('device guid is HTML-escaped', !fleetHtml.includes('<img src=x'),
        'raw img/onerror survived');

  console.log('\n' + '='.repeat(60));
  if (failures) {
    console.log(`RESULT: ${failures} UI check(s) FAILED`);
    process.exit(1);
  }
  console.log('RESULT: all UI checks passed');
}

main().catch(e => {
  console.error('HARNESS ERROR:', e && e.stack || e);
  process.exit(1);
});
