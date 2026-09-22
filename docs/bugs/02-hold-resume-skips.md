# BUGS #2 — Hold/resume skips: full record

Archive of the original entry. The short entry is in `BUGS.md`; this file holds
the complete history, moved here verbatim because it is long enough that no one
reads it before acting. **Read this before changing any timing arithmetic.**

> Original entry, as it stood on 2026-09-21. The header below is the entry's own.

---

### 2. Hold/resume skips, and the first two attempts to fix it stalled

**THE REQUIREMENT, stated 2026-09-19, and this entry had it wrong.** The entry
treats the resume skip as benign and chases the A/V glitch. The operator
requirement is the opposite way round:

- **Playing a LIVE room — behave like a DVR.** Holding freezes the picture and
  keeps recording. Resuming continues from where it froze, now time-shifted
  behind live, with catch-up offered.
- **Playing a RECORDING — behave like Netflix.** Pause and resume exact, jog
  exact, and timers that report position of total without jumping.

`plays_as_recording` (D1) is the discriminator, and it is now plumbed to the
dock, the appliance and the page.

**Measured 2026-09-19: resume does not continue where it paused, and the loss
grows with the hold.**

| hold | on screen at PAUSE | anchored at RESUME | lost |
|------|--------------------|--------------------|------|
| 1.4 s | 23.555 s | 23.755 s | 0.20 s |
| 10.9 s | 31.955 s | 33.045 s | 1.09 s |
| 66.7 s | 41.621 s | 45.525 s | 3.90 s |

**Cause.** `pause()` stops delivery and stops fetching new segments, but the
DECODER keeps decoding the fragments it already holds. Those frames reach a
queue nothing is draining, wait 250 ms in `enqueue_frame`, and are dropped. The
programme lost on resume is exactly those dropped frames: video drops rose by 35
across the 11 s hold and 93 across the 67 s hold, which at ~30 fps is 1.17 s and
3.1 s against measured losses of 1.09 s and 3.90 s. A recorder freezes the READ
head; this froze the reader and let the decoder run on, discarding.

This also retires the entry's own "the seat does NOT move" measurement, which
generalised from a single short hold. At 1.4 s the move is 200 ms and reads as
noise; at 67 s it is 3.9 s.

**Cause of the jumping timers (separate defect, same root shape).** Every
displayed clock derives from `pin_wall_ms - pin_base_pts`. `adopt()` clears the
media->wall pin on every epoch bump, so a resume re-pins — but no new fragment
is fed on a resume, so `restart_wall_ms` still holds the FIRST fragment's wall
time while `pin_base_pts` is the current position. The pair is mismatched and
the origin walks backwards by however far has been played:

```
09:00:38  first pts  0.000s -> origin 1789463961282
09:01:02  first pts 23.721s -> origin 1789463937561
09:01:21  first pts 33.024s -> origin 1789463928258
09:02:36  first pts 45.188s -> origin 1789463916094      (24 s of walk)
```

The pin log line already carries the test — "the pts here must match the one the
playout anchored on; when those two differ, the clock has been pinned to a
position already left". Anchored 45.525 s against pinned 45.188 s, 337 ms apart.
Logged, never checked, so nobody saw it.

**Option (a) was tried, measured, and REVERTED the same day. Do not try it
again.** `resume()` sought back to `last_out_pts_ns` via `seek_to_media_ms()` +
`after_jump()`. Two runs killed it:

```
RESUMED at   9.367s -> anchored  6.019s   (backward, fragment start)
RESUMED at 940.967s -> anchored 948.013s  (SEVEN SECONDS FORWARD)
both runs: first 1s after resume — video 0 frame(s), audio 0 frame(s)
           media clock pinned 3.6 s / 3.7 s AFTER the resume
```

Three separate faults, any one of them disqualifying:

1. **It stalls.** Seeking INTO a fragment means decoding and discarding from
   that fragment's start to the target: 3.3 s of skip cost 3.6 s of frozen
   picture, and a whole segment is the worst case. That is the same
   multi-second freeze that got `82b4183` and `12a54e4` reverted. A jog pays
   this and an operator accepts it; a resume cannot.
2. **It lands on the wrong fragment**, forward by 7 s in the second run, because
   the seek resolves through the media->wall mapping — and that mapping drifts
   (below). Broken clock, wrong seek, clock re-pinned wrong.
