// SPDX-License-Identifier: GPL-3.0-or-later
//
// media.js — the page's media-time arithmetic, with no DOM in it.
//
// Split out of app.js so it can be TESTED. On 2026-09-20 the timeline and
// scrubbing stopped working entirely on the Pi player, and every fault was in
// four lines of arithmetic that nothing exercised: app.js had no tests at all,
// because it needs a browser. These functions do not, so tests/test_media_time.js
// runs them directly.
//
// THE TRAP, which this project has now hit six times: positions start at ZERO.
// Media time is how far into the programme a moment sits, so the beginning of a
// recording is 0, the first segment is 0, and a cue dropped on the opening word
// is 0. `if (!x)` is therefore not a test for "missing" — it is a test for
// "missing OR at the very start", and the second one is the ordinary case. Use
// Number.isFinite, or an explicit >= 0, and let -1 or null mean absent.

(function (root) {
  'use strict';

  // Where a cue sits in the programme, from its own anchor.
  //
  // Mirrors marker_media_ms() in core/model.h — keep them in step. Prefers the
  // cue's own media time; falls back to converting a time of day through the
  // event's start, which is what cues written before the field carry.
  //
  // Returns null when it cannot be placed, NOT 0, precisely because 0 is a real
  // answer meaning "at the very beginning".
  function markerMediaMs(m, started_ms) {
    if (!m) return null;
    if (typeof m.at_media_ms === 'number' && m.at_media_ms >= 0) return m.at_media_ms;
    if (m.at_ms > 0 && started_ms > 0 && m.at_ms >= started_ms) return m.at_ms - started_ms;
    return null;
  }

  // The span the timeline draws, or null when there is nothing to draw.
  //
  // A recording runs to its end; a live room runs to the live edge. `from` is
  // the earliest media time still in storage, which for a recording whose first
  // segment has not expired is 0 — the case `!from` threw away.
  function timelineRange(s) {
    if (!s) return null;
    const from = (typeof s.earliest_ms === 'number') ? s.earliest_ms : NaN;
    const to = s.plays_as_recording
      ? (typeof s.end_ms === 'number' && s.end_ms > 0 ? s.end_ms : s.live_ms)
      : s.live_ms;
    if (!Number.isFinite(from) || !Number.isFinite(to) || to <= from) return null;
    return { from: from, to: to };
  }

  // Where a media time falls across the drawn span, clamped to it.
  function pctOf(ms, from, to) {
    const span = to - from;
    if (!(span > 0)) return 0;
    return Math.max(0, Math.min(100, ((ms - from) / span) * 100));
  }

  // The media time a click at `frac` across the bar is asking for.
  function seekTargetMs(frac, from, to) {
    const f = Math.max(0, Math.min(1, frac));
    return Math.round(from + f * (to - from));
  }

  const api = { markerMediaMs, timelineRange, pctOf, seekTargetMs };
  if (typeof module === 'object' && module.exports) module.exports = api;
  else Object.assign(root, api);
})(typeof globalThis !== 'undefined' ? globalThis : this);
