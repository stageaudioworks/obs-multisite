# Duplicated derivations sweep (2026-09-18) — full record

Archive of the D1–D4 sweep. D1 and D4 are **DONE**; D2 and D3 remain open and
are carried as short entries in `BUGS.md`.

> As it stood on 2026-09-21, verbatim.

---

## Duplicated derivations (sweep, 2026-09-18)

Not bugs yet — this is the shape that produced twelve faults in one stretch, and
these are the places it is still present. Each is *one question answered in more
than one place*, so each can drift. The medicine is always the same: expose the
authority's answer and delete the second copy.

### D1. "Is this played as a recording?" — five answers

Authority: `DecoderSession::plays_as_recording_locked()` (`is_vod` OR pinned).

Second copies:

- the OBS dock's own condition for the span and the clock:
  `s.ended || s.interrupted || !s.pinned_event_id.empty()` — written by hand in
  two places in `decoder_dock.cpp` (the timeline span and the position text);
- the OBS source's status strings (the `BROADCAST ENDED — playing out the
  recording` family in `multisite_source.cpp`);
- the appliance's status strings (`player.cpp`, same family);
- the appliance page's mapping (`EVENT = {LIVE, RECORDING, INTERRUPTED}` in
  `web/app.js`).

The dock's copy was the dangerous one: it decides what the timeline spans, and it
was WRONG for an interrupted pinned event until this session — which is the whole
argument.

**DONE (2026-09-18):** `DecoderSession::plays_as_recording()` is exposed through
the snapshot and the dock's three hand-written copies are gone (the timeline
span, the position text, and the cue-list times). The third was found only by
grepping for the pattern after the first two were replaced — which is the
argument for the sweep happening at all.

**CORRECTION (2026-09-19) — what remains on the page is not what this entry
said, and it is not cosmetic.** The line above named the page's
`EVENT = {LIVE, RECORDING, INTERRUPTED}` mapping. That enum is used in exactly
one place (`app.js:456-458`) and it badges entries in the EVENT LIST from the
catalog's per-event state. That is a different question from "is what I am
playing right now a recording", and replacing it would break the list. Leave it.

The page's actual second copies were SIX expressions, not the mapping this entry
named — and finding six when three were expected is the same lesson the dock
taught, one layer out:

- the timeline span, `s.ended ? (s.end_ms || s.live_ms) : s.live_ms`;
- the timeline's right-hand label, `s.ended ? hhmmss(to) : ... + ' (now)'`;
- the cue times, `const vod = !!s.ended || !!s.interrupted;`;
- the position readout, `} else if (s.ended) {` — "elapsed of total" vs behind-live;
- the transport, `const live = !s.ended;` — which offers "Catch up to now";
- the readout row, `if (!s.ended) push('Behind the main site', ...)`.

The first three came from reading the entry. The last three were found only by
grepping `s.ended` after the first three were replaced — exactly how the dock's
third copy surfaced. **Grep for the pattern after you think you are done; the
count you started with is not the count.**

Every one omitted the `pinned` term the authority carries (`is_vod` OR pinned),
so for a pinned event the page spanned its timeline to a live edge that was not
moving, printed cue times as times of day, offered "Catch up to now" for an
event the operator had deliberately stepped away from, and reported how far
behind live a recording was. The SAME defect the dock had.

**DONE (2026-09-19).** `Status::plays_as_recording` is filled from
`DecoderSession::plays_as_recording()` in `player.cpp`, served as
`plays_as_recording` by `/api/status`, and all six page sites read it. The
`EVENT` enum is untouched and now carries a comment saying what it is for, so
the next sweep does not "finish the job" and break the event-list badges.

`tests/test_decoder.cpp` (test 6) now covers the authority, which had no test at
all — it was extracted and three call sites rewired without one. The assertion
that matters pins the event that is CURRENTLY LIVE: the room is neither ended
nor interrupted, so every hand-written copy evaluated to false, while
`plays_as_recording()` is true. A test using an ended event — as the existing
one did — passes for the bug and the fix alike.

**The status strings were mis-named too (2026-09-19).** This entry listed them
as further copies of "is this played as a recording". They are not: they switch
on `RoomState` and say what the ROOM is doing, which is a different question and
correctly ignores pinning — a pinned playback should log a live room as live.

What they actually were is byte-identical copies OF EACH OTHER, the same
conditional written out in `multisite_source.cpp` and `player.cpp`, so the
wording had to be changed in two files or the plugin and the box would describe
the same room differently. Now `room_state_words()` in `decoder_session.h`,
beside `is_vod()` — whose comment already said these states "differ in how they
should be DESCRIBED", which is exactly the function that was missing.

D1 is complete. Worth keeping from it: the entry named the wrong duplicate
THREE times — the `EVENT` enum, the count of page sites, and these strings. A
sweep entry is a lead, not an inventory; grep the pattern before trusting the
list, and again after replacing what you found.

### D2. Segment length — two fallbacks

`DecoderSession` owns `m_segment_duration_s` (manifest hint, else the configured
value, else 6.0). The dock re-derives its own from the snapshot and applies its
own fallback: `s.segment_duration_s > 0.1 ? s.segment_duration_s : 6.0`
(`decoder_dock.cpp:1507`). Two fallbacks for one number.

Harmless while both say 6.0, which is why it has not bitten — and this is exactly
how the media-vs-wall timeline faults started. Fix: the snapshot's
`segment_duration_s` should already BE the session's answer with its fallback
applied, and the dock should not re-apply one.

### D3. Values handed out that nothing consumes

`s.start_buffer_s` is still in the snapshot after readiness moved to the session
(`gate_s`/`ready_buffer_s`), and the dock no longer uses it. Dead data of this
kind is an invitation: it is what the *next* person reaches for when they want to
show progress, which is how the "84 s of 60 s" readout happened in the first
place. Either remove it or document it as legacy.

### D4. The queue caps were counts, not durations — and one of them was under the gate

**Status: FIXED, and it was not cosmetic.** Filed as tidiness ("worth deciding
whether these should be expressed in time") and under-rated: `kMaxQueuedVideo`
at 12 frames holds **367 ms** at 29.97 fps, while `kMaxDeliveryLeadNs` — the
lead the delivery loop is trying to hold — is **400 ms**. A cap below the gate
is a permanent jam: video can never build its working set, so the producer sits
against the cap, blocks the full 250 ms and drops the frame.

The operator reported it as playback stalls on 2026-09-19 and the log showed it
plainly, every window:

```
queue at resume held video 12 frame(s)/367 ms, audio 48 frame(s)/1003 ms
lead video mean=+398ms min=+100ms | audio mean=+398ms min=+375ms | dropped 25 v / 22 a
```

Audio, with 1003 ms against the same 400 ms gate, held `min +375ms`. Video
collapsed to `min +100ms`. That asymmetry is the whole diagnosis.

Two faults in the original sizing, which was `lead / frame_interval`:

1. **N frames span N-1 intervals.** Twelve frames hold eleven intervals =
   367 ms. The resume log had been printing that number for weeks; nobody read
   it against the 400 ms it was meant to equal.
2. **Even thirteen would sit exactly ON the gate.** A cap equal to the lead is
   full by construction. A cap is a bound, not a target, and needs headroom for
   the hesitation it exists to absorb.

Now `kMaxQueuedNs`, one duration for both streams, with a `static_assert` tying
it to `kMaxDeliveryLeadNs` so the two cannot drift apart again, and count
backstops for nonsense pts. Expressing it in time also fixes the frame-rate
dependence nobody had noticed: at 60 fps the old twelve frames held 183 ms,
under half the gate.

**Cost, stated rather than discovered later:** ~93 MB of queued video at 1080p30
where the old cap was ~37 MB. That is the price of a 400 ms lead, not of this
bound — any queue that feeds a 400 ms gate without jamming must hold
appreciably more than 400 ms. At 4K it is ~370 MB, which is what the count
backstops are really for.

**Not fully explained: 22 of the 47 drops were audio.** Audio had 1003 ms
against a 400 ms gate and should never have jammed on capacity, so the delivery
loop itself must have stopped draining for ≥250 ms at a stretch. The cap fix
does not address that and may not cure it. Rather than stack a second guess on
the first, the drop path now logs how long since the loop last handed a frame
to OBS, whether it was paused, and how much programme the stream was holding —
a gap near 250 ms means delivery did nothing at all and the fault is in the
loop or the handover, not the bound.

---

## Closure note (2026-09-21)

**D2 — DONE, as a duplication removal.** `DecoderSession::segment_duration_s()`
is now the single authority and returns the value with its fallback (`> 0.1 ?
: 6.0`) already applied. Removed the second copies at `decoder_dock.cpp`
(which re-floored the snapshot value) and at `decoder_session.cpp`'s
`pump_downloads` window and `start_reserve_segments()`; `segment_ms()` and
`buffered_ahead_s()` now read the accessor too.

Honest scope, per standards §9: **nothing observable changed.** Both
assignments into the raw `m_segment_duration_s` are guarded `> 0.1` and its
initial value is 6.0, so the consumer floors were dead code — they could never
have produced a different answer. There is therefore no old-behaviour-failing
test to write (standards §5 applies to fixes), and none was invented. What this
buys is the rule itself: one question, one answer place, so the *next* consumer
cannot add a third floor and drift, which is exactly how the media-vs-wall
faults began.

**D3 — DONE.** `start_buffer_s` was assigned into the dock's snapshot struct
(`multisite_ui.h`) from `multisource_source.cpp` and read by nobody. Removed
both. `buffered_span_s`, also write-only, was left in place — it carries a
comment describing the buffer-fill readout it exists for, so it is a
deliberately-kept seam rather than dead data of the kind D3 names.

Verified: core suite 55/55 under ctest, and the OBS plugin (`build-obs`,
`obs-multisite` target) links clean.