3. **The target was wrong anyway.** `last_out_pts_ns` is assigned in
   `deliver_video` at ENQUEUE time, not when a frame reaches OBS, so it runs a
   delivery-lead ahead of the picture: `PAUSED ... on screen 8.967s` then
   `RESUMED at 9.367s`, 400 ms out. It is the head of the queue, not the
   picture. Anything that wants "where the picture is" needs a different value.

**Option (b) MEASURED AND WORKING, 2026-09-19.** Four holds on a pinned
recording:

| hold | on screen at PAUSE | resumed at | loss |
|------|--------------------|------------|------|
| 5.2 s | 505.500 s | 505.859 s | 359 ms |
| 4.0 s | 510.100 s | 510.424 s | 324 ms |
| 10.3 s | 516.600 s | 516.633 s | 33 ms |
| 2.9 s | 733.333 s | 733.699 s | 366 ms |

Constant and INDEPENDENT of hold length — the longest hold lost the least.
No stall on any resume (frames flowing in every first-second window), no
"playout clock fell behind" line at all, and drops fell from 135 v / 175 a to
0-3. The residual ~350 ms is the queue clear at resume, as predicted.

**The clock origin walk: DIAGNOSED AND FIXED 2026-09-19.**

The same run isolated it exactly:

```
fragment wall 1789455501964, first pts 505.533 -> origin 1789454996431
fragment wall 1789455501964, first pts 510.133 -> origin 1789454991831
fragment wall 1789455501964, first pts 516.633 -> origin 1789454985331
```

The SAME fragment wall against an advancing pts, so the origin walked back by
exactly the pts advance (4.6 s, then 6.5 s). `adopt()` cleared the media->wall
pin on every epoch bump, so a resume re-pinned — but a resume feeds no new
fragment, so `restart_wall_ms` still held the wall time of the fragment fed at
the last DECODER restart. The pair described two different fragments.

Fixed by separating two things that were one: `PlayoutTimeline::adopt` now takes
a playout epoch AND a media epoch. A resume bumps only the playout epoch (wall
time moved on, media time did not, frames in flight are stale). A seek, jump,
stop or decoder restart bumps both. `test_playout_timeline` pins it: adopting a
new playout epoch against the same media epoch must leave `clock_offset_ms()`
untouched.

Note this was NOT the 1% wall-vs-pts drift suspected earlier when option (a) was
in place — that reading was contaminated by the seek landing on a different
fragment each time. One mechanism, not two.

**Earlier suspicion, now retired:** Across 942 s of pts the fragment wall advanced only 931.5 s, so the origin moved 10.5 s
in one playback run. Fragment wall times and media pts disagree by roughly 1%.
That is upstream of the resume path — it is about what wall time a fragment is
recorded as starting at — and it is what makes seek-by-time land wrong. Separate
defect, not yet diagnosed.

**Fix applied instead (option (b)): the producer parks while held.**
`enqueue_frame()` now parks the producer while `paused` is set, instead of
timing out after 250 ms and dropping the frame. The decoder stalls naturally:
nothing is decoded, nothing is discarded, and the decoder has not advanced past
the hold — so there is nothing to seek back TO. `resume()` re-anchors the playout
clock (wall time moved on during the hold, media time did not) and otherwise
leaves the position alone.

Parking is polled at 100 ms rather than waited on outright. `flushing` and
`running` release a parked producer, and both `stop_playback()` and
`after_jump()` set `flushing` before joining the decoder's worker — so the
deadlock the 250 ms timeout stood in for is already defended by the flag that
was always the real defence. The poll means even a missed notification cannot
wedge the decoder, and a wedged decoder here is a frozen OBS.

**CONFIRMED ON A SECOND RUN, 2026-09-19, including a 47 s hold:**

| hold | on screen at PAUSE | resumed at | loss |
|------|--------------------|------------|------|
| 3.0 s | 8.367 s | 8.728 s | 361 ms |
| 5.9 s | 729.833 s | 730.200 s | 367 ms |
| 46.8 s | 746.033 s | 746.392 s | 359 ms |

Flat across a fifteenfold range of hold length, and 359-367 ms is exactly the
video queue span of 367 ms — so the residual is the queue clear and nothing
else. No `media clock pinned` line after any resume either: the origin survives
a hold now, and is re-pinned only by PLAY and by a real seek.

Remaining wrinkle, not chased: video's min lead after a resume settles at
130-170 ms against audio's 378-395 ms. Positive and stable, so nothing is
delivered late and no burst warning fires, but video has markedly less margin
than audio for the first second. Was -441 ms before any of this.

