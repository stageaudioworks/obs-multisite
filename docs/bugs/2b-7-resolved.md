# BUGS #2b–#7 — resolved entries: full record

Archive of entries that were **FIXED** and are no longer open action items. They
lived in the `## Open` section until 2026-09-21, which made the project look less
healthy than it was. Each is kept verbatim; `BUGS.md` carries a one-line index
entry for it under "Recently landed".

> Entries as they stood on 2026-09-21, verbatim.

---

### 2b. The media->wall mapping is position-dependent, by ~1.1% — and the cue system rests on it

**Status: FIXED 2026-09-19, pending a run on real content. This is the one that
matters for cues.** Entry 2's fixes make a
hold behave; this decides whether a cue lands where it was placed.

Two pins from one recording, both taken after genuine decoder restarts, on the
build where the pin is otherwise correct:

```
pts   0.000 s -> segment wall 1789455009498
pts 714.008 s -> segment wall 1789455715564
```

Wall advanced 706.066 s while media advanced 714.008 s. **Media runs 7.94 s fast
over 714 s: 1.11%.** A cue stored against the wall clock therefore resolves to a
media position that is wrong in proportion to its distance from wherever the
clock was last pinned — about a minute out by the end of a 90-minute service.
No amount of pause/resume correctness helps with this.

**Prime suspect, now instrumented.** `DecoderSession::next_segment()` fills
`starts_at_ms` from the manifest's `at_ms` when it can, and otherwise ESTIMATES
it as `event_start + seq * nominal_duration` — which assumes every segment is
exactly the nominal length. The manifest holds a rolling window (50 entries), so
any older segment takes the estimate. That estimate was returned
indistinguishably from a measured value, and it is what the media clock is
pinned to.

Note the ratio: 6067/6000 = 1.0111, and 182 frames at 30 fps is 6.0667 s. A GOP
rounded to a frame count would make every segment 6.067 s of media while the
estimate advances 6.000 s — which is the observed drift almost exactly. Not
confirmed, but it is the first thing to check.

**Instrumented 2026-09-19, measurement only:** `PlayableSegment` now carries
`starts_at_estimated`, and the pin line says which it got:

```
media clock pinned — fragment wall N (measured, from the manifest), ...
media clock pinned — fragment wall N (ESTIMATED from seq x nominal duration), ...
```

**SETTLED FROM THE CODE, no test run needed.** `session.cpp:355`:

```c
// Content time, not upload time: event start plus this segment's offset in
// the programme. Upload time would drift with network delays.
ms.at_ms = m_manifest.started_at_ms + (int64_t)(seg.pts_offset_s * 1000.0);
```

The ENCODER defines a segment's wall time as `event_start + media_pts`. So for
any measured segment `origin = at_ms - pts` is just `event_start`, constant, by
construction. The decoder's fallback computes the same quantity by a DIFFERENT
formula — `event_start + seq * nominal_duration` — and the two agree only while
every segment is exactly the nominal length. At ~6.067 s (182 frames at 30 fps
against a nominal 6.000 s) the estimate slips 67 ms per segment: 1.11%, matching
the measurement to five decimal places across two independent intervals.

And it is not an edge case. A finished recording's manifest keeps only the last
50 segments, so 558 of this event's 608 segments have no at_ms at all. Nearly
every cue in the event resolved through the estimate.

**Cues are wall-clock anchored and the encoder stamps them with `now_ms()`
(`session.cpp:445`)** — a true time of day, with no compensating error. So the
cue's anchor was right and the mapping that resolved it was wrong: the error did
not cancel, it just accumulated with distance from the pin.

**The fix: use the encoder's own anchor.** `PlayableSegment` now carries
`event_started_at_ms`, and `PlayoutTimeline::set_event_start_ms()` takes it as
the origin directly. The fragment pin remains only as a fallback for a session
that cannot report an event start. This also retires the whole class of "pinned
to a position already left" bugs, because there is no longer a pairing to get
wrong — no fragment wall, no first-pts-seen, just the event's start and the
frame's own pts.

The pin log line now prints BOTH: the origin taken from the event start, and
what the fragment pin would have said, with the difference in ms. They should
agree near the start of an event and diverge by ~11 ms per second of programme
further in; that difference is the bug, now visible instead of silent.

