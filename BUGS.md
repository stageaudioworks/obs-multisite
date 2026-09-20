# Bugs and short-term build items

Working list, meant to survive a change of machine or a change of agent —
each entry has enough context to act on without anyone having been in the
room when it was written. Delete an entry once it's fixed and released;
this file is not a changelog.

Last updated: 2026-09-16.

---

## Open

### 0. Pi player: playback can stall indefinitely while downloads keep succeeding

**Status: root cause not found. Needs a live thread-dump on next repro.**

Seen on `rpi5-nathan` running `multisite player 0.1.7`, following room
`main-auditorium`. The player's own status line (`src/appliance/player.cpp`,
logged every 60s) showed, for an 11-minute stretch:

```
20:37:12  playing head=16 live=81 behind=388s buffered=245s cached=26 downloaded=160 frames_out=23580 fps=0.0 dropped=3
20:39:04  playing head=16 live=81 behind=388s buffered=245s cached=31 downloaded=165 frames_out=23580 fps=0.0 dropped=3
...
20:48:12  playing head=16 live=81 behind=388s buffered=346s cached=65 downloaded=199 frames_out=23580 fps=0.0 dropped=3
```

`head` and `frames_out` are frozen the entire time; `cached`/`downloaded`
keep climbing throughout. `live=81` staying fixed is *correct* here — the
room had just gone to `BROADCAST ENDED`, so that event's manifest is
genuinely final — that part is not the bug.

**What that rules out:**
- Not a network/download problem. Segments keep arriving (`cached`,
  `downloaded` climbing) the whole time.
- Not the delivery queue's drop-under-backpressure path
  (`Player::enqueue()`, bounded to a 250ms wait before dropping a frame
  rather than blocking forever). If that path were engaging repeatedly for
  11 minutes, `dropped` would be in the thousands; it sits flat at `3`
  throughout. So `on_video`/`on_audio` aren't even being called — decode
  isn't happening at all, not "happening but being dropped."

**Where that points:** `Player::feed_loop()` (`src/appliance/player.cpp`)
calls `sess->next_segment()` then `dec->push_fragment(seg->media)` — the
latter is documented as "blocks when the decoder is full." If `feed_loop`
is stuck inside that call, `next_segment()` never runs again, which is
exactly why `m_head` freezes while everything upstream (poll, prefetch)
keeps working. Not confirmed — this is the leading hypothesis, not a
diagnosis.

**Correlating detail, cause or symptom, not yet known which:**
`sound has broken up 10 times` / `20 times` (ALSA xrun warnings, from
`src/appliance/alsa_output.cpp`) appears right as each stall begins.

**Since this was written**, the delivery queue is bounded per stream rather
than as one total (`kMaxQueuedVideo` / `kMaxQueuedAudio`) and the status line
now splits drops — `dropped=N (V v / A a)`. The reasoning above that ruled out
the drop path still holds, and the split makes the next repro say which stream
is affected. It also removes one contributor worth knowing about: the delivery
thread both presents the picture and calls `snd_pcm_writei`, which blocks, and
under the old flat bound audio could take every slot while it was blocked,
leaving no room for a picture. That is the mechanism behind the ALSA xruns
correlating with each stall, so it may or may not have been part of this — the
thread dump is still what decides.

**Next step, the moment this reproduces again** (box still up, stall
ongoing):
```sh
gdb -p $(pgrep multisite-player) -batch -ex "thread apply all bt"
```
That single command turns the hypothesis above into a diagnosis — it will
show exactly which call `feed_loop` (and the decode thread) are parked in.
Save the output into this entry before doing anything else.

**Files likely involved once the stack trace is in hand:**
`src/appliance/player.cpp` (`feed_loop`, `deliver_loop`, `enqueue`),
`src/core/cmaf_decoder.cpp` (`push_fragment`), `src/appliance/alsa_output.cpp`.

**Since this was written, again:** the status line now watches itself for
this exact shape — `state == "playing"` with `fps` near zero for two
consecutive updates while `downloaded` keeps climbing — and logs a `WARN`
naming the box's own pid and the `gdb` command above, rather than requiring
someone to notice the pattern across several quiet status lines. This does
NOT fix the stall or find its root cause; it only makes the next occurrence
impossible to miss and easy to act on immediately. `src/appliance/player.cpp`,
around the 60-second status log.

**Since this was written, a third time — the suspected mechanism is now
bounded.** The leading hypothesis is `feed_loop` parked in
`CmafDecoder::push_fragment()`, waiting for queue space that a wedged decode
thread will never free. That wait is no longer unbounded: `push_fragment`
now waits only while the decoder is still PRODUCING (a monotonic
`last_progress_ms` is stamped on every emitted frame and every fragment),
and once nothing has come out for ten seconds it declares the decoder wedged,
returns `false`, and sets an error. Both feeds act on that: the Pi player
throws the decoder away and rebuilds it (`Player::teardown_decoder()`, which
re-requests the init segment), and the OBS source logs it plainly. So the
worst case is now a reported, acted-on event that costs at most one segment,
not a silent freeze with downloads still climbing. A decoder wedged INSIDE
FFmpeg rather than in our push would still park `stop()`'s join — the thread
dump remains the way to settle which — but this code is no longer part of the
path that can hang. `src/core/cmaf_decoder.{h,cpp}`, `src/appliance/player.cpp`
and `src/obs/multisite_source.cpp`; the boundary is pinned by
`tests/test_cmaf_decode.cpp`.

**Since this was written, a fourth time — the other half is bounded too.**
The paragraph above closed `push()`'s wait, but `stop()` still joined the
decode thread unconditionally, so a thread wedged *inside* an FFmpeg call
(the case that paragraph flagged as still open) would park teardown's `join`
for ever — which is exactly what `Player::teardown_decoder()` calls to rebuild.
`CmafDecoder::stop()` now waits only a bounded grace period (5 s) for the
worker to return; if it does not, the thread is detached and its state is
intentionally leaked (freeing it would be a use-after-free in a thread we can
no longer control), an error is set, and the caller builds a fresh decoder.
The abandoned path is pinned by a new case in `tests/test_cmaf_decode.cpp`
that plants a wedge in a frame callback and asserts `stop()` returns anyway.
`src/core/cmaf_decoder.{h,cpp}`.

What remains genuinely un-diagnosed is *why* the decode thread wedged in the
first place — the thread dump is still the only thing that settles it, and the
self-watching WARN above is still what triggers one. The difference is that
the process now survives it and keeps relaying.

---

### 1. AES67 audio: proven over an event, PTP lock accuracy at a receiver is not

**Status: eight channels of clean AES67 audio, proven on a bench Pi and since
run through a multi-hour test — picture and sound watched and listened to
together the whole way through, no drift found. Still unmeasured: the PTP
lock accuracy a Pi's network interface can actually hold, checked at a
receiver over that same length of time.**

The lip-sync half of this is not really an AES67 question: audio and video
are scheduled off the same delivery-queue clock and the same first-frame
anchor (`Player::anchor_pts()`, `src/appliance/player.cpp`) regardless of
which card the sound goes out on, so the two cannot drift apart from each
other whether the output is AES67, HDMI or USB. What the multi-hour run
actually proves is that this holds under sustained real load, not that
playback is slaved to the AES67 card's own PTP-disciplined sample clock —
that clock is the RAVENNA driver's doing, real and relevant to a *receiving*
console's own sync to the network, but external to this project entirely.