**Expected residual: about 370 ms.** Resume still clears the queue, which holds
~367 ms of video, so that much is still skipped. It should now be CONSTANT
rather than growing with the hold, which is the thing to check. Removing it
means keeping the queue and re-timing it at resume — which is what `82b4183`
did and why it was reverted. Worth revisiting ONLY because the reason it failed
(stale frames tripping the stall resync, past a staleness check that compared a
value with itself) has since been fixed — but not before this is measured.

**Not yet verified.** 50/50 core tests pass and the plugin builds, but the suite
does not reach `multisite_source.cpp`. What to look for: `RESUMED at X —
continuing from where the picture stopped`, where X matches the preceding
`PAUSED ... on screen X`; and the media clock origin staying put across resumes
instead of walking.

**Still open after this:** the decoder still decodes and discards while held.
The seek makes that harmless rather than fixing it, and it is wasted work on a
Pi. Worth revisiting as option (b) — stopping the decoder during a hold — once
this is proven.

**Status: OPEN, understood, deliberately not fixed yet. The skip is benign; a
revert is in place so playback does not stall. Do not re-apply the reverted
commits — see "what was tried".**

**Symptom.** Hold, then resume: the picture and sound come back a fraction of a
second out of step. OBS reports `Max audio buffering reached` and sometimes
`audio is lagging (over by 352 ms) ... Restarting source audio`, and that flush
is what reads as the picture jumping. In a long hold it is worse: the report that
prompted this entry was a five-minute hold, and the resume stalled the picture
for several seconds.

**What was measured** (from two rounds of logs, and the reason the on-screen pts
logging below was added):

- the seat does NOT move. `PAUSED at segment 3 — on screen 4.288s` then
  `playout anchored ... (pts 3.388s)`: a 300 ms difference across a hold, so
  nothing skips a segment and the earlier "starts 2 segments ahead" report was a
  different fault, since fixed;
- the OBS audio buffering grows with the HOLD LENGTH: 661 ms after a 1 s hold,
  the 960 ms maximum after 2 s. That is the tell, and it says the fault is in
  what resume hands over, not in where it hands it over.

**CORRECTION, before acting on anything above: the root cause as first written
here was WRONG, and the fix it implies has already been warned against.**

There is only ONE anchor, not two. `anchor_pts` sets a single `first_pts_ns`
from whichever frame arrives first, and both streams compute their due time
against it via `playout_due_ns`. The ~350 ms gap between the streams' first
frames after a re-anchor is not a fault being introduced: it is the **CMAF
interleave gap**, documented in `src/core/playout_clock.h` as measured at
~344 ms in the field — and the 500 ms cushion at the anchor exists specifically
to absorb it. That is also why the anchor lands on audio on one resume and video
on the next, with no sync consequence.

`playout_clock.h` carries an explicit "READ THIS BEFORE FIXING THE ARITHMETIC",
added by an earlier investigation into this same area, and the fix proposed below
is the third variant of the same misreading of it. Do not re-derive the anchor
again without reading that header and `tests/test_playout_clock.cpp` first.

**What is therefore still the real question:** the cushion is meant to absorb
the interleave gap, and on resume OBS nonetheless reports
`audio is lagging (over by 352.49 ms) at max audio buffering. Restarting source
audio`. So the fault is NOT the anchor's reference; it is somewhere between the
re-anchor and what OBS receives, and the place to look first is the AUDIO
delivery path on resume — and why the cushion fails to cover the gap in that
one case (`kMaxDeliveryLeadNs`? the epoch bump? frames released immediately
because they are already due?).

**Original note, kept for the record — it is the wrong diagnosis:** video and
audio were said to be re-anchored separately on different references. They are
not. The queue asymmetry described below is real and separate, and does mean
resume can hand OBS an unbalanced batch.

**What was tried, and why it did not stand:**

1. **Keep the queue and re-anchor on its front** (commit `82b4183`, reverted by
   `cc53500`). Correct for a short hold and wrong for a long one: after minutes of
   holding the queue is ancient, so the delivery loop's stall resync fires
   (`playout clock fell 316.1s behind`) and the re-anchor costs SECONDS of frozen
   picture. That traded a half-second glitch for a multi-second pause, which is
   the wrong direction for this project.
2. **Trim the queue to the range both streams cover** (`12a54e4`, also reverted).
   Addresses the unpaired tail, but it sits on top of (1) and inherits the same
   long-hold problem.

Both were reverted together; the resume path is back to clearing the queue and
re-anchoring on the next frame, which is known to skip a little and to not stall.