**Related, and NOT fixed — the two time bases disagree under an encoder stall.**
Segments use content time (`started + pts_offset`) while markers use true wall
time (`now_ms()`). While the encoder runs continuously these are the same. If it
stalls, media time pauses while the wall clock does not, and a marker placed
after the stall carries a time of day the content-time mapping cannot produce.
Worth deciding deliberately which base a cue is in — the operator means "this
moment in the programme", which argues for content time.

### 2c. Seeking was slow and landed late — five faults, all measured

**Status: FIXED 2026-09-19 and verified on real content.**

| | before | after |
|---|---|---|
| seek to picture | 1816-5355 ms | 77-263 ms |
| landing error | 1.5-6.7 s late | 21-58 ms |
| decoder blocked on the queue | 3-4 s per seek | 0 ms |
| "SEEKING..." | stuck to the 30 s timeout | clears on arrival |
| "playout clock fell Ns behind" | every few seeks | gone |

Found by measuring in stages rather than guessing, and that mattered: the
first two fixes each removed a real delay and changed nothing an operator
felt, because the wait MOVED rather than went.

1. **The feed loop slept through jumps.** It waits until playout catches up
   before feeding the next fragment, and tested `running` alone — so a seek
   landing mid-wait was invisible for up to the 2.5 s feed lead. Decoder
   restart went from 2060/3360/4120 ms to 23/30/30.

2. **The delivery loop waited for frames it was about to discard.** It held
   each frame until nearly due and THEN asked whether to keep it, so a seek
   1.8 s into a fragment cost 1.8 s of real time. The wait now sits below the
   decision.

3. **Discarded frames still went through the queue.** Each was deep-copied
   (~3 MB) and pushed through a 12-frame queue to be dropped at the far end,
   so the decoder spent 92-97% of every skip ASLEEP waiting for space — 3808
   ms of a 4021 ms skip, while the decode itself was 213 ms for 292 frames
   (~1370 fps). The skip now runs in deliver_video/deliver_audio, before the
   copy and before the queue: 432 frames in 193 ms. It also makes the rule
   playout_timeline.h insists on — a frame dropped by the skip must not pin
   the clock — true by construction, because such a frame never reaches the
   delivery loop.

4. **seek_to_media_ms picked the segment with the NOMINAL duration** while
   everything else had moved to the measured one, so a click landed
   `seq * (true - nominal)` late: proportional to depth, invisible near the
   start. Predicted and observed agree — seq 44 predicted 1474 ms late and
   measured 1539; seq 200 predicted 6700 and measured 6710. This is also what
   stuck the interface: the arrival check allows 2.5 s, and past ~75 segments
   in the overshoot exceeded it, so "SEEKING..." never cleared however long
   the picture had been playing. Two sibling sites fixed with it
   (seek_to_wall_ms's out-of-window fallback, and a cue's time of day to a
   segment in add_marker), making FIVE places this one formula was written.

5. **The pacing credited a fragment its full duration** even when the seek
   discarded most of it, so the feed loop thought it was ahead by the whole
   fragment when it was ahead by the remainder. It waited that much too long
   for the next one, playback ran dry, and the 2 s stall resync fired — a
   mechanism for network stalls, triggered by arithmetic. Only visible once
   the skip got fast; while discarding 400 frames took four seconds, the loop
   had spent that time waiting anyway.

**Still open, deliberately.** A seek into a COLD cache waits for the segment
to download — measured at 9.65 s once, correctly reported by the feed-wait
line as "cache holds 9 segment(s)". `pump_downloads` does start from the new
head, so the target is prioritised; the wait is the in-flight batch finishing
first, because a pump computes its list under the lock and then downloads
without it. Cancelling in-flight downloads on a seek would fix it and is NOT
obviously safe: sticky cancellation is what broke End Broadcast for six days
(entry above). Low value — it only bites on a cold cache — and worth doing
only with that history in mind.

### 3. Clicking the timeline lands on the segment, not the moment

