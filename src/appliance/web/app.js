/*
 * app.js — the operator interface.
 *
 * One rule runs through all of it: the language is the language of an event,
 * not of a video pipeline. Times are clock times, durations are minutes, and
 * nothing here ever says "segment", "buffer" or "live edge". The audience is a
 * volunteer who has been handed a tablet, not the person who wrote it.
 *
 * The page polls one status document and draws everything from it, so no two
 * parts of the interface can ever disagree about what is happening.
 */

'use strict';

const $  = (sel) => document.querySelector(sel);
const $$ = (sel) => Array.from(document.querySelectorAll(sel));

// Matches multisite::RoomState.
const ROOM = { UNKNOWN: 0, OFFLINE: 1, LIVE: 2, ENDED: 3, INTERRUPTED: 4 };// Matches multisite::EventState.
// Per-event state from the catalog, for badging rows in the EVENT LIST below.
// This is NOT "is what I am playing now a recording" — that question has one
// answer, s.plays_as_recording, and this enum must not be pressed into
// answering it. See BUGS.md D1.
const EVENT = { UNKNOWN: 0, LIVE: 1, RECORDING: 2, INTERRUPTED: 3 };

// Where an operator is sent when a newer build exists: the releases page lists
// what changed and carries the download, which is the whole answer.
const RELEASES_URL = 'https://github.com/stageaudioworks/obs-multisite/releases';

let status = null;
let settings = null;
let systemInfo = null;
let pollTimer = null;
// The offset between this device's clock and the box's, so a phone with the
// wrong time still shows the same reading as the box does.
let clockSkewMs = 0;

/* ── Small helpers ───────────────────────────────────────────────────────── */

// Elapsed time within an event: "4:05", or "1:24:15" for a long one. This is
// how every position on this page is stated.
function elapsedClock(ms) {
  if (!ms || ms < 0) ms = 0;
  const t = Math.floor(ms / 1000);
  const h = Math.floor(t / 3600);
  const m = Math.floor((t % 3600) / 60);
  const sec = String(t % 60).padStart(2, '0');
  return h > 0 ? `${h}:${String(m).padStart(2, '0')}:${sec}` : `${m}:${sec}`;
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
  return gb >= 1 ? gb.toFixed(1) + ' GB' : (n / 1e6).toFixed(0) + ' MB';
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
    status = await api('GET', '/api/status');
    clockSkewMs = status.now_ms - Date.now();
    drawStatus();
  } catch (e) {
    $('#state').textContent = 'No answer';
    $('#state').className = 'state offline';
    $('#room').textContent = 'Cannot reach the player — is it still powered?';
    $('#net').hidden = true;
  }
}

// The internet pill. `null` until a request has actually been observed, so the
// pill never claims a reading it does not have.
function netFor(s) {
  if (!s.configured || !s.link_known) return null;
  if (s.link_health === 0) return { text: 'Internet good', cls: 'good' };
  if (s.link_health === 1) return { text: 'Internet unstable', cls: 'degraded' };
  return { text: 'No internet', cls: 'off' };
}

// What the banner says. A live event, a recording of a past one, and a
// broadcast that has just closed are three different things to an operator and
// must not read the same.
function bannerFor(s) {
  if (!s.configured) return { text: 'Not set up', cls: 'offline' };
  // The venue's connection to the bucket is gone. This is not the main site
  // going off air, so it must not read "Nothing on air".
  if (s.link_known && s.link_health === 2) return { text: 'No connection', cls: 'offline' };
  switch (s.room_state) {
    case ROOM.LIVE: return { text: 'Live', cls: 'live' };
    case ROOM.ENDED:
      return s.was_live
        ? { text: 'Broadcast ended', cls: 'ended' }
        : { text: 'Recording', cls: 'recording' };
    case ROOM.INTERRUPTED:
      return { text: 'Interrupted', cls: 'ended' };
    case ROOM.OFFLINE: return { text: 'Nothing on air', cls: 'offline' };
    default: return { text: 'Looking…', cls: 'offline' };
  }
}

function drawStatus() {
  const s = status;
  const banner = bannerFor(s);
  $('#state').textContent = banner.text;
  $('#state').className = 'state ' + banner.cls;

  // The always-on internet pill.
  const net = netFor(s);
  const netEl = $('#net');
  if (net) {
    netEl.hidden = false;
    netEl.textContent = net.text;
    netEl.className = 'net ' + net.cls;
  } else {
    netEl.hidden = true;
    netEl.className = 'net';
  }

  let room = s.room_id || '';
  if (s.pinned_event_id) room += ' · playing a past event';
  if (s.live_elsewhere) room += ' · something is live now';
  $('#room').textContent = room;

  $('#lock').textContent = s.locked ? 'Locked' : 'Lock';
  $('#lock').className = 'lock' + (s.locked ? ' on' : '');

  // ── The reading that matters ───────────────────────────────────────────
  const clock = $('#clock');
  clock.textContent = elapsedClock(s.playhead_ms);
  // While a jump is in flight the time shown is where playback is GOING, not
  // where the picture is. Say so rather than letting it read as fact.
  clock.classList.toggle('provisional', !!s.seek_target_ms);

  let sub;
  if (!s.configured) {
    sub = 'Open Settings and enter the storage details.';
  } else if (s.loading) {
    sub = 'Loading…';
  } else if (!s.playing) {
    sub = 'Not going out. Downloading in the background.';
  } else if (s.paused) {
    sub = 'Holding the picture. Still downloading.';
  } else if (s.buffering) {
    sub = 'Gathering enough to start…';
  } else if (s.plays_as_recording) {
    // A recording has an end, so it reports position out of length the way a
    // media player does. "Behind live" means nothing here — and that is true
    // of a PINNED event too, while the room it came from is still live.
    // playhead_ms IS how far in, so there is no event start to subtract. It
    // used to be a time of day and this line turned it back into a position —
    // the conversion that drifted.
    sub = `${elapsed(s.playhead_ms)} of ${elapsed(s.total_ms)}` +
          (s.at_end ? ' · at the end' : '');
  } else if (s.link_known && s.link_health === 2 && s.buffered_ahead_s > 1) {
    // The internet is gone but the buffer still has content: the one thing an
    // operator needs to hear is that there is time to act.
    sub = 'No internet — still playing, about ' + spoken(s.buffered_ahead_s) + ' left';
  } else if (s.behind_live_s < 3) {
    sub = 'Right up to date';
  } else {
    sub = spoken(s.behind_live_s) + ' behind the main site';
  }
  if (s.current_marker) sub += ' · ' + s.current_marker;
  $('#position').textContent = sub;

  drawTimeline(s);
  drawTransport(s);
  drawCues(s);
  drawReadout(s);

  // An internet outage takes priority over the raw error text while it lasts:
  // "no internet, but you have N minutes of buffer" is the sentence an operator
  // can act on, and "HTTP 0" is not.
  if (net && net.cls === 'off') {
    const left = s.buffered_ahead_s > 1
      ? ' The picture keeps playing for about ' + spoken(s.buffered_ahead_s) + ' more.'
      : '';
    notify('No internet — the player cannot reach the broadcast storage.' + left, true);
  } else {
    notify(s.last_error || '', true);
  }
}