**SUPERSEDED — kept visible because it is the trap, not the fix.** What follows
was written under the "two anchors" diagnosis that the CORRECTION above refutes.
Read in order, this entry used to talk a reader out of the wrong fix and then
hand it to them anyway as the next action; it is the fourth variant of the same
misreading and it must not be implemented.

> ~~Anchor BOTH streams on ONE reference at resume — the position that was being
> held — rather than letting each take its own first frame. That is a change to
> how the playout clock is established (`anchor_pts`, `first_pts_ns`,
> `playout_base_ns`, and the delivery loop's stall resync in
> `src/obs/multisite_source.cpp`), and it needs a test that asserts the two
> streams' anchors agree to within a frame after a resume.~~

There is nothing to make agree: `anchor_pts` already sets ONE `first_pts_ns` for
both streams, and a test asserting the two anchors agree would pass today,
against the unfixed bug, and prove nothing.

**What to do next is a MEASUREMENT, not a change.** The cushion is supposed to
absorb the interleave gap and on resume it demonstrably does not. Four candidates
are named above and the logs so far cannot separate them: the 500 ms cushion
itself, `kMaxDeliveryLeadNs`, the epoch bump, and frames released immediately
because they are already due. The discriminating measurement is **what the audio
path hands OBS in the first 500 ms after a resume** — how many frames and what
pts span, not merely where the anchor landed. Take that reading before changing
any arithmetic.

Worth taking in the same pass: the 12/48 queue-cap asymmetry (D4) is what lets a
resumed queue be unbalanced, so log both queue depths in MILLISECONDS at resume
rather than in frames. If audio is holding ~1 s and video ~0.4 s, that is a
measurement pointing at this symptom, and it also settles D4's open question with
a number instead of an opinion. It is still not claimed to be the cause.

**Instrumentation added 2026-09-19 to take that reading (measurement only — no
behaviour changed).** Three lines, each of which rules a candidate in or out.
None of them existed before, which is why three rounds of logs could not
separate the causes: the periodic lead report averages over its whole interval,
and that is exactly how a 500 ms event at resume disappears.

1. `interleave gap this anchor: <stream> leads by N ms (cushion 500 ms, ...)`
   — printed once per re-anchor, when the first frame of the stream that did
   NOT win the anchor arrives. Until now the ~344 ms gap was a field average
   quoted in `playout_clock.h` and never measured per resume. If it comes back
   OVER 500 ms, the cushion is simply too small and the arithmetic is innocent;
   if it is well inside, the anchor is not where to look.

2. `queue at resume held video N frame(s)/X ms, audio N frame(s)/Y ms` — taken
   before the queue is cleared. The caps are COUNTS (12 video, 48 audio), so
   frame numbers cannot say whether the two streams held equivalent amounts of
   programme. This is D4's question asked in milliseconds, which is the only
   unit in which it has an answer.

3. `first 1s after resume — video N frame(s), X ms of programme, min lead A ms;
   audio ...` — a window armed at resume and reported once, separate from the
   periodic report so neither dilutes the other. A **min lead at or below zero**
   means frames were already due when handed over and left as a burst rather
   than paced; that is the "released immediately because already due" candidate,
   and it is the shape OBS reports as lagging. A warning line calls it out
   explicitly when it happens, so it does not have to be spotted by eye.

**How to read the result.** If (3) shows a non-positive min lead, the fault is
the burst at handoff and NOT the anchor. If (1) exceeds the cushion, the cushion
is the fault. If audio's programme-ms in (2) or (3) far exceeds video's, D4's
asymmetry is real and is delivering an unbalanced batch. If all three look
healthy, everything up to our handoff is exonerated and the question moves to
what OBS does with timestamps it accepted — which would be the first time that
has been established rather than assumed.

**READING TAKEN 2026-09-19, ~54 s hold on a pinned recording. The cushion is
not too small — it is OVERWRITTEN 11 ms after it is set.**

Raw, in order:

```
08:45:21.430 queue at resume held video 12 frame(s)/367 ms, audio 48 frame(s)/1003 ms
08:45:21.430 RESUMED at 39207s behind live (dropped 60 queued frames, clock re-anchoring)
08:45:21.431 playout anchored on first video frame (pts 6.088s)
08:45:21.431 interleave gap this anchor: audio leads by 355 ms (cushion 500 ms, within the cushion)
08:45:21.442 playout clock fell 50.4s behind (stall?) — re-anchored
08:45:22.974 first 1s after resume — video 40 frame(s), 1300 ms of programme, min lead 66 ms;
                                     audio 47 frame(s),  981 ms of programme, min lead 377 ms
```

