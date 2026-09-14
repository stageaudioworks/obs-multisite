/*
 * app.js — the main site's remote control.
 *
 * The same rules as the campus player's page, because the same sort of person
 * reads it in the same sort of room: the words are the words of an event rather
 * than of a video pipeline, one status document is polled and everything is
 * drawn from it so no two parts of the page can disagree, and nothing is loaded
 * from the internet — a church network has better uses for the bandwidth.
 */

'use strict';

const $ = (sel) => document.querySelector(sel);

let status = null;
let pollTimer = null;
// The difference between this device's clock and OBS's, so a phone with the
// wrong time still shows the same reading as the machine does.
let clockSkewMs = 0;
// The event-name field is pre-filled with the current time and kept fresh until
// somebody types in it, exactly as the dock does.
let eventNameTouched = false;
let markerSignature = '';

/* ── Small helpers ───────────────────────────────────────────────────────── */

function elapsed(ms) {
  const s = Math.max(0, Math.round(ms / 1000));
  const h = Math.floor(s / 3600);
  const m = Math.floor((s % 3600) / 60);
  const sec = s % 60;
  const pad = (n) => String(n).padStart(2, '0');
  return h ? `${h}:${pad(m)}:${pad(sec)}` : `${m}:${pad(sec)}`;
}

function bytes(n) {
  if (!n) return '—';
  const gb = n / 1e9;
  return gb >= 1 ? gb.toFixed(2) + ' GB' : (n / 1e6).toFixed(0) + ' MB';
}

function rate(n) {
  if (!n) return '—';
  const mbps = (n * 8) / 1e6;
  return mbps >= 1 ? mbps.toFixed(1) + ' Mbps'
                   : ((n * 8) / 1e3).toFixed(0) + ' kbps';
}

async function api(method, path, body) {
  const opts = { method, headers: {} };
  if (body !== undefined) {
    opts.headers['Content-Type'] = 'application/json';
    opts.body = JSON.stringify(body);
  }
  const res = await fetch(path, opts);
  const text = await res.text();
  let data = null;
  try { data = text ? JSON.parse(text) : null; } catch (e) { /* not JSON */ }
  if (!res.ok) {
    const message = (data && data.error) || text || ('HTTP ' + res.status);
    throw new Error(message);
  }
  return data;
}

// A control answers with the new status, so the page is up to date the moment
// the button is released rather than at the next poll.
async function act(path, body) {
  try {
    status = await api('POST', path, body);
    drawStatus();
    notify('');
  } catch (e) {
    notify(e.message, true);
  }
}

function notify(message, isError) {
  const box = $('#alert');
  if (!message) { box.hidden = true; return; }
  box.hidden = false;
  box.textContent = message;
  box.style.borderColor = isError ? 'var(--danger)' : 'var(--line)';
}

/* ── Status ──────────────────────────────────────────────────────────────── */

async function refreshStatus() {
  try {
    status = await api('GET', '/api/encoder/status');
    clockSkewMs = (status.now_ms || Date.now()) - Date.now();
    drawStatus();
  } catch (e) {
    const pill = $('#state');
    pill.textContent = 'No answer';
    pill.className = 'state offline';
    notify('OBS is not answering this page: ' + e.message, true);
  }
}

