/*
 * app.js — the campus side's remote control.
 *
 * This is the campus player's page, reached from inside OBS instead of from a
 * box under a television: the same words, the same layout, the same one status
 * document polled twice a second. A volunteer who has used one already knows
 * the other, which is the whole point of it looking like this.
 *
 * Nothing here says "segment", "buffer" or "live edge". Times are clock times
 * and durations are minutes, because the person holding the tablet is watching
 * a service, not a pipeline.
 */

'use strict';

const $ = (sel) => document.querySelector(sel);
const $$ = (sel) => Array.from(document.querySelectorAll(sel));

// Matches multisite::RoomState.
const ROOM = { UNKNOWN: 0, OFFLINE: 1, LIVE: 2, ENDED: 3, INTERRUPTED: 4 };
// Matches multisite::EventState.
const EVENT = { UNKNOWN: 0, LIVE: 1, RECORDING: 2, INTERRUPTED: 3 };

let status = null;
let events = null;
let pollTimer = null;
// The offset between this device's clock and the machine's, so a phone with the
// wrong time still shows the same reading as the picture does.
let clockSkewMs = 0;
let selectedEvent = null;
let eventsSignature = '';

/* ── Small helpers ───────────────────────────────────────────────────────── */

function hhmmss(ms) {
  if (!ms) return '--:--:--';
  return new Date(ms).toLocaleTimeString('en-GB', { hour12: false });
}

function shortDateTime(ms) {
  if (!ms) return '';
  return new Date(ms).toLocaleString('en-GB', {
    weekday: 'short', day: 'numeric', month: 'short',
    hour: '2-digit', minute: '2-digit', hour12: false,
  });
}

// Durations as somebody would say them out loud.
function spoken(seconds) {
  seconds = Math.max(0, Math.round(seconds));
  if (seconds < 60) return seconds + ' s';
  const m = Math.floor(seconds / 60);
  if (m < 60) return m + ' min';
  const h = Math.floor(m / 60);
  const rem = m % 60;
  return rem ? `${h} h ${rem} min` : `${h} h`;
}

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
    status = await api('GET', '/api/decoder/status');
    clockSkewMs = (status.now_ms || Date.now()) - Date.now();
    drawStatus();
  } catch (e) {
    const pill = $('#state');
    pill.textContent = 'No answer';
    pill.className = 'state offline';
    notify('OBS is not answering this page: ' + e.message, true);
  }
}

function netFor(s) {
  if (!s.link_known) return null;
  const h = s.link_health;
  const text = h === 0 ? 'Internet fine' : h === 1 ? 'Internet poor' : 'Internet down';
  const cls = h === 0 ? 'good' : h === 1 ? 'degraded' : 'off';
  return { text: text, cls: cls, colo: s.colo || '' };
}