function drawTransport(s) {
  // Whether "now" is somewhere this transport can go. Pinned to a past event,
  // it is not: the button has to offer the end of the recording, not the live
  // edge of a room the operator deliberately stepped away from.
  const live = !s.plays_as_recording;
  $('#btn-play').disabled = !s.configured || (s.playing && !s.paused);
  $('#btn-hold').disabled = !s.playing || s.paused;
  $('#btn-stop').disabled = !s.playing;
  $('#btn-live').disabled = !live || !s.configured;
  $('#btn-play').textContent = s.paused ? 'Continue' : 'Play';
  $('#btn-live').textContent = live ? 'Catch up to now' : 'Go to the end';
  $$('.jog button').forEach((b) => { b.disabled = !s.configured; });
}

// The bar spans what storage still holds. For a finished recording that is the
// whole event and it must not move; while live the right-hand edge is the
// live edge and necessarily grows.
function drawTimeline(s) {
  // Media time, and `|| started_ms` would be two faults at once: it mixes a
  // position with a time of day, and it treats earliest_ms === 0 as absent
  // when 0 is exactly where a recording with its first segment still in
  // storage begins. Explicit, and numeric.
  const from = (typeof s.earliest_ms === 'number') ? s.earliest_ms : 0;
  // s.plays_as_recording, not s.ended: a pinned event is played as a recording
  // while the room is still live, and spanning it to the live edge made the bar
  // grow under a playhead that was not moving.
  const to = s.plays_as_recording ? (s.end_ms || s.live_ms) : s.live_ms;
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

  // What is actually on this box's disk — the part that would keep playing if
  // the connection died.
  $('#tl-downloaded').innerHTML = (s.cached_spans || []).map((sp) => {
    const a = pct(sp.from_ms), b = pct(sp.to_ms);
    return `<i style="left:${a}%;width:${Math.max(0.4, b - a)}%"></i>`;
  }).join('');

  $('#tl-markers').innerHTML = (s.markers || [])
    .filter((m) => m.at_ms >= from && m.at_ms <= to)
    .map((m) => `<i style="left:${pct(m.at_ms)}%" title="${escapeHtml(m.label)}"></i>`)
    .join('');

  $('#tl-head').style.left = pct(s.playhead_ms) + '%';
  $('#tl-left').textContent = elapsedClock(from);
  $('#tl-right').textContent =
    s.plays_as_recording ? elapsedClock(to) : elapsedClock(to) + ' (now)';
}

function drawCues(s) {
  const box = $('#cues');
  const markers = s.markers || [];
  if (!markers.length) { box.innerHTML = ''; return; }
  // A recording's cue times run 00:00 to the end of the event; live they are
  // times of day. Same rule the playhead readout follows.
  const vod = !!s.plays_as_recording;
  const started = s.started_ms || 0;
  box.innerHTML = markers.map((m) => {
    // Where in the programme this cue sits, from its own anchor. The box used
    // to subtract the event start from a time of day, which needed a wall->media
    // mapping that drifted 1.11% — about forty seconds by the end of an hour.
    // -1 means a cue older than the field with no event start to convert it.
    const media = (m.at_media_ms >= 0)
      ? m.at_media_ms
      : ((m.at_ms && started && m.at_ms >= started) ? m.at_ms - started : null);
    const passed = media !== null && s.playhead_ms && started
                 && (started + media) <= s.playhead_ms;
    const who = m.author
      ? ' <span class="muted">' + escapeHtml(m.author) + '</span>' : '';
    // Elapsed for a recording; time of day only while following a live event.
    // Elapsed whether live or recorded: a cue means "this moment in the
    // service", and that reads the same either way.
    const when = (media !== null) ? elapsedClock(media) : '';
    return `<button class="${passed ? 'passed' : ''}" data-marker="${escapeHtml(m.id)}">
              ${escapeHtml(m.label)}${who} <span class="muted">${when}</span>
            </button>`;
  }).join('');
}

// Whether the sound is worth flagging in the readout.
//
// "Muted" is not a fault — it is a setting somebody chose — so it does not earn
// the amber border that means "go and look at this". A card that would not open
// does, and those are the two states the box itself distinguishes for us.
function audioOutWarn(s) {
  return s.audio_state === 'failed';
}

function drawReadout(s) {
  const cells = [];
  const push = (k, v, warn) =>
    cells.push(`<div class="cell${warn ? ' warn' : ''}"><div class="k">${k}</div>
                <div class="v">${v}</div></div>`);

  const net = netFor(s);
  const offline = !!(net && net.cls === 'off');

  push('Internet', net ? net.text : '—', offline);
  // The reliability figure that actually matters mid-event: how long this
  // campus could keep broadcasting if its connection died right now.
  push('Could keep going for', spoken(s.buffered_ahead_s),
       offline || (s.playing && !s.paused && s.buffered_ahead_s < 30));
  push('Ready on disk', s.cached_segments ? spoken(s.cached_segments * 6) : '—');
  // Meaningless against a recording: there is no live edge to be behind.
  if (!s.plays_as_recording)
    push('Behind the main site', spoken(s.behind_live_s));
  push('Picture', s.video_width ? `${s.video_width}×${s.video_height}` : '—');
  push('Sound', s.audio_channels ? s.audio_channels + ' channels' : '—');
  // Where the sound is actually going, not just where it was asked to go. The
  // description comes from the card that is open, so it changes to say "muted"
  // or to carry the card's own error the moment either of those happens — the
  // readout and the meters beside it can then never disagree.
  push('Going out on', escapeHtml(s.output_description || '—'),
       !s.video_output_ok);
  push('Sound out on',
       escapeHtml(s.audio_description || s.audio_state || '—'),
       audioOutWarn(s));
  if (s.download_failures || s.checksum_failures)
    push('Re-fetched', String(s.download_failures + s.checksum_failures), true);
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
  } catch (e) {
    notify(e.message, true);
  }
}

$('#btn-play').onclick = () => control(status && status.paused ? '/api/continue' : '/api/play');
$('#btn-hold').onclick = () => control('/api/hold');
$('#btn-stop').onclick = () => control('/api/stop');
$('#btn-live').onclick = () => control('/api/catch-up');