function drawStatus() {
  const s = status;
  if (!s) return;

  const pill = $('#state');
  pill.textContent = s.live ? 'On air' : 'Not sending';
  pill.className = 'state ' + (s.live ? 'live' : 'offline');

  // The internet reading, and only once it is a measurement. Claiming it is fine
  // before anything has been tried is the one wrong answer that matters most on
  // this page: it is the reason a broadcast would fail and nobody would know.
  const net = $('#net');
  if (s.link_known) {
    const h = s.link_health;
    net.hidden = false;
    net.textContent = h === 0 ? 'Internet fine'
                    : h === 1 ? 'Internet poor'
                              : 'Internet down';
    net.className = 'net ' + (h === 0 ? 'good' : h === 1 ? 'degraded' : 'off');
  } else {
    net.hidden = true;
  }

  $('#room').textContent = s.room_id
    ? (s.bucket ? s.room_id + ' into ' + s.bucket : s.room_id)
    : 'no room set';

  const lock = $('#lock');
  lock.textContent = s.locked ? 'Locked' : 'Lock';
  lock.classList.toggle('on', !!s.locked);

  $('#clock').textContent =
    new Date(Date.now() + clockSkewMs).toLocaleTimeString('en-GB', { hour12: false });

  $('#on-air').textContent = s.live
    ? 'On air for ' + elapsed((s.uptime_s || 0) * 1000) +
      (s.event_name ? ' — ' + s.event_name : '')
    : (s.configured ? 'Ready to go live' : 'Storage is not set up yet');

  $('#btn-go').disabled = !!s.live;
  $('#btn-end').disabled = !s.live;

  // A field nobody has typed into keeps naming "now"; once it has been edited,
  // or once the event is on air and has a name of its own, it is left alone.
  const name = $('#event-name');
  if (s.live) name.value = s.event_name || '';
  else if (!eventNameTouched) name.value = s.default_event_name || '';

  drawMarkers(s.marker_labels || []);
  drawReadout(s);

  // The other half of the plugin, when this machine has one.
  const other = $('#other-side');
  if (s.other_page) { other.href = s.other_page; other.hidden = false; }
  else other.hidden = true;
}

/* ── The readout ─────────────────────────────────────────────────────────── */

// Rebuilt only when the buttons actually change: they are pressed mid-event,
// and replacing them every poll would drop a press somebody was already making.
function drawMarkers(labels) {
  const sig = labels.join('|');
  if (sig === markerSignature) return;
  markerSignature = sig;

  const box = $('#markers');
  box.textContent = '';
  if (!labels.length) {
    const p = document.createElement('p');
    p.className = 'hint';
    p.textContent = 'No marker buttons are set up. They are named in Settings.';
    box.appendChild(p);
    return;
  }
  labels.forEach((label) => {
    const b = document.createElement('button');
    b.type = 'button';
    b.textContent = label;
    b.onclick = () => act('/api/encoder/marker?label=' + encodeURIComponent(label));
    box.appendChild(b);
  });
}

function cell(label, value, warn) {
  return `<div class="cell${warn ? ' warn' : ''}">` +
         `<div class="k">${label}</div><div class="v">${value}</div></div>`;
}

function drawReadout(s) {
  const link = !s.link_known ? 'not measured yet'
             : s.link_health === 0 ? 'fine'
             : s.link_health === 1 ? 'poor'
                                   : 'down';
  const net = s.link_known
    ? (s.colo ? link + ' via ' + s.colo : link)
    : link;

  $('#readout').innerHTML =
    cell('Confirmed', s.confirmed || 0) +
    cell('Waiting to send', s.pending || 0, (s.pending || 0) > 20) +
    cell('Retries', s.retries || 0, (s.retries || 0) > 0) +
    cell('Sent so far', bytes(s.bytes)) +
    cell('Upload rate', s.upload_samples ? rate(s.upload_bytes_per_s) : '—') +
    cell('Internet', net, s.link_known && s.link_health > 0) +
    cell('Storage', s.storage_host || '—') +
    cell('Last problem', s.last_error ? s.last_error : 'none', !!s.last_error);
}

/* ── Controls ────────────────────────────────────────────────────────────── */

$('#event-name').addEventListener('input', () => { eventNameTouched = true; });

$('#btn-go').onclick = () => {
  const name = $('#event-name').value.trim();
  act('/api/encoder/go-live', { event_name: name });
};

$('#btn-end').onclick = () => act('/api/encoder/end');

// Locking is answered by the server rather than by the page, so two phones
// cannot disagree about whether the controls are locked.
$('#lock').onclick = async () => {
  try {
    await api('POST', '/api/lock?on=' + (status && status.locked ? 'false' : 'true'));
    await refreshStatus();
  } catch (e) {
    notify(e.message, true);
  }
};

/* ── Settings ────────────────────────────────────────────────────────────── */