**Status: FIXED.** The dock divided the clicked time by the segment length and
threw the remainder away, so every click landed up to 6 s from where it was
made. `DecoderSession::seek_to_media_ms()` now owns the conversion — segments are
the unit of transfer, not of seeking — and sets `m_pending_skip_ms` under the
same lock that seats the head, so the feed loop cannot serve the segment before
its offset is set. The dock hands over a time and nothing else.

`tests/test_decoder.cpp` asserts `skip_to_ms == 3000` for a click 3 s into the
second segment. A segment-only assertion passes for both the bug and the fix,
which is how this survived as long as it did.

### 2d. The seek's skip used one base for two streams, so the picture ran ~311 ms behind the sound

**Status: FIXED.** Reported as "audio sync seems to drift", found on a finished
recording, and caused by 44d33b0 the same afternoon — the commit that moved the
sub-segment skip out of the delivery loop and into `deliver_video` /
`deliver_audio`, so a frame the seek was going to throw away is dropped before
the ~3 MB copy and before the queue. That move was right and is worth keeping.
What it quietly broke was the ORDER the skip relied on.

In the delivery loop, frames arrive in **presentation order** — the loop takes
the minimum timestamp in the window. The first frame to reach the target is
therefore genuinely the earliest moment at or after it, and every frame behind
it has already gone past. One shared base and one disarm were correct there for
free, by ordering, without anyone having to think about it. The comment left on
the field said exactly that, and said it was still true:

> base is INT64_MIN until the first frame of the fragment claims it, and audio
> and video share it exactly as they did before, because whichever arrives
> first defines where the fragment starts.

At the producer, frames arrive in **decode and track order**. Audio and video
do not take turns. The operator's log measured the gap on every re-anchor:

```
interleave gap this anchor: audio leads by 311 ms (cushion 500 ms, within the cushion)
lead video mean=+383ms min=+85ms (106) | audio mean=+398ms min=+394ms (149)
```

Audio always speaks first, so audio always claimed the shared base — about
311 ms earlier than video's first pts. Every video frame was then measured from
an origin 311 ms too early, crossed the target that much sooner, and kept
311 ms of picture the audio had already discarded. The picture ran that far
behind the sound. It reads as *drift* rather than a fixed offset because the
gap is re-measured at every re-anchor and came out different each time — 0,
235, 300, 311, 312, 313, 319 ms across one session. The first anchor in that
log skipped nothing and had a 0 ms gap: sync was fine until a seek skipped.

The fix is one arm per stream — its own base, its own disarm — in
`src/core/seek_skip.h`.

**The reason it shipped is the more useful part.** The skip had a test while it
lived in `PlayoutTimeline`; moving it to the producer moved it out of
`test_playout_timeline`'s reach, and nothing replaced it. The behaviour went
from tested to untested in a commit that was about performance and said nothing
about coverage. `tests/test_seek_skip.cpp` now pins it, and was checked against
a shim of the old shared-base logic first: it fails five ways there and passes
here. The seek log also counts the two streams separately now — one total hid
this completely, because the total looked plausible while video was skipping
311 ms less than audio.

### 4. The Pi player's timeline and scrubbing died on a zero — the sentinel trap, sixth time

**Status: FIXED.** Reported the morning after the elapsed-time change as "the
timeline and scrubbing in the pi player is not working at all". Not a decoder
fault and not a regression in the positions themselves: four guards in
`web/app.js` tested a media time with `!x`.

Media time is how far into the programme a moment sits. The start of a
recording is therefore **0** — and for a recording whose first segment is still
in storage, `earliest_ms == 0` is not an edge case, it is the ordinary state.
Before the change the same field was a wall-clock epoch, never anywhere near
zero, so `!from` had been safe for as long as it had existed.

Four sites, all the same mistake:

1. `drawTimeline`: `if (!from || !to || to <= from) return;` — blanked the
   whole timeline and returned.
2. The click handler: `if (!from || !to) return;` — every scrub returned early.
   `Number('')` is also 0, so emptiness has to be tested before the number is.
3. The hover handler: the same.
4. `passed` in the cue list: `s.playhead_ms &&` failed at the start of an event,
   which is exactly when it matters.