$$('.jog button').forEach((b) => {
  b.onclick = () => control('/api/jog?seconds=' + encodeURIComponent(b.dataset.jog));
});

$('#btn-delay').onclick = () => {
  const minutes = Number($('#delay-min').value || 0);
  control('/api/delay?seconds=' + (minutes * 60));
};

$('#cues').addEventListener('click', (e) => {
  const b = e.target.closest('button[data-marker]');
  if (b) control('/api/marker?id=' + encodeURIComponent(b.dataset.marker));
});

// Drop a cue with whatever name is typed. The one control here that writes, so
// a refusal ("no site name is set") is shown rather than swallowed.
$('#btn-cue').onclick = async () => {
  const label = $('#cue-name').value.trim() || 'Cue';
  try {
    status = await api('POST', '/api/cue?label=' + encodeURIComponent(label));
    $('#cue-name').value = '';
    notify('');
    drawStatus();
  } catch (e) { notify(e.message, true); }
};

$('#lock').onclick = async () => {
  const on = !(status && status.locked);
  try {
    status = await api('POST', '/api/lock?on=' + (on ? '1' : '0'));
    drawStatus();
  } catch (e) { notify(e.message, true); }
};

// Clicking the timeline goes to that moment. Hovering reports what is under
// the cursor first, so a click is never a guess.
const timeline = $('#timeline');
timeline.addEventListener('click', (e) => {
  const from = Number(timeline.dataset.from), to = Number(timeline.dataset.to);
  if (!from || !to) return;
  const r = timeline.getBoundingClientRect();
  const at = from + ((e.clientX - r.left) / r.width) * (to - from);
  control('/api/seek?ms=' + Math.round(at));
});
timeline.addEventListener('pointermove', (e) => {
  const from = Number(timeline.dataset.from), to = Number(timeline.dataset.to);
  if (!from || !to) { $('#tl-hover').textContent = ''; return; }
  const r = timeline.getBoundingClientRect();
  const at = from + ((e.clientX - r.left) / r.width) * (to - from);
  $('#tl-hover').textContent = elapsedClock(at);
});
timeline.addEventListener('pointerleave', () => { $('#tl-hover').textContent = ''; });

/* ── Events (the event list) ───────────────────────────────────────────── */

async function refreshEvents() {
  try {
    const listing = await api('GET', '/api/events');
    const ul = $('#events');
    const note = $('#events-note');

    if (listing.loading && !listing.listed_once) note.textContent = 'Looking…';
    else if (listing.error) note.textContent = listing.error;
    else if (listing.no_catalog) note.textContent =
      'Recordings are listed from cloud storage, and none is set up on this '
      + 'machine — an event sent over the LAN only cannot be listed here. '
      + 'What is on air can still be watched.';
    else if (listing.fallback_scan) note.textContent =
      'These were found by scanning; older events may take a moment.';
    else if (listing.skipped) note.textContent =
      listing.skipped + ' could not be read and are not shown.';
    else note.textContent = '';

    if (!listing.events.length) {
      ul.innerHTML = listing.listed_once
        ? '<li class="muted">No events stored for this room yet.</li>' : '';
      return;
    }

    const playing = status ? (status.event_id || '') : '';
    ul.innerHTML = listing.events.map((e) => {
      const badge = e.state === EVENT.LIVE ? ['live', 'On air now']
                  : e.state === EVENT.INTERRUPTED ? ['interrupted', 'Cut short']
                  : e.state === EVENT.RECORDING ? ['', 'Recording']
                  : ['', 'Unknown'];
      const isPlaying = e.event_id === playing;
      const when = escapeHtml(shortDateTime(e.started_ms));
      const label = (e.name || '').trim();
      const main = label || when;
      const small = label
        ? `${when}${e.duration_s ? ' · ' + spoken(e.duration_s) + ' long' : ''}`
        : (e.duration_s ? spoken(e.duration_s) + ' long' : '');
      return `<li class="${isPlaying ? 'playing' : ''}">
        <span class="badge ${badge[0]}">${badge[1]}</span>
        <span class="when">${escapeHtml(main)}
          <small>${escapeHtml(small)}</small></span>
        <button data-load="${escapeHtml(e.event_id)}">
          ${isPlaying ? 'Playing' : 'Load'}</button>
      </li>`;
    }).join('');
  } catch (e) {
    $('#events-note').textContent = e.message;
  }
}

$('#events').addEventListener('click', async (e) => {
  const b = e.target.closest('button[data-load]');
  if (!b) return;
  await control('/api/load?event=' + encodeURIComponent(b.dataset.load));
  refreshEvents();
});
$('#btn-refresh-events').onclick = async () => {
  $('#events-note').textContent = 'Looking…';
  await api('POST', '/api/events/refresh');
  setTimeout(refreshEvents, 1200);
};
$('#btn-follow-live').onclick = async () => {
  await control('/api/follow-live');
  refreshEvents();
};

/* ── Settings ────────────────────────────────────────────────────────────── */

// Which fields the storage-provider dropdown needs (PROJECT-SCOPE.md §8.6) —
// fetched once, since the list is static. Keyed by provider key ("r2", "aws",
// …) for updateProviderFields() below.
let storageProviders = {};

async function loadStorageProviderChoices() {
  try {
    const data = await api('GET', '/api/storage/providers');
    const sel = $('#c-provider');
    storageProviders = {};
    sel.innerHTML = (data.providers || []).map((p) => {
      storageProviders[p.key] = p;
      // display_name already carries its own "(coming soon)" where that
      // applies (see storage_providers.cpp) — just disable the option.
      return `<option value="${escapeHtml(p.key)}"${p.available ? '' : ' disabled'}>
                ${escapeHtml(p.display_name)}
              </option>`;
    }).join('');
  } catch (e) {
    /* An older box with no such endpoint yet. The raw fields still work. */
  }
}

// Shows only the fields the selected provider actually needs — an account id
// for R2, a region for AWS/Backblaze/Wasabi, both endpoint and region for
// Custom — the same rule the OBS docks apply to the identical dropdown.
function updateProviderFields() {
  const key = $('#c-provider').value;
  const info = storageProviders[key];
  if (!info) return;
  $('#field-account').hidden  = !info.needs_account_id;
  $('#field-endpoint').hidden = !info.needs_endpoint;
  $('#field-region').hidden   = !info.needs_region;
}

