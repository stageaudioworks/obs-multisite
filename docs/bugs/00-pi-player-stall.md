# BUGS #0 — Pi player stall: full record

Archive of the original entry. The short entry is in `BUGS.md`. This file holds
the full account, kept because the root cause is still unknown and the history
is what rules out the dead ends.

> Original entry, as it stood on 2026-09-21.

---

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