**The cushion is exonerated.** The gap is 355 ms and the cushion is 500 ms, so
it covers it with 145 ms to spare. The ~344 ms in `playout_clock.h` is confirmed
as a real figure (355 here), and it also matches the "over by 352 ms" in the
original report almost exactly.

**The gap is a RESUME-ONLY phenomenon.** Measured at 0 ms at a fresh decoder
start and 0 ms at a seek — both streams begin a new decoder at the same pts —
and 355 ms only at a resume, on a decoder that has been running. Two anchors at
decoder start landed on AUDIO; this resume landed on VIDEO, confirming the
header's "audio on one resume and video on the next".

**What actually happens: the stall resync fires 11 ms after the re-anchor and
assigns a new base.** 50.4 s is the hold length; the threshold
(`kClockResyncThresholdNs`) is 2 s, so ANY hold over two seconds guarantees this
if a pre-hold timestamp reaches the delivery loop. The resync does

```c
new_base = now - (pts - first) + kMaxDeliveryLeadNs;   // 400 ms
```

which REPLACES the 500 ms cushion `anchor_pts` set 11 ms earlier with a 400 ms
delivery lead, computed from whichever frame tripped it, and then rewrites every
queued frame's timestamp against it. So the answer to "why does the cushion not
cover the gap on resume" is that by the time the gap matters, the cushion is no
longer there.

**The damage is one-sided, and D4 is why.** Video was handed 1300 ms of
programme in 1000 ms of wall clock — 1.3x real time — and its lead collapsed
from ~400 ms to 66 ms. Audio ran at real time (981 ms in 1000 ms) with its lead
intact at 377 ms. The queue reading is the reason the two are not alike: at
resume video held 367 ms and audio 1003 ms, BOTH AT THEIR CAPS. D4 predicted
"about 0.4 s and 1 s" from the 12/48 counts; measured 367 ms and 1003 ms. D4 is
no longer a suspicion, it is a measurement, and it is coupled to this bug.

**ROOT CAUSE FOUND 2026-09-19, and it is not any of the four candidates: the
staleness check was a no-op.** The delivery loop read the epoch like this:

```c
item_epoch = ctx->timeline_epoch.load();   // at POP time
...
tl.adopt(ctx->timeline_epoch.load());
if (item_epoch != tl.epoch()) continue;    // the same atomic, compared to itself
```

The epoch was never stamped on the frame. It was loaded from `ctx` when the
frame was popped and then compared against that same `ctx` value microseconds
later, so the comparison could not fail. Three separate comments describe this
check as the thing that stops a frame from a timeline already left re-anchoring
the clock. It never stopped anything.

`PlayoutTimeline::epoch()` even documents the intended design — "Stamp for
frames leaving the queue now. Compare with what comes back." — and the stamp
simply was not applied to the frame.

**How that produces the symptom, end to end.** During a hold the decoder thread
sits in `enqueue_frame` with the queue at its caps. `resume()` clears the queue,
bumps the epoch and notifies. The waiting producer wakes, finds space, and
pushes a frame whose `timestamp` was computed BEFORE the wait, against the
pre-hold playout base. The dead epoch check waves it through; it is ~54 s past
due; it trips the 2 s stall resync, which ASSIGNS
`now - (pts - first) + kMaxDeliveryLeadNs` — replacing the 500 ms cushion
`anchor_pts` set 11 ms earlier with a 400 ms lead, and rewriting every queued
frame against it. Hence video delivering 1300 ms of programme in 1000 ms of wall
clock with its lead collapsing to 66 ms, while audio, already sitting further
from the threshold, kept 377 ms.

Every measurement fits: the 50.4 s ≈ the 54 s hold; any hold over the 2 s
threshold reaches it, which is why the fault scaled with hold length; and the
cushion was never the problem.

**The fix.** `PendingFrame` now carries `epoch`, stamped in `enqueue_frame`
AFTER its up-to-250 ms wait and under `dq_mtx` — the same lock every epoch bump
is already taken under, so a frame cannot be stamped half-way through a timeline
change. The delivery loop compares the frame's own epoch. Stamping where the
frame is BUILT would have reintroduced the bug in a subtler form, since the wait
is exactly where a resume overtakes a frame.

Note what this did NOT touch: the anchor, the cushion, `playout_due_ns`, and the
resync arithmetic are all unchanged. The lever was the one the reading pointed
at.