async function loadSettings() {
  settings = await api('GET', '/api/config');
  const set = (sel, value) => { const el = $(sel); if (el) el.value = value; };
  set('#c-room', settings.room_id);
  set('#c-site', settings.site_name || '');
  set('#c-bucket', settings.bucket);
  set('#c-account', settings.r2_account_id);
  set('#c-endpoint', settings.endpoint_host);
  set('#c-region', settings.region);
  await loadStorageProviderChoices();
  set('#c-provider', settings.storage_provider);
  updateProviderFields();
  set('#c-key', settings.access_key_id);
  set('#c-secret', settings.secret_access_key);
  set('#c-lan-host', settings.lan_host || '');
  set('#c-lan-port', settings.lan_port);
  set('#c-lan-token', settings.lan_auth_token || '');
  set('#c-buffer', settings.buffer_minutes);
  set('#c-prebuffer', settings.prebuffer_segments);
  set('#c-start-buffer', settings.start_buffer_seconds);
  set('#c-cache', settings.cache_dir);
  set('#c-idle', settings.idle_mode);
  set('#c-tile', String(settings.tile_index ?? -1));
  set('#c-hwdecode', String(settings.hardware_decode !== false));
  set('#c-follownext', String(settings.follow_next_event !== false));
  set('#c-checkupdates', String(settings.check_updates !== false));
  set('#c-channels', settings.audio_channels);
  set('#c-audio-track', settings.audio_track);
  set('#c-audio-on', String(settings.audio_enabled));
  set('#c-autoplay', String(settings.auto_play));
  set('#c-zerotier', settings.zerotier_network_id || '');
  set('#c-cloudflared', settings.cloudflared_token || '');
  set('#c-aes67-on', String(!!settings.aes67_manage));
  set('#c-aes67-address', settings.aes67_address || '');
  set('#c-aes67-channels', settings.aes67_channels);

  // The network audio output takes the sound onto the AES67 card, and that card
  // is what the stream publishes — so while it is on, the output device is not
  // the operator's to choose. Offering a picker that cannot be honoured is how
  // the setting and the sound came to disagree in the first place.
  const onNet = !!settings.aes67_manage;
  $('#c-alsa').disabled = onNet;
  $('#c-alsa-note').textContent = onNet
    ? 'Taken over by the network audio output below: the sound is going to the ' +
      'AES67 card, because that is the card the stream publishes. Switch that ' +
      'off to choose a device here again.'
    : 'HDMI carries up to eight channels; a de-embedder at the campus recovers ' +
      'each one separately.';

  $('#idle-image-field').hidden = settings.idle_mode !== 'image';
  set('#c-idle-image', settings.idle_image_path);
  await loadOutputChoices();
}

$('#c-idle').addEventListener('change', (e) => {
  $('#idle-image-field').hidden = e.target.value !== 'image';
});

$('#c-provider').addEventListener('change', updateProviderFields);

// The display and sound-card pickers list what the box actually has, so a
// setting cannot be typed that the hardware will refuse.
//
// The box also says whether the sound device is the operator's to choose at
// all: while the network audio output is on, the sound has to be on the AES67
// card, because that is the card the daemon publishes. `audioDeviceLocked` is
// what the box said, and it is the box's answer and not the page's guess — the
// setting in the form could have been changed a moment ago and not yet saved.
let audioDeviceLocked = false;

async function loadOutputChoices() {
  try {
    const outputs = await api('GET', '/api/outputs');
    audioDeviceLocked = !!outputs.audio_device_locked;
    const disp = $('#c-display');
    disp.innerHTML = '<option value="">First connected screen</option>' +
      (outputs.displays || []).map((d) =>
        `<option value="${escapeHtml(d.connector)}"${
          d.connector === settings.connector ? ' selected' : ''}>
           ${escapeHtml(d.connector)}${d.connected ? '' : ' (nothing plugged in)'}
         </option>`).join('');

    const modes = [];
    (outputs.displays || []).forEach((d) => {
      if (settings.connector && d.connector !== settings.connector) return;
      (d.modes || []).forEach((m) => modes.push(m));
    });
    const sel = $('#c-mode');
    sel.innerHTML = '<option value="0x0x0">Whatever the screen prefers</option>' +
      modes.map((m) => {
        const key = `${m.width}x${m.height}x${Math.round(m.refresh_mhz / 1000)}`;
        const chosen = settings.out_width === m.width &&
                       settings.out_height === m.height &&
                       settings.out_fps === Math.round(m.refresh_mhz / 1000);
        return `<option value="${key}"${chosen ? ' selected' : ''}>
                  ${m.width}×${m.height} at ${(m.refresh_mhz / 1000).toFixed(2)} Hz
                  ${m.preferred ? ' — the screen prefers this' : ''}</option>`;
      }).join('');

    const alsa = $('#c-alsa');
    // Which device is the card the network output owns, if the box knows. The
    // server marks it, rather than the page guessing from the name: the name is
    // the daemon's, and matching on a substring in the browser is the sort of
    // thing that breaks when it changes.
    alsa.innerHTML = '<option value="default">Follow the system</option>' +
      (outputs.audio_devices || []).map((d) =>
        `<option value="${escapeHtml(d.id)}"${
          d.id === settings.alsa_device ? ' selected' : ''}>
           ${escapeHtml(d.description)}${
          d.locks_audio_device ? ' — the network audio output uses this' : ''}
         </option>`).join('');

    // The box's answer wins over the form's: this is about where the sound is,
    // not about what somebody has just typed. The reason travels from the box
    // too, so the sentence beside the picker and the lock cannot drift apart.
    if (audioDeviceLocked) {
      alsa.disabled = true;
      $('#c-alsa-note').textContent =
        outputs.audio_device_lock_reason ||
        'The network audio output below owns the sound card.';
    }
  } catch (e) {
    /* An older box, or no outputs to list. The text fields still work. */
  }
}

