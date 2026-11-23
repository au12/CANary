/* CANary on-device web interface. No dependencies and no build step: this file is what the browser runs.
   SPDX-License-Identifier: GPL-3.0-or-later */
(() => {
'use strict';

const $ = (s, r = document) => r.querySelector(s);
const $$ = (s, r = document) => Array.from(r.querySelectorAll(s));
const view = $('#view');
const ESC = { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' };
const esc = s => String(s == null ? '' : s).replace(/[&<>"']/g, c => ESC[c]);
const icon = (id, cls) => `<svg${cls ? ` class="${cls}"` : ''} aria-hidden="true"><use href="#i-${id}"/></svg>`;
const BUILD = (document.currentScript && document.currentScript.src.split('?v=')[1]) || '';

const S = { st: null, fails: 0, cfg: null, profiles: [], activeProfile: '', dtc: null, meta: null, imperial: true, timeSent: false, away: null };

/* ───────── talking to the device ───────── */

async function api(path, o = {}) {
  const init = { method: o.method || (o.body !== undefined ? 'POST' : 'GET'), cache: 'no-store', headers: {} };
  if (o.body !== undefined) {
    init.body = JSON.stringify(o.body);
    init.headers['Content-Type'] = 'application/json';
  }
  const ctl = new AbortController();
  const timer = setTimeout(() => ctl.abort(), o.timeout || 9000);
  init.signal = ctl.signal;
  let res;
  try {
    res = await fetch(path, init);
  } finally {
    clearTimeout(timer);
  }
  let data = null;
  try { data = await res.json(); } catch (e) { /* an empty or non-JSON answer */ }
  if (!res.ok || (data && data.ok === false)) {
    const err = new Error((data && data.error) || `CANary answered with an error (${res.status})`);
    err.status = res.status;
    throw err;
  }
  return data;
}

let toastTimer;
function toast(msg, bad) {
  const t = $('#toast');
  t.textContent = msg;
  t.className = 'toast' + (bad ? ' bad' : '');
  t.hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { t.hidden = true; }, bad ? 6500 : 2600);
}

/* Replaces markup only when it changed, so polling never flickers or drops a text selection. */
function setHTML(el, html) {
  if (el && el.__html !== html) {
    el.innerHTML = html;
    el.__html = html;
  }
}

/* ───────── numbers, units and time ───────── */

const IMPERIAL = {
  'km': ['mi', 0.621371, 0], 'km/h': ['mph', 0.621371, 0], 'kPa': ['psi', 0.1450377, -1],
  'L': ['gal', 0.2641721, 1], 'L/h': ['gal/h', 0.2641721, 1], 'Nm': ['lb-ft', 0.7375621, 0],
};

/* The device always speaks metric; the reader chooses what to see. "Δ" marks a difference rather than a level. */
function conv(v, unit, dec) {
  let u = unit || '';
  if (S.imperial) {
    if (u === '°C') return { v: v * 1.8 + 32, u: '°F', dec };
    if (u === 'Δ°C') return { v: v * 1.8, u: '°F', dec };
    const c = IMPERIAL[u];
    if (c) return { v: v * c[1], u: c[0], dec: c[2] < 0 ? (Math.abs(v * c[1]) >= 100 ? 0 : 1) : Math.min(2, dec + c[2]) };
  }
  if (u.charAt(0) === 'Δ') u = u.slice(1);
  return { v, u, dec };
}

function num(v, dec) {
  let r = Number(Number(v).toFixed(dec));
  if (r === 0) r = 0; /* never "-0" */
  return r.toLocaleString(undefined, { minimumFractionDigits: dec, maximumFractionDigits: dec });
}

function dur(s) {
  s = Math.max(0, Math.round(s));
  if (s < 90) return `${s} s`;
  const m = Math.floor(s / 60), r = s % 60;
  if (m < 10 && r >= 5) return `${m} min ${r} s`;
  if (s < 3600) return `${Math.round(s / 60)} min`;
  return `${Math.floor(m / 60)} h ${m % 60} min`;
}

function stopwatch(s) {
  s = Math.max(0, Math.round(s));
  const h = Math.floor(s / 3600), m = Math.floor(s % 3600 / 60), r = s % 60;
  return h ? `${h}:${String(m).padStart(2, '0')}:${String(r).padStart(2, '0')}` : `${m}:${String(r).padStart(2, '0')}`;
}

const tight = u => !u || u === '%' || u === '°';

/* A reading with its unit, in the reader's units: "25.0 kPa", "3.5°", "7 min". */
function val(v, unit, dec) {
  if (v == null || !isFinite(v)) return 'no reading';
  if (unit === 's') return dur(v);
  const c = conv(v, unit, dec == null ? 1 : dec);
  return num(c.v, c.dec) + (tight(c.u) ? '' : ' ') + c.u;
}

function valParts(v, unit, dec) {
  if (v == null || !isFinite(v)) return null;
  if (unit === 's') return { n: stopwatch(v), u: '' };
  const c = conv(v, unit, dec == null ? 1 : dec);
  return { n: num(c.v, c.dec), u: c.u };
}

const dist = (km, dec = 0) => val(km, 'km', dec);

function bytes(n) {
  if (n >= 1e9) return `${num(n / 1e9, 1)} GB`;
  if (n >= 1e6) return `${num(n / 1e6, 1)} MB`;
  if (n >= 1e3) return `${num(n / 1e3, 0)} KB`;
  return `${n} bytes`;
}

const known = unix => unix > 1600000000;
const dayStart = d => new Date(d.getFullYear(), d.getMonth(), d.getDate()).getTime();
const clock = unix => new Date(unix * 1000).toLocaleTimeString(undefined, { hour: 'numeric', minute: '2-digit' });

function when(unix) {
  if (!known(unix)) return 'Time not known';
  const d = new Date(unix * 1000);
  const days = Math.round((dayStart(new Date()) - dayStart(d)) / 86400000);
  if (days === 0) return `Today, ${clock(unix)}`;
  if (days === 1) return `Yesterday, ${clock(unix)}`;
  const opt = { weekday: 'short', day: 'numeric', month: 'short' };
  if (d.getFullYear() !== new Date().getFullYear()) opt.year = 'numeric';
  return `${d.toLocaleDateString(undefined, opt)}, ${clock(unix)}`;
}

/* The same, for the middle of a sentence. */
const whenIn = unix => when(unix).replace(/^(Today|Yesterday)/, w => w.toLowerCase());

function ago(unix) {
  const s = Date.now() / 1000 - unix;
  if (s < 90) return 'just now';
  if (s < 3600) return `${Math.round(s / 60)} min ago`;
  if (s < 86400 * 2) return `${Math.round(s / 3600)} h ago`;
  return `${Math.round(s / 86400)} days ago`;
}

const COUNT = ['No', 'One', 'Two', 'Three', 'Four', 'Five', 'Six', 'Seven', 'Eight', 'Nine'];
const count = n => COUNT[n] || String(n);
const plural = (n, one, many) => (n === 1 ? one : many || one + 's');

/* ───────── status vocabulary ───────── */

const SEV = {
  critical: ['crit', 'crit', 'Critical'], warning: ['warn', 'warn', 'Warning'], advisory: ['adv', 'info', 'Advisory'],
  ok: ['ok', 'check', 'Passed'], nodata: ['wait', 'wait', 'Waiting for data'], unavailable: ['off', 'dash', 'Not available'],
};
const RANK = { critical: 3, warning: 2, advisory: 1 };
const lamp = sev => {
  const s = SEV[sev] || SEV.nodata;
  return `<span class="lamp ${s[0]}" role="img" aria-label="${s[2]}">${icon(s[1])}</span>`;
};
const pill = (text, cls) => `<span class="pill${cls ? ' ' + cls : ''}">${esc(text)}</span>`;

const CAR_STATE = {
  running: 'Engine running', ignition_on: 'Ignition on', off: 'Car off',
  detecting: 'Looking for the car', identifying: 'Identifying the car', starting: 'Starting up',
};

async function dtcDb() {
  if (!S.dtc) {
    try {
      S.dtc = await api('/dtc.json' + (BUILD ? `?v=${BUILD}` : ''));
    } catch (e) {
      return { codes: {}, patterns: [], ranges: [] };
    }
  }
  return S.dtc;
}

/* What a code means: exact text when it is a standard code, otherwise the system it belongs to. */
function dtcText(db, code) {
  if (db.codes[code]) return { text: db.codes[code], exact: true };
  for (const p of db.patterns) {
    const m = new RegExp(p.re).exec(code);
    if (m) return { text: p.text.replace(/\{(\d)\}/g, (_, i) => m[+i]), exact: true };
  }
  for (const r of db.ranges) if (new RegExp(r.re).test(code)) return { text: r.text, exact: false };
  return { text: 'No description available', exact: false };
}

/* The line under a code: which control unit holds it, how it failed if the unit says, and what its state means. */
function codeNote(db, c, state) {
  const parts = [c.module || c.ecu || ''];
  const ftb = c.ftb ? (db.failure_types || {})[c.ftb.toString(16).toUpperCase().padStart(2, '0')] : '';
  if (ftb) parts.push(`Reported as: ${ftb}`);
  if (!dtcText(db, c.code).exact) parts.push('The car maker defines the exact fault for this code');
  if (state) parts.push(state.replace(/\.$/, ''));
  return `<span>${esc(parts.filter(Boolean).join('. '))}.</span>`;
}

/* ───────── frame: navigation, top bar, connection ───────── */

function renderFrame() {
  const st = S.st;
  if (!st) return;
  const vs = st.vehicle.state;
  const parts = [`<span><i class="dot ${vs === 'running' ? 'run' : vs === 'ignition_on' ? 'ign' : ''}"></i>${CAR_STATE[vs] || esc(vs)}</span>`];
  if (st.net.connected) parts.push(`<span class="opt">${icon('wifi')}${esc(st.net.ssid)}</span>`);
  else if (st.net.ap_active) parts.push(`<span class="opt">${icon('wifi')}Own network: ${esc(st.net.ap_ssid)}</span>`);
  if (st.sync.configured) {
    let s;
    if (st.sync.busy && st.sync.up_total > 0) s = `Uploading ${Math.round(100 * st.sync.up_done / st.sync.up_total)}%`;
    else if (st.sync.busy) s = 'Syncing';
    else if (st.sync.last_error) s = 'Sync failed';
    else if (known(st.sync.last_ok)) s = `Synced ${ago(st.sync.last_ok)}`;
    else s = 'Not synced yet';
    parts.push(`<a href="#/device" class="opt" style="text-decoration:none"><span>${icon('up')}${s}</span></a>`);
  }
  if (st.battery_v != null) parts.push(`<span>Battery ${num(st.battery_v, 1)} V</span>`);
  setHTML($('#topState'), parts.join(''));

  const foot = [`<strong>${esc(st.device.name)}</strong>`, esc(st.vehicle.profile.name)];
  if (st.vehicle.vin) foot.push(esc(st.vehicle.vin));
  foot.push(`Firmware ${esc(st.device.fw)}`);
  setHTML($('#railFoot'), foot.map(x => `<span>${x}</span>`).join(''));

  const att = $('#navAttention');
  att.hidden = !st.health.attention;
  att.textContent = st.health.attention;
  att.className = 'nav-count ' + (SEV[st.health.worst] || SEV.ok)[0];
  const codes = $('#navCodes');
  codes.hidden = !st.health.dtc_count;
  codes.textContent = st.health.dtc_count;
  codes.className = 'nav-count' + (st.health.mil ? ' warn' : '');
  document.title = st.device.name && st.device.name !== 'CANary' ? `${st.device.name} \u2013 CANary` : 'CANary';
}

function setBanner() {
  const b = $('#banner');
  let msg = '';
  if (S.away) msg = S.away;
  else if (S.fails >= 2) msg = 'Can’t reach CANary. It may be asleep because the car is off, or this device has left its network. Still trying.';
  b.hidden = !msg;
  if (msg) b.textContent = msg;
}

async function pollStatus() {
  try {
    const st = await api('/api/status', { timeout: 5000 });
    const first = !S.st;
    S.st = st;
    S.imperial = !!st.units_imperial;
    S.fails = 0;
    if (S.away && S.awayDone && S.awayDone(st)) return;
    if (!S.timeSent) {
      S.timeSent = true;
      api('/api/time', { body: { unix: Math.floor(Date.now() / 1000), tz_offset_min: -new Date().getTimezoneOffset() } }).catch(() => {});
    }
    renderFrame();
    if (first && page && page.tick) page.tick();
    if (page && page.onStatus) page.onStatus(st);
  } catch (e) {
    S.fails++;
  }
  setBanner();
}

/* After a restart or an update the device drops off for a while; wait for it and reload the page. */
function waitForRestart(message) {
  const bootBefore = S.st ? S.st.device.uptime_s : 0;
  let gone = false;
  S.away = message;
  S.awayDone = st => {
    if (!gone && st.device.uptime_s >= bootBefore) return false;
    location.reload();
    return true;
  };
  setBanner();
  const watch = setInterval(() => {
    if (S.fails > 0) gone = true;
  }, 500);
  setTimeout(() => { gone = true; clearInterval(watch); }, 20000);
}

/* ───────── limit gauge ───────── */

const decOf = x => (Number.isInteger(x) ? 0 : Number.isInteger(Math.round(x * 1e6) / 1e5) ? 1 : 2);

/* Shows where a reading sits against its thresholds. The scale is piecewise: the healthy range gets a fixed
   share and each alert band an equal share of the rest, so bands stay readable however the numbers are spaced.
   Checks that can fail in both directions get alert bands on both sides of the healthy range. */
function gauge(m, compact) {
  const lv = (m.levels || []).filter(l => l.value != null);
  if (!lv.length || m.value == null) return '';
  const hi = lv.filter(l => l.op.charAt(0) === 'g').sort((a, b) => a.value - b.value); /* mild to severe */
  const lo = lv.filter(l => l.op.charAt(0) === 'l').sort((a, b) => a.value - b.value); /* severe to mild */
  const stepOf = a => (a.length > 1 ? (a[a.length - 1].value - a[0].value) / (a.length - 1) : Math.max(Math.abs(a[0].value) * 0.25, 1e-6));
  const v = m.value, both = lo.length && hi.length;
  let okFrom, okTo;
  if (both) {
    okFrom = lo[lo.length - 1].value;
    okTo = hi[0].value;
  } else if (hi.length) {
    const first = hi[0].value, step = stepOf(hi);
    okTo = first;
    okFrom = first > 0 && first / step <= 6 ? 0 : first - 3 * step;
    if (v < okFrom) okFrom = v - step * 0.25;
  } else {
    const first = lo[lo.length - 1].value, step = stepOf(lo);
    okFrom = first;
    okTo = first < 0 && -first / step <= 6 ? 0 : first + 3 * step;
    if (v > okTo) okTo = v + step * 0.25;
  }
  const okW = both ? 40 : 46, sideW = both ? 30 : 54;
  const stops = [], zones = [], ticks = [];
  const tdec = Math.max.apply(null, lv.map(l => decOf(l.value)));
  let x = 0;
  if (lo.length) {
    const w = sideW / lo.length;
    stops.push([lo[0].value - stepOf(lo), 0]);
    lo.forEach(l => {
      zones.push([w, l.severity]);
      x += w;
      stops.push([l.value, x]);
      ticks.push({ at: x, label: val(l.value, m.unit, tdec), sev: l.severity, word: 'below' });
    });
  } else {
    stops.push([okFrom, 0]);
  }
  zones.push([okW, 'ok']);
  x += okW;
  if (hi.length) {
    const w = sideW / hi.length;
    hi.forEach(h => {
      stops.push([h.value, x]);
      ticks.push({ at: x, label: val(h.value, m.unit, tdec), sev: h.severity, word: 'above' });
      zones.push([w, h.severity]);
      x += w;
    });
    stops.push([hi[hi.length - 1].value + stepOf(hi), 100]);
  } else {
    stops.push([okTo, 100]);
  }
  let pos = v <= stops[0][0] ? 0 : 100;
  for (let i = 1; i < stops.length; i++) {
    if (v <= stops[i][0]) {
      const a = stops[i - 1], b = stops[i];
      pos = v <= a[0] ? a[1] : a[1] + (b[1] - a[1]) * (v - a[0]) / ((b[0] - a[0]) || 1);
      break;
    }
  }
  pos = Math.max(1, Math.min(99, pos));
  const spoken = `Reading ${val(v, m.unit, m.dec)}. ` + ticks.map(k => `${SEV[k.sev][2]} ${k.word} ${k.label}`).join(', ') + '.';
  const bar = `<div class="limit" role="img" aria-label="${esc(spoken)}"><div class="track">${zones.map(z => `<i class="zone ${SEV[z[1]][0]}" style="width:${z[0]}%"></i>`).join('')}</div><i class="needle" style="left:${pos.toFixed(1)}%"></i></div>`;
  if (compact) return bar;
  return `<div class="limit-wrap">${bar}<div class="limit-ticks" aria-hidden="true">${ticks.map(k => `<span style="left:${k.at.toFixed(1)}%">${esc(k.label)}</span>`).join('')}</div></div>`;
}

/* The sentence for a check, re-rendered from its template so the figure appears in the reader's units. */
function say(m) {
  if (m.tpl && m.value != null) return m.tpl.replace('{value}', val(m.value, m.unit, m.dec));
  return m.text || '';
}

/* A finding that needs attention: what, how much, where it sits against its limits, and on request why. */
function findingOpen(m, open) {
  const causes = (m.causes || []).length
    ? `<div><div class="causes-title">Common causes</div><ul class="causes">${m.causes.map(c => `<li>${esc(c)}</li>`).join('')}</ul></div>` : '';
  const why = m.detail || causes
    ? `<details class="why" data-why="${esc(m.id)}"${open ? ' open' : ''}><summary>${icon('chev')}What it means and common causes</summary>${m.detail ? `<p>${esc(m.detail)}</p>` : ''}${causes}</details>` : '';
  return `<article class="finding open">${lamp(m.status)}<h3>${esc(m.headline || m.title)}</h3>
    <div class="value">${m.value != null ? esc(val(m.value, m.unit, m.dec)) : ''}</div>
    <p class="say">${esc(say(m))}</p><div class="more">${gauge(m)}${why}</div></article>`;
}

/* ───────── Health ───────── */

const Health = {
  every: 3000,
  enter() {
    view.innerHTML = '<div id="hBody"></div>';
    this.tick();
  },
  async tick() {
    if (!S.st) return;
    let f, d, db;
    try {
      [f, d, db] = await Promise.all([api('/api/findings'), api('/api/dtcs'), dtcDb()]);
    } catch (e) {
      return;
    }
    const el = $('#hBody');
    if (el) setHTML(el, this.render(S.st, f, d, db));
  },
  render(st, f, d, db) {
    const mons = f.monitors;
    const bad = mons.filter(m => RANK[m.status]).sort((a, b) => RANK[b.status] - RANK[a.status]);
    const passed = mons.filter(m => m.status === 'ok');
    const waiting = mons.filter(m => m.status === 'nodata');
    const absent = mons.filter(m => m.status === 'unavailable');
    const codeSev = d.codes.length ? d.severity : 'ok';
    const things = bad.length + (RANK[codeSev] ? 1 : 0);
    const worst = [codeSev].concat(bad.map(m => m.status)).reduce((a, b) => ((RANK[b] || 0) > (RANK[a] || 0) ? b : a), 'ok');
    const vs = st.vehicle.state;
    const drive = f.trip_active ? 'so far on this drive' : 'on the last drive';

    let cls = 'none', glyph = 'nodata', head, sub;
    if (things) {
      cls = SEV[worst][0];
      glyph = worst;
      const top = bad[0] && RANK[bad[0].status] >= (RANK[codeSev] || 0) ? (bad[0].headline || bad[0].title) : `${d.codes.length} trouble ${plural(d.codes.length, 'code')} stored`;
      if (worst === 'advisory') {
        head = `${count(things)} ${plural(things, 'thing')} to keep an eye on`;
        sub = 'Nothing needs doing yet. CANary keeps watching and will raise it if it gets worse.';
      } else {
        head = `${count(things)} ${plural(things, 'thing needs', 'things need')} attention`;
        sub = worst === 'critical'
          ? `Most serious: ${top}. Have it looked at before driving much further.`
          : 'Nothing looks urgent, but it is worth having checked soon.';
      }
    } else if (passed.length) {
      cls = 'ok';
      glyph = 'ok';
      head = 'All clear';
      sub = `${passed.length === 1 ? 'The one check' : `All ${passed.length} checks`} completed ${drive} passed.`
        + (waiting.length ? ` ${count(waiting.length)} more ${plural(waiting.length, 'is', 'are')} still collecting data.` : '');
    } else if (vs === 'running' || vs === 'ignition_on') {
      head = 'Collecting data';
      sub = 'The first results appear after a few minutes of driving.';
    } else if (vs === 'off') {
      head = 'No drive checked yet';
      sub = 'CANary checks the engine while it runs. Take the car for a drive, then look again.';
    } else {
      head = 'Looking for the car';
      sub = 'Plug CANary into the diagnostic port and switch the ignition on.';
    }

    const odo = st.odometer_km != null ? dist(st.odometer_km) : 'Not read yet';
    const facts = [
      ['Checks passed', passed.length + bad.length ? `${passed.length} of ${passed.length + bad.length + waiting.length}` : 'None yet'],
      ['Trouble codes', d.codes.length ? String(d.codes.length) : 'None'],
      ['Odometer', odo],
      ['Battery', st.battery_v != null ? `${num(st.battery_v, 1)} V` : 'No reading'],
    ];
    let html = `<section class="verdict ${cls}">${lamp(glyph)}<h1>${esc(head)}</h1><p>${esc(sub)}</p>
      <div class="verdict-facts">${facts.map(x => `<div><b>${esc(x[1])}</b>${x[0]}</div>`).join('')}</div></section>`;

    if (things) {
      let rows = '';
      const codeRow = () => {
        const names = d.codes.slice(0, 4).map(c => `${c.code} ${dtcText(db, c.code).text}`).join('; ') + (d.codes.length > 4 ? '; and more' : '');
        return `<article class="finding open">${lamp(codeSev)}<h3>${d.codes.length} trouble ${plural(d.codes.length, 'code')} stored</h3><div class="value"></div>
          <p class="say">${esc(names)}</p><div class="more"><div><a class="btn small" href="#/codes">See trouble codes</a></div></div></article>`;
      };
      let codeDone = !RANK[codeSev];
      for (const m of bad) {
        if (!codeDone && RANK[codeSev] > RANK[m.status]) {
          rows += codeRow();
          codeDone = true;
        }
        rows += findingOpen(m, this.why[m.id] != null ? this.why[m.id] : m === bad[0]);
      }
      if (!codeDone) rows += codeRow();
      html += `<h2 class="group-title">Needs attention</h2><div class="sheet">${rows}</div>`;
    }
    html += this.setup(st);
    if (passed.length) {
      html += `<h2 class="group-title">Passed <small>${passed.length} ${plural(passed.length, 'check')} ${drive}</small></h2><div class="sheet">`
        + passed.map(m => `<article class="finding compact">${lamp('ok')}<h3>${esc(m.title)}</h3><p class="say">${esc(say(m))}</p>${gauge(m, true)}</article>`).join('') + '</div>';
    }
    if (waiting.length) {
      html += `<details class="fold"${this.open.waiting ? ' open' : ''} data-fold="waiting"><summary>${icon('chev')}Waiting for data <small>${waiting.length} ${plural(waiting.length, 'check')}</small></summary><div class="sheet">`
        + waiting.map(m => {
          const seen = m.seen_s > 0 ? `${dur(m.seen_s)} so far.` : 'None yet on this drive.';
          return `<article class="finding compact wide">${lamp('nodata')}<h3>${esc(m.title)}</h3><p class="say">Needs ${dur(m.need_s)} of ${esc(m.needs || 'the right driving conditions')}. ${seen}</p></article>`;
        }).join('') + '</div></details>';
    }
    if (absent.length) {
      html += `<details class="fold"${this.open.absent ? ' open' : ''} data-fold="absent"><summary>${icon('chev')}Not available on this car <small>${absent.length} ${plural(absent.length, 'check')}</small></summary><div class="sheet">`
        + absent.map(m => `<article class="finding compact wide">${lamp('unavailable')}<h3>${esc(m.title)}</h3><p class="say">${m.why ? `This car does not report: ${esc(m.why)}.` : 'Not supported by this car.'}</p></article>`).join('')
        + '</div></details>';
    }
    return html;
  },
  open: { waiting: false, absent: false },
  why: {},
  /* What is still missing before CANary can do its whole job. Disappears once both are done. */
  setup(st) {
    const rows = [];
    if (!st.net.connected) rows.push(['Join your home Wi-Fi', 'CANary syncs whenever the car comes home.']);
    if (!st.sync.configured) rows.push(['Connect to Glovebox', 'It keeps the odometer up to date and turns your drives into a weekly report.']);
    if (!rows.length) return '';
    return `<h2 class="group-title">Finish setting up</h2><div class="sheet">${rows.map(r => `<div class="setup">${lamp('nodata')}<div><b>${r[0]}</b><span>${r[1]}</span></div><a class="btn small" href="#/device">Set up</a></div>`).join('')}</div>`;
  },
};
view.addEventListener('toggle', e => {
  const d = e.target.dataset || {};
  if (page !== Health) return;
  if (d.fold) Health.open[d.fold] = e.target.open;
  if (d.why) Health.why[d.why] = e.target.open;
}, true);

/* ───────── Live data ───────── */

const GROUPS = [['engine', 'Engine'], ['boost', 'Boost'], ['fuel', 'Fuel'], ['ignition', 'Ignition'], ['valve timing', 'Valve timing'],
  ['temperatures', 'Temperatures'], ['lubrication', 'Lubrication'], ['electrical', 'Electrical'], ['status', 'Status']];

function sparkPaths(hist) {
  const pts = hist.filter(x => x != null);
  if (pts.length < 2) return ['', ''];
  let lo = Math.min.apply(null, pts), hi = Math.max.apply(null, pts);
  if (hi - lo < 1e-9) { lo -= 1; hi += 1; }
  const W = 150, H = 34, pad = 4, n = pts.length;
  const xy = pts.map((p, i) => [(i / (n - 1)) * W, H - pad - ((p - lo) / (hi - lo)) * (H - pad * 2)]);
  const line = xy.map((p, i) => `${i ? 'L' : 'M'}${p[0].toFixed(1)} ${p[1].toFixed(1)}`).join('');
  return [line, `${line}L${W} ${H}L0 ${H}Z`];
}

const Live = {
  every: 2000,
  filter: '',
  all: false,
  shape: '',
  enter() {
    view.innerHTML = `<h1 class="page-title">Live data</h1>
      <p class="page-sub" id="lvSub"></p>
      <div class="toolbar"><input class="field" id="lvFind" type="search" placeholder="Find a reading" aria-label="Find a reading" value="${esc(this.filter)}">
        <label class="check"><input type="checkbox" id="lvAll"${this.all ? ' checked' : ''}>Include readings this car does not report</label></div>
      <div id="lvBody"></div>`;
    this.shape = '';
    $('#lvFind').addEventListener('input', e => { this.filter = e.target.value.trim().toLowerCase(); this.shape = ''; this.tick(); });
    $('#lvAll').addEventListener('change', e => { this.all = e.target.checked; this.shape = ''; this.tick(); });
    this.tick();
  },
  async tick() {
    if (!S.st) return;
    let d;
    try {
      d = await api('/api/signals?hist=1');
    } catch (e) {
      return;
    }
    const body = $('#lvBody');
    if (!body) return;
    const vs = S.st.vehicle.state, st = S.st.vehicle;
    const sub = vs === 'running' || vs === 'ignition_on'
      ? `Reading ${st.signals_live} values from the car, the fastest several times a second.${st.learning ? ` Still working out what this car answers (${st.learning} requests left to try).` : ''}`
      : 'The car is off, so nothing is being read. Values appear when the ignition is switched on.';
    $('#lvSub').textContent = sub;

    const q = this.filter;
    const list = d.signals.filter(s => (this.all || s.available) && (!q || s.name.toLowerCase().includes(q) || s.id.includes(q)));
    const shape = d.profile + '|' + S.imperial + '|' + list.map(s => s.id + (s.available ? '' : '!')).join(',');
    if (shape !== this.shape) {
      this.shape = shape;
      let html = '';
      const order = GROUPS.map(g => g[0]);
      const names = Object.fromEntries(GROUPS);
      const groups = {};
      list.forEach(s => { const g = order.includes(s.group) ? s.group : 'other'; (groups[g] = groups[g] || []).push(s); });
      order.concat('other').forEach(g => {
        const rows = groups[g];
        if (!rows) return;
        html += `<h2 class="group-title">${names[g] || 'Other'}</h2><div class="sheet">` + rows.map(s => `<div class="sig${s.available ? '' : ' na'}" data-id="${esc(s.id)}">
          <div class="name">${esc(s.name)}<span>${s.available ? esc(s.source) : 'Not reported by this car'}</span></div>
          <svg class="spark" viewBox="0 0 150 34" preserveAspectRatio="none" aria-hidden="true"><path class="fillp"/><path class="ln"/></svg>
          <div class="val"></div></div>`).join('') + '</div>';
      });
      if (!list.length) html = `<div class="sheet empty"><h3>Nothing matches</h3>${q ? 'Try a shorter word, or include the readings this car does not report.' : 'No readings yet. They appear once CANary has identified the car.'}</div>`;
      body.innerHTML = html;
    }
    const rows = {};
    $$('.sig', body).forEach(r => { rows[r.dataset.id] = r; });
    list.forEach(s => {
      const row = rows[s.id];
      if (!row) return;
      const p = s.available ? valParts(s.v, s.unit, s.dec) : null;
      setHTML($('.val', row), p ? `${esc(p.n)}${p.u ? `<small>${esc(p.u)}</small>` : ''}` : (s.available ? '<span class="idle">waiting</span>' : ''));
      const paths = s.hist && s.hist.length > 1 ? sparkPaths(s.hist) : ['', ''];
      const spark = $('.spark', row);
      $('.ln', spark).setAttribute('d', paths[0]);
      $('.fillp', spark).setAttribute('d', paths[1]);
      if (s.min != null) row.title = `Lowest ${val(s.min, s.unit, s.dec)}, highest ${val(s.max, s.unit, s.dec)} since the engine started`;
    });
  },
};

/* ───────── Trouble codes ───────── */

const Codes = {
  every: 5000,
  enter() {
    view.innerHTML = '<h1 class="page-title">Trouble codes</h1><div id="cdBody"></div>';
    this.tick();
  },
  async tick() {
    if (!S.st) return;
    let d, db;
    try {
      [d, db] = await Promise.all([api('/api/dtcs'), dtcDb()]);
    } catch (e) {
      return;
    }
    const el = $('#cdBody');
    if (!el) return;
    const mods = d.modules.length;
    const readAt = known(d.read_at) ? ` Last read ${whenIn(d.read_at)}.` : '';
    let html = `<p class="page-sub">${d.read_at ? `CANary asks ${mods} control ${plural(mods, 'unit')} for stored faults at the start of every drive and every few minutes after.${readAt}` : 'Codes are read when the ignition is switched on.'}</p>`;
    html += `<div class="sheet mil ${d.mil ? 'on' : ''}">${lamp(d.mil ? 'warning' : d.read_at ? 'ok' : 'nodata')}<div><b>${d.mil ? 'The check engine light is on' : d.read_at ? 'The check engine light is off' : 'Not read yet'}</b>
      <span>${d.mil ? 'The engine computer has confirmed a fault that affects emissions.' : d.read_at ? 'The engine computer is not asking for attention.' : 'Switch the ignition on and CANary will read the codes.'}</span></div></div>`;
    if (d.codes.length) {
      const order = { stored: 0, permanent: 1, pending: 2 };
      const codes = d.codes.slice().sort((a, b) => (order[a.kind] || 0) - (order[b.kind] || 0) || a.code.localeCompare(b.code));
      html += `<h2 class="group-title">${codes.length} ${plural(codes.length, 'code')}</h2><div class="sheet">` + codes.map(c => {
        const t = dtcText(db, c.code);
        const kind = c.kind === 'pending' ? ['Pending', 'adv', 'Seen once. It becomes a stored code if it happens again.']
          : c.kind === 'permanent' ? ['Permanent', '', 'Stays until the car has re-tested itself and passed.']
            : ['Stored', 'warn', 'Confirmed by the control unit.'];
        return `<div class="code">${lamp(c.kind === 'pending' ? 'advisory' : c.kind === 'permanent' ? 'nodata' : 'warning')}<div class="id">${esc(c.code)}</div>
          <div class="what">${esc(t.text)}${codeNote(db, c, kind[2])}</div>${pill(kind[0], kind[1])}</div>`;
      }).join('') + '</div>';
    } else if (d.read_at) {
      html += `<div class="sheet empty" style="margin-top:14px"><h3>No trouble codes</h3>None of the ${mods} control ${plural(mods, 'unit')} CANary asked has a fault stored.</div>`;
    }
    if (d.read_at) html += `<p class="footnote">Asked: ${d.modules.map(esc).join(', ')}.</p>`;
    html += '<p class="footnote">CANary only reads. It never clears codes, because clearing them also erases what a mechanic needs to find the cause.</p>';
    setHTML(el, html);
  },
};

/* ───────── Trips ───────── */

function uploadPill(t, st) {
  if (t.uploaded_at > 0) return pill(t.raw ? 'Uploaded' : 'Uploaded, removed here', 'ok');
  if (st && st.sync.uploading === t.id && st.sync.up_total > 0) return pill(`Uploading ${Math.round(100 * st.sync.up_done / st.sync.up_total)}%`, 'adv');
  if (!t.raw) return pill('Summary only');
  if (st && st.sync.configured && st.sync.mode !== 'lubelogger') return pill('Waiting to upload');
  return pill('On this device');
}

const Trips = {
  every: 4000,
  enter() {
    view.innerHTML = '<h1 class="page-title">Trips</h1><p class="page-sub" id="trSub"></p><div id="trBody"></div>';
    this.tick();
  },
  async tick() {
    if (!S.st) return;
    let d;
    try {
      d = await api('/api/trips');
    } catch (e) {
      return;
    }
    const el = $('#trBody');
    if (!el) return;
    const st = S.st;
    $('#trSub').textContent = !st.storage.present
      ? 'No SD card found. Drives are still checked, but nothing is recorded until a card is inserted.'
      : !st.storage.logging ? 'Recording is switched off in the device settings.'
        : `Every drive is recorded to the SD card. Raw data is removed ${d.retention_days} ${plural(d.retention_days, 'day')} after Glovebox has received it; the summary stays.`;
    let html = '';
    if (st.trip.active) {
      html += `<div class="sheet"><a class="trip" href="#/trips/${encodeURIComponent(st.trip.id)}">${`<span class="lamp rec" role="img" aria-label="Recording"><i></i></span>`}
        <div class="when">Driving now<span>${st.trip.recording ? 'Recording' : 'Not recording'}, ${bytes(st.trip.bytes)}</span></div>
        <div class="num">${esc(dist(st.trip.distance_km, 1))}<span>distance</span></div><div class="num opt">${dur(st.trip.duration_s)}<span>so far</span></div><div class="num opt"></div>${pill('In progress', 'adv')}</a></div>`;
    }
    const trips = d.trips.filter(t => t.id !== d.active);
    if (trips.length) {
      html += `<h2 class="group-title">${trips.length} recorded ${plural(trips.length, 'drive')}</h2><div class="sheet">` + trips.map(t => {
        const sev = RANK[t.worst] ? t.worst : 'ok';
        return `<a class="trip" href="#/trips/${encodeURIComponent(t.id)}">${lamp(sev)}
          <div class="when">${esc(when(t.start))}<span>${t.findings ? `${t.findings} ${plural(t.findings, 'finding')}` : 'All checks passed'}<i class="only-narrow">, ${esc(dist(t.distance_km, 1))} in ${dur(t.duration_s)}</i></span></div>
          <div class="num opt">${esc(dist(t.distance_km, 1))}<span>distance</span></div><div class="num opt">${dur(t.duration_s)}<span>duration</span></div>
          <div class="num opt">${esc(val(t.max_speed, 'km/h', 0))}<span>top speed</span></div>${uploadPill(t, st)}</a>`;
      }).join('') + '</div>';
    } else if (!st.trip.active) {
      html += '<div class="sheet empty" style="margin-top:16px"><h3>No drives recorded yet</h3>A drive is saved once the engine has run for more than 20 seconds.</div>';
    }
    setHTML(el, html);
  },
};

/* Nice round axis steps. */
function niceStep(span, target) {
  const raw = span / target, mag = Math.pow(10, Math.floor(Math.log10(raw))), f = raw / mag;
  return (f < 1.5 ? 1 : f < 3 ? 2 : f < 7 ? 5 : 10) * mag;
}

const CHART_DEFAULT = ['speed', 'rpm', 'boost', 'coolant_temp', 'fuel_trim_b1', 'vbat'];

const Trip = {
  every: 0,
  id: null,
  sum: null,
  picked: null,
  series: [],
  async enter(id) {
    this.id = id;
    this.sum = null;
    this.series = [];
    view.innerHTML = `<a class="back" href="#/trips">${icon('chev')}Trips</a><div id="tdBody"><p class="muted" style="margin-top:16px">Loading the drive</p></div>`;
    const live = S.st && S.st.trip.active && S.st.trip.id === id;
    try {
      if (!S.meta) {
        const sig = await api('/api/signals');
        S.meta = Object.fromEntries(sig.signals.map(s => [s.id, s]));
      }
      await dtcDb();
      this.sum = live ? null : await api(`/api/trips/${encodeURIComponent(id)}`);
    } catch (e) {
      if (!live) {
        $('#tdBody').innerHTML = `<div class="sheet empty" style="margin-top:16px"><h3>That drive is not on the device</h3>${esc(e.message)}</div>`;
        return;
      }
    }
    if (this.id !== id) return;
    const have = this.sum ? Object.keys(this.sum.signals || {}).filter(k => this.sum.signals[k].n > 1) : Object.keys(S.meta).filter(k => S.meta[k].available);
    this.have = have;
    if (!this.picked) this.picked = CHART_DEFAULT.slice();
    this.render();
    this.load();
    if (live) this.every = 5000;
    else this.every = 0;
  },
  tick() {
    if (!this.sum) this.load();
  },
  render() {
    const s = this.sum, st = S.st, db = S.dtc || { codes: {}, patterns: [], ranges: [] };
    let html;
    if (s) {
      const avg = s.duration_s > 0 ? s.distance_km / (s.duration_s / 3600) : 0;
      const stats = [[dist(s.distance_km, 1), 'Distance'], [dur(s.duration_s), 'Duration'], [val(s.max_speed, 'km/h', 0), 'Top speed'], [val(avg, 'km/h', 0), 'Average speed']];
      if (s.fuel_l > 0) stats.push([val(s.fuel_l, 'L', 1), 'Fuel used']);
      if (s.odo_end_km > 0) stats.push([dist(s.odo_end_km), 'Odometer']);
      html = `<h1 class="page-title">${esc(when(s.start))}</h1><p class="page-sub">${esc(s.vin || 'Unknown car')}, recorded with the ${esc((S.st && S.st.vehicle.profile.id === s.profile ? S.st.vehicle.profile.name : s.profile) || 'default')} profile.</p>
        <div class="stats" style="margin-top:16px">${stats.map(x => `<div><b>${esc(x[0])}</b>${x[1]}</div>`).join('')}</div>`;
      const bad = (s.findings || []).filter(m => RANK[m.status]).sort((a, b) => RANK[b.status] - RANK[a.status]);
      const ok = (s.findings || []).filter(m => m.status === 'ok');
      if (bad.length) {
        html += `<h2 class="group-title">Findings on this drive</h2><div class="sheet">${bad.map(m => `<article class="finding open">${lamp(m.status)}<h3>${esc(m.headline || m.title)}</h3>
          <div class="value">${esc(val(m.value, m.unit, m.dec))}</div><p class="say">${esc(say(m))}</p></article>`).join('')}</div>`;
      }
      if ((s.dtcs || []).length) {
        html += `<h2 class="group-title">Trouble codes during this drive</h2><div class="sheet">${s.dtcs.map(c => `<div class="code">${lamp(c.kind === 'pending' ? 'advisory' : 'warning')}<div class="id">${esc(c.code)}</div>
          <div class="what">${esc(dtcText(db, c.code).text)}${codeNote(db, c, '')}</div>${pill(c.kind === 'pending' ? 'Pending' : c.kind === 'permanent' ? 'Permanent' : 'Stored', c.kind === 'pending' ? 'adv' : 'warn')}</div>`).join('')}</div>`;
      }
      if (ok.length) {
        html += `<details class="fold"><summary>${icon('chev')}Passed <small>${ok.length} ${plural(ok.length, 'check')}</small></summary><div class="sheet">${ok.map(m => `<article class="finding compact wide">${lamp('ok')}<h3>${esc(m.title)}</h3><p class="say">${esc(say(m))}</p></article>`).join('')}</div></details>`;
      } else if (!bad.length) {
        html += '<p class="footnote">This drive was too short for any check to finish.</p>';
      }
    } else {
      html = `<h1 class="page-title">Driving now</h1><p class="page-sub">${st ? `${dur(st.trip.duration_s)} and ${dist(st.trip.distance_km, 1)} so far. The charts update as the drive goes on.` : ''}</p>`;
    }
    const raw = s ? !s.raw_deleted && s.bytes > 0 : true;
    html += '<h2 class="group-title">Recorded data</h2>';
    if (raw) {
      html += `<div class="sheet"><div class="chips" id="tdChips"></div><div id="tdCharts" class="charts"><p class="muted" style="padding:16px 18px">Loading charts</p></div>
        <div class="section-foot"><a class="btn small" href="/api/trips/${encodeURIComponent(this.id)}/log" download>${icon('down')}Download the raw log${s ? ` (${bytes(s.bytes)})` : ''}</a>
        ${s ? uploadPill({ id: s.id, uploaded_at: s.uploaded_at, raw: true }, st) : ''}</div></div>`;
    } else {
      html += `<div class="sheet empty"><h3>Raw data is no longer on the device</h3>It was removed after Glovebox received it${known(s.uploaded_at) ? ` on ${new Date(s.uploaded_at * 1000).toLocaleDateString(undefined, { day: 'numeric', month: 'long' })}` : ''}. The full recording is in Glovebox.</div>`;
    }
    $('#tdBody').innerHTML = html;
    this.chips();
  },
  /* The readings on the charts, each removable, and a list to add another from. */
  chips() {
    const el = $('#tdChips');
    if (!el) return;
    const name = id => (S.meta[id] ? S.meta[id].name : id);
    const shown = this.have.filter(id => this.picked.includes(id));
    const rest = this.have.filter(id => !this.picked.includes(id));
    const groups = {};
    rest.forEach(id => { const g = (S.meta[id] && S.meta[id].group) || 'other'; (groups[g] = groups[g] || []).push(id); });
    const label = Object.fromEntries(GROUPS);
    const opts = Object.keys(groups).map(g => `<optgroup label="${esc(label[g] || 'Other')}">${groups[g].map(id => `<option value="${esc(id)}">${esc(name(id))}</option>`).join('')}</optgroup>`).join('');
    el.innerHTML = shown.map(id => `<button class="chip" data-sig="${esc(id)}" aria-label="Remove ${esc(name(id))} from the charts">${esc(name(id))}<span aria-hidden="true">\u00D7</span></button>`).join('')
      + (rest.length ? `<select class="chip add" id="tdAdd" aria-label="Add a reading to the charts"><option value="">Add a reading</option>${opts}</select>` : '');
    el.onclick = e => {
      const b = e.target.closest('button.chip');
      if (!b) return;
      this.picked = this.picked.filter(id => id !== b.dataset.sig);
      this.chips();
      this.load();
    };
    const add = $('#tdAdd');
    if (add) {
      add.onchange = () => {
        if (!add.value) return;
        this.picked.push(add.value);
        this.chips();
        this.load();
      };
    }
  },
  async load() {
    const box = $('#tdCharts');
    if (!box) return;
    const want = this.have.filter(id => this.picked.includes(id));
    if (!want.length) {
      box.innerHTML = '<p class="muted" style="padding:16px 18px">Pick a reading above to chart it.</p>';
      return;
    }
    const points = Math.max(120, Math.min(600, Math.round(box.clientWidth / 1.5)));
    const id = this.id, out = [];
    try {
      for (let i = 0; i < want.length; i += 6) {
        const d = await api(`/api/trips/${encodeURIComponent(id)}/series?points=${points}&sig=${want.slice(i, i + 6).join(',')}`, { timeout: 20000 });
        d.series.forEach(s => out.push(s));
        this.duration = d.duration_s;
      }
    } catch (e) {
      if (this.id === id) box.innerHTML = `<p class="muted" style="padding:16px 18px">${esc(e.message)}</p>`;
      return;
    }
    if (this.id !== id) return;
    this.series = out.filter(s => s.found && s.t.length > 1);
    this.draw();
  },
  draw() {
    const box = $('#tdCharts');
    if (!box) return;
    if (!this.series.length) {
      box.innerHTML = '<p class="muted" style="padding:16px 18px">Nothing was recorded for those readings on this drive.</p>';
      return;
    }
    const W = Math.max(280, box.clientWidth - 36), H = 136, L = 46, top = 14, bot = 6, T = Math.max(this.duration || 1, 1);
    const x = t => L + (t / T) * (W - L);
    const tstep = niceStep(T / 60, Math.max(3, Math.floor((W - L) / 110))) * 60;
    box.innerHTML = this.series.map((s, k) => {
      const meta = S.meta[s.id] || { dec: 1 };
      const c0 = conv(0, s.unit, meta.dec), f = v => conv(v, s.unit, meta.dec).v;
      const lo = s.lo.map(f), hi = s.hi.map(f), v = s.v.map(f);
      let mn = Math.min.apply(null, lo), mx = Math.max.apply(null, hi);
      const flat = mx - mn < 1e-9;
      const step = flat ? 1 : niceStep(mx - mn, 3);
      const y0 = flat ? mn - 1 : Math.floor(mn / step) * step, y1 = flat ? mx + 1 : Math.ceil(mx / step) * step;
      const y = val_ => top + (1 - (val_ - y0) / (y1 - y0)) * (H - top - bot);
      const gap = (T / s.t.length) * 4;
      let line = '', band = '', seg = [];
      const flush = () => {
        if (seg.length > 1) {
          line += seg.map((i, j) => `${j ? 'L' : 'M'}${x(s.t[i]).toFixed(1)} ${y(v[i]).toFixed(1)}`).join('');
          band += seg.map((i, j) => `${j ? 'L' : 'M'}${x(s.t[i]).toFixed(1)} ${y(hi[i]).toFixed(1)}`).join('')
            + seg.slice().reverse().map(i => `L${x(s.t[i]).toFixed(1)} ${y(lo[i]).toFixed(1)}`).join('') + 'Z';
        }
        seg = [];
      };
      s.t.forEach((t, i) => {
        if (i && t - s.t[i - 1] > gap) flush();
        seg.push(i);
      });
      flush();
      let grid = '';
      const gdec = step >= 1 ? 0 : step >= 0.1 ? 1 : 2;
      for (let g = y0; g <= y1 + step * 1e-6; g += step) {
        grid += `<line class="grid" x1="${L}" x2="${W}" y1="${y(g).toFixed(1)}" y2="${y(g).toFixed(1)}"/><text x="${L - 8}" y="${(y(g) + 4).toFixed(1)}" text-anchor="end">${esc(num(g, gdec))}</text>`;
      }
      let axis = '';
      if (k === this.series.length - 1) {
        for (let t = 0; t <= T; t += tstep) axis += `<text x="${x(t).toFixed(1)}" y="${H + 14}" text-anchor="${x(t) > W - 24 ? 'end' : 'middle'}">${stopwatch(t)}</text>`;
      }
      const summary = flat ? `${num(mn, c0.dec)}${tight(c0.u) ? '' : ' '}${c0.u} throughout` : `${num(mn, c0.dec)} to ${num(mx, c0.dec)}${tight(c0.u) ? '' : ' '}${c0.u}`;
      return `<div class="chart" data-k="${k}"><div class="chart-head"><span>${esc(s.name)}</span><small data-sum="${esc(summary)}">${esc(summary)}</small></div>
        <svg viewBox="0 0 ${W} ${H + (axis ? 18 : 0)}" style="height:${H + (axis ? 18 : 0)}px" role="img" aria-label="${esc(s.name)} over the drive, ${esc(summary)}">
        ${grid}<path class="band" d="${band}"/><path class="line" d="${line}"/><line class="cross" y1="${top}" y2="${H - bot}" x1="0" x2="0" visibility="hidden"/>${axis}</svg></div>`;
    }).join('');
    const charts = $$('.chart', box);
    const move = e => {
      const r = $('svg', charts[0]).getBoundingClientRect();
      const t = Math.max(0, Math.min(T, ((e.clientX - r.left - L) / (r.width - L)) * T));
      charts.forEach((ch, k) => {
        const s = this.series[k], meta = S.meta[s.id] || { dec: 1 };
        let best = 0;
        for (let i = 1; i < s.t.length; i++) if (Math.abs(s.t[i] - t) < Math.abs(s.t[best] - t)) best = i;
        const cross = $('.cross', ch);
        cross.setAttribute('x1', x(s.t[best]).toFixed(1));
        cross.setAttribute('x2', x(s.t[best]).toFixed(1));
        cross.setAttribute('visibility', 'visible');
        $('small', ch).textContent = `${val(s.v[best], s.unit, meta.dec)} at ${stopwatch(s.t[best])}`;
      });
    };
    const leave = () => charts.forEach(ch => {
      $('.cross', ch).setAttribute('visibility', 'hidden');
      const sm = $('small', ch);
      sm.textContent = sm.dataset.sum;
    });
    box.onpointermove = move;
    box.onpointerdown = move;
    box.onpointerleave = leave;
  },
  leave() {
    this.id = null;
  },
};
let resizeTimer;
window.addEventListener('resize', () => {
  clearTimeout(resizeTimer);
  resizeTimer = setTimeout(() => { if (page === Trip && Trip.series.length) Trip.draw(); }, 150);
});

/* ───────── Device ───────── */

const WHY_AWAKE = {
  'sleep is turned off': 'Sleep is switched off below',
  'this board cannot measure battery voltage': 'This hardware cannot measure battery voltage, so it stays awake',
  'running from USB power': 'Running from USB power',
  'vehicle is awake': 'The car is awake',
  'vehicle just turned off': 'The car has just been switched off',
  'battery is low': 'The battery is low, so CANary sleeps at once',
  'waiting before sleep': 'The car is off; staying up a few minutes to sync',
  'upload in progress': 'Finishing an upload first',
  'web interface in use': 'Staying awake while this page is open',
  'vehicle off': 'The car is off',
};
const signalWord = rssi => (rssi >= -60 ? 'strong signal' : rssi >= -72 ? 'good signal' : rssi >= -82 ? 'weak signal' : 'very weak signal');

async function saveConfig(patch, done) {
  try {
    const d = await api('/api/config', { body: patch });
    S.cfg = d.config;
    S.profiles = d.profiles;
    S.activeProfile = d.active_profile;
    toast(done || 'Saved');
    return true;
  } catch (e) {
    toast(e.message, true);
    return false;
  }
}

const Device = {
  every: 5000,
  async enter() {
    view.innerHTML = '<h1 class="page-title">Device</h1><p class="muted" style="margin-top:12px" id="dvLoading">Loading settings</p>';
    try {
      const d = await api('/api/config');
      S.cfg = d.config;
      S.profiles = d.profiles;
      S.activeProfile = d.active_profile;
    } catch (e) {
      $('#dvLoading').textContent = e.message;
      return;
    }
    if (page !== Device) return;
    const topics = [['wifi', 'Wi-Fi'], ['glovebox', 'Glovebox'], ['vehicle', 'Vehicle'], ['power', 'Power'], ['storage', 'Storage'], ['system', 'System'], ['activity', 'Activity']];
    view.innerHTML = `<h1 class="page-title">Device</h1>
      <nav class="jump" aria-label="Settings">${topics.map(t => `<button data-jump="${t[0]}">${t[1]}</button>`).join('')}</nav>
      ${topics.map(t => `<h2 class="group-title" id="dv-${t[0]}-title">${t[1]}</h2><div class="sheet" id="dv-${t[0]}"></div>`).join('')}
      <details class="fold" id="dvAdvanced"><summary>${icon('chev')}Advanced</summary><div class="sheet" id="dv-advanced"></div></details>`;
    topics.forEach(t => this.draw(t[0]));
    this.draw('advanced');
    this.live(S.st);
    this.tick();
  },
  draw(name) {
    const el = $(`#dv-${name}`);
    if (el) el.innerHTML = this[name]();
    if (name === 'activity') this.tick();
    this.live(S.st);
  },
  onStatus(st) { this.live(st); },
  async tick() {
    try {
      const d = await api('/api/events');
      const el = $('#dvLog');
      if (!el) return;
      setHTML(el, d.events.length ? d.events.slice(0, 60).map(ev => `<div><time>${known(ev.t) ? esc(when(ev.t).replace('Today, ', '')) : '+' + stopwatch(ev.up)}</time><span class="${ev.level === 'w' ? 'w' : ev.level === 'e' ? 'e' : ''}">${esc(ev.text)}</span></div>`).join('') : '<div><time></time><span>Nothing has happened yet.</span></div>');
    } catch (e) { /* shown by the banner */ }
  },

  /* The parts of each section that follow the device's state rather than its settings. */
  live(st) {
    if (!st) return;
    const set = (id, html) => setHTML($('#' + id), html);
    const n = st.net;
    set('dvWifiState', n.connected
      ? `${lamp('ok')}<div><b>Connected to ${esc(n.ssid)}</b><span>Address ${esc(n.ip)}, ${signalWord(n.rssi)}. Also reachable as canary.local.</span></div>`
      : `${lamp('nodata')}<div><b>Not connected to Wi-Fi</b><span>${n.ap_active ? `You are reaching CANary through its own network, ${esc(n.ap_ssid)}.` : 'Looking for a known network.'}</span></div>`);
    const sy = st.sync;
    let syncHead, syncText, syncLamp = 'ok';
    if (!sy.configured) { syncLamp = 'nodata'; syncHead = 'Not connected'; syncText = 'Enter the address and vehicle number below.'; }
    else if (sy.busy) { syncLamp = 'nodata'; syncHead = sy.up_total > 0 ? `Uploading a drive, ${Math.round(100 * sy.up_done / sy.up_total)}%` : 'Syncing now'; syncText = `${sy.pending_trips} ${plural(sy.pending_trips, 'drive')} waiting.`; }
    else if (sy.last_error) { syncLamp = 'warning'; syncHead = 'The last sync failed'; syncText = sy.last_error; }
    else if (known(sy.last_ok)) {
      syncHead = sy.mode === 'lubelogger' ? 'Connected, odometer only' : 'Connected';
      syncText = `Last synced ${whenIn(sy.last_ok)}.`
        + (sy.odometer_sent_km != null ? ` Odometer sent: ${dist(sy.odometer_sent_km)}.` : '')
        + (sy.mode === 'lubelogger' ? ' This server does not accept drive data, so only the odometer is sent. Update to a Glovebox version with CANary support for weekly reports.' : sy.pending_trips ? ` ${sy.pending_trips} ${plural(sy.pending_trips, 'drive')} waiting to upload.` : ' Every drive has been uploaded.');
    } else { syncLamp = 'nodata'; syncHead = 'Not synced yet'; syncText = n.connected ? 'CANary syncs within a minute of joining Wi-Fi.' : 'CANary syncs when it joins Wi-Fi.'; }
    set('dvSyncState', `${lamp(syncLamp)}<div><b>${esc(syncHead)}</b><span>${esc(syncText)}</span></div>`);
    const v = st.vehicle;
    set('dvCar', `<dt>Car</dt><dd>${CAR_STATE[v.state] || esc(v.state)}</dd>
      <dt>VIN</dt><dd>${v.vin ? esc(v.vin) : 'Not read yet'}</dd>
      <dt>Profile in use</dt><dd>${esc(v.profile.name)}</dd>
      <dt>Connection</dt><dd>${v.can_bitrate ? `CAN at ${v.can_bitrate / 1000} kbit/s, ${v.can_id_bits}-bit addresses` : 'Not detected yet'}</dd>
      <dt>Readings</dt><dd>${v.signals_live} of ${v.signals_total} available on this car${v.learning ? `, still testing ${v.learning} requests` : ''}</dd>
      <dt>Odometer</dt><dd>${st.odometer_km != null ? `${dist(st.odometer_km)}${known(st.odometer_time) ? `, read ${ago(st.odometer_time)}` : ''}` : 'Not read yet'}</dd>`);
    const p = st.power;
    set('dvPowerState', `${lamp(p.can_sleep ? 'nodata' : 'ok')}<div><b>${p.can_sleep ? 'Going to sleep' : 'Awake'}</b><span>${esc(WHY_AWAKE[p.why] || p.why)}. Battery ${st.battery_v != null ? num(st.battery_v, 2) + ' V' : 'not measured'}.</span></div>`);
    const sd = st.storage;
    const used = sd.total - sd.free;
    set('dvCard', sd.present
      ? `<div class="state">${lamp('ok')}<div><b>${bytes(sd.total)} card, ${bytes(sd.free)} free</b><span>${st.trip.count} ${plural(st.trip.count, 'drive')} on the card${st.trip.active ? ', one being recorded now' : ''}.</span></div></div>
         <div class="meter" role="img" aria-label="${Math.round(100 * used / Math.max(sd.total, 1))}% used"><i style="width:${Math.max(1, Math.round(100 * used / Math.max(sd.total, 1)))}%"></i></div>`
      : `<div class="state">${lamp('warning')}<div><b>No SD card found</b><span>Insert a card formatted as FAT32, or insert any card and format it here. Without one, drives are checked but not recorded.</span></div></div>`);
    set('dvClock', `${st.device.time_set ? esc(new Date(st.device.time * 1000).toLocaleString(undefined, { dateStyle: 'medium', timeStyle: 'short' })) : 'Not set yet. It is set from Glovebox or this browser.'}`);
    set('dvUptime', `${dur(st.device.uptime_s)}`);
  },

  wifi() {
    const w = S.cfg.wifi;
    const nets = w.networks.map((nw, i) => `<div><span class="grow">${esc(nw.ssid)}</span><button class="btn small" data-act="wifi-remove" data-i="${i}">Remove</button></div>`).join('');
    return `<div class="section"><div class="state" id="dvWifiState"></div>
      <h2 style="margin-top:18px">Known networks</h2>
      <p>CANary joins whichever of these is in range, strongest first. Add your home network so it syncs when the car is parked.</p>
      ${nets ? `<div class="netlist">${nets}</div>` : ''}
      ${w.networks.length >= 4 ? '<p style="margin-top:12px">Four networks is the limit. Remove one to add another.</p>' : `<form class="form" data-form="wifi-add">
        <label>Network name<input class="field" name="ssid" required maxlength="32" autocomplete="off" autocapitalize="none" spellcheck="false"></label>
        <label>Password<input class="field" name="pass" type="password" maxlength="64" autocomplete="off"><small>Leave empty for an open network.</small></label>
        <div class="actions"><button class="btn primary">Add network</button><button class="btn" type="button" data-act="wifi-scan">Show networks in range</button></div>
        <div id="dvScan"></div></form>`}
      </div>
      <div class="section"><h2>CANary’s own network</h2>
      <p>When no known network is in range, CANary starts its own so you can always reach this page. Pressing the button on the device does the same for 15 minutes.</p>
      <form class="form" data-form="ap">
        <label>Password for that network<input class="field" name="ap_pass" type="password" minlength="8" maxlength="63" autocomplete="off" placeholder="${w.ap_pass_set ? 'Unchanged' : 'None set'}"><small>At least 8 characters. The default is canary-setup.</small></label>
        <label>When to run it<select class="field" name="ap_mode"><option value="0"${w.ap_mode ? '' : ' selected'}>Only when no known network is in range</option><option value="1"${w.ap_mode ? ' selected' : ''}>Always</option></select></label>
        <div class="actions"><button class="btn primary">Save</button><button class="btn" type="button" data-act="ap-on">Turn it on for 15 minutes</button></div>
      </form></div>`;
  },

  glovebox() {
    const g = S.cfg.glovebox;
    return `<div class="section"><div class="state" id="dvSyncState"></div>
      <p style="margin-top:14px">Every time CANary joins Wi-Fi it sends the odometer reading to Glovebox and uploads the drives recorded since last time. Glovebox turns them into the weekly report.</p>
      <form class="form" data-form="glovebox">
        <label>Glovebox address<input class="field" name="url" type="url" inputmode="url" placeholder="http://192.168.1.10:8080" value="${esc(g.url)}" autocapitalize="none" spellcheck="false"></label>
        <label>API key<input class="field" name="api_key" type="password" autocomplete="off" placeholder="${g.api_key_set ? 'Saved. Type to replace it' : 'Paste the key'}"><small>Glovebox creates one for you on the vehicle’s CANary page. Leave empty if your Glovebox has no login.</small></label>
        <div class="row">
          <label>Vehicle number<input class="field" name="vehicle_id" type="number" min="1" inputmode="numeric" value="${g.vehicle_id || ''}"><small>Shown on the same page in Glovebox.</small></label>
          <label>Odometer in Glovebox<select class="field" name="odometer_unit"><option value="mi"${g.odometer_unit === 'km' ? '' : ' selected'}>Miles</option><option value="km"${g.odometer_unit === 'km' ? ' selected' : ''}>Kilometres</option></select><small>Used only with plain LubeLogger.</small></label>
        </div>
        <label>Odometer correction<input class="field" name="odometer_offset" type="number" step="0.1" value="${g.odometer_offset || 0}"><small>Added to every reading, in the unit above. Use it if the instrument cluster was replaced.</small></label>
        <label class="check"><input type="checkbox" name="upload_logs"${g.upload_logs ? ' checked' : ''}>Upload recorded drives for the weekly report</label>
        <div class="actions"><button class="btn primary">Save</button><button class="btn" type="button" data-act="sync">Sync now</button>${g.api_key_set ? '<button class="btn" type="button" data-act="key-clear">Forget the API key</button>' : ''}</div>
      </form></div>`;
  },

  vehicle() {
    const cur = S.cfg.vehicle.profile;
    const autoName = (S.profiles.find(p => p.id === S.activeProfile) || {}).name || S.activeProfile;
    const opts = [`<option value="auto"${cur === 'auto' ? ' selected' : ''}>Choose from the VIN${cur === 'auto' ? ` (now ${esc(autoName)})` : ''}</option>`]
      .concat(S.profiles.map(p => `<option value="${esc(p.id)}"${cur === p.id ? ' selected' : ''}>${esc(p.name)}</option>`));
    return `<div class="section"><dl class="kv" id="dvCar" style="margin-top:0"></dl>
      <div class="actions" style="margin-top:14px"><button class="btn small" data-act="wake">Look for the car now</button></div></div>
      <div class="section"><h2>Vehicle profile</h2>
      <p>A profile tells CANary what a car can report beyond the standard engine data, and which checks to run. It is picked from the VIN; choose one yourself if the pick is wrong.</p>
      <form class="form" data-form="vehicle">
        <label>Profile<select class="field" name="profile">${opts.join('')}</select></label>
        <label>Show values in<select class="field" name="units"><option value="1"${S.cfg.units_imperial ? ' selected' : ''}>Miles, °F and psi</option><option value="0"${S.cfg.units_imperial ? '' : ' selected'}>Kilometres, °C and kPa</option></select></label>
        <div class="actions"><button class="btn primary">Save</button></div>
      </form></div>`;
  },

  power() {
    const p = S.cfg.power;
    return `<div class="section"><div class="state" id="dvPowerState"></div>
      <p style="margin-top:14px">CANary stays plugged in. To protect the battery it goes to sleep when the car is off and wakes when the alternator lifts the voltage. While asleep it sends nothing on the car’s network and this page is unreachable.</p>
      <form class="form" data-form="power">
        <label class="check"><input type="checkbox" name="sleep_enabled"${p.sleep_enabled ? ' checked' : ''}>Sleep when the car is off</label>
        <div class="row">
          <label>Sleep below<input class="field" name="sleep_voltage" type="number" step="0.1" min="11.5" max="14.5" value="${p.sleep_voltage}"><small>Volts. A resting battery is about 12.6.</small></label>
          <label>Wake above<input class="field" name="wake_voltage" type="number" step="0.1" min="12" max="15" value="${p.wake_voltage}"><small>Volts. A charging system is about 14.</small></label>
        </div>
        <label>Stay awake after the engine stops<input class="field" name="sleep_delay_min" type="number" min="1" max="60" value="${Math.round(p.sleep_delay_s / 60)}"><small>Minutes. Long enough to reach Wi-Fi and finish uploading.</small></label>
        <div class="actions"><button class="btn primary">Save</button></div>
      </form></div>`;
  },

  storage() {
    const l = S.cfg.log;
    return `<div class="section"><div id="dvCard"></div>
      <form class="form" data-form="storage">
        <label class="check"><input type="checkbox" name="enabled"${l.enabled ? ' checked' : ''}>Record every drive to the SD card</label>
        <label>Keep raw data after upload<input class="field" name="retention_days" type="number" min="1" max="365" value="${l.retention_days}"><small>Days. After that only the summary of each drive stays on the card; Glovebox keeps the full recording.</small></label>
        <div class="actions"><button class="btn primary">Save</button><button class="btn danger" type="button" data-act="format">Erase the card</button></div>
        <div id="dvFormat"></div>
      </form></div>`;
  },

  system() {
    const st = S.st;
    return `<div class="section"><h2>Firmware</h2>
      <dl class="kv"><dt>Version</dt><dd>${st ? esc(st.device.fw) : ''}</dd><dt>Hardware</dt><dd>${st ? esc(st.device.board) : ''}</dd><dt>Device</dt><dd>${st ? esc(st.device.id) : ''}</dd>
      <dt>Running for</dt><dd id="dvUptime"></dd><dt>Clock</dt><dd id="dvClock"></dd></dl>
      <form class="form" data-form="ota">
        <label>Install an update<input class="field file" name="file" type="file" accept=".bin,application/octet-stream" required><small>Choose a canary .bin file from the releases page. The current firmware stays in place until the new one has been verified.</small></label>
        <div class="actions"><button class="btn primary">Install update</button><button class="btn" type="button" data-act="reboot">Restart CANary</button></div>
        <div id="dvOta"></div>
      </form></div>
      <div class="section"><h2>Password for this page</h2>
      <p>${S.cfg.ui_password_set ? 'A password is set.' : 'No password is set: anyone on the same network can open this page.'} The browser asks for it; any user name works.</p>
      <form class="form" data-form="password">
        <label>New password<input class="field" name="ui_password" type="password" autocomplete="new-password" maxlength="40"></label>
        <div class="actions"><button class="btn primary">Set password</button>${S.cfg.ui_password_set ? '<button class="btn" type="button" data-act="password-clear">Remove the password</button>' : ''}</div>
      </form>
      <form class="form" data-form="name" style="margin-top:22px">
        <label>Name of this device<input class="field" name="device_name" maxlength="31" value="${esc(S.cfg.device_name)}"><small>Shown here and in Glovebox, for example the car it lives in.</small></label>
        <div class="actions"><button class="btn primary">Save</button></div>
      </form></div>`;
  },

  activity() {
    return `<div class="section"><p style="margin-top:0">What CANary has been doing, newest first.</p><div class="log" id="dvLog"></div>
      <div class="actions" style="margin-top:12px"><a class="btn small" href="/api/devicelog" download>${icon('down')}Download the full log</a></div></div>`;
  },

  advanced() {
    return `<div class="section"><h2>What this car answers</h2>
      <p>CANary tries every request in the profile once and remembers which ones the car answers. This table is what to send along when a profile needs fixing.</p>
      <div class="actions" style="margin-top:12px"><button class="btn small" data-act="support">Show the table</button></div><div id="dvSupport" class="scroll-x"></div></div>
      <div class="section"><h2>Ask a control unit directly</h2>
      <p>For working out a new car. Only read requests are allowed (services 01, 02, 03, 06, 07, 09, 0A, 19 and 22); anything that could change the car is refused.</p>
      <form class="form" data-form="diag">
        <div class="row"><label>Send to<input class="field" name="tx" value="7E0" required spellcheck="false" autocapitalize="characters"><small>Address in hex, for example 7E0 for the engine.</small></label>
        <label>Reply from<input class="field" name="rx" placeholder="automatic" spellcheck="false" autocapitalize="characters"><small>Leave empty for the usual reply address.</small></label></div>
        <label>Request<input class="field" name="data" value="22 F1 90" required spellcheck="false" autocapitalize="characters"><small>Up to 7 bytes in hex. 22 F1 90 asks for the VIN.</small></label>
        <div class="actions"><button class="btn primary">Send</button></div><div id="dvDiag"></div>
      </form></div>
      <div class="section"><h2>System details</h2><div class="actions" style="margin-top:12px"><button class="btn small" data-act="sysinfo">Show details</button></div><dl class="kv" id="dvSys"></dl></div>`;
  },
};

/* Device page: forms */
view.addEventListener('submit', async e => {
  const form = e.target.closest('form[data-form]');
  if (!form) return;
  e.preventDefault();
  const f = new FormData(form), kind = form.dataset.form, btn = $('button.primary', form);
  const get = k => String(f.get(k) == null ? '' : f.get(k)).trim();
  if (btn) btn.disabled = true;
  try {
    if (kind === 'wifi-add') {
      const nets = S.cfg.wifi.networks.filter(nw => nw.ssid !== get('ssid')).map(nw => ({ ssid: nw.ssid }));
      nets.push({ ssid: get('ssid'), pass: String(f.get('pass') || '') });
      if (await saveConfig({ wifi: { networks: nets } }, 'Network added')) Device.draw('wifi');
    } else if (kind === 'ap') {
      const patch = { wifi: { ap_mode: +get('ap_mode') } };
      if (get('ap_pass')) patch.wifi.ap_pass = get('ap_pass');
      if (await saveConfig(patch)) Device.draw('wifi');
    } else if (kind === 'glovebox') {
      const patch = { glovebox: { url: get('url'), vehicle_id: +get('vehicle_id') || 0, odometer_unit: get('odometer_unit'), odometer_offset: +get('odometer_offset') || 0, upload_logs: f.get('upload_logs') === 'on' } };
      if (get('api_key')) patch.glovebox.api_key = get('api_key');
      if (await saveConfig(patch, 'Saved. Syncing now')) Device.draw('glovebox');
    } else if (kind === 'vehicle') {
      if (await saveConfig({ vehicle: { profile: get('profile') }, units_imperial: get('units') === '1' })) {
        S.imperial = get('units') === '1';
        S.meta = null;
        Device.draw('vehicle');
      }
    } else if (kind === 'power') {
      await saveConfig({ power: { sleep_enabled: f.get('sleep_enabled') === 'on', sleep_voltage: +get('sleep_voltage'), wake_voltage: +get('wake_voltage'), sleep_delay_s: Math.round(+get('sleep_delay_min') * 60) } });
      Device.draw('power');
    } else if (kind === 'storage') {
      if (await saveConfig({ log: { enabled: f.get('enabled') === 'on', retention_days: +get('retention_days') } })) Device.draw('storage');
    } else if (kind === 'password') {
      if (!get('ui_password')) toast('Type a password first', true);
      else if (await saveConfig({ ui_password: get('ui_password') }, 'Password set')) Device.draw('system');
    } else if (kind === 'name') {
      if (await saveConfig({ device_name: get('device_name') || 'CANary' })) pollStatus();
    } else if (kind === 'diag') {
      const out = $('#dvDiag');
      out.innerHTML = '<div class="out">Asking</div>';
      try {
        const r = await api('/api/diag/request', { body: { tx: get('tx').replace(/\s/g, ''), rx: get('rx').replace(/\s/g, ''), data: get('data').replace(/\s/g, '') }, timeout: 6000 });
        const hex = r.data.replace(/(..)/g, '$1 ').trim();
        const txt = hex.split(' ').slice(3).map(h => { const c = parseInt(h, 16); return c >= 32 && c < 127 ? String.fromCharCode(c) : '.'; }).join('');
        out.innerHTML = `<div class="out"><b>${esc(hex)}</b><br>From ${esc(r.from)} in ${r.ms} ms${/[A-Za-z0-9]{4}/.test(txt) ? `. As text: ${esc(txt)}` : ''}</div>`;
      } catch (err) {
        out.innerHTML = `<div class="out">${esc(err.message.charAt(0).toUpperCase() + err.message.slice(1))}.</div>`;
      }
    } else if (kind === 'ota') {
      installUpdate(form);
    }
  } finally {
    if (btn && kind !== 'ota') btn.disabled = false;
  }
});

function installUpdate(form) {
  const file = form.elements.file.files[0], out = $('#dvOta'), btn = $('button.primary', form);
  if (!file) return;
  const xhr = new XMLHttpRequest();
  out.innerHTML = '<div class="out" id="dvOtaText">Uploading</div><div class="meter"><i id="dvOtaBar" style="width:0"></i></div>';
  xhr.upload.onprogress = ev => {
    if (!ev.lengthComputable) return;
    const pct = Math.round(100 * ev.loaded / ev.total);
    $('#dvOtaBar').style.width = pct + '%';
    $('#dvOtaText').textContent = pct < 100 ? `Uploading, ${pct}%` : 'Checking the new firmware';
  };
  const fail = msg => {
    out.innerHTML = `<div class="out">The update was not installed: ${esc(msg)}. The current firmware is untouched.</div>`;
    btn.disabled = false;
  };
  xhr.onload = () => {
    let r = null;
    try { r = JSON.parse(xhr.responseText); } catch (e) { /* not JSON */ }
    if (xhr.status === 200 && r && r.ok) {
      out.innerHTML = '<div class="out">Installed. CANary is restarting with the new firmware.</div>';
      waitForRestart('Installing the update. CANary restarts and this page reloads by itself.');
    } else {
      fail((r && r.error) || `error ${xhr.status}`);
    }
  };
  xhr.onerror = () => fail('the connection dropped');
  xhr.open('POST', '/api/ota');
  xhr.setRequestHeader('Content-Type', 'application/octet-stream');
  xhr.send(file);
}

/* Device page: buttons */
view.addEventListener('click', async e => {
  const jump = e.target.closest('[data-jump]');
  if (jump) {
    const t = $(`#dv-${jump.dataset.jump}-title`);
    if (t) t.scrollIntoView({ behavior: matchMedia('(prefers-reduced-motion: reduce)').matches ? 'auto' : 'smooth', block: 'start' });
    return;
  }
  const b = e.target.closest('[data-act]');
  if (!b) return;
  const act = b.dataset.act;
  try {
    if (act === 'wifi-remove') {
      const nets = S.cfg.wifi.networks.filter((_, i) => i !== +b.dataset.i).map(nw => ({ ssid: nw.ssid }));
      if (await saveConfig({ wifi: { networks: nets } }, 'Network removed')) Device.draw('wifi');
    } else if (act === 'wifi-scan') {
      const out = $('#dvScan');
      out.innerHTML = '<p class="muted">Looking for networks</p>';
      b.disabled = true;
      try {
        const d = await api('/api/wifi/scan', { timeout: 15000 });
        out.innerHTML = d.networks.length
          ? `<div class="netlist">${d.networks.map(nw => `<button type="button" data-act="wifi-pick" data-ssid="${esc(nw.ssid)}">${icon('wifi')}<span class="grow">${esc(nw.ssid)}</span><span class="muted">${signalWord(nw.rssi)}${nw.secure ? '' : ', open'}</span></button>`).join('')}</div>`
          : '<p class="muted">No networks found. Try again in a moment.</p>';
      } finally {
        b.disabled = false;
      }
    } else if (act === 'wifi-pick') {
      const form = b.closest('form');
      form.elements.ssid.value = b.dataset.ssid;
      form.elements.pass.focus();
    } else if (act === 'ap-on') {
      await api('/api/ap', { method: 'POST' });
      toast('CANary’s own network is on for 15 minutes');
    } else if (act === 'sync') {
      await api('/api/sync', { method: 'POST' });
      toast('Syncing now');
      setTimeout(pollStatus, 800);
    } else if (act === 'wake') {
      await api('/api/wake', { method: 'POST' });
      toast('Looking for the car');
    } else if (act === 'reboot') {
      await api('/api/reboot', { method: 'POST' });
      waitForRestart('CANary is restarting. This page reloads by itself.');
    } else if (act === 'key-clear') {
      if (await saveConfig({ glovebox: { api_key: '' } }, 'API key removed')) Device.draw('glovebox');
    } else if (act === 'password-clear') {
      if (await saveConfig({ ui_password: '' }, 'Password removed')) Device.draw('system');
    } else if (act === 'format') {
      $('#dvFormat').innerHTML = `<div class="out">This erases every recorded drive on the card, uploaded or not. <span class="actions" style="display:inline-flex;margin-left:8px"><button class="btn small danger" type="button" data-act="format-yes">Erase everything</button><button class="btn small" type="button" data-act="format-no">Keep it</button></span></div>`;
    } else if (act === 'format-no') {
      $('#dvFormat').innerHTML = '';
    } else if (act === 'format-yes') {
      await api('/api/storage/format', { method: 'POST' });
      $('#dvFormat').innerHTML = '<div class="out">Erasing the card. This takes up to a minute.</div>';
      setTimeout(() => { const el = $('#dvFormat'); if (el) el.innerHTML = ''; }, 20000);
    } else if (act === 'support') {
      const d = await api('/api/support');
      const word = { yes: 'Yes', no: 'No', unknown: 'Not tried yet' };
      $('#dvSupport').innerHTML = `<table class="grid"><thead><tr><th>Control unit</th><th>Request</th><th>Answered</th><th>Used for</th><th>Good</th><th>Failed</th></tr></thead><tbody>${d.requests.map(r => `<tr><td>${esc(r.ecu)}</td><td>${esc(r.service)} ${esc(r.code)}</td><td>${word[r.supported] || esc(r.supported)}</td><td>${r.signals.map(esc).join(', ')}</td><td>${r.ok}</td><td>${r.errors}</td></tr>`).join('')}</tbody></table>`;
      b.textContent = 'Refresh the table';
    } else if (act === 'sysinfo') {
      const d = await api('/api/system');
      const rows = [['Build', d.build], ['Framework', d.idf], ['Started by', d.reset_reason], ['Running from', d.partition], ['Free memory', `${bytes(d.heap_free)} (lowest ${bytes(d.heap_min)})`],
        ['Free extended memory', bytes(d.psram_free)], ['Interpreter chip', d.obd_chip || 'not detected'], ['SD card', d.sd_card || 'none'],
        ['Traffic on the car’s network', S.st ? `${S.st.vehicle.bus_tx} sent, ${S.st.vehicle.bus_rx} received, ${S.st.vehicle.bus_err} errors` : '']];
      $('#dvSys').innerHTML = rows.map(r => `<dt>${r[0]}</dt><dd>${esc(r[1])}</dd>`).join('');
    }
  } catch (err) {
    toast(err.message, true);
  }
});

/* ───────── router ───────── */

const PAGES = { health: Health, live: Live, codes: Codes, trips: Trips, device: Device };
let page = null, pageTimer = null;

function route() {
  const parts = location.hash.replace(/^#\/?/, '').split('/').filter(Boolean);
  const name = PAGES[parts[0]] ? parts[0] : 'health';
  const next = name === 'trips' && parts[1] ? Trip : PAGES[name];
  if (page && page.leave) page.leave();
  clearInterval(pageTimer);
  page = next;
  $$('[data-nav]').forEach(a => {
    const on = a.dataset.nav === name;
    a.classList.toggle('on', on);
    if (on) a.setAttribute('aria-current', 'page');
    else a.removeAttribute('aria-current');
  });
  window.scrollTo(0, 0);
  page.enter(parts[1] ? decodeURIComponent(parts[1]) : null);
  pageTimer = setInterval(() => {
    if (document.visibilityState === 'visible' && page.every && page.tick && !S.away) page.tick();
  }, page.every || 4000);
}

window.addEventListener('hashchange', route);
document.addEventListener('visibilitychange', () => { if (document.visibilityState === 'visible') pollStatus(); });
setInterval(() => { if (document.visibilityState === 'visible') pollStatus(); }, 2000);
route();
pollStatus();
})();