function drawStatus() {
  const s = status;
  if (!s) return;

  const pill = $('#state');
  const net = $('#net');

  if (!s.have_source) {
    // Not a player that has stopped: there is nothing here to play. The fix is
    // in the scene collection, and the page must say so rather than showing
    // zeroes that look like a fault in the broadcast.
    pill.textContent = 'No source in this scene';
    pill.className = 'state offline';
    net.hidden = true;
    $('#room').textContent = 'add a Multisite Source to the scene';
  } else {
    const state = s.room_state;
    pill.textContent = state === ROOM.LIVE ? 'Receiving live'
                     : state === ROOM.ENDED ? 'Finished'
                     : state === ROOM.INTERRUPTED ? 'Sending stopped'
                     : state === ROOM.OFFLINE ? 'Nothing is being sent'
                                              : 'Working it out…';
    pill.className = 'state ' + (state === ROOM.LIVE ? 'live'
                    : state === ROOM.ENDED ? 'ended'
                    : state === ROOM.INTERRUPTED ? 'ended'
                    : 'offline');

    const n = netFor(s);
    if (n) {
      net.hidden = false;
      net.textContent = n.colo ? n.text + ' via ' + n.colo : n.text;
      net.className = 'net ' + n.cls;
      net.title = s.storage_host ? 'from ' + s.storage_host : '';
    } else {
      net.hidden = true;
    }

    $('#room').textContent = s.live_elsewhere
      ? (s.room_id || '') + ' — something else is live now'
      : (s.room_id || '') + (s.pinned_event_id ? ' — playing an earlier event' : '');
  }

  // Two locks can be in force: this page's, and the one the desk set. Both are
  // shown, because "why will this not respond" has two different answers.
  const lock = $('#lock');
  const locked = !!s.locked || !!s.source_locked;
  lock.textContent = locked ? 'Locked' : 'Lock';
  lock.classList.toggle('on', locked);
  lock.title = s.source_locked
    ? 'Locked from the OBS panel on this machine'
    : 'Stop anything being changed by accident';

  if (!s.have_source) return;

  const shown = s.seek_target_ms || s.playhead_ms;
  const clock = $('#clock');
  clock.textContent = hhmmss(shown);
  clock.className = 'clock' + (s.seek_target_ms ? ' provisional' : '');

  // Stopped first: a stopped source downloads nothing, so every line below it
  // describes a position that cannot move. "2 minutes behind the main site"
  // under a stopped decoder is a stale number someone would act on.
  $('#position').textContent = s.stopped
    ? 'Stopped — nothing downloading'
    : s.loading
    ? 'Loading…'
    : s.ended
      ? 'A finished recording, ' + spoken((s.total_ms || (s.end_ms - s.started_ms)) / 1000) + ' long'
      : s.seek_target_ms ? 'Going to ' + hhmmss(s.seek_target_ms)
      : s.buffering ? 'Waiting for the picture'
      : s.paused ? 'Held — ' + spoken(s.behind_live_s) + ' behind the main site'
      : spoken(s.behind_live_s) + ' behind the main site';

  drawTransport(s);
  drawTimeline(s);
  drawCues(s);
  drawReadout(s);
  drawNotice(s);

  const other = $('#other-side');
  if (s.other_page) { other.href = s.other_page; other.hidden = false; }
  else other.hidden = true;
}

function drawNotice(s) {
  if (!s.configured) {
    notify('This machine has no storage details yet. Open Settings and enter them.', true);
  } else if (s.link_known && s.link_health === 2 && !s.ended) {
    notify('No internet — this campus cannot reach the broadcast storage.' +
           (s.buffered_ahead_s
             ? ' The picture keeps playing for about ' + spoken(s.buffered_ahead_s) + ' more.'
             : ''), true);
  } else {
    notify(s.last_error || '', true);
  }
}

function drawTransport(s) {
  const live = !s.ended;
  $('#btn-play').disabled = !s.configured || (s.playing && !s.paused);
  $('#btn-hold').disabled = !s.playing || s.paused;
  $('#btn-stop').disabled = !s.playing;
  $('#btn-live').disabled = !s.configured || (!live && !s.at_end);
  $('#btn-play').textContent = s.paused ? 'Continue' : 'Play';
  $('#btn-live').textContent = live ? 'Catch up to now' : 'Go to the end';
  $$('.jog button').forEach((b) => { b.disabled = !s.configured; });
}

// The bar spans what storage still holds. For a finished recording that is the
// whole event and it must not move; while live the right-hand edge is the live
// edge and necessarily grows.
function drawTimeline(s) {
  const from = s.earliest_ms || s.started_ms;
  const to = s.ended ? (s.end_ms || s.live_ms) : s.live_ms;
  const el = $('#timeline');
  if (!from || !to || to <= from) {
    el.dataset.from = ''; el.dataset.to = '';
    $('#tl-stored').style.cssText = '';
    $('#tl-downloaded').innerHTML = '';
    $('#tl-markers').innerHTML = '';
    $('#tl-head').style.left = '0';
    $('#tl-left').textContent = '';
    $('#tl-right').textContent = '';
    return;
  }
  el.dataset.from = String(from);
  el.dataset.to = String(to);
  const span = to - from;
  const pct = (ms) => Math.max(0, Math.min(100, ((ms - from) / span) * 100));

  $('#tl-stored').style.left = '0';
  $('#tl-stored').style.right = '0';

  // What is on this machine's disk — the part that would keep playing if the
  // connection died.
  $('#tl-downloaded').innerHTML = (s.cached_spans || []).map((sp) => {
    const a = pct(sp.from_ms), b = pct(sp.to_ms);
    return `<i style="left:${a}%;width:${Math.max(0.4, b - a)}%"></i>`;
  }).join('');

  // A cue's position comes from its media anchor — where in the programme it
  // sits — placed on this timeline against the event start. Its own at_ms
  // needed a wall->media mapping that drifted 1.11%, about forty seconds by the
  // end of an hour. cueAt() falls back for cues older than the field.
  $('#tl-markers').innerHTML = (s.markers || [])
    .map((m) => ({ at: cueAt(m, s), label: m.label }))
    .filter((m) => m.at !== null && m.at >= from && m.at <= to)
    .map((m) => `<i style="left:${pct(m.at)}%" title="${escapeHtml(m.label)}"></i>`)
    .join('');

  $('#tl-head').style.left = pct(s.seek_target_ms || s.playhead_ms) + '%';
  $('#tl-left').textContent = hhmmss(from);
  $('#tl-right').textContent = s.ended ? hhmmss(to) : hhmmss(to) + ' (now)';
}