$('#settings-form').addEventListener('submit', async (e) => {
  e.preventDefault();
  const note = $('#settings-note');
  note.textContent = 'Saving…';

  // What the network audio output was before this save. Read before the PUT,
  // because afterwards `settings` is the new configuration and there would be
  // nothing left to compare against.
  const before = settings || {};

  const mode = ($('#c-mode').value || '0x0x0').split('x').map(Number);
  const body = {
    room_id: $('#c-room').value.trim(),
    site_name: $('#c-site').value.trim(),
    storage_provider: $('#c-provider').value,
    bucket: $('#c-bucket').value.trim(),
    r2_account_id: $('#c-account').value.trim(),
    endpoint_host: $('#c-endpoint').value.trim(),
    region: $('#c-region').value.trim() || 'auto',
    access_key_id: $('#c-key').value.trim(),
    secret_access_key: $('#c-secret').value,
    lan_host: $('#c-lan-host').value.trim(),
    lan_port: Number($('#c-lan-port').value) || 9080,
    lan_auth_token: $('#c-lan-token').value,
    buffer_minutes: Number($('#c-buffer').value),
    prebuffer_segments: Number($('#c-prebuffer').value),
    start_buffer_seconds: Number($('#c-start-buffer').value),
    cache_dir: $('#c-cache').value.trim(),
    connector: $('#c-display').value,
    out_width: mode[0] || 0,
    out_height: mode[1] || 0,
    out_fps: mode[2] || 0,
    tile_index: Number($('#c-tile').value),
    hardware_decode: $('#c-hwdecode').value === 'true',
    follow_next_event: $('#c-follownext').value === 'true',
    check_updates: $('#c-checkupdates').value === 'true',
    idle_mode: $('#c-idle').value,
    idle_image_path: $('#c-idle-image').value.trim(),
    audio_enabled: $('#c-audio-on').value === 'true',
    alsa_device: $('#c-alsa').value,
    audio_channels: Number($('#c-channels').value),
    audio_track: Number($('#c-audio-track').value),
    auto_play: $('#c-autoplay').value === 'true',
  };

  try {
    // A disabled picker still has a value, and sending it is how the sound came
    // to be moved off the AES67 card while the stream stayed switched on: the
    // page offered `default` in the box, the box saved `default`, and the
    // stream it was still publishing went silent. While the network output owns
    // the device, the device is not sent at all — the box keeps whatever it has,
    // which is the AES67 card it was told to use.
    if ($('#c-alsa').disabled) delete body.alsa_device;

    settings = await api('PUT', '/api/config', body);

    // The network audio output is not an ordinary setting: switching it on also
    // moves the sound onto the AES67 card and tells the daemon what to publish.
    // Saving the rest of the box without doing that would leave the setting and
    // the sound disagreeing, which is the fault this exists to remove. Only an
    // actual change is sent, so pressing Save twice does not disturb a stream
    // that is already working.
    const net = {
      enabled: $('#c-aes67-on').value === 'true',
      address: $('#c-aes67-address').value.trim(),
      channels: Number($('#c-aes67-channels').value),
    };
    const netChanged = net.enabled !== !!before.aes67_manage ||
                       net.address !== (before.aes67_address || '') ||
                       net.channels !== before.aes67_channels;

    let message = 'Saved.';
    if (netChanged) {
      const s = await api('POST', '/api/aes67/source', net);
      if (s.problem) message = s.problem;
    }
    note.textContent = message;
    await loadSettings();
    setTimeout(() => { note.textContent = ''; },
               message === 'Saved.' ? 3000 : 12000);
  } catch (err) {
    note.textContent = err.message;
  }
});

/* ── Remote access ───────────────────────────────────────────────────────────
   Its own button rather than the main Save, because applying it does
   something immediate — joins a network, starts a tunnel — and an operator
   wants to see that answer, including when the tool is not installed yet. */

$('#btn-remote').addEventListener('click', async () => {
  const note = $('#remote-note');
  note.textContent = 'Applying…';
  try {
    const r = await api('POST', '/api/remote', {
      zerotier_network_id: $('#c-zerotier').value.trim(),
      cloudflared_token: $('#c-cloudflared').value,
    });
    const problems = [r.zerotier_error, r.cloudflared_error].filter(Boolean);
    note.textContent = problems.length ? problems.join(' ') : 'Applied.';
    await loadSettings();
    loadRemote();
  } catch (err) {
    note.textContent = err.message;
  }
  setTimeout(() => { note.textContent = ''; }, 8000);
});

/* ── The sound on the network ────────────────────────────────────────────────
   Its own button rather than the main Save, like remote access, because
   applying it does something immediate on another program — starts a daemon,
   creates a stream — and the answer matters more than the setting did.

   Everything shown here is read back from the daemon rather than assumed from
   the settings beside it. The two can disagree, and when they do the daemon is
   the one that is true: a stream can be configured and switched off, or
   switched on and fed nothing because the player is still writing the sound to
   HDMI. Those look identical from the settings and are different faults. */

async function loadAes67() {
  const facts = $('#aes67-facts');
  if (!facts) return;
  let s = null;
  try { s = await api('GET', '/api/aes67'); } catch (e) { return; }

  const rows = [];
  const add = (k, v) => rows.push(`<dt>${k}</dt><dd>${escapeHtml(v)}</dd>`);

  if (s.installed) {
    add('AES67 service', s.service_active ? 'running' : 'not running');
    if (s.rest_reachable) {
      add('PTP clock', s.ptp_locked
          ? ('locked' + (s.ptp_gmid ? ' to ' + s.ptp_gmid : '') +
             (s.ptp_jitter ? ' (±' + s.ptp_jitter.toFixed(1) + ' ns)' : ''))
          : (s.ptp_status || 'not locked'));
      if (s.source_present) {
        add('Stream', (s.source_enabled ? 'sending' : 'stopped') +
            (s.source_address ? ' to ' + s.source_address : '') +
            (s.sdp_port ? ':' + s.sdp_port : ''));
        add('Stream format', (s.sdp_codec || 'L24') + ', ' +
            (s.sdp_channels || s.source_channels) + ' channels, 48 kHz');
      } else {
        add('Stream', 'not created yet');
      }
      add('AES67 sound card', s.card_present
          ? (s.player_on_card ? 'registered, and this player is using it'
                              : 'registered, but this player is NOT using it')
          : 'not registered with ALSA');
    }
  } else {
    add('AES67', 'not installed on this box');
  }
  facts.innerHTML = rows.join('');

  // The one thing an operator needs: why the sound is, or is not, leaving.
  const problems = [];
  if (s.error) problems.push(s.error);
  if (!s.installed) {
    problems.push('The AES67 stack is a separate install on this box; until it ' +
                  'is there, nothing here can send anything.');
  } else {
    if (!s.service_active)
      problems.push('The AES67 daemon is not running.');
    if (s.service_active && !s.rest_reachable)
      problems.push('The daemon is running but is not answering, so it cannot ' +
                    'be set up or asked anything.');
    if (s.rest_reachable && s.ptp_known && !s.ptp_locked)
      problems.push('The clock is not locked. This daemon is a PTP slave: with ' +
                    'nothing on the network handing out the clock it sends no ' +
                    'audio at all. That is a network question, not a fault here.');
    if (s.sources_known && !s.source_present)
      problems.push('No stream has been set up yet — switch on the network ' +
                    'audio output under Settings.');
    if (s.source_present && !s.source_correct)
      problems.push('The stream on the daemon is not the shape this box asks ' +
                    'for. Applying the network audio output under Settings will ' +
                    'put it right.');
    if (s.source_present && !s.source_enabled)
      problems.push('The stream is switched off, so nothing is being sent.');
    if (s.source_present && !s.card_present)
      problems.push('The AES67 sound card is not registered with ALSA, so the ' +
                    'kernel module is probably not loaded.');
    if (s.source_present && s.card_present && !s.player_on_card)
      problems.push('This player is not writing to the AES67 card, so the ' +
                    'stream would carry silence — which the network audio ' +
                    'output under Settings normally arranges by itself. Apply ' +
                    'it again to put the sound back on that card.');
  }

  const hint = $('#aes67-hint');
  if (hint) {
    hint.textContent = problems.length ? problems.join(' ')
                                       : (s.installed ? 'Sound is going onto the network.' : '');
  }

  // The SDP verbatim, when the daemon has published one.
  const box = $('#aes67-sdp-box');
  if (box) {
    if (s.sdp) {
      box.hidden = false;
      $('#aes67-sdp').textContent = s.sdp;
    } else {
      box.hidden = true;
    }
  }
}