`scripts/player/merging-aes67.sh` installs an open AES67 stack: Merging's
`ravenna-alsa-lkm` kernel module, which registers a virtual ALSA card, plus the
GPL `aes67-daemon` from `aes67-linux-daemon` that talks to that module over
netlink and stands in for Merging's own Butler — the part that is amd64-only and
licensed, and the reason an open build runs on a Pi at all. The daemon does RTP,
SDP/SAP and PTP itself, and lets a stream be aimed at a chosen multicast address,
port and channel map. It has now been run on a bench Pi: the module built against
the running kernel, the daemon came up, the card appeared, the player opened it,
and eight channels of clean audio arrived. (An earlier route used a licensed
virtual sound card, which also played eight channels on the bench but pinned an
eight-millisecond buffer and could not be aimed at a chosen destination.) The
operator-facing version of this is
[docs/SATELLITE.md](docs/SATELLITE.md#aes67-audio-on-the-network).

The stream itself is no longer something an operator has to build in the daemon's
own interface. `merging-aes67.sh` creates one as part of the install — eight
channels, L24, at the multicast address its own configuration names — and the
player then keeps that source in shape and switches it: **Settings → Sound on the
network** for on/off, the address and the width, and **This box → Sound on the
network** for what is actually being sent, read back from the daemon. The
player's page is therefore the everyday route, and the daemon's own WebUI on 8081
is for the rest of what the daemon can do.

**What is actually left, in order:**

1. **A PTP master must exist on the network.** The daemon slaves to a clock; it
   does not hand one out. With no master — a Dante device, a console, an Anubis
   — it never reports "locked" and no audio flows. This is the most likely
   reason for silence after a clean install, and it is a network question rather
   than a fault in the install.
2. **PTP lock accuracy, measured at the receiver, not the Pi.** Lip sync over
   a multi-hour run is now settled (see the status line above). What is not:
   how well PTP itself holds — AES67 wants both ends within a millisecond,
   and a Pi's network interface does no hardware timestamping, so the
   achievable accuracy is whatever the software manages. That needs a
   receiving console's own read on it, over the same length of time. The Pi's
   own side of that comparison is now recorded (see below).
3. **Dante routing is by hand.** A source shows up in Dante Controller, but
   connecting it to a receiver is a manual step in that application.
4. **A kernel upgrade means rerunning the installer.** The module is built from
   source against the running kernel and is not put through DKMS, because its
   build takes a branch of the submodule and a compiler choice a DKMS hook
   cannot reconstruct reliably. Rerunning the script rebuilds it.

**Since this was written:** the Pi's half of that comparison is now recorded,
where before it was only readable live in the daemon's own WebUI. The 60-second
status line gains ` ptp=locked 12.3ns` (or `UNLOCKED`) whenever the AES67 probe
has read `/ptp/status`, and the box's own page shows the same jitter beside the
grandmaster it locked to. This does NOT answer point 2 — the number that decides
it is the *receiver's*, so a console's read over a long run is still what closes
this — but a full-length service now leaves a per-minute trace of how tightly
the Pi held the clock, so the two ends can be compared after the fact instead of
the question resting on nobody having looked. `src/appliance/player.{h,cpp}`,
`src/appliance/web/app.js`.

**Next step:** a full-length service on the picture and the sound together,
which is point 2 above — capturing the `ptp=` trace from the Pi's journal and
the receiving console's own lock figure over the same run.

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

**Earlier suspicion, now retired:** Across
942 s of pts the fragment wall advanced only 931.5 s, so the origin moved 10.5 s
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

## Recently landed (context, not action items)

- **AV1 goes out over RTMP now, with the caveat that used to be the refusal —
  and the old refusal had a bug in its advice.** An AV1 event aimed at an RTMP
  destination was told to "send this to an SRT destination instead", and SRT
  cannot carry AV1 either: ffmpeg has no AV1 stream type in MPEG-TS, in either
  direction, so the guidance walked somebody from one dead end into the next.
  The test asserted that advice — `p.remedy.find("SRT") != npos` — which is
  precisely how it survived a review that was looking for typos.

  Both halves are now what is true. **Over RTMP, AV1 travels as Enhanced RTMP**,
  like HEVC, verified through the relay's own argument vector: byte 0 = `0x90`
  (`isExVideoHeader`, KeyFrame, SequenceStart) with a FourCC of `av01`, reading
  back as AV1. So what remains is a *destination* question, and refusing on it
  was the same mistake as refusing HEVC on an assumption that had stopped being
  true. `sendability` puts the caveat above every destination instead — only a
  site that documents AV1 ingest will take it over RTMP (YouTube does), one that
  refuses it will drop the stream as soon as it starts, and over SRT it cannot go
  at all — and the relay's supervision reports that failure rather than hiding
  it. **Over SRT the refusal stands**, because there is nothing to send.

  Two smaller things fell out. The status line read
  `(vc == "hevc") ? "HEVC" : "H.264"`, so an AV1 stream announced itself as
  H.264 — harmless while AV1 could not be sent at all, and a lie the moment it
  could. And an unknown codec is still refused outright on both protocols, which
  is where the gate now draws its line: what we can name and carry, we carry;
  what we cannot, we decline with a sentence rather than guessing.

- **An event recorded with cloud upload off cannot be listed — and now says so
  rather than looking broken.** The report was an AV1 event that played live and
  was then nowhere in the recordings list. It turned out to be neither AV1 nor
  the listing being wrong: the encoder's own log had said it at go-live —

  ```
  20:13:48.104 [multisite] cloud delivery is disabled for this event —
  publishing to the LAN cache only, nothing leaves this machine
  ```

  — so nothing was published and there was nothing to list. Playback worked
  because the live pointer still named the event and the LAN object server
  serves that event's objects, which is exactly the asymmetry that got reported:
  play it fine, cannot see it in a list.

  The list is built from the cloud transport and only from it
  (`multisource_source.cpp`: `if (tx) { cat = make_shared<EventCatalog>(…) }`),
  and that is right rather than lazy — the LAN side "only ever knows about
  whichever one is live right now", so there is no history over there to
  enumerate. What was wrong was the silence. With no catalogue nothing was ever
  cached and `listed_once` stayed false, so the dock sat on "Looking for
  recordings…" for ever and the appliance showed an empty list with no
  explanation. A dead end that reads as a spinner is worse than one that reads
  as a sentence.

  Fixed by making the absence of a catalogue a fact the UIs can report:
  `EventListing::no_catalog` in both layers, set by the source and by the
  player, worded where the words live — `Dock.EventsNoCloud` in the dock, a
  plain sentence in the appliance's page. Two strings at the point of the
  decision grew the clause they were missing as well:
  `Dock.CloudDisableConfirm` and `Dock.CloudOffSuffix` now say that an event
  published this way will not appear in any recordings list, because that
  dialog is the moment an operator chooses it. And the log line that used to
  read "upload verified in bucket" now reads "upload verified where it was
  sent", since with cloud off the store is the LAN cache — a line claiming a
  bucket that never saw the bytes is how somebody concludes their event was
  archived when it was not.

  **Worth knowing, and deliberately not "fixed":** a LAN-only recording is not
  durable. `SpoolQueue::begin_event` clears every `.seg` and `.meta` at the
  start of the next event, and the LAN object server 404s any event id that is
  not the current one, so the bucket remains the only archive. An event recorded
  this way lives on the encoder's disk until the next broadcast starts. Making
  the LAN side a place recordings live would mean building a listing it was
  never meant to have; saying so is the honest version until somebody wants it.

- **AV1 is now round-tripped rather than merely carried — and the report that
  started it was not AV1's fault.** An AV1 event played live and was then
  nowhere in the recordings list. Since the live path worked, the question was
  the listing, and the answer is that the listing has never known one codec from
  another. That is now a test rather than an assertion.

  What was actually missing was proof. `cmaf` and `cmaf_hevc` round-tripped the
  other two codecs and nothing in the tests said "av1" anywhere, so "carried but
  lightly exercised" was carrying a lot of weight in the README: an AV1 setting
  produced a stream that nothing in this repository had ever decoded. Added:

  - **`cmaf_av1`** with `cmake/run_av1_test.cmake`: an AV1 fixture (1280×720,
    two AAC tracks) through the real muxer, then every fragment decoded back —
    180 frames from the first, 60 from the second, both audio tracks present.
  - **`cmaf_av1_decode`**: the muxer's own output read back through
    `CmafDecoder`, as the H.264 fixture is.
  - **`test_event_catalog`**: an ended AV1 event lists as a recording, a live
    one is still the live one, and neither is skipped or reported unplayable.
  - **`test_session` 9b**, which is the report reproduced properly: a real
    `Session` with `video.codec = "av1"`, real segments, a real `end()`, and
    then the real `EventCatalog` over the same store. It lists as a Recording
    with nothing skipped.

  Two things the first draft of that script did, both worth not repeating.
  **It passed while proving nothing**: passing `-aom-params` alongside
  libsvtav1 is an "Unrecognized option" error, so the fixture never generated,
  the script printed "skipping", and two green tests sat there covering nothing.
  It now distinguishes an environment gap (no encoder at all, or a GPU encoder
  with no card) from a fault — if a *software* encoder is present and the
  fixture still fails to build, that is a `FATAL_ERROR`. **And the encoder
  parameters were wrong twice over** — `qp` alongside CRF mode is SVT-AV1's "bad
  parameter" — which the louder failure surfaced at once. A test that cannot
  fail is worse than no test, and this one briefly was.

  What this does *not* settle is the appliance tier: no Pi hardware decodes AV1,
  so an AV1 campus is dav1d in software, on the tier this project exists to keep
  cheap. Until 1080p30 AV1 is measured on a bench Pi, the docks keep labelling
  AV1 "experimental" and the relay's AV1 gate stays shut — where the blocker is
  the destination rather than the container, per the entry below.

- **HEVC reaches a streaming site now: the relay was refusing it for a
  constraint that had stopped being true.** The codec gate refused HEVC on any
  RTMP destination, on the stated grounds that "ffmpeg will happily mux HEVC
  into FLV and exit 0 (enhanced RTMP), producing a well-formed stream that the
  destination then rejects. Measured, not assumed." Both halves were wrong, and
  the second was wrong in an instructive way.

  **FLV carries HEVC**, through Enhanced RTMP: an `ExVideoTagHeader` with
  `isExVideoHeader` set, a `VideoPacketType` and a FourCC (`avc1`, `hvc1`,
  `av01`). It is a released specification — E-RTMP v2, contributors including
  Adobe, Google, Meta, Twitch, FFmpeg and OBS — and ffmpeg has written it since
  **6.1**, where the Changelog says so outright: "Support HEVC,VP9,AV1 codec in
  enhanced flv format". **YouTube takes it**: its encoder settings page lists
  H.264, H.265 and AV1 under RTMP/RTMPS, and recommends H.265 over RTMP(S) for
  HDR.

  So why did a measurement say otherwise? What was doing the measuring. This
  container ran **Debian bookworm's ffmpeg 5.1**, whose FLV muxer has no HEVC in
  its codec-tag table at all — it calls `unsupported_codec` and stops. A test
  from that image could not have produced an Enhanced RTMP stream to be
  accepted *or* rejected, so what got written down as a fact about destinations
  was a fact about our own ffmpeg. A toolchain limit and a destination limit
  look exactly alike when only one of them is in front of you, which is the part
  worth keeping.

  Changed: the container is **trixie** (ffmpeg **7.1**, which writes the tag);
  the gate allows HEVC on both protocols, with the reasoning and the byte-level
  evidence in the comment above it; AV1 stays refused, now for the honest reason
  — nothing but YouTube obviously takes it — instead of a wrong one about what
  ffmpeg can do; and an HEVC room's banner note is a caveat about the
  destination rather than an obstacle, because the event is no longer unsendable
  anywhere.

  Verified: a copy remux of HEVC+AAC through the relay's own argument vector
  (`-c copy`, `-flvflags no_duration_filesize`, `-f flv`) emits video tag byte 0
  = **0x90** — `isExVideoHeader=1`, `VideoFrameType.KeyFrame`,
  `VideoPacketType.SequenceStart` — then the FourCC `hvc1`, where the same run
  with H.264 emits the legacy `0x17`. ffprobe reads the result back as hevc +
  aac. **What was then unverified was the last mile — and AV1, on the same code
  path, has since settled it**: an AV1 event went from a real encoder through the
  relay to YouTube and played there for over ten minutes without a fault. HEVC
  has not been through that test itself, so its own last mile stays written down
  as unproven.

  Also corrected, because the old fact was load-bearing in nine other places:
  `PROJECT-SCOPE.md` §8.2 twice, §9's capability table, the Phase 7 text, and
  Phase 15's "dead end for the relay" conclusion — which rested on the video and
  now rests on FLAC, where it belonged all along. Plus `README.md`,
  `docs/STREAMING.md`, `docs/DEVELOPER.md`'s test table, `relay/README.md`,
  `site/docs.html`, `site/index.html`, and the **standing** Known gaps section
  in `.github/RELEASE-NOTES.md`, which is republished with every release rather
  than being history.

- **The TSan job found a race in the verify note — the one member of
  `RetryUploader` that was not already atomic.** It went red on `4998582`, a
  commit that touched nothing but markdown, which is how a flake that had been
  rolling dice for a while finally landed: `session`, 1 of 41, `thread` job only
  (the ASan/UBSan job and clang-tidy both passed, and the same job had passed on
  the commit before). The report was `operator delete` inside
  `RetryUploader::upload_one` (`retry_uploader.cpp:53`) writing against a
  `std::string::_M_assign` read by the main thread in `Session::status()` —
  reached from a poll loop at `tests/test_session.cpp:424`.

  **The cause was an accessor, not a missing lock in the obvious place.**
  `last_verify_note()` returned `const std::string&`, so `Session::status()`
  copied that buffer on the *caller's* thread. It did so under the Session's own
  mutex, which the upload thread never takes and which therefore synchronises
  nothing; the uploader meanwhile assigned the string at three points
  (`retry_uploader.cpp:44`, `:49`, `:52`) with no lock at all. Everything else in
  the class was already safe — every `UploaderStats` counter is a `std::atomic`,
  as is `m_health` — so one member was the entire gap.

  **It matters outside the test.** The note is logged by the encoder dock as
  `upload verified in bucket: …` or `UPLOAD VERIFICATION FAILED: …`
  (`src/obs/multisite_output.cpp:789-794`), and the window is the first
  `verify_first_n` (3) confirmations of *every* event — exactly the moment an
  operator is watching the dock. A torn read there is a garbled log line at
  best, and a read of memory the uploader has already freed at worst, which is
  undefined behaviour rather than a wrong sentence.

  **Fixed by giving the note its own lock and handing out a copy:**
  `m_note_mtx`, `set_verify_note()` as the only writer, and
  `last_verify_note()` returning `std::string` by value. Its own mutex rather
  than the Session's, because the reader already holds the Session's and the two
  must not invert — the note lock is never held across the confirm callbacks, so
  they cannot. Both callers wanted a copy anyway (`session.cpp:406` and
  `test_session.cpp:376`), so nothing else changed.

- **Back to stock in one command, and the way into the box left up.**
  `scripts/player/uninstall.sh` takes the player off a box; this adds `--stock`,
  which is the whole job asked once — the player, the AES67 stack under it, and
  the packages that existed only to build the two of them — and makes the
  ZeroTier guarantee explicit rather than implied. A box being returned to stock
  is usually one at the back of a hall that nobody is standing next to, cleaned
  up from the office *over ZeroTier*, so a run that keeps it now checks
  `zerotier-one` is enabled and running, starts it if something had stopped it,
  and prints the address and the joined networks the way the player's own
  identity screen reads them. The one combination that would clean the box and
  lock somebody out of it — `--stock` with `--purge-remote-access` — is refused
  instead of obeyed, and a ZeroTier that will not come up makes the run exit
  non-zero with that sentence rather than a quiet success.

- **The suite now runs under sanitizers, and the first run found a real data
  race in the HTTP server.** Nothing in this tree had ever been checked by a
  tool rather than by a test, while thirteen files here spawn threads — so a
  passing test that had raced, or scribbled past an allocation, read exactly
  like a passing test that had not. `MULTISITE_SANITIZE=thread` and
  `=address,undefined` build the same 41 tests under TSan and ASan/UBSan, and
  `.github/workflows/analysis.yml` runs both on every push, plus clang-tidy
  against a curated check list (`.clang-tidy`).

  **What it found:** `HttpServer::m_listen_fd` was a plain `long long`,
  written by `stop()` on the caller's thread while `accept_loop()` read it on
  its own — reported in three separate tests (`http_server`,
  `lan_object_server`, `lan_transport`), one root cause each time. Beyond the
  race itself, re-reading the handle mid-loop meant `accept()` could be called
  on a descriptor that had already been closed and reissued — and this process
  runs a *second* HttpServer (the encoder's LAN object server), whose sockets
  are exactly what that number would be reused for. Now atomic, cleared with
  an `exchange(-1)` before the close, and loaded once per iteration with a
  `break` when it has gone. The wake mechanism is untouched on purpose:
  whether `shutdown()` alone releases a blocked `accept()` differs between
  macOS and Linux, and that is not a thing to change without both in front of
  you.

  **What it found second, and worse — only on Linux.** The same suite under
  TSan on the Ubuntu runner reported thirteen more races across the same
  three tests, all one root cause: `~HttpServer()` destroying
  `m_conn_done` while a detached connection thread was still inside
  `pthread_cond_broadcast` on it. The connection lambda did its erase from
  `m_conn_fds` — the thing `drop_connections()` waits on, and therefore the
  thing that releases the caller to destroy the server — and *then* went on
  to close the socket, notify, and decrement `m_connections`, all against an
  object that was by then free to disappear. Unloading the plugin, or
  switching remote control off, while a phone still had the control page open
  is exactly that shape.

  Everything the thread touches now happens before the erase, with the erase
  and the notify together under one lock, so `drop_connections()` cannot
  return until the broadcast has finished. The socket close moved inside that
  same lock rather than after it, which keeps the original invariant (a
  handle `stop()` can still see is one that is still open) intact.

  Worth noting that macOS TSan reported none of these — the Mac run was clean
  both before and after. Two platforms in CI is doing real work here, not
  duplicating one answer.

  **Residual, deliberately:** connection threads are detached and
  `drop_connections()` waits a bounded five seconds, so a thread genuinely
  stuck past that still gets its object destroyed underneath it. Waiting
  forever would trade that for hanging OBS on shutdown — the failure this
  project has already chased once. The timeout now logs an error naming how
  many connections it gave up on, so if it ever happens it says so rather
  than being inferred later from a crash report.

  ASan and UBSan came back clean across all 41 tests on both platforms — no
  leaks, no use-after-free, no undefined behaviour on any covered path.

  **One false positive, and why it is handled the way it is.** `cmaf_decode`
  failed under TSan with a race between `av_mallocz` on FFmpeg's own
  `av:h264:df2` worker and our `av_frame_unref`. That handoff is FFmpeg's and
  it is correct — but it goes through an atomic refcount inside libavutil,
  and CI links the distro's prebuilt libavutil, which carries no sanitizer
  instrumentation. TSan therefore cannot see the refcount and reports every
  handoff as a race. The decoder now asks for one thread when, and only when,
  `MULTISITE_TSAN_BUILD` is set; shipping builds still decode on every core,
  which the Pi needs. A suppression for libavutil/libavcodec was the obvious
  alternative and is worse: TSan matches a suppression against *any* frame in
  either stack, so it would equally have hidden a genuine race between two of
  our own threads over an AVFrame — a mistake this code could actually make.

  **clang-tidy found no correctness bugs**, and that is worth writing down so
  nobody re-litigates it from the raw count. 88 findings, all read. The two
  families that looked most dangerous were false in every instance: the five
  `unchecked-optional-access` are one properly-guarded variable whose check
  sits ~60 lines above its use, past where the analysis gives up, and the
  five `implicit-widening-of-multiplication` are all compile-time constants
  (`256*1024`, `30*60*1000`) orders of magnitude below `INT_MAX`. The 14
  `empty-catch` are all the same deliberate pattern — parse something that
  may be garbage, fall back to a default — and notably *not* the family the
  End Broadcast bug belonged to, which was a discarded return value;
  `bugprone-unused-return-value` reported nothing. The 16 `concurrency-
  mt-unsafe` are all `strerror`, whose worst case is one thread printing
  another's error text, never wrong behaviour.

  So the check list stays advisory (`WarningsAsErrors: ''`). A 0-for-10 hit
  rate on the two checks most worth betting on is the argument for leaving it
  that way, not for turning it up. Known remaining nits, none urgent: the
  `HeaderFilterRegex` lets `src/appliance/../vendor/nlohmann/json.hpp`
  through on the unnormalised path, `strerror` wants a portable wrapper, and
  `http_server.cpp` turns a malformed `Content-Length` into 0 in silence
  where a 400 would be more honest.


- **Ending a broadcast published nothing at all — the fix for the shutdown
  hang switched the transport off one line before everything that still
  needed it.** Reported from a real 5h38m event on 2026-09-14, whose log ends:

  ```
  22:49:10.254: draining 1 queued segment(s) before stopping
  22:49:18.290: stopped: 5632 confirmed, 9 retries, 5633 segments muxed
  22:49:18.290: 1 segment(s) were still unsent when the drain deadline passed
  ```

  Exactly 8.036s between those lines — the whole drain deadline — and nine
  retries appearing only at the very end, after an event that logged `0
  retries` throughout. That shape is not a slow link, it is a transport
  refusing every request instantly and a retry loop backing off against it
  until the clock ran out.

  `9b75bfd` (2026-09-13, "Close three tracked bugs") gave
  `RetryUploader::stop()` a `m_transport.cancel_pending()` so joining its
  upload thread could not wait out a request timeout. Correct in itself. But
  `Session::end()` is `stop()` followed by three things that all go through
  that same transport — `drain_blocking()`, `manifest.json` as `ended`, and
  `live.json` as `ended` — and S3Transport's cancel is **sticky**, by design
  and by documented intent. So from 2026-09-13 every clean End Broadcast:
  uploaded nothing further, took the full 8 seconds not to, and **never marked
  the event ended in the bucket at all**. The lost segment is the small half.
  The large half is that satellites kept polling a room whose encoder had
  gone home, and would eventually classify a normal Sunday as `interrupted`.

  Three things had to be true for this to survive six days, and all three are
  now false:

  1. **`resume_pending()` was not on the `Transport` interface**, only on
     `S3Transport` and `LanTransport` as concrete methods — so `RetryUploader`
     and `Session`, which both hold a `Transport&`, could not have re-armed it
     even if they had known to. It is now a virtual on the interface, paired
     with `cancel_pending()` and documented as the obligation that comes with
     cancelling something you intend to keep using.
  2. **Nothing re-armed it.** `Session::end()` now resumes immediately after
     `stop()` has joined the thread (the one moment it is safe, with nothing
     in flight to lose its cancellation), and `Session::begin_common()` —
     the shared path behind `start_new()` and `resume()` — resumes before it
     publishes anything, so a transport inherited from a previous Session
     arrives usable. That second one was a live bug too: resuming a crashed
     event on a reused transport failed outright, which `test_session`'s
     tests 4 and 13 started failing to prove the moment the mock got honest.
  3. **The failure was silent.** `put_bytes()` recorded the reason and
     returned; `end()` discarded it and the operator's log signed off with a
     tidy `stopped: 5632 confirmed`. `end()` now returns whether the event
     was really marked ended, and `multisite_output.cpp` logs an ERROR naming
     the consequence when it was not.

  The test suite passed throughout all of this, which is the part worth
  keeping: `MemStore`, the mock every session test runs against, inherited
  the interface's no-op `cancel_pending()` and so was strictly more forgiving
  than the real transport. It now models sticky cancellation the way
  S3Transport actually behaves, and `test_session`'s new test 18 ends a
  session with a segment still spooled and checks all four things that
  matter: the segment lands, `live.json` says ended, the manifest agrees, and
  the transport is left usable. Reverting either fix fails it.

  **Not explained:** the same log shows a further ~7.8s between the last
  `stopped:` line and the dock's next message. It may be nothing more than
  the dock's own refresh timer, and nothing in the stop path accounts for it.
  Worth watching on the next event rather than guessing at now.

- **The OBS plugin's own remote-control pages (§8.4) now match the "standard"
  every other web surface in this project had already converged on** —
  collapsible `<details>`/`<summary>` settings sections, the storage
  provider dropdown (§8.6), and the encoder's LAN/cloud-disable settings
  (§8.7). Both `data/web/encoder/` and `data/web/decoder/` had none of the
  three: they still had the original six raw storage fields with no
  provider-aware show/hide, no collapsible sections at all, and — since
  these pages predate LAN delivery entirely — no `lan_host`/`lan_port`/
  `lan_auth_token` fields and no way to disable cloud storage, even though
  `BroadcastSettings` and `DecoderSettings` have carried those fields for a
  while now. `src/obs/web/commands.cpp`'s `apply_encoder_settings()` and
  `apply_decoder_settings()` gained the identical provider-derivation block
  the appliance's and the relay's `apply_edit()` already run (same
  `storage_providers.h` table, same "only act on it if the page actually
  sent one" fallback for an older client); `encoder_settings_json()` and
  `decoder_settings_json()` resolve an empty `storage_provider` via
  `detect_provider()` the same way. A `GET /api/storage/providers` route
  lives in `web_ui.cpp`, registered once outside the encoder/decoder role
  split since the list itself is role-agnostic — registering it inside both
  `register_encoder_api()`/`register_decoder_api()` would double-register it
  on a machine running both.

  The "disable cloud storage" checkbox is the one field that could not go
  through the generic name-matched load/save loop both pages already used:
  it reads "disable", the setting reads "enable", so it is the one name
  (`cloud_enabled_off`) excluded from that loop and translated by hand in
  both directions, with the confirmation dialog firing on check rather than
  on Save — same wording as the dock's, same "cloud off with LAN also off"
  safety net applied server-side in `apply_encoder_settings()`.

  Verified by building the real plugin (`obs-multisite.plugin`) against a
  full OBS Studio checkout and confirming the settings JSON compiles/loads
  correctly, and by serving the actual `data/web/encoder`/`decoder` pages
  statically to check the collapsible sections, the provider dropdown's
  hide/show, and the LAN section all render as designed — no live OBS
  instance was driven end-to-end this pass, so the settings round-trip
  through a running plugin is still unverified.

- **LAN delivery, cloud-disable and the storage provider dropdown now reach
  the simulcast relay too** — the same three §8.6/§8.7 capabilities already
  built for the OBS decoder and the Raspberry Pi appliance, ported to
  `relay/`. The relay reads its live feed through `RoomFeeder`, which was a
  concrete `S3Transport` wrapped around `DecoderSession`; it is now the same
  three-way `S3Transport`/`LanTransport`/`FallbackTransport` construction the
  appliance's `Player::rebuild_session()` already does, keyed off whichever
  of cloud or LAN (or both) `ConfigStore` reports configured. `ConfigStore`
  gained `LanConfig` (`lan_host`/`lan_port`/`lan_auth_token`, stored in the
  existing generic `settings` key/value table — no schema migration needed)
  and `storage_provider`, plus `configured()` as the combined gate that
  replaces `storage_configured()` everywhere a LAN-only relay needs to be
  treated as set up.

  One thing does *not* follow LAN: past-event browsing, download and
  rebroadcast. All three are `list()`-based (`RoomFeeder::events()`/
  `event_parts()`), and `LanObjectServer` only ever holds the event currently
  in progress — the identical limitation the appliance and the OBS decoder
  already have, for the identical reason. `RoomFeeder` now holds `m_transport`
  (cloud, nullable), `m_lan_transport` and `m_fallback_transport` separately
  from `m_active` (whichever of the three the live feed and `event.json`
  fetch actually use), so a LAN-only relay simply has no `m_transport` and
  those three methods say so rather than crashing or returning nonsense.
  `check_storage()` (the Settings page's "Test storage" button) tests
  whichever path is actually configured — `S3Transport::self_test()` when
  there's cloud, a real `live.json` GET checked against
  `LanTransport::last_request_reached_server()` when there's only LAN.

  `/api/status`'s `storage_configured` field is renamed `configured` (now
  cloud-or-LAN) and gained `lan_configured`/`lan_active`, mirroring the
  appliance's `/api/storage` shape — `lan_active` is "is the LAN path
  healthy right now", not "did the last request come from LAN" (see
  `FallbackTransport::last_get_was_primary()`'s doc comment; a 404 for an
  ordinary miss must not read as LAN being down). `RELAY_LAN_HOST`/
  `RELAY_LAN_PORT`/`RELAY_LAN_AUTH_TOKEN` seed the settings on first run,
  matching every other `RELAY_*` variable's "seeds once, the database wins
  after" rule.

  Verified live against a real `multisite-relay` process: booted LAN-only
  against an unreachable test address (`configured: true`, `lan_active:
  false`, `check_storage` reporting "Could not reach the encoder..."),
  switched to AWS via the provider dropdown (`region` in, `s3.us-east-1
  .amazonaws.com` derived out, a real credentials-redirect back from AWS
  proving `self_test()` actually reached the network), confirmed both paths
  configured together, then cleared cloud back to LAN-only and watched
  `/api/events` correctly go empty and the "Test storage" error return. The
  settings page's provider dropdown and its hideable fields, and the
  Sending tab's new "via LAN" / "via cloud (LAN unreachable)" line, were
  checked in the browser against the same running instance.

- **The appliance's settings page collapses into sections now** — Room
  and storage, LAN, buffering, Picture, Sound, Remote access, On power-up,
  Network audio output — instead of one long flat scroll of every field at
  once, which is what all the storage/LAN work above had grown it into.
  Reuses the exact `<details>`/`<summary>` pattern already on the same page
  for Preview picture and Sound meters — no JavaScript, the browser's own
  disclosure widget, so it needed a CSS rule (`details.settings-section >
  summary`, styled to match the existing section headings) and nothing else.
  "Where the event comes from" opens by default since it's what a first-time
  setup actually needs; the rest start closed. Pure `web/index.html` +
  `web/style.css` — no C++, no rebuild needed on a box that already has the
  binary, just updated files.

- **The storage provider dropdown (§8.6, Phase 13) now reaches the Raspberry
  Pi appliance's web settings page too.** Spotted immediately after the LAN
  work above landed there: the appliance never got Phase 13 at all — its web
  page still had the original six raw fields with no provider-aware show/hide,
  because that phase was scoped to "both docks" back when the appliance had
  no reason to touch storage config beyond typing it in once. Ported using the
  exact same `src/core/storage_providers.h/.cpp` table the two OBS docks
  already read: `Config` gained `storage_provider`; a new
  `GET /api/storage/providers` route lists the choices (mirrors `/api/outputs`
  listing hardware choices, rather than folding a static list into
  `/api/config`); `apply_edit()` derives `endpoint_host`/`r2_account_id`/
  `region` from whichever one field the operator typed, the identical logic
  `DecoderDock::onSaveSettings()` runs, just building a `Config` instead of
  setting `S3Config` members; `config_json()` resolves an empty
  `storage_provider` via `detect_provider()` the same way `loadIntoFields()`
  does, so an upgrade never misrepresents an existing AWS/Backblaze/Wasabi/
  Custom setup as something it isn't. `web/index.html` gained the dropdown
  and three now-hideable fields; `web/app.js` fetches the provider list once
  and shows/hides them the same way `updateProviderFields()` does in Qt.

  Verified live against a real `multisite-player` process: fetched the
  provider list, then round-tripped all three shapes through a real
  `PUT /api/config` — R2 (account id in, blank endpoint + `region: auto`
  out), AWS (region in, `s3.eu-west-2.amazonaws.com` derived out, a stale
  account id from a previous save correctly cleared), and Custom (both raw
  fields preserved exactly) — all three came back exactly as the OBS docks'
  own derivation would produce them.

- **LAN / direct delivery and cloud-disable now reach the Raspberry Pi
  appliance too, not just the OBS decoder plugin (PROJECT-SCOPE.md §8.7).**
  Ported the exact wiring from `multisite_source.cpp` and `decoder_settings.h`
  onto `src/appliance/config.h/.cpp` (three new fields, `configured()` split
  into `cloud_configured()`/`lan_configured()`, same as the OBS side) and
  `player.h/.cpp` (`Player::rebuild_session()` builds the same
  LAN/cloud/fallback choice; `m_transport` stayed a concrete `S3Transport`
  specifically for `storage_health()`'s cloud-only figures — colo, server,
  probe — none of which mean anything for a LAN leg). The web settings page
  (`web/index.html`, `web/app.js`, `/api/config` in `api.cpp`) gained the
  matching host/port/token fields, with the same "dots mean unchanged" secret
  convention the bucket key already used.

  **A second copy of the same indicator bug, caught before it shipped.**
  Building this surfaced that `lan_active` in a LAN-only configuration (no
  `FallbackTransport` at all, since there's no cloud leg to fall back
  between) was hard-coded `true` whenever a LAN host was merely *configured*
  — regardless of whether it actually answered. Live-testing the appliance
  caught it immediately: pointed a real `multisite-player` at an unreachable
  port and it happily reported `lan_active: true`. Fixed by asking
  `LanTransport::last_request_reached_server()` directly when there's no
  `FallbackTransport` to ask instead — and the identical bug existed in
  `multisite_source.cpp`'s decoder snapshot too (LAN-only there has the same
  shape), fixed there in the same commit.

  Verified with a real `multisite-player` process (`--no-display`) against
  first an unreachable port (`lan_active` correctly `false`, no crash, clean
  "room is offline") and then a real OBS encoder with LAN on and cloud off
  (`lan_active` correctly `true`, `room_state` LIVE, real segments cached) —
  an entire event received with no bucket involved at any point, across two
  independent codebases talking the same protocol.

- **Four issues from using the cloud-disable/LAN work above, all fixed.**

  1. **The decoder dock's "via LAN"/"via cloud" indicator really could read
     wrong**, not just as a live-testing artifact. `FallbackTransport` used
     to track "did the very last `get()` happen to come from LAN" — but a
     LAN 404 for a key that's an ordinary, expected miss (a segment that
     aged out of the retention window, `markers.json` before the first
     marker) would flip it to "cloud" even with LAN working perfectly.
     `Transport` gained `last_request_reached_server()` (default `true`;
     `LanTransport` already tracked exactly this) so a miss can be told
     apart from a connection failure — `FallbackTransport`'s health flag now
     only changes on the latter. Verified live: polled `decoder/status`
     every 3s for 24s straight during a cloud-disabled broadcast;
     `lan_active` held `true` throughout, where it had flickered before.
  2. **The encoder dock's LAN status line was widening the whole dock** —
     appending "(cloud upload disabled — LAN only)" to a single-line stat
     cell in a compact grid. Moved to the label's tooltip instead of its
     text.
  3. **The cloud-upload checkbox was in the LAN group box.** It says what
     happens to cloud storage, so it now lives in the Storage box with the
     rest of the cloud settings — `updateLanFields()` still shows/hides it
     based on whether LAN is on, just reaching into a different box's layout
     to do it. Relabelled and inverted while moving it, too: it read "Also
     upload to cloud storage" (checked = enabled), which undersold that
     cloud is the normal, default path and this is the exception. It's now
     "Disable cloud storage (LAN only)" (checked = disabled), `m_disableCloud`
     in the code — `onSaveSettings()` and `loadIntoFields()` negate it at the
     one point each where it crosses into `BroadcastSettings::cloud_enabled`,
     which keeps its own sense (true = enabled) unchanged everywhere else.
  4. **Disabling cloud upload now asks for confirmation** (re-enabling
     doesn't — that's the safe direction). `EncoderDock::onDisableCloudToggled`
     intercepts the checkbox instead of saving directly; declining reverts it
     without saving.