// Where a cue sits on this page's timeline, in the same units the timeline uses.
//
// at_media_ms is the cue's own anchor: milliseconds into the programme, exact
// and needing no conversion. Placing it here is event start plus that. A cue
// written before the field falls back to its recorded time of day, which is all
// there is for it.
function cueAt(m, s) {
  if (m.at_media_ms >= 0 && s.started_ms) return s.started_ms + m.at_media_ms;
  return m.at_ms || null;
}

function drawCues(s) {
  const box = $('#cues');
  const markers = s.markers || [];
  if (!markers.length) { box.innerHTML = ''; return; }
  const vod = !!s.plays_as_recording;
  box.innerHTML = markers.map((m) => {
    const at = cueAt(m, s);
    const passed = at && s.playhead_ms && at <= s.playhead_ms;
    // Elapsed into the programme for a recording; time of day while live.
    const when = (m.at_media_ms >= 0 && vod)
      ? elapsed(m.at_media_ms)
      : (at ? hhmmss(at) : '');
    return `<button class="${passed ? 'passed' : ''}" data-marker="${escapeHtml(m.id)}">
              ${escapeHtml(m.label)} <span class="muted">${when}</span>
            </button>`;
  }).join('');
}

function drawReadout(s) {
  const cells = [];
  const push = (k, v, warn) =>
    cells.push(`<div class="cell${warn ? ' warn' : ''}"><div class="k">${k}</div>
                <div class="v">${v}</div></div>`);

  const net = netFor(s);
  const offline = !!(net && net.cls === 'off');

  push('Internet', net ? net.text : '—', offline);
  // The reliability figure that matters mid-event: how long this campus could
  // keep playing if its connection died right now.
  push('Could keep going for', spoken(s.buffered_ahead_s),
       offline || (s.playing && !s.paused && s.buffered_ahead_s < 30));
  push('Ready on disk', s.cached_segments ? spoken(s.cached_segments * 6) : '—');
  if (!s.ended) push('Behind the main site', spoken(s.behind_live_s));
  push('Download rate', s.download_samples ? rate(s.download_bytes_per_s) : '—');
  push('Sound', s.audio_channels
    ? s.audio_channels + ' channels' + (s.audio_track_label ? ' — ' + s.audio_track_label : '')
    : '—');
  if (s.interrupted) push('Ended', 'the sending stopped early', true);

  $('#readout').innerHTML = cells.join('');
}

function escapeHtml(s) {
  return String(s == null ? '' : s).replace(/[&<>"']/g, (c) => ({
    '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;',
  }[c]));
}

/* ── Controls ────────────────────────────────────────────────────────────── */

async function control(path) {
  try {
    status = await api('POST', path);
    drawStatus();
    notify('');
  } catch (e) {
    notify(e.message, true);
  }
}

// "Play" means "go to air", and while held it means "carry on" — the same two
// jobs the dock's Play button has, because it is the same button to an operator.
$('#btn-play').onclick = () => control(status && status.paused ? '/api/decoder/continue'
                                                              : '/api/decoder/play');