$('#btn-aes67').addEventListener('click', async () => {
  const note = $('#aes67-note');
  note.textContent = 'Applying…';
  try {
    const s = await api('POST', '/api/aes67/source', {
      enabled: $('#c-aes67-on').value === 'true',
      address: $('#c-aes67-address').value.trim(),
      channels: Number($('#c-aes67-channels').value),
    });
    note.textContent = s.problem ? s.problem : 'Applied.';
    await loadSettings();
    loadAes67();
  } catch (err) {
    note.textContent = err.message;
  }
  setTimeout(() => { note.textContent = ''; }, 8000);
});


/* ── This box ────────────────────────────────────────────────────────────── */

async function loadSystem() {
  try {
    systemInfo = await api('GET', '/api/system');
  } catch (e) { return; }
  const s = systemInfo;

  const rows = [];
  const add = (k, v) => rows.push(`<dt>${k}</dt><dd>${escapeHtml(v)}</dd>`);

  add('Name', s.hostname);
  (s.interfaces || []).forEach((n) => {
    if (n.ipv4) add('Address (' + n.name + ')', n.ipv4 + ':' + location.port);
  });
  add('Player version', s.version);
  if (s.update_newer && s.update_latest) {
    // Built here rather than through add(), which escapes its value: this one
    // is a link, and the tag is the only part of it that came from outside.
    rows.push(
      '<dt>Update</dt><dd><a href="' + RELEASES_URL + '" target="_blank" ' +
      'rel="noopener">' + escapeHtml(s.update_latest) + ' is available</a></dd>'
    );
  }
  if (s.model) add('Hardware', s.model);
  if (s.os_version) add('System', s.os_version);
  add('Clock', s.time.local_time + ' (' + s.time.timezone + ')');
  add('Network time', s.time.ntp_enabled
      ? (s.time.ntp_synchronised ? 'on, in step' : 'on, not yet in step') : 'off');
  // A clock that is out puts this campus's cues in the wrong place on every
  // other site's timeline, so it earns its own line. The figure is the store's
  // own clock (from the Date header on ordinary traffic) against this box's.
  const skew = Number(s.time.clock_skew_ms || 0);
  if (Math.abs(skew) >= 2000)
    add('Clock is out', (skew > 0 ? '+' : '') + Math.round(skew / 1000) + ' s against the store');
  add('Running for', spoken(s.uptime_s));
  if (s.cpu_temp_c) add('Temperature', s.cpu_temp_c.toFixed(1) + ' °C');
  if (s.disk && s.disk.total_bytes)
    add('Cache disk', `${bytes(s.disk.free_bytes)} free of ${bytes(s.disk.total_bytes)}` +
        (s.disk.is_sd_card ? ' — this is the SD card' : ''));
  $('#facts').innerHTML = rows.join('');

  // Both of these explain an event that stutters, and neither is visible any
  // other way.
  const warnings = [];
  if (s.under_voltage) warnings.push('The power supply is not keeping up.');
  if (s.throttled) warnings.push('The box is running hot and slowing itself down.');
  if (s.disk && s.disk.is_sd_card)
    warnings.push('The cache is on the SD card, which it will wear out. ' +
                  'Move it to a USB SSD.');
  if (s.disk && s.disk.health === 'critical')
    warnings.push(`The cache disk is almost full (${bytes(s.disk.free_bytes)} free). ` +
                  'Free up space or move the cache to a bigger drive.');
  else if (s.disk && s.disk.health === 'low')
    warnings.push(`The cache disk is getting low (${bytes(s.disk.free_bytes)} free).`);
  if (warnings.length) notify(warnings.join(' '), true);

  $('#s-ntp').value = String(!!s.time.ntp_enabled);

  if (!$('#s-tz').options.length) {
    try {
      const tz = await api('GET', '/api/system/timezones');
      $('#s-tz').innerHTML = tz.timezones
        .map((z) => `<option${z === s.time.timezone ? ' selected' : ''}>${escapeHtml(z)}</option>`)
        .join('');
    } catch (e) { /* leave it empty */ }
  }

  loadStorage(false);
  loadRemote();
  loadAes67();
}

/* ── Remote access ───────────────────────────────────────────────────────────
   What is set up, and what the box managed to do with it. "Set up but not
   running" is the state worth showing plainly: it is the one that means the
   box still cannot be reached. */

async function loadRemote() {
  const el = $('#remote-facts');
  if (!el) return;
  let r = null;
  try { r = await api('GET', '/api/remote'); } catch (e) { return; }
  const rows = [];
  const add = (k, v) => rows.push(`<dt>${k}</dt><dd>${escapeHtml(v)}</dd>`);

  const zt = r.zerotier || {};
  if (zt.joined) {
    add('Remote access address', zt.ip + ' (ZeroTier)');
  } else if (!zt.network_id) {
    add('ZeroTier', zt.installed ? 'Installed, no network set' : 'Not installed');
  } else if (!zt.installed) {
    add('ZeroTier', 'Not installed on this box');
  } else {
    add('ZeroTier', zt.running
      ? 'Joining ' + zt.network_id + ' — waiting to be authorised'
      : 'Set up, but the service is not running');
  }

  const cf = r.cloudflared || {};
  if (!cf.configured) {
    add('Cloudflare tunnel',
        cf.installed ? 'Installed, not set up' : 'Not installed');
  } else if (!cf.installed) {
    add('Cloudflare tunnel', 'Not installed on this box');
  } else if (cf.running) {
    add('Cloudflare tunnel', cf.hostname ? 'Running — ' + cf.hostname : 'Running');
  } else {
    add('Cloudflare tunnel', 'Set up, but the service is not running');
  }

  el.innerHTML = rows.join('');
}