**FIRST ATTEMPT AT THE FIX WAS WRONG — stamped in the right spirit, the wrong
place (2026-09-19).** The epoch was stamped in `enqueue_frame`, AFTER its
up-to-250 ms wait, on the argument that the wait is exactly where a resume
overtakes a frame. That argument is backwards. A frame waiting there wakes up
into the NEW epoch and gets stamped with it, so it still looks current — the bug
again, with a label on it.

Three resumes proved it, and the short one proved it best:

```
hold  1.4 s: NO resync (1.4 s is under the 2 s threshold), but
             first 1s after resume — video min lead -441 ms  ← frames 441 ms LATE
hold 11   s: playout clock fell 9.1s behind — re-anchored, video min lead  +97 ms
hold 67   s: playout clock fell 62.3s behind — re-anchored, video min lead +389 ms
```

The short hold is the useful one: with no resync to mask it, the stale frames
simply went out 441 ms late — which is the jump, seen directly for the first
time. It also shows the resync was never the disease. It is a dressing over
stale frames, and on the 67 s hold it happened to leave the leads healthy, which
is why this fault reads as intermittent.

**The epoch that matters is the one the TIMESTAMP was computed under**, since
that is what makes the timestamp meaningful or stale. Now stamped beside
`playout_due_ns` in `deliver_video`/`deliver_audio` (all four construction
sites, companion audio included), reading the epoch BEFORE `playout_base_ns` so
that a resume landing between the two reads errs towards dropping a good frame
rather than passing a stale one.

**STILL NOT VERIFIED against a real hold.** The reasoning accounts for every
number in the log above, and 50/50 tests pass, but the suite does not cover
`multisite_source.cpp` — this is plugin glue, not core — so the only proof is a
hold and a resume on the real thing. What success looks like: NO "playout clock
fell Ns behind (stall?)" line after a resume, and both min leads in the "first 1s
after resume" line staying near 400 ms instead of video collapsing to 66 ms.

**Superseded, kept for the record:** that the resync's reassignment is what
compressed video's lead. The readings are all consistent with it, but nothing
yet records WHICH frame tripped the resync or what base it was computed against
— a frame whose epoch survived the bump, or one whose timestamp was built from
the pre-hold base that `resume()` never clears (it resets `first_pts_ns` and not
`playout_base_ns`). That is the next two-line measurement, and it should be
taken before anything is changed.

**Note for whoever fixes this:** the lever this points at is the resync
overwriting the base, NOT the anchor. Re-deriving the anchor is the trap this
entry has already warned about three times, and this reading does not implicate
it — one anchor, correctly placed, 355 ms of gap, a cushion big enough to hold
it.

**Earlier instrumentation that stays.** `PAUSED` now prints the pts of the last
frame handed to OBS and the queue depth, and `RESUMED` prints what it continued
from.
Without that figure the two candidate causes were indistinguishable and cost
three rounds of guessing — keep it, and keep it honest.

---

## 2026-09-21 — resume VERIFIED, and the old unexplained half reproduced

### The resume fix is confirmed

First real hold since the epoch-stamp fix. Verbatim log, trimmed to the markers:

```
17:06:45.300  PAUSED at segment 143 — on screen 848.067s, 70 frame(s) queued
17:06:45.531  dropped a audio frame after waiting 250 ms — ... PAUSED,
              this stream held 1003 ms of programme (bound 1000 ms)
17:06:46.939  queue at resume held video 22 frame(s)/700 ms, audio 48 frame(s)/1003 ms
17:06:46.939  RESUMED ... 70 queued frame(s) discarded, clock re-anchoring) — held from 848.067s
17:06:46.939  playout anchored on first video frame (pts 848.100s)
17:06:46.939  interleave gap this anchor: audio leads by 351 ms (cushion 500 ms, within the cushion)
17:06:51.683  first 1s after resume — video 27 frame(s), 867 ms of programme, min lead 395 ms;
                                      audio 26 frame(s), 533 ms of programme, min lead 395 ms
```

Read against what the entry predicted:

- **held 848.067 s → anchored 848.100 s = 33 ms.** Constant, not growing with
  the hold. The ~370 ms residual expected from the queue clear is *not present
  here* — 33 ms, because the queue was cleared and re-anchored cleanly.
- **NO `playout clock fell Ns behind (stall?)` line.** The failure that
  contaminated every earlier run is gone.