const SECRET_PLACEHOLDER = '\u2022\u2022\u2022\u2022\u2022\u2022\u2022\u2022';

// Fields the generic load/save loop below must not touch itself: each is
// either populated from its own endpoint (the encoder list, the provider
// list) or carries the opposite sense of the setting it represents (the
// "disable cloud" checkbox against `cloud_enabled`).
const SETTINGS_SPECIAL = new Set([
  'video_encoder_id', 'storage_provider', 'cloud_enabled_off',
]);

// Which fields the storage-provider dropdown needs (PROJECT-SCOPE.md section
// 8.6) \u2014 fetched once, since the list is static. Keyed by provider key
// ("r2", "aws", \u2026) for updateProviderFields() below \u2014 identical to the
// appliance's and the relay's copy of this same logic.
let storageProviders = {};

async function loadStorageProviderChoices() {
  try {
    const data = await api('GET', '/api/storage/providers');
    const sel = $('#s-provider');
    const keep = sel.value;
    storageProviders = {};
    sel.textContent = '';
    (data.providers || []).forEach((p) => {
      storageProviders[p.key] = p;
      const o = document.createElement('option');
      o.value = p.key;
      // display_name already carries its own "(coming soon)" where that
      // applies (see storage_providers.cpp) \u2014 just disable the option.
      o.textContent = p.display_name;
      o.disabled = !p.available;
      sel.appendChild(o);
    });
    if (keep) sel.value = keep;
  } catch (e) {
    /* An older build with no such endpoint yet. The raw fields still work. */
  }
}

// Shows only the fields the selected provider actually needs \u2014 an account id
// for R2, a region for AWS/Backblaze/Wasabi, both endpoint and region for
// Custom \u2014 the same rule the Qt docks, the appliance and the relay apply to
// the identical dropdown.
function updateProviderFields() {
  const info = storageProviders[$('#s-provider').value];
  if (!info) return;
  $('#field-account').hidden  = !info.needs_account_id;
  $('#field-endpoint').hidden = !info.needs_endpoint;
  $('#field-region').hidden   = !info.needs_region;
}
$('#s-provider').addEventListener('change', updateProviderFields);

// Disabling cloud storage means this event has nowhere else to go if a
// satellite can't reach this machine over LAN \u2014 the same confirmation the
// dock asks before applying it. Checking the box asks immediately, rather
// than waiting for Save, so a change of mind costs nothing: the box simply
// reverts.
$('#s-cloud-off').addEventListener('change', (ev) => {
  if (!ev.target.checked) return;
  const ok = confirm(
    "Stop uploading to cloud storage? Nothing from this event will leave " +
    "this machine, and satellites that can't reach it over LAN will have " +
    'no cloud copy to fall back on.');
  if (!ok) ev.target.checked = false;
});

async function loadSettings() {
  try {
    const s = await api('GET', '/api/encoder/settings');

    // The encoder list is what this machine actually has, offered in the order
    // the dock offers it: hardware first, because it frees the CPU.
    const sel = $('#s-encoder');
    const keep = s.video_encoder_id;
    sel.textContent = '';
    (s.encoders || []).forEach((e) => {
      const o = document.createElement('option');
      o.value = e.id;
      o.textContent = e.name + (e.hardware ? ' (hardware)' : '');
      sel.appendChild(o);
    });
    sel.value = keep;

    await loadStorageProviderChoices();
    $('#s-provider').value = s.storage_provider || 'custom';
    updateProviderFields();
    // The checkbox reads "disable", the setting reads "enable" \u2014 opposite
    // senses of the same fact, so this is the one field the generic loop
    // below must not assign directly.
    $('#s-cloud-off').checked = s.cloud_enabled === false;

    const f = $('#settings-form');
    for (const el of f.elements) {
      if (!el.name || SETTINGS_SPECIAL.has(el.name)) continue;
      const v = s[el.name];
      if (v === undefined) continue;
      if (el.type === 'checkbox') el.checked = !!v;
      else el.value = typeof v === 'boolean' ? String(v) : v;
    }
    $('#settings-note').textContent = '';
  } catch (e) {
    $('#settings-note').textContent = e.message;
  }
}