/* ── Storage ─────────────────────────────────────────────────────────────────
   The three questions asked whenever a campus stutters, and the ones nothing
   here could answer: is the bucket reachable, where is it being served from,
   and what is the link managing. Without a probe these come from the segment
   traffic already flowing, so opening this panel costs nothing; "Check now"
   spends one request to say so definitively, including when nothing plays.  */

function storageLine(d) {
  if (!d.configured) return ['Not set up yet', false];
  // LAN-only (PROJECT-SCOPE.md §8.7): no cloud endpoint at all, so none of
  // reachable/readable/probed below mean anything — they describe the
  // bucket specifically, and there isn't one.
  if (!d.bucket && !d.endpoint) return ['LAN only — no cloud storage configured', false];
  if (d.probed && !d.reachable) return [d.error || 'Cannot be reached', true];
  if (d.probed && !d.readable)  return [d.error || 'Refused', true];
  if (d.probed) return ['Reachable' + (d.round_trip_ms
      ? ' — answered in ' + d.round_trip_ms + ' ms' : ''), false];
  if (d.reachable) return ['Reachable', false];
  return ['Not checked yet', false];
}

async function loadStorage(probe) {
  const el = $('#storage-facts');
  if (!el) return;
  let d;
  try {
    d = await api('GET', '/api/storage' + (probe ? '?probe=1' : ''));
  } catch (e) {
    el.innerHTML = '<dt>Storage</dt><dd class="bad">' + escapeHtml(e.message) + '</dd>';
    return;
  }

  const rows = [];
  const add = (k, v) => rows.push(`<dt>${k}</dt><dd>${escapeHtml(v)}</dd>`);
  const [state, bad] = storageLine(d);
  rows.push(`<dt>Storage</dt><dd${bad ? ' class="bad"' : ''}>${escapeHtml(state)}</dd>`);
  if (d.endpoint) add('Endpoint', d.endpoint + (d.bucket ? ' / ' + d.bucket : ''));
  // The most useful figure for a site a long way from its bucket: a box in
  // Johannesburg served from Amsterdam explains a latency nothing local can.
  if (d.colo) add('Served from', d.colo);
  else if (d.server) add('Served by', d.server);
  // Only once something has actually been measured. A zero would read as a
  // dead link rather than as an absence of evidence.
  if (d.rate_samples > 0 && d.bytes_per_s)
    add('Link speed', (d.bytes_per_s * 8 / 1e6).toFixed(1) + ' Mbps observed');
  // LAN / direct delivery (§8.7, "Visibility") — only shown once a LAN host
  // is actually configured. Which path answers can change request to
  // request (a segment that aged out of the LAN's retention window falls
  // back to cloud for that one alone), so this describes the most recent
  // fetch, not a sticky mode.
  if (d.lan_configured) add('Delivery path', d.lan_active ? 'via LAN' : 'via cloud');
  el.innerHTML = rows.join('');
}

$('#btn-tz').onclick = async () => {
  try {
    await api('POST', '/api/system/time?timezone=' + encodeURIComponent($('#s-tz').value));
    loadSystem();
  } catch (e) { notify(e.message, true); }
};
$('#btn-ntp').onclick = async () => {
  try {
    await api('POST', '/api/system/time?ntp=' + $('#s-ntp').value);
    loadSystem();
  } catch (e) { notify(e.message, true); }
};
$('#btn-settime').onclick = async () => {
  try {
    await api('POST', '/api/system/time?epoch_ms=' + Date.now());
    loadSystem();
  } catch (e) { notify(e.message, true); }
};

// The three that interrupt an event get a confirmation. Everything else is
// instant on purpose.
function confirmThen(question, path) {
  return async () => {
    if (!window.confirm(question)) return;
    try { await api('POST', path); notify('Asked the box to do that.', false); }
    catch (e) { notify(e.message, true); }
  };
}
$('#storage-check').onclick = async (e) => {
  const b = e.currentTarget;
  const was = b.textContent;
  b.disabled = true;
  b.textContent = 'Checking…';
  try { await loadStorage(true); }
  finally { b.disabled = false; b.textContent = was; }
};

$('#btn-restart').onclick =
  confirmThen('Restart the player? The picture will go away for a few seconds.',
              '/api/system/restart');
$('#btn-reboot').onclick =
  confirmThen('Reboot the box? It will be off air for about a minute.',
              '/api/system/reboot');
$('#btn-shutdown').onclick =
  confirmThen('Shut down? Somebody will have to press the power button to ' +
              'bring it back.', '/api/system/shutdown');

/* ── Log ─────────────────────────────────────────────────────────────────── */

async function refreshLog() {
  try {
    const data = await api('GET', '/api/log?lines=200');
    const el = $('#log');
    const atBottom = el.scrollTop + el.clientHeight >= el.scrollHeight - 40;
    el.innerHTML = data.lines.map((l) => {
      const t = new Date(l.at_ms).toLocaleTimeString('en-GB', { hour12: false });
      return `<span class="${l.level}">${t}  ${escapeHtml(l.text)}</span>`;
    }).join('\n');
    if (atBottom) el.scrollTop = el.scrollHeight;
  } catch (e) { /* the box will be back */ }
}
$('#btn-log-refresh').onclick = refreshLog;

/* ── Preview ─────────────────────────────────────────────────────────────── */
//
// A low-rate copy of the picture going out, refreshed in the browser. Watching
// it does not change what is on the screen in the room — it is the same moment,
// sampled a few times a second. Each frame is fetched off-screen and only
// swapped in once it has arrived, so a slow box or a dropped request never
// blanks a picture that is already showing on the tablet.
//
// Two views of the same instant: what is going out (which is one split, when a
// tile is selected) and the whole feed that arrived. The server keeps a
// fallback frame per view, so switching the selector never blanks the picture.

let previewTimer = null;     // the setTimeout that schedules the next fetch
let previewRate = 0;         // frames per second, from the selector
let previewInFlight = false; // one request at a time, never overlapping
let previewEverHad = false;  // at least one frame has actually arrived

function stopPreview() {
  if (previewTimer) { clearTimeout(previewTimer); previewTimer = null; }
  previewInFlight = false;
  // Keep whatever frame is already on screen rather than blanking it.
}

function startPreview() {
  stopPreview();
  previewRate = Number($('#preview-rate').value || 0);
  if (!previewRate || !$('#preview-box').open) return;
  schedulePreview(0);
}

function schedulePreview(delay) {
  if (!previewRate || !$('#preview-box').open) return;
  previewTimer = setTimeout(fetchPreview, delay);
}