$('#btn-hold').onclick = () => control('/api/decoder/hold');
$('#btn-stop').onclick = () => control('/api/decoder/stop');
$('#btn-live').onclick = () => control('/api/decoder/catch-up');

$$('.jog button').forEach((b) => {
  b.onclick = () => control('/api/decoder/jog?seconds=' + encodeURIComponent(b.dataset.jog));
});

$('#btn-delay').onclick = () => {
  const minutes = Number($('#delay-min').value || 0);
  control('/api/decoder/delay?seconds=' + (minutes * 60));
};

$('#cues').addEventListener('click', (e) => {
  const b = e.target.closest('button[data-marker]');
  if (b) control('/api/decoder/marker?id=' + encodeURIComponent(b.dataset.marker));
});

$('#lock').onclick = async () => {
  if (status && status.source_locked) {
    notify('The controls on this machine are locked from the OBS panel.', true);
    return;
  }
  const on = !(status && status.locked);
  try {
    await api('POST', '/api/lock?on=' + (on ? '1' : '0'));
    await refreshStatus();
  } catch (e) {
    notify(e.message, true);
  }
};

// Clicking the timeline goes to that moment. Hovering reports what is under the
// cursor first, so a click is never a guess — the same behaviour the campus
// player's own page has.
const timeline = $('#timeline');
timeline.addEventListener('click', (e) => {
  const from = Number(timeline.dataset.from), to = Number(timeline.dataset.to);
  if (!from || !to) return;
  const r = timeline.getBoundingClientRect();
  const at = from + ((e.clientX - r.left) / r.width) * (to - from);
  control('/api/decoder/seek?ms=' + Math.round(at));
});
timeline.addEventListener('pointermove', (e) => {
  const from = Number(timeline.dataset.from), to = Number(timeline.dataset.to);
  if (!from || !to) { $('#tl-hover').textContent = ''; return; }
  const r = timeline.getBoundingClientRect();
  const at = from + ((e.clientX - r.left) / r.width) * (to - from);
  $('#tl-hover').textContent = hhmmss(at);
});
timeline.addEventListener('pointerleave', () => { $('#tl-hover').textContent = ''; });

/* ── Recordings ──────────────────────────────────────────────────────────── */

async function refreshEvents() {
  try {
    events = await api('GET', '/api/decoder/events');
    const note = $('#events-note');
    if (events.loading && !events.listed_once) note.textContent = 'Looking…';
    else if (events.error) note.textContent = events.error;
    else if (events.fallback_scan) note.textContent =
      'These were found by scanning; older events may take a moment.';
    else if (events.skipped) note.textContent =
      events.skipped + ' could not be read and are not shown.';
    else note.textContent = '';

    // Rebuilt only when the contents change: this refreshes while somebody is
    // reaching for a row, and replacing the list under their finger loses the
    // selection they were about to press Load on.
    const sig = JSON.stringify((events.events || []).map(
      (e) => [e.event_id, e.name, e.started_ms, e.duration_s, e.state]));
    if (sig === eventsSignature) {
      markPlayingRow();
      return;
    }
    eventsSignature = sig;

    const ul = $('#events');
    if (!(events.events || []).length) {
      ul.innerHTML = events.listed_once
        ? '<li class="muted">No events stored for this room yet.</li>' : '';
      return;
    }
    ul.innerHTML = events.events.map((e) => {
      const badge = e.state === EVENT.LIVE ? ['live', 'On air now']
                  : e.state === EVENT.INTERRUPTED ? ['interrupted', 'Stopped early']
                  : null;
      return `<li data-event="${escapeHtml(e.event_id)}">
                <span class="when">${escapeHtml(e.name || e.event_id)}
                  <small>${shortDateTime(e.started_ms)}${e.duration_s
                    ? ' · ' + spoken(e.duration_s) : ''}</small></span>
                ${badge ? `<span class="badge ${badge[0]}">${badge[1]}</span>` : ''}
              </li>`;
    }).join('');

    ul.querySelectorAll('li[data-event]').forEach((li) => {
      li.onclick = () => {
        ul.querySelectorAll('li').forEach((x) => x.classList.remove('playing'));
        li.classList.add('playing');
        selectedEvent = li.dataset.event;
      };
      li.ondblclick = () => {
        selectedEvent = li.dataset.event;
        loadSelected();
      };
    });
    markPlayingRow();
  } catch (e) {
    $('#events-note').textContent = e.message;
  }
}