$('#settings-form').addEventListener('submit', async (ev) => {
  ev.preventDefault();
  const note = $('#settings-note');
  note.textContent = 'Saving…';

  const out = {};
  for (const el of ev.target.elements) {
    if (!el.name || el.name === 'cloud_enabled_off') continue;
    if (el.type === 'checkbox') { out[el.name] = el.checked; continue; }
    if (el.type === 'number') {
      const n = Number(el.value);
      if (!Number.isNaN(n)) out[el.name] = n;
      continue;
    }
    out[el.name] = el.value.trim();
  }
  out.cloud_enabled = !$('#s-cloud-off').checked;
  // The dots mean "unchanged", so they are never posted back as a secret.
  if (out.secret_access_key === SECRET_PLACEHOLDER) delete out.secret_access_key;
  if (out.lan_auth_token === SECRET_PLACEHOLDER) delete out.lan_auth_token;

  try {
    await api('POST', '/api/encoder/settings', out);
    note.textContent = 'Saved. The next broadcast uses it.';
    await loadSettings();
    await refreshStatus();
  } catch (e) {
    note.textContent = e.message;
  }
});

/* ── This page, and the log ──────────────────────────────────────────────── */

function drawFacts() {
  const s = status || {};
  const rows = [
    ['OBS plugin version', s.version || '—'],
    ['This page', location.origin + '/'],
    ['Machine role', 'main site — sending'],
    ['Room', s.room_id || '—'],
    ['Bucket', s.bucket || 'not set'],
  ];
  $('#facts').innerHTML = rows
    .map((r) => `<dt>${r[0]}</dt><dd>${r[1]}</dd>`)
    .join('');
}

async function refreshLog() {
  try {
    const r = await api('GET', '/api/log?lines=200');
    const pre = $('#log');
    const at = document.scrollingElement;
    const wasAtEnd = at.scrollHeight - at.scrollTop - at.clientHeight < 40;
    pre.textContent = '';
    (r.lines || []).forEach((e) => {
      const span = document.createElement('span');
      const cls = e.level === 'error' ? 'error'
                : e.level === 'warn'  ? 'warn'
                : e.level === 'debug' ? 'debug' : '';
      if (cls) span.className = cls;
      const t = new Date(e.at_ms).toLocaleTimeString('en-GB', { hour12: false });
      span.textContent = t + '  ' + e.text + '\n';
      pre.appendChild(span);
    });
    if (wasAtEnd) pre.scrollTop = pre.scrollHeight;
  } catch (e) {
    $('#log').textContent = 'could not read the log: ' + e.message;
  }
}

$('#btn-log-refresh').onclick = refreshLog;

/* ── Tabs and the poll loop ──────────────────────────────────────────────── */

document.querySelectorAll('.tab[data-tab]').forEach((tab) => {
  tab.onclick = () => {
    document.querySelectorAll('.tab[data-tab]').forEach((t) =>
      t.classList.toggle('is-on', t === tab));
    document.querySelectorAll('.panel').forEach((p) =>
      p.classList.toggle('is-on', p.id === 'tab-' + tab.dataset.tab));
    if (tab.dataset.tab === 'settings') { loadSettings(); drawFacts(); }
    if (tab.dataset.tab === 'log') refreshLog();
  };
});

function startPolling() {
  if (pollTimer) clearInterval(pollTimer);
  refreshStatus();
  pollTimer = setInterval(() => {
    refreshStatus();
    const logOpen = document.querySelector('#tab-log').classList.contains('is-on');
    if (logOpen && document.querySelector('#log-follow').checked) refreshLog();
  }, 500);
}

// A tablet left on a music stand should stop asking while its screen is off,
// and pick straight back up when somebody looks at it.
document.addEventListener('visibilitychange', () => {
  if (document.hidden) {
    if (pollTimer) { clearInterval(pollTimer); pollTimer = null; }
  } else {
    startPolling();
  }
});

startPolling();
loadSettings().catch(() => {});