- **LAN / direct delivery is now built end to end, and cloud upload can be
  turned off entirely (PROJECT-SCOPE.md §8.7, Phase 14).** The encoder half
  landed in an earlier pass; this pass built the decoder's matching client
  (`LanTransport`), the LAN-preferred/cloud-fallback logic
  (`FallbackTransport`), and — since an operator asked for it directly — a
  way to disable cloud delivery altogether for a LAN-only setup
  (`NullTransport`).

  `LanTransport` (`src/core/lan_transport.h/.cpp`) is a plain HTTP client
  against `LanObjectServer`'s own routes, with a short connect timeout (LAN
  should fail fast, not hold a decoder's poll loop hostage waiting out a
  cloud-sized timeout) and the same cancel-on-teardown discipline
  `S3Transport` already has, for the same reason: a decoder source torn down
  mid-request must not freeze OBS's UI thread waiting one out.
  `FallbackTransport` (`src/core/fallback_transport.h`) wraps a LAN and a
  cloud `Transport&` and tries LAN first on every `get()`, falling through to
  cloud only for requests LAN didn't answer — deliberately per-request
  rather than a session-wide health-tracked mode, so a segment that aged out
  of the LAN retention window falls back for that segment alone, and the
  very next request tries LAN again rather than staying "stuck" on cloud.
  `DecoderSettings` gained `lan_host`/`lan_port`/`lan_auth_token`, and its
  `configured()` gate now accepts LAN alone — a satellite with no cloud
  credentials at all now works, which needed `Session` to gain a fourth hook
  (`set_live_published_callback`) so a LAN-only encoder's `live.json` reaches
  `LanObjectServer` too: without it, a LAN-only satellite following the room
  (rather than a pinned past event) had no way to discover which event is
  live, since none of the other three hooks cover that object.

  **A real gap found live, not just a cosmetic one:** validating this
  end-to-end (cloud disabled, decoder pointed at the encoder's LAN port on
  the same machine) turned up that `markers.json` was never wired into LAN
  delivery at all — `add_marker()` published it straight to the bucket and
  nowhere else. With cloud able to be off entirely now, that meant a marker
  dropped mid-event genuinely never reached a LAN-only satellite; it wasn't
  only that the decoder dock's "via LAN"/"via cloud" indicator flickered to
  "cloud" every time the 5-second markers poll ran (which is how it was
  actually noticed — the indicator kept reporting cloud even though every
  segment was demonstrably arriving over LAN). Fixed with a fifth hook,
  `set_markers_published_callback`, and a matching `LanObjectServer` route —
  the exact same shape as `live.json` got. Re-verified live: dropped a real
  marker mid-broadcast with cloud disabled, confirmed it reached the decoder
  (`curl` against the LAN port directly, and the decoder's own vendor-API
  status showing the marker), and confirmed the active-path indicator then
  read "via LAN" consistently rather than flickering.

  `NullTransport` (`src/core/null_transport.h`) is what "cloud delivery
  disabled" actually is: handed to `Session` in place of `S3Transport`, every
  PUT reports instant success, so the entire spool → retry-uploader →
  manifest pipeline runs completely unmodified — segments confirm
  immediately, the LAN hooks fire on schedule — and `Session` never learns
  cloud is off. The encoder dock's checkbox for it only appears once LAN
  delivery is turned on, and both `BroadcastController::go_live()` and
  `multisite_output.cpp`'s own `out_start()` independently refuse to go live
  with both cloud and LAN off (nothing would be delivered anywhere).

  The decoder dock's storage-link line now says which path the most recent
  fetch actually took ("via LAN" / "via cloud") once a LAN host is
  configured — the "Visibility" item §8.7 listed as not built.

  `LanTransport`, `FallbackTransport` and `NullTransport` are all proven over
  real loopback sockets or in-memory mocks (`tests/test_lan_transport.cpp`,
  `tests/test_fallback_transport.cpp`, `tests/test_null_transport.cpp`, plus
  new cases in `test_session.cpp` and `test_lan_object_server.cpp` covering
  the markers fix above) — 41 test binaries, all green.