A fifth was a different error in the same family: the timeline placed cue ticks
with `m.at_ms`, a time of day, on a scale that now starts at 0 — so every cue
sat far off the right-hand end. The cue list had been converted and the timeline
had not, one quantity derived in two places again (D1's lesson).

The comment immediately above fault 1 read *"when 0 is exactly where a
recording with its first segment still in storage begins. Explicit, and
numeric"* — the guard for this trap was written, and then undone three lines
later by the test that follows it.

**Why it shipped: `app.js` had no tests, because it needs a browser.** The
arithmetic does not. `web/media.js` now holds `markerMediaMs`, `timelineRange`,
`pctOf` and `seekTargetMs` with no DOM in them, `tests/test_media_time.js`
exercises them under `ctest`, and `app.js` calls them rather than keeping its
own copies. Verified for real as well as by test: the page was served against a
stub status and driven in a browser — the range comes out 0→3,600,000, the
playhead sits at 33.3% for 20:00 of 1:00:00, all three cue kinds land correctly
(including a legacy `at_ms` one at 66.7%), and a click at the far left now
issues `/api/seek?ms=0` where it previously issued nothing at all.

### 5. "29831921 min 43 sec behind" — a position subtracted from an epoch

**Status: FIXED.** Reported from the decoder dock on a live room. The number is
not noise: 29,831,921 min 43 s is 1,789,915,303 seconds, which is the current
Unix time. That is the tell for this whole family of fault — when a readout
comes out as *roughly now in some unit*, a wall clock has been subtracted from
something that is not one.

`SourceCtx::live_edge_wall_ms` held the live edge as a time of day.
`Status::playhead_ms` became a MEDIA time in the wall-clock removal (#2b/#2c).
The smoothing override then did:

```cpp
const double behind = (double)(edge_now - out.playhead_ms) / 1000.0;
```

which with a playhead 18 s into the programme is `1789915303000 - 18000`.
The comment above it said "express behind live as the gap between two real
times" — true when it was written, and left describing the old world.

The live edge is now held in media time (`live_edge_media_ms`, from
`media_ms_for_seq`), so both sides of the subtraction are the same kind of
quantity. `interpolate_position` needed no change: it advances a POSITION by
elapsed real time, which is exactly what a live edge does in media time.

Two smaller faults fixed in passing, both the 0-sentinel trap again: the edge
was only stamped when `at > 0`, so it was never stamped during the first
segment of an event (media time 0); and the override required
`out.playhead_ms > 0`, which is the start of an event.

**The fallback was right all along.** `DecoderSession::behind_live_s()` counts
segments — `(live - head) * segment_ms` — and is commensurate by construction.
Only the override that smooths it between polls was wrong, and the appliance,
which uses the plain version with no override, was never affected.

**Why nothing caught it.** There is no unit here to test: the expression lives
inside a snapshot function that needs OBS, and the two operands are both
`long long`. A media time and a time of day are the same type, so nothing but
reading it can tell them apart — which is the third time this week that a
quantity in the wrong frame has type-checked perfectly and shipped. Worth
considering whether these should be distinct types rather than a comment.

### 6. OBS froze mid-broadcast whenever uploads stalled — a network PUT under the status lock

**Status: FIXED.** Reported as occasional UI stalls, narrowed by the operator to
"live, when uploads are stalling", which is what made it findable: a fault that
only appears when the network is slow is almost always something blocking while
holding a lock.

`publish_manifest_locked()` did exactly what its name said:

```cpp
std::string Session::publish_manifest_locked() {
    std::string json = m_manifest.to_json();
    put_json(event_prefix() + "manifest.json", json);   // synchronous HTTPS PUT
    return json;
}
```

All three callers hold `m_mtx`, and one of them is `on_confirmed` — **once per
segment, for the whole broadcast**. `Session::status()` takes the same mutex,
and the encoder dock calls it from a **1 Hz QTimer on the OBS UI thread**.

Healthy link: the PUT is 50–100 ms and nobody notices. Stalled link: the UI
thread waits out the request. Measured with a stub transport that sleeps 1.2 s
on a manifest write, `status()` took **1202 ms**; with the fix, **0 ms**.

**The near-miss is the interesting part.** The comment directly above was
scrupulous about not firing *callbacks* under the lock — *"callbacks are never
fired locked in this file, on principle: SpoolQueue's own drop callback earned
that rule the hard way"* — and the function returns its JSON specifically so the
LAN hook can run after release. The lock was thought about carefully. The
network call inside it was not seen.

And the fix already existed in the same file, applied to the wrong copy.
`put_bytes()` defers the SECOND bucket's writes for precisely this reason:

> *"this runs on the encode thread, and a second put that waits on a request
> timeout would stall the live feed on the insurance policy. Latest wins per
> key — a manifest is rewritten every segment and the mirror only needs the
> current one."*

Every word of that applies to the primary. The pattern was invented to protect
the encode thread from the mirror, and the primary went on blocking.

A manifest is latest-wins, so one publisher thread now owns the writing, takes
the newest JSON and publishes it with nothing held; a manifest superseded while
a PUT is in flight is dropped rather than queued. One writer also makes
out-of-order publishes impossible, which a plain "unlock, then PUT" would have
allowed. `begin` and `end` flush and wait, bounded, because those two writes are
what "this event exists" and "this event is over" mean to a satellite.

**Pinned by `test_session` case 21**, verified against the old behaviour first:
it reports 1202 ms and fails there.

**Found by reading, not by logging — which is itself the finding.** The core had
no logging facility at all: 600 lines of `session.cpp` doing every upload in the
project without one log call, because it may not depend on OBS or the appliance
and nobody had given it a seam. Failure detail was computed and discarded
(`r.http_status`, `r.error`) at every site. `src/core/log.h` now provides a sink
the host installs, and the upload paths say what happened.

### 7. Encoder crash on Windows: QPointer used as a cross-thread receiver

**Status: FIXED — strongly indicated, not proven.** An access violation
(c0000005) in `obs-multisite.dll` on a Windows encoder running v0.1.23-alpha.
The addresses could not be symbolised, because **no PDB ships with the release
or the CI artifact**, so this is a shape match rather than a read stack. Say so
plainly rather than claiming a diagnosis the evidence does not support.

What the dump shows: the crashed thread is one of ours, and shares its bottom
three frames with another thread that is inside `libcurl` — so two workers of
the same type, one mid-request, one faulting. Meanwhile the UI thread is inside
Qt widget code with our frames in it, i.e. one of our dialogs was live or being
torn down.

The fault that matches: eight sites passed a `QPointer` as the RECEIVER of
`QMetaObject::invokeMethod` from a worker thread.

```cpp
QPointer<StorageDialog> guard(this);
std::thread([guard, ...]() {
    if (guard)
        QMetaObject::invokeMethod(guard.data(), [...]{...}, Qt::QueuedConnection);
```

`QPointer` is not thread-safe. The worker reads it while the UI thread may be
clearing it in `~QObject`, and even an atomic read would not save it: `if
(guard)` then `guard.data()` is check-then-use, and the widget can die in the
gap. `invokeMethod` then dereferences freed memory.

The storage dialog's size-tally pool is the likeliest site — those workers
outlive a dialog the operator closes while a slow bucket is still being
measured, which is exactly a crash while siblings sit in `libcurl`.

**The fix is one word per site.** The inner `if (!guard) return;` was already
there and always correct — it runs on the UI thread, where `QPointer` is safe.
Only the receiver was wrong, so it is now `qApp`, which outlives every widget.

**And the real lesson is the one about symbols.** A crash report we cannot read
is a bug we cannot fix. The Windows job now stages `obs-multisite.pdb` as a
separate CI artifact — not in the release download, since it is a debugging
tool and larger than the DLL — and `BUILD-INFO.txt` records the commit, because
the dump did not say which build faulted and that had to be asked. A PDB only
matches the exact binary it was built with, so this had to be in place *before*
the next crash, not after.

Fixed alongside, from the caption code review and for the same reason —
unsynchronised state crossing threads: `CaptionBridge::m_output` was a plain
pointer written by `stop()` on the UI thread and read by the graphics thread
(now atomic, loaded once, and cleared before the callbacks are torn down), and
`split_caption` could loop for ever on a byte limit small enough to land inside
a multi-byte character.