- **Both min leads 395 ms**, video and audio equal. The old signature was video
  collapsing to 66 ms while audio held 377 ms.

The 2026-09-19 epoch-stamp fix is therefore **verified on real content**, not
merely reasoned. `PlayoutTimeline`'s playout-vs-media epoch split holds too: the
origin did not walk.

### The new fault, immediately after

Seconds later, delivery stops handing frames to OBS and the queue jams:

```
17:06:51.683  head=145 live=608 behind=2778s buffered=279s ... frames_out=7845
17:06:51.683  lead video mean=+398ms min=+395ms (591) | audio mean=+398ms min=+395ms (908) | dropped 0 v / 8 a
17:06:52.309  dropped a audio frame ... delivery last handed over 1265 ms ago, playing, held 1003 ms (bound 1000 ms)
17:06:54.327  dropped a audio frame ... delivery last handed over 3283 ms ago, playing, held 1003 ms (bound 1000 ms)
17:06:56.347  dropped a audio frame ... delivery last handed over 5302 ms ago, playing, held 1003 ms (bound 1000 ms)
```

`frames_out` frozen at 7845 from 17:06:51 onward. `delivery last handed over`
climbs 1265 → 3283 → 5302 ms with no bound. Audio sits at 1003 ms and drops a
frame every ~2 s (the log rate-limit). This is the entry's previously
unexplained observation — *"22 of the 47 drops were audio... the delivery loop
itself must have stopped draining for ≥250 ms at a stretch"* — reproduced and
open-ended here.

### What is confirmed about the mechanism

`queued_span_ns()` (`multisite_source.cpp:715`) returns the pts span of one
stream's queue, and `enqueue_frame` accepts a frame only while that span is
below `kMaxQueuedNs = 1000 ms` (`:780`). Two separate facts, both read from the
log:

1. **48 audio frames span 1003 ms** (audio ≈ 20.9 ms/frame). So a full audio
   queue is **3 ms over the 1000 ms bound** — permanently "full" by the span
   test. That number is structural, not a draining artefact.
2. **`queued_span_ns` has a count backstop** (`:726`): at
   `n >= kQueueHardCapAudio` (480) it returns `kMaxQueuedNs` regardless of span.
   Audio reached only 48 frames here, so the backstop did *not* fire — the
   *span* test alone declared full.

The `kMaxQueuedNs` comment already states the principle: *"A queue whose
capacity equals the lead the delivery loop is trying to hold is full by
construction."* This is the same fault expressed in span rather than count, at a
bound 3 ms too tight for the stream it bounds.

### What is NOT established

Whether the too-tight span bound *causes* delivery to stop, or delivery stops
for its own reason and the bound prevents recovery. Both fit the log. They are
not separable from these four lines, and no arithmetic should be changed until
they are — this is the fifth time this area has been "fixed" from a plausible
reading.

Next measurement: instrument the deliver loop's wait at `:954` — how long it is
parked, at each stage — and log whether `enqueue_frame` is dropping while the
delivery loop is awake or asleep. That separates the two.

### Diagnostic repaired while capturing this

The `queue at resume` line had a format string with a fifth `%.0f` and only four
arguments, so `the bound is` printed whatever was in the register — `0`. The
number this whole investigation depends on was undefined behaviour. Fixed:
`kMaxQueuedNs / 1e6` is now passed. Note `plugin_log_line` is marked
`format(printf, 2, 3)` and the attribute **does** fire on the macro pattern when
`-Wformat` is on — verified with a standalone repro. The reason nothing caught
it is that **the plugin build compiles with no warning flags at all**:
`build-obs` reports `CMAKE_CXX_FLAGS:STRING=` (empty), so `-Wformat` is never
enabled for `src/obs/`. The core build has its flags; the plugin target does
not. That is the class-level hole, and it is why a format bug could sit in the
diagnostic the investigation rests on.

---

## 2026-09-22 — the cause: the deliver loop STOPS RUNNING (measured)

The instrumented build (commit `0e160c9`, measurement-only) reproduced the stall
on a Mac. The reading is decisive and it refutes the hypothesis this archive
previously recorded.

### What was measured