- **End Broadcast could hang OBS's main thread indefinitely, with no crash
  report to show for it.** Found while live-testing the LAN wiring below,
  once Go-Live and End were actually exercised back to back rather than in
  isolation: `Session::end()` calls `RetryUploader::drain_blocking(deadline)`
  on OBS's UI thread (via `obs_output_stop`), and its own doc comment
  promises to stop "until the spool is empty or `deadline` passes" — but
  `upload_one()`'s internal retry loop never looked at that deadline, only
  at `m_running`. With `max_attempts = 0` (retry forever, the production
  default) a single segment stuck retrying could hold the drain, and the
  main thread with it, open-ended. A `sample` of the hung process during
  testing caught it red-handed: the UI thread parked inside
  `RetryUploader::upload_one`'s backoff `sleep_for`. Almost certainly the
  explanation for an OBS process that vanished mid-test earlier in this
  same pass with no crash report at all — a hang long enough eventually
  looks like a dead process from the outside, not a crash.

  Fixed in `src/core/retry_uploader.h/.cpp`: `upload_one()` now takes an
  optional deadline, checked before each attempt and inside the backoff
  sleep's slicing loop; `drain_blocking()` passes its own deadline through
  instead of only checking it between segments. A segment still unsent when
  the deadline passes is left in the spool exactly as a crash would leave
  it — picked up and retried on the next resume (§5.1) — logged rather than
  silently dropped. Re-verified live: two full Go-Live → upload → LAN-curl →
  End Broadcast cycles, both draining cleanly inside the deadline with OBS
  staying alive throughout. The default was also lowered from 30s to 8s
  while in this code: a healthy link uploads a segment in low single digits
  of seconds (per the encoder's own logs), so 30s of a frozen UI thread
  waiting on a stuck one bought little beyond making a bounded wait feel
  like a hang.

- **LAN / direct delivery, encoder half only — the decoder half is not built
  yet.** Per `PROJECT-SCOPE.md` §8.7 (Phase 14): a satellite on the same
  network or an existing VPN will eventually download straight from the
  encoder instead of the bucket, automatically, falling back to cloud the
  instant that path isn't healthy. This pass built the encoder's side of
  that — the half that can be proven without a decoder-side transport to
  pair it with.

  `HttpServer` (`src/core/http_server.h`) gained `route_prefix()`: a
  "starts with" route, matched longest-prefix-first, needed because a
  segment's path names a sequence number — there is no way to pre-register
  one exact route per possible value. A new `LanObjectServer`
  (`src/core/lan_object_server.h/.cpp`) uses it to serve
  `manifest.json`/`event.json`/`init.mp4`/`segments/{seq}.m4s` — the
  identical object shape a cloud decoder already reads — with an optional
  bearer-token check.

  **A real design correction, found while building it, not just anticipated
  in advance:** the plan was to serve straight from the durable spool. That
  cannot work — the spool's entire job is to delete a segment the moment the
  bucket confirms it (see `spool_queue.h`), so the segment a LAN decoder is
  actually most likely to want — recent, ordinary, already-confirmed
  programme — is by design the one the spool no longer has. `LanObjectServer`
  keeps its own bounded retention window instead, reusing the exact
  `SegmentCache` class a decoder already uses for its own cache. `Session`
  gained three narrow, optional hooks (`set_event_started_callback`,
  `set_segment_confirmed_callback`, `set_manifest_published_callback`),
  fired at moments `begin_common()`/`on_confirmed()`/
  `publish_manifest_locked()` already have the relevant bytes or JSON in
  hand — cost nothing when unset, and `Session` never learns LAN delivery
  exists.

  Proven end to end over a real loopback socket
  (`tests/test_lan_object_server.cpp`): bootstrapping an event, segments
  becoming servable the instant they confirm (not after a bucket round
  trip), the retention cap actually evicting the oldest segment, a new event
  discarding the previous one's window, and the auth token being enforced
  once configured. Also new: `tests/test_session.cpp` proves the three hooks
  fire with the right data (38 tests total pass).

  **Explicitly not built, and not claimed to be:** the decoder side (a
  `LanTransport` implementing the existing `Transport` interface),
  discovery, automatic LAN/cloud preference and fallback, and the decoder
  dock's active-path indicator. `PROJECT-SCOPE.md` §8.7 is marked
  accordingly rather than as done.

- **A storage provider dropdown replaces six blank fields with only the ones
  each provider actually needs.** Built per `PROJECT-SCOPE.md` §8.6 (Phase
  13). Choosing Cloudflare R2, AWS S3, Backblaze B2, Wasabi or Custom shows
  only that provider's fields — an account id for R2, a region for the other
  three (the hostname is derived from it), both endpoint and region for
  Custom — instead of an operator having to know which of six fields their
  provider needs and what shape its hostname convention takes.

  `S3Config` itself did not change: the derivation lives in front of it,
  entirely in a new `src/core/storage_providers.h/.cpp` (fully unit-tested,
  no Qt, no OBS — `tests/test_storage_providers.cpp`), used identically by
  both docks. A configuration saved before this existed reads back as
  whichever provider its endpoint actually matches (`detect_provider()`), or
  Custom if none does — never misrepresented as something it isn't. Also the
  seam Phase 12's brokered "Multisite Cloud" option will slot into later: it
  is already a greyed-out entry in the same dropdown, not a second settings
  surface waiting to be built.

  One deliberate deviation from the original design write-up, corrected in
  place there: the provider table was designed as a bundled
  `data/providers.json`, editable without a rebuild. Built instead as a
  compiled table — these five providers' hostname conventions are a
  technical fact that essentially never changes, not operator-facing content
  worth the file-loading machinery. See §8.6 for the reasoning.

  Qt-layer wiring (the dropdown itself, show/hide per field, both docks) is
  unverified locally for the usual reason — no libobs/Qt6 SDK on this
  machine — but matches the `updateAudioFields()`/`QFormLayout::setRowVisible`
  pattern already used elsewhere in both docks exactly.

- **Resuming an interrupted event now asks, instead of always deciding
  silently.** Go Live used to call `Session::check_resumable()` and, if it
  found an unfinished event on disk, resume it unconditionally — no prompt,
  every time, whether the crash was thirty seconds old or three weeks old.
  Right default for the first case, wrong for the second: nothing on screen
  would have said today's broadcast had silently continued last month's
  leftover event, and choosing "start new" (had it existed) would have
  deleted whatever the old event hadn't finished uploading, with no warning.

  Built per the design in `PROJECT-SCOPE.md` §5.1: `SpoolState` now tracks
  `last_activity_ms`, and `SessionConfig::resume_stale_after_ms` (default 30
  minutes) draws the line — the same shape as the decoder's own
  `stale_after_ms`. Below it, nothing changes: silent auto-resume, no click,
  same as always, except the dock now shows a persistent *"Resumed event
  from HH:MM, N segments already confirmed"* line with an *"End this and
  start fresh"* escape hatch, rather than nothing at all. Above it, Go Live
  shows a dialog instead of guessing, naming by number what "start new" would
  abandon. A new free function, `peek_resumable()`, answers the staleness
  question from the spool directory alone — no `Session`, no `Transport` —
  because the dock has to decide before Go Live creates anything, and a
  deferred-start encoder (VideoToolbox and friends) may not construct its
  `Session` until well after the click. Core logic covered by
  `test_reliability.cpp`, `test_session.cpp` (staleness, the resumed-status
  fields, `peek_resumable()` — 36 tests total pass); the dock's dialog and
  status line are Qt/OBS-layer and unverified locally for the usual reason —
  no libobs/Qt6 SDK on this machine.

- **Three tracked bugs closed in one pass: the encoder's shutdown hang, the
  Windows install path, and the orphaned decoder cache directory.**

  **The encoder had the same shutdown hazard the decoder used to.**
  `RetryUploader::stop()` joined its upload thread the same way
  `stop_workers()` used to on the decoder — through a blocking
  `Transport::put()` call with a ~30-second timeout and nothing to cancel it,
  so tearing down while an upload was in flight could hang for as long as
  that request had left. Fixed the same way: `Transport::cancel_pending()` is
  now a virtual on the abstract interface (a no-op default, so the mock
  transports every test runs against need no real behaviour), overridden by
  `S3Transport`, and called from `RetryUploader::stop()` before
  `m_thread.join()`. `tests/test_retry_cancel.cpp` proves it with a mock that
  blocks forever until cancelled: `stop()` now returns in single-digit
  milliseconds instead of running out the clock.

  **The Windows build and the operator guide both used to point at a
  location OBS has said it will stop reading** — files merged into
  `C:\Program Files\obs-studio\`. Both now use the layout OBS's plugins guide
  recommends: one self-contained `obs-multisite\` directory, holding
  `bin\64bit\` and `data\`, copied into `C:\ProgramData\obs-studio\plugins\`
  (`.github/workflows/obs-plugin.yml`, `docs/OPERATOR.md`, `QUICKSTART.md`).
  `OPERATOR.md` carries a migration note for anyone installed the old way.
  This was also the actual prerequisite blocking Phase 11 (keeping
  installations current) — noted in `PROJECT-SCOPE.md` and `README.md`.

  **A crash mid-event used to leave the decoder cache's whole directory
  behind, forever.** A clean event switch has always deleted the directory
  being switched *away from*; nothing ever revisited the cache root looking
  for one abandoned by a process that never got the chance to switch away —
  a crash, a force-kill, power loss. `SegmentCache::set_event()` now sweeps
  every OTHER subdirectory of the cache root, not just the specific one being
  left, so however many of these have piled up get cleaned on the next event
  switch — including the very first switch away from the `"pending"`
  placeholder every fresh `DecoderSession` starts with. `tests/test_segment_cache.cpp`
  proves the sweep, and separately proves it does NOT touch the directory
  actually being switched into — resuming into the same still-live event
  after a restart keeps whatever was already banked, which is the entire
  point of having a durable cache.

  Left open, deliberately: the Pi playback stall (still needs a live thread
  dump to diagnose — the status line now at least says so unmistakably when
  it happens, see entry 0 above) and PTP lock accuracy at a receiver (AES67
  lip-sync itself is now settled — see entry 1 above — but this needs a
  receiving console's own read over a multi-hour event, not something
  fixable in code).

- **The silence that the idle keep-alive writes was not silence, and Stop stopped
  nothing.** Both found on `rpi5-nathan` within a minute of playing an event, and
  both worth writing down because neither looked like what it was.

  **The noise** was an out-of-bounds read, and the giveaway was that it was
  *loud*: the keep-alive thread that holds the sound card up while nothing is
  playing feeds it a buffer of zeros, and on this box that buffer was sized from
  the card's **period** while the write made from it was a 20 ms **cushion**. The
  RAVENNA card's period is one millisecond, so the buffer held 1,536 bytes and the
  top-up asked ALSA to play 30,720 — 29,184 bytes of heap, read as IEEE-754 floats.
  Arbitrary bytes read as float are almost never small, so a function whose entire
  job is to be inaudible was instead putting digital hash at 0 dBFS onto the AES67
  stream every time the box went idle. The two numbers are now decided in one
  place (`src/appliance/idle_keepalive.h`), the buffer is sized for the largest
  write that can be made from it, and `keep_fed()` refuses any write longer than
  the buffer it holds — a future drift is a missed top-up rather than a read past
  the end. `tests/test_idle_keepalive.cpp` checks the property that actually
  matters (the buffer is never shorter than the write, for any geometry a driver
  might grant) and carries the geometry that found it as a row in its table.

  **Stop** cleared the delivery queue and set the playing flag, but nothing in the
  delivery path ever consulted that flag: `deliver_loop()` gated on Hold alone, and
  `enqueue()` did not check either one. So the queue was refilled by the decoder as
  fast as it was cleared and played out anyway — a stop that stopped the head and
  nothing else. It was invisible in the log because the status line derived its
  state from the *session*, which has no idea the operator pressed anything, so a
  stopped box went on reporting `playing` while the frames kept flowing. Delivery
  now refuses to dispatch unless the box is playing, `enqueue()` drops decoded
  frames rather than holding them for the next play, and the status line reports
  the operator's state first. Hold deliberately keeps its queue (Continue resumes
  in place); Stop deliberately discards it.

- **The encoder's local spool is now capped, and a low disk warns before it
  matters.** Auditing cache/spool lifecycle across the encoder, decoder and
  Pi appliance found one real gap: `RetryUploader` retries forever by design
  (`max_attempts = 0`), and the durable spool it drains from had no size cap
  at all — a long or badly degraded upload link filled the encoder machine's
  disk with unconfirmed segments, unbounded. (The decoder/Pi side was already
  fine — `keep_behind_segments` + `max_cached_segments` bound it on two
  independent axes; see entry 4 above for the one gap found there.)

  `SessionConfig::max_spool_bytes` (default 4 GiB, 0 = old unlimited
  behaviour) now bounds it: `SpoolQueue::enqueue()` drops the OLDEST
  unconfirmed segment when over the cap — never the one just written, so
  progress never stalls — and advances a floor (`SpoolQueue::floor()` /
  `Manifest::first_available_seq`) past whatever it drops. That floor is what
  keeps this safe: the decoder's playback loop had a deliberate rule to hold
  position on a missing segment rather than ever skip one silently
  (`decoder_session.cpp`, "nothing is silently dropped from the programme"),
  and a naive drop-oldest would have made a viewer or a Pi player stall on a
  dropped segment forever. `next_segment()` now treats a head below the
  encoder's declared floor as a directive to jump forward (raising a
  discontinuity, same as a seek) rather than as an ordinary gap — the two
  cases are counted separately (`Stats::gap_skips` vs. `gaps_waited`). A
  second race this exposed: the uploader could already be mid-retry on
  exactly the segment about to be evicted; `RetryUploader::upload_one` now
  checks `SpoolQueue::floor()` on every attempt so a stale retry can't
  resurrect a dropped segment into the manifest after the fact.

  A new `src/core/disk_health.h` (pure threshold arithmetic, no syscalls —
  same split as `storage_health.h` for the network) backs a live low-disk
  reading on both ends: the encoder dock's new "Local disk" row (checked
  whether idle or live, so it is visible before Go Live) and the Pi's `This
  box` page / operator-interface warning banner, using the free-space number
  the appliance already had (`sysinfo.cpp`'s `statvfs` call) rather than a new
  syscall. Tests: `tests/test_disk_health.cpp`, plus new cases in
  `test_reliability.cpp`, `test_session.cpp` and `test_decoder.cpp` (34
  tests total pass). Not verified: the Qt/OBS dock UI compiles by inspection
  and matches the existing `m_link` row's pattern exactly, but was not
  built — this machine has no libobs/Qt6 SDK, and standing one up needs the
  obs-deps download described in `docs/DEVELOPER.md`.

- **The sound card is opened at the right width, stays open, and is metered.**
  Three faults that all presented as a silent room, and none of which said so.
  **The width** was the worst: a box with the network audio output on opened its
  card with the two-channel fallback before any feed had arrived, and "follow the
  feed" then latched that two-channel stream against an eight-channel feed — every
  listener heard channels 0 and 1 of eight for the life of the process, with
  nothing logged, because nothing had failed. The width is now decided in one
  place (`src/appliance/audio_plan.h`), and with nothing to go on the answer is
  "not yet" rather than a guess. On the network it is the width being published,
  so the card and the stream cannot disagree. **The mute** closed the card, which
  on an AES67 box takes the stream off the air — receivers dropped it and
  un-muting did not bring it back until they re-subscribed. Muting now writes
  silence to a card that stays open. **Nothing kept an enabled stream up**: a
  stopped daemon, a card the sound had drifted off, a source the daemon had
  forgotten, and the switch did none of it again. There is now a reconcile pass on
  its own thread, sharing a lock with the interface's switch so a stream somebody
  deliberately switched off cannot come back on air by itself. All three are
  invisible from the feed, so the new meters are tapped where the samples are
  handed to the card rather than where they arrive: the bars fall for a mute, a
  card that would not open and a silent event alike, and the reason under them
  names which — in the same words the status readout uses, so the two cannot
  disagree. The panel is drawn at the card's width, not the feed's. Status gains
  the audio state as a word (closed / open / failed) and the card's own message.
  The arithmetic lives in headers with no ALSA in them, so it is checked on a
  laptop with no card: `tests/test_audio_plan.cpp`, `tests/test_audio_levels.cpp`.

- **Phase 10, tiles: one composited feed, several pictures.** A room that
  composites two or four cameras into one feed used to have that composite cut
  apart again by hand at every satellite, with crop filters, every time. The main
  site now says how it composited (**Settings → Media → Pictures in this feed**:
  `1x1`, `2x1`, `1x2`, `2x2`), and the layout travels in `event.json` as an
  optional field written only when it is split — so an event from before this
  existed parses as one picture and nothing has to migrate
  (`TileLayout::parse`, `src/core/model.cpp`). It is declared rather than
  detected: a 3840×1080 frame is a legitimate ultrawide picture as well as a
  plausible pair, and guessing wrong cuts a programme in half.

  At the satellite each region is its own source — `multisite_tile_source`,
  "Multisite Picture (Decoder)" — which attaches to whichever Multisite Source
  is following the same room and receives a view of frames that source has
  already decoded, so **one download and one decode, however many regions are
  taken out of it**. The fan-out hands each registered tile a frame whose plane
  pointers are offset and whose strides are the original ones, which is why a
  tile costs no copy and no allocation on a path that runs thirty times a second;
  every edge rounds down to an even number, because the frame is I420 and an odd
  offset has no chroma sample to start from. A tile carries no audio of its own,
  deliberately: the programme audio belongs to the feed, not to one quarter of
  it. Each tile source has a **Send to screen** setting that opens one of OBS's
  own fullscreen projectors, which is how a region reaches a second monitor or a
  DeckLink without this code knowing what either of those is — the "assigned
  outputs" half of the phase, on the OBS side. A build with no frontend API keeps
  the setting and warns instead of projecting (`MULTISITE_HAVE_FRONTEND_API`),
  and several outputs from one appliance box needs hardware beyond this project.

  The appliance got the single-screen half: a **Composited feed** setting
  (`tile_index`, `-1` whole picture, `0..3` in reading order) crops in the
  present path as a non-owning view, with the layout re-read from the manifest on
  every poll — so a room that sends one camera this week and four next week needs
  nothing reconfigured at the satellite. An absent, `1x1` or out-of-range tile is
  the whole picture, which is recoverable by hand where a wrongly cropped one is
  not obviously wrong at all. The preview then had to catch up, because with a
  tile selected the screen and the preview were two different pictures: it now
  offers *what's going out* and *the whole feed*, each with its own
  last-good-JPEG cache, and the layout and selection are read under the same lock
  as the frame so the two are always the same instant.

  Every way of getting this geometry wrong is quiet — a rectangle a pixel out is
  still a picture, and one that took chroma at luma resolution is still a picture
  in the wrong colours — so it is pinned by `tests/test_tile_layout.cpp` (reading
  order, even edges, out-of-range gives the whole frame) and
  `tests/test_tile_crop.cpp` (plane offsets and strides; every luma and chroma
  sample of a copied tile checked against a position-dependent pattern). Commits
  `59a56b8`, `faf1de5`, `c1a277d`, `685a0cd`; shipped in `v0.1.15-alpha` and
  `v0.1.16-alpha`.

- **The player configures and switches the AES67 stream.** The stream used to be
  something an operator built by hand in the daemon's own interface. Now the
  installer creates it and the player owns it: **Settings → Network audio output**
  for the switch, the multicast address and the channel count, and **This box →
  Network audio output** for what is actually being sent — the daemon's state, the
  clock and the grandmaster it locked to, the address and port on the wire, and
  the SDP it publishes. The readout names the four faults that are otherwise
  indistinguishable from a settings page: a clock that is not locked, a stream
  switched off, a card not registered, and the player still writing the sound to
  HDMI. Reading is never behind Lock; changing it is. The daemon's own WebUI on
  8081 stays for the rest of what the daemon can do.

- **The licensed virtual sound card is gone, and a script takes it off a box.**
  The player no longer detects that card by name or writes into its daemon's
  memory: nothing in the tree installs it, watches for it, or knows its device
  node. `scripts/player/` carries a one-shot, idempotent uninstaller for a box set
  up before the change — it stops and disables the service, unloads the module,
  and removes the unit, daemon, settings, licence, DKMS registration, `/usr/src`
  copy and installed `.ko`, then points the player's `alsa_device` back at
  `default`. **A box still running that stack has to be cleaned before or with
  this update**: with the card-specific write path removed, the player opens that
  card as an ordinary ALSA device, and that driver pins an 8 ms buffer that cannot
  hold a decoded frame — so an un-purged box on the new player is the one
  combination that sounds worse than before.

- **Merging's open AES67 stack is now the audio route.** AES67 audio previously
  went out through a licensed virtual sound card, which worked on the bench
  but pinned an 8 ms ALSA buffer that cannot hold a ~21 ms decoded frame — so the
  card under-ran on every frame (`sound has broken up` twice a second) and the
  stream could not be aimed at a chosen address. `scripts/player/merging-aes67.sh`
  replaces it with Merging's `ravenna-alsa-lkm` kernel module and the GPL
  `aes67-daemon`, which registers a normal ALSA card and does RTP, SDP/SAP and PTP
  itself. Confirmed on the bench: clean eight-channel audio out of the network, no
  under-runs, destination selectable. Following it needed no change to the
  player's audio path — it is an ordinary ALSA card — though the controls for the
  stream itself came later, in the entry above. Lip sync over a full event is
  now settled too (a multi-hour run, no drift); PTP accuracy at a receiver is
  still open. Full account in entry 1 above.

- **Decoder seek accuracy and the timeline readout** — released in
  `v0.1.13-alpha`. Two parts are operator-visible. **Seeking now lands on the moment asked for and says
  so**, within a millisecond, verified against a live event: asked 14:40:43.768,
  reported 14:40:43.769. It previously started playing from the position just
  left, ran on for seconds, then jumped somewhere else and reported a time up to
  a whole segment early. **"Behind live" no longer swings by six seconds** — it
  was a count of segments on both sides and is now the gap between two real
  times, so a two-minute delay reads as two minutes instead of flicking between
  1:54 and 2:00. Also: the decode dock's playhead is interpolated between state
  refreshes rather than stepping twice a second, and its header is split into
  what this box is doing (`PLAYING`/`HELD`/`STOPPED`/`READY`) and what the main
  site is doing — Hold on a finished recording previously showed no indication
  at all.

  Getting there took five broken attempts in one afternoon, each found by the
  operator rather than the suite, because nothing tested this path. The rules
  now live in `src/core/playout_timeline.h` as one ordered decision the delivery
  loop calls, with `tests/test_playout_timeline.cpp` failing on every one of
  those five. **Known residual, not fixed:** a segment's `at_ms` and its media
  pts disagree by up to ~61s on an event whose encoder has restarted, so
  `seq x duration` is not interchangeable with `at_ms`. Seeks are unaffected —
  `at_ms` is used consistently at both ends — but three places still fall back
  to the estimate for segments outside the manifest window, and that is a
  minute-scale error waiting for a seek into one of them.

- **`v0.1.12-alpha`** — released 2026-09-11, tagged on `3bca35c`. The release
  body is `.github/RELEASE-NOTES.md` read at the tag, so the notes commit has to
  land *before* the tag: writing the notes and tagging in one push publishes the
  previous release's body, with the new sections in the tagged tree but absent
  from the release anyone reads. Tag the current `main`, never a commit in
  isolation — the tree only builds as a whole. A tag that has to be withdrawn is
  `gh release delete <tag> --yes --cleanup-tag` plus `git push origin
  :refs/tags/<tag>`, and the re-cut is public a second time, as happened on
  2026-09-11 before this one was cut properly.
- **`96c5d8c`** — the Pi player's hold fix (a held picture outranks the idle
  screen) and its four-boolean rule, `test_idle_screen`.
- **`814cacf`, `4baad6a`** — Stop releases the picture: in-flight requests
  cancelled, the poll loop stopped, the decoder released, the cache deliberately
  kept. `resume_pending()` undoes `cancel_pending()`; `stopped` became a state of
  its own rather than a reading of `!playing`, because loading is also not playing
  and loading has to download.
- **`61b03fd`** — the decode dock's header split into two labels, playback and
  source, so neither can misstate the other. `READY` is new.
- **`9361cca`, `6b5c812`, `bd60b61`, `207182d`** — decoder/lipsync measurement:
  the delivery queue bounded per stream, the delivery lead measured at the
  handout, A/V drift measured rather than reasoned about, and the unsigned cast
  cleared as a suspect (it wraps in the cast and again in the addition; the two
  cancel bit-for-bit). The lipsync cause itself is still open.
- **`0bc1036`** — reverted `a40aad5` and `0444fe9`, a night's decoder UX. Nathan
  saw a frozen timer, a state reading stopped while video played, and Play doing
  nothing, on a build carrying them, and the live status endpoint showed a
  healthy core by every measure available. Known-good beats a build nobody can
  account for. Two things from that session are worth re-landing deliberately:
  Play is disabled precisely because playback is already running, and the badge
  for a pinned recording reports the *event's* state rather than the *playback*
  state.
- **`658cd5f`** — the decoder shutdown-hang fix and `test_s3_cancel`.
- **`ca41a99`** — `m_head` wasn't reset when the session's followed event
  changed, only the "is it seated" flag was — so switching events (or a
  room going live again after ending) could report the OLD event's
  position against the NEW event's manifest. This is shared code
  (`src/core/decoder_session.cpp`), so it applies to the OBS decoder and
  the Pi player both. Confirmed working correctly on the Pi player: after
  a `BROADCAST ENDED` → `LIVE` transition, `head` correctly reset to `0`
  rather than staying frozen at the old event's position (separate from
  the still-open stall in item 0 above, which happens *before* any such
  transition, while continuing to play out the ended recording).
- **`96ef6d0`** — the playing clock could pair a fragment's wall time with
  a different fragment's pts after a jump. Fixed with a once-per-decoder
  latch. Not yet exercised by a real jump-and-check in the field — if a
  jump-to-marker ever shows a position that looks wrong again, this is
  the first place to look.

---

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

## Design questions raised but not decided (not bugs — separate from the above)

Kept here only as pointers so they aren't lost; each needs a decision, not
a fix.

- **An update experience like DistroAV's** (raised 2026-09-20, researched, not
  decided). Worth correcting the premise first: **DistroAV does not install
  anything from inside OBS.** Its "Install Update" button is
  `QDesktopServices::openUrl(releaseUrl)` and nothing more
  (`src/forms/update.cpp`). Actual installation is a *separate* manual button
  in its settings that shells out to `brew reinstall --cask` or
  `winget install`, in a visible terminal, with Linux told it is unsupported —
  elevation and file replacement delegated entirely to the OS package manager.
  Nothing replaces files in process, on any platform. What is nice about it is
  the *presentation*, not the mechanism.

  We already have most of the mechanism. `src/obs/update_check.{h,cpp}` asks
  the GitHub releases API once per run over libcurl, off the UI thread, with a
  machine-wide opt-out — and, unlike DistroAV, carries **no identifier at
  all**. Theirs posts to its own API with OBS's install GUID, OS name, CPU
  architecture and a SHA-256 of its own binary in the User-Agent. For a church
  running this on its own hardware that is a privacy posture we should keep,
  not copy.

  So the delta is one dialog. What is worth taking:
  - A `QDialog` parented to `obs_frontend_get_main_window()`, bracketed by
    `obs_frontend_push_ui_translation` / `pop`, shown from a
    `QTimer::singleShot` so it does not fight OBS's own startup.
  - **Release notes rendered as Markdown with `QTextDocument::MarkdownNoHTML`.**
    They keep a debug-only harness that feeds the dialog hostile markdown
    (script tags, `onerror`, `javascript:` and `data:` URLs) to check the
    escaping — our notes come from the GitHub API, so the same care applies.
  - **Skip this version / Remind me later**, plus the auto-check toggle in the
    dialog itself, persisted in the OBS config.
  - Opening the release page for the install, and leaving installation to
    whatever put the plugin there.

  What not to take: the telemetry; the server-controlled UI delay and check
  interval, which they apply with no bounds check (a compromised endpoint could
  set either to anything); and any ambition to replace our own DLL while OBS
  has it loaded.

  **Licence is compatible.** DistroAV is GPL-2.0-**or-later**, so its code can
  be taken into this GPL-3.0-or-later project. Note it is the "or later" that
  makes this true — GPL-2.0-only would not be. Their HTTP layer is itself
  copied from OBS Studio's `remote-text.{hpp,cpp}`, because OBS ships Qt with
  no TLS backend and `QNetworkAccessManager` therefore cannot do HTTPS from a
  plugin. We already use libcurl directly, so that problem does not reach us.

  Open question: is a modal dialog at startup the right thing for a machine
  that goes live on a schedule? An operator opening OBS twenty minutes before a
  service does not want a dialog in front of the Go Live button. The dock line
  we have now is quieter, and may simply be correct for this audience.

- **Bucket-mediated status/control**: should campus *appliances* report
  status the same way OBS decoders would, and is the relay meant to be the
  only control plane, or should the encoder's own dock see campuses too?
  Answering the second question decides whether a `desired.json`-per-target
  scheme needs compare-and-swap on its revision from the start.
- **DASH manifest for browser preview/playback**: operator-only preview
  (behind the relay's existing login, no public bucket needed) vs. public
  viewing; live vs. "last Sunday, downloadable" only.
- **Product names** for the three shipped components (OBS plugin pair, Pi
  player, simulcast portal/relay) — versions now all derive from one
  `PROJECT_VERSION`; names are still whatever the code happens to call
  them.
