// SPDX-License-Identifier: GPL-3.0-or-later
//
// test_media_time.js — the Pi player page's media-time arithmetic.
//
// Every case here is the 2026-09-20 regression, reported as "the timeline and
// scrubbing in the pi player is not working at all". Positions had moved from
// times of day to media time the day before; media time starts at 0, and four
// separate guards tested it with `!x`, which reads 0 as absent. The start of a
// recording is the ordinary case, so the timeline blanked itself and every
// scrub returned early.
//
// app.js needs a browser and so had no tests at all, which is exactly why this
// shipped. The arithmetic does not need one, so it lives in media.js and is
// tested here.
const t = require('../src/appliance/web/media.js');

let fail = 0;
function check(cond, msg) {
  if (cond) { console.log('  [ok]   ' + msg); }
  else { console.log('  [FAIL] ' + msg); fail++; }
}
const eq = (a, b, msg) => check(Object.is(a, b), msg + ' (got ' + JSON.stringify(a) + ')');

console.log('== a recording that starts at 0 still has a timeline ==');
{
  // THE BUG. earliest_ms === 0 is a recording whose first segment is still in
  // storage: the most ordinary state there is.
  const r = t.timelineRange({ earliest_ms: 0, end_ms: 3600000, plays_as_recording: true });
  check(r !== null, 'a range starting at 0 is drawn, not thrown away');
  eq(r.from, 0, 'from is 0');
  eq(r.to, 3600000, 'to is the end of the recording');
}

console.log('== and the rest of the range cases ==');
{
  eq(t.timelineRange(null), null, 'no status, no range');
  eq(t.timelineRange({}), null, 'no earliest_ms, no range');
  eq(t.timelineRange({ earliest_ms: 0, live_ms: 0, plays_as_recording: false }), null,
     'an empty span is not a range');
  eq(t.timelineRange({ earliest_ms: 500, end_ms: 100, plays_as_recording: true }), null,
     'a backwards span is not a range');
  const live = t.timelineRange({ earliest_ms: 0, live_ms: 60000, plays_as_recording: false });
  eq(live.to, 60000, 'a live room runs to the live edge');
  // A recording with no end recorded yet falls back to the live edge rather
  // than collapsing to nothing.
  const pinned = t.timelineRange(
    { earliest_ms: 0, end_ms: 0, live_ms: 90000, plays_as_recording: true });
  eq(pinned.to, 90000, 'a recording with no end yet uses the live edge');
}

console.log('== a scrub to the very start asks for 0, and is a real request ==');
{
  eq(t.seekTargetMs(0, 0, 3600000), 0, 'the left-hand end is 0');
  eq(t.seekTargetMs(1, 0, 3600000), 3600000, 'the right-hand end is the end');
  eq(t.seekTargetMs(0.5, 0, 3600000), 1800000, 'the middle is the middle');
  eq(t.seekTargetMs(-1, 0, 3600000), 0, 'dragging off the left clamps');
  eq(t.seekTargetMs(2, 0, 3600000), 3600000, 'dragging off the right clamps');
  // An offset span: a recording whose opening segments have expired.
  eq(t.seekTargetMs(0, 600000, 3600000), 600000, 'an offset span starts where it starts');
}

console.log('== a cue at the very beginning is at 0, not missing ==');
{
  eq(t.markerMediaMs({ at_media_ms: 0 }, 1789817891035), 0,
     'a cue on the opening word is 0');
  eq(t.markerMediaMs({ at_media_ms: 125000 }, 1789817891035), 125000,
     'a cue carrying its own media time uses it');
  // The legacy path: a cue older than the field, converted through the start.
  eq(t.markerMediaMs({ at_media_ms: -1, at_ms: 1789817891035 + 60000 }, 1789817891035),
     60000, 'an older cue converts through the event start');
  eq(t.markerMediaMs({ at_media_ms: -1, at_ms: 0 }, 1789817891035), null,
     'a cue with neither anchor cannot be placed');
  eq(t.markerMediaMs({ at_media_ms: -1, at_ms: 5 }, 0), null,
     'no event start, nothing to convert with');
  eq(t.markerMediaMs({ at_media_ms: -1, at_ms: 1 }, 1789817891035), null,
     'a cue before the event start is not placed at a negative position');
  eq(t.markerMediaMs(null, 0), null, 'no cue, no position');
}

console.log('== placing things on the bar ==');
{
  eq(t.pctOf(0, 0, 1000), 0, 'the start of the span is 0%');
  eq(t.pctOf(500, 0, 1000), 50, 'the middle is 50%');
  eq(t.pctOf(1000, 0, 1000), 100, 'the end is 100%');
  eq(t.pctOf(-50, 0, 1000), 0, 'before the span clamps to 0%');
  eq(t.pctOf(5000, 0, 1000), 100, 'after the span clamps to 100%');
  eq(t.pctOf(0, 0, 0), 0, 'a zero span does not divide by zero');
}

console.log(fail ? 'FAILED' : 'all passed');
process.exit(fail ? 1 : 0);