```
07:21:31.539  queue at resume held video 22 frame(s)/700 ms, audio 48 frame(s)/1003 ms (bound 1000 ms)
07:21:31.539  RESUMED ... held from 529.567s
07:21:31.539  playout anchored on first video frame (pts 529.600s)
07:21:31.818  dropped a audio frame ... loop parked in due-wait, last popped 8 ms ago
07:21:31.818    — parked 8 ms into a due-wait for a audio frame timestamped 421 ms in the future
07:21:32.790  first 1s after resume — video 27 frame(s) ... min lead 395 ms; audio 26 frame(s) ... min lead 395 ms
07:21:35.372  dropped a audio frame ... loop parked in due-wait, last popped 17 ms ago
07:21:35.373    — parked 17 ms into a due-wait for a audio frame timestamped 400 ms in the future
07:21:37.414  dropped a audio frame ... loop parked in due-wait, last popped 1270 ms ago
07:21:37.414    — parked 1270 ms into a due-wait for a audio frame timestamped 5039 ms in the future
07:21:39.443    — parked 3299 ms into a due-wait for a audio frame timestamped 3009 ms in the future
07:21:41.468    — parked 5324 ms into a due-wait for a audio frame timestamped 984 ms in the future
07:21:42.057  deliver loop waited 5913 ms for one frame (its timestamp was +395 ms from now, audio)
```

### The decisive line, and why

`deliver loop waited 5913 ms for one frame (its timestamp was +395 ms from now,
audio)` is logged **by the deliver loop itself, immediately after the wait
returns**. It says the loop entered the wait with the frame **395 ms** away and
the wait took **5913 ms**.

The wait is:

```c
while (ctx->running.load()) {
    const uint64_t now = os_gettime_ns();
    if (item.timestamp <= now + kMaxDeliveryLeadNs) break;
    uint64_t wait_ns = item.timestamp - now - kMaxDeliveryLeadNs;
    if (wait_ns > 50000000ULL) wait_ns = 50000000ULL;   // ≤ 50 ms slices
    std::this_thread::sleep_for(std::chrono::nanoseconds(wait_ns));
}
```

`item.timestamp` is a fixed value — it is computed once at enqueue and never
rewritten. A loop that re-reads the clock and sleeps ≤50 ms at a time cannot
take 5.9 s for a target 395 ms away. **The only explanation is that the thread
did not run.** It was descheduled (or blocked inside `sleep_for`) for the whole
interval.

While it did not run it popped nothing, so the queue stayed at its cap,
`enqueue_frame` timed out after 250 ms and dropped every audio frame, and
`frames_out` froze. Every earlier observation follows from this one fact.

### Two hypotheses this refutes

1. **The queue bound.** The `1003 ms against a 1000 ms bound` reading is a
   *symptom*: the span sits at the cap because the loop that would drain it is
   not running, not because the bound is 3 ms too small. The count-backstop
   theory (audio hits 48 frames before its span reaches 1000 ms) is wrong — the
   span reached 1003 ms precisely because nothing was draining it.
2. **A far-future timestamp.** The waited-for timestamp was 395 ms out. The
   `parked N ms into a due-wait ... timestamped M ms in the future` lines
   suggested otherwise, and are misleading: see below.

### A diagnostic that lies, and must be read with care

The `parked N ms into a due-wait ... timestamped M ms in the future` line is
emitted from `enqueue_frame`, on a *different thread*, which reads `dl_in_wait`
and `dl_wait_start_ns` non-atomically. Its `M` values on successive lines
(`421`, `400`, `5039`, `3009`, `984` ms) disagree with each other and with the
`+395 ms` the loop itself measured. The cross-thread sample is indicative only;
**the loop's own `deliver loop waited …` line is authoritative.**

This is the second time in two days a self-authored diagnostic was the thing
that misled: the earlier one printed `bound 0` from a missing printf argument.
Both were caught, but the pattern is worth stating — a measurement added to
settle a question is itself code, and gets no more trust than the thing it
measures.

### What is NOT known

**Why the thread is descheduled.** Candidates, none tested:
- CPU contention on the Mac (OBS running encoders and muxers alongside the
  decoder, on this machine, at this moment);
- the thread blocked inside `sleep_for` for far longer than its slice;
- a priority inversion against a busy thread;
- contention on a lock the loop takes elsewhere in its iteration.

This is the **same shape as BUGS #0** — a worker thread parked, its producer
never progressing — and the two should be read together. That entry's remedy is
a thread dump while stalled; the same applies here.

### Next step

On the next repro, capture thread state **while it is stalled**:

```
sample <obs pid> 10                       # macOS, 10 s of samples
# or
lldb -p <obs pid> -o "thread backtrace all" -o detach -o quit
```

Look for: is the deliver thread running or parked inside `sleep_for`, and what
else is on the CPU at that moment. **No arithmetic changes until that is seen.**