async function fetchPreview() {
  previewTimer = null;
  if (!previewRate || !$('#preview-box').open || previewInFlight) return;
  previewInFlight = true;
  try {
    const view = $('#preview-view').value || 'out';
    const res = await fetch('/preview.jpg?view=' + encodeURIComponent(view) +
                            '&t=' + Date.now());
    if (!res.ok) throw new Error('HTTP ' + res.status);
    const blob = await res.blob();
    const url = URL.createObjectURL(blob);
    const img = $('#preview-img');
    const prev = img.dataset.url;
    img.src = url;                          // swap only after the frame arrived
    $('#preview-none').hidden = true;
    previewEverHad = true;
    if (prev) URL.revokeObjectURL(prev);    // free the one we just replaced
    img.dataset.url = url;
  } catch (e) {
    // A failed fetch must not blank a picture that is already showing.
    if (!previewEverHad) $('#preview-none').hidden = false;
  } finally {
    previewInFlight = false;
    schedulePreview(1000 / previewRate);
  }
}

$('#preview-rate').addEventListener('change', startPreview);
$('#preview-view').addEventListener('change', startPreview);
$('#preview-box').addEventListener('toggle', startPreview);

/* ── Sound meters ──────────────────────────────────────────────────────────
   What the sound card is being given. The numbers come from the box already
   converted to dBFS: doing the logarithm here as well would put the scale in
   two places, and the one on the box is the one the tests cover.

   The panel is drawn from the channel count the endpoint reports, so it is the
   card's width and not the feed's — a stereo card fed a six-track feed shows
   two bars, because two is what is leaving the box. */

let meterShown = 0;   // how many bars the panel currently has

function meterBar(i, db, live) {
  // dBFS to a bar length: 0 dB is full and -60 dB is empty, which is the span
  // that is actually useful on a stage — the bottom twenty decibels of a studio
  // scale are a bar that never quite disappears, which reads as "a little
  // signal" when there is none.
  const span = 60;
  const dbc = Math.max(-span, Math.min(0, db));
  const pct = ((dbc + span) / span) * 100;
  const cls = !live ? '' : (db > -1 ? ' over' : (db > -6 ? ' hot' : ''));
  const text = db <= -120 ? '—' : db.toFixed(1);
  return `<div class="bar${cls}">
            <span class="ch">${i + 1}</span>
            <span class="track"><span class="fill" style="width:${pct.toFixed(1)}%"></span></span>
            <span class="db">${text}</span>
          </div>`;
}

function drawMeters(m) {
  const box = $('#meters');
  const n = Number(m.channels || 0);
  const peaks = m.peak || [];
  const dbs = m.db || [];

  if (!n) {
    box.classList.add('flat');
    // Once, not every poll: replacing the text ten times a second is how a
    // panel ends up fighting the screen reader that is reading it.
    if (meterShown !== 0) {
      box.innerHTML = '<div class="empty">No output channels to meter yet.</div>';
      meterShown = 0;
    }
    return;
  }

  box.classList.toggle('flat', !m.live);
  // Only re-lay-out when the width actually changes. Rebuilding the bars every
  // poll would restart the width transition on every one of them, which turns a
  // meter into a flicker.
  if (meterShown !== n) {
    box.innerHTML = Array.from({ length: n }, (_, i) => meterBar(i, -120, false)).join('');
    meterShown = n;
  }
  const bars = box.querySelectorAll('.bar');
  for (let i = 0; i < n && i < bars.length; i++) {
    const db = Number(dbs[i] ?? -120);
    const live = !!m.live;
    const cls = !live ? '' : (db > -1 ? ' over' : (db > -6 ? ' hot' : ''));
    bars[i].className = 'bar' + cls;
    const fill = bars[i].querySelector('.fill');
    const span = 60;
    const pct = ((Math.max(-span, Math.min(0, db)) + span) / span) * 100;
    fill.style.width = pct.toFixed(1) + '%';
    bars[i].querySelector('.db').textContent = db <= -120 ? '—' : db.toFixed(1);
    // The peak, so the bar can be read without the tabular figure beside it.
    bars[i].title = `${(peaks[i] || 0).toFixed(3)} peak`;
  }
  $('#meters-reason').textContent = m.reason_text || '';
}

async function refreshMeters() {
  try {
    drawMeters(await api('GET', '/api/audio/levels'));
  } catch (e) {
    // An older box without the endpoint: say so once rather than every poll.
    if (meterShown !== -1) {
      $('#meters').innerHTML = '<div class="empty">This box does not report sound levels.</div>';
      $('#meters').classList.add('flat');
      $('#meters-reason').textContent = '';
      meterShown = -1;
    }
  }
}

$('#meters-box').addEventListener('toggle', () => {
  if ($('#meters-box').open && $('#tab-play').classList.contains('is-on'))
    refreshMeters();
});

/* ── Tabs and the poll loop ──────────────────────────────────────────────── */

$$('.tab').forEach((tab) => {
  tab.onclick = () => {
    $$('.tab').forEach((t) => t.classList.toggle('is-on', t === tab));
    $$('.panel').forEach((p) =>
      p.classList.toggle('is-on', p.id === 'tab-' + tab.dataset.tab));
    if (tab.dataset.tab === 'events') refreshEvents();
    if (tab.dataset.tab === 'settings') loadSettings();
    if (tab.dataset.tab === 'system') loadSystem();
    if (tab.dataset.tab === 'log') refreshLog();
    // The preview only runs on the tab that shows it.
    if (tab.dataset.tab === 'play') startPreview(); else stopPreview();
  };
});

function startPolling() {
  if (pollTimer) clearInterval(pollTimer);
  refreshStatus();
  pollTimer = setInterval(() => {
    refreshStatus();
    // Only on the tab that shows them, and only while the panel is open. The
    // levels endpoint reads and clears the meter, so not asking is the same as
    // not measuring — nothing is lost by skipping it.
    if ($('#tab-play').classList.contains('is-on') && $('#meters-box').open)
      refreshMeters();
    if ($('#tab-log').classList.contains('is-on') && $('#log-follow').checked)
      refreshLog();
  }, 500);
}

// A tablet left on a music stand should stop asking while its screen is off,
// and pick straight back up when somebody looks at it.
document.addEventListener('visibilitychange', () => {
  if (document.hidden) {
    if (pollTimer) { clearInterval(pollTimer); pollTimer = null; }
    stopPreview();
  } else {
    startPolling();
    if ($('#tab-play').classList.contains('is-on')) startPreview();
  }
});

startPolling();
loadSettings().catch(() => {});
refreshEvents();