// Which row is playing is current state, so it is marked from the status rather
// than from anything remembered when the list was drawn.
function markPlayingRow() {
  const playing = status ? (status.event_id || '') : '';
  document.querySelectorAll('#events li[data-event]').forEach((li) =>
    li.classList.toggle('playing', li.dataset.event === playing));
}

async function loadSelected() {
  const id = selectedEvent;
  if (!id) { notify('Choose an event from the list first.', true); return; }
  try {
    status = await api('POST', '/api/decoder/load-event', { event_id: id });
    drawStatus();
    notify('Loading that recording…');
  } catch (e) {
    notify(e.message, true);
  }
}

$('#btn-load').onclick = loadSelected;

$('#btn-return-live').onclick = async () => {
  try {
    status = await api('POST', '/api/decoder/return-to-live');
    drawStatus();
    notify('Back to whatever this room is playing now.');
  } catch (e) {
    notify(e.message, true);
  }
};

$('#btn-events-refresh').onclick = async () => {
  try {
    await api('POST', '/api/decoder/events/refresh');
  } catch (e) {
    notify(e.message, true);
  }
  refreshEvents();
};

/* ── Settings ────────────────────────────────────────────────────────────── */

const SECRET_PLACEHOLDER = '\u2022\u2022\u2022\u2022\u2022\u2022\u2022\u2022';

// Which fields the storage-provider dropdown needs (PROJECT-SCOPE.md section
// 8.6) — fetched once, since the list is static. Keyed by provider key
// ("r2", "aws", …) for updateProviderFields() below — identical to the
// appliance's, the relay's and the encoder page's copy of this same logic.
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
      o.textContent = p.display_name;
      o.disabled = !p.available;
      sel.appendChild(o);
    });
    if (keep) sel.value = keep;
  } catch (e) {
    /* An older build with no such endpoint yet. The raw fields still work. */
  }
}

function updateProviderFields() {
  const info = storageProviders[$('#s-provider').value];
  if (!info) return;
  $('#field-account').hidden  = !info.needs_account_id;
  $('#field-endpoint').hidden = !info.needs_endpoint;
  $('#field-region').hidden   = !info.needs_region;
}
$('#s-provider').addEventListener('change', updateProviderFields);

async function loadSettings() {
  try {
    const s = await api('GET', '/api/decoder/settings');

    await loadStorageProviderChoices();
    $('#s-provider').value = s.storage_provider || 'custom';
    updateProviderFields();

    const f = $('#settings-form');
    for (const el of f.elements) {
      if (!el.name || el.name === 'storage_provider') continue;
      const v = s[el.name];
      if (v === undefined) continue;
      el.value = v;
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
    if (!el.name) continue;
    if (el.type === 'number') {
      const n = Number(el.value);
      if (!Number.isNaN(n)) out[el.name] = n;
      continue;
    }
    out[el.name] = el.value.trim();
  }
  // The dots mean "unchanged", so they are never posted back as a secret.
  if (out.secret_access_key === SECRET_PLACEHOLDER) delete out.secret_access_key;
  if (out.lan_auth_token === SECRET_PLACEHOLDER) delete out.lan_auth_token;

  try {
    await api('POST', '/api/decoder/settings', out);
    note.textContent = 'Saved. This machine is already using it.';
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
    ['Machine role', 'campus — receiving'],
    ['Room', s.room_id || '—'],
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
    if (tab.dataset.tab === 'events') refreshEvents();
    if (tab.dataset.tab === 'settings') { loadSettings(); drawFacts(); }
    if (tab.dataset.tab === 'log') refreshLog();
  };
});

function startPolling() {
  if (pollTimer) clearInterval(pollTimer);
  refreshStatus();
  pollTimer = setInterval(() => {
    refreshStatus();
    if ($('#tab-events').classList.contains('is-on')) refreshEvents();
    if ($('#tab-log').classList.contains('is-on') && $('#log-follow').checked)
      refreshLog();
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





