# Bugs and short-term build items

Working list, meant to survive a change of machine or a change of agent. Each
open entry is **one screen** — status, symptom, root cause, next step, files,
and the thing not to try. The measurements, reverted attempts and reasoning that
produced those conclusions are in `docs/bugs/`, linked from each entry. Read the
archive before changing timing, seeking or playout code; the short entry names
the trap, the archive shows why it is one.

This file is not a changelog. Delete an entry once it's fixed and released. The
full record of a fixed entry is kept in `docs/bugs/`, not here — nothing is
thrown away, it is moved out of the reader's way.

Last updated: 2026-09-21.

---

## Open

> **Next up (2026-09-22), in order:** entry **2** first — the delivery-stopped-
> draining stall reproduced live on 2026-09-21, whose next measurement the entry
> names (instrument the deliver loop's wait at `multisite_source.cpp:954`
> before changing any arithmetic). Then Phase 12, spec'd in
> `docs/scope/phase12-capability-map.md` and
> `docs/scope/spec-cloud-identity.md`, gated on the map's approval and on the
> decoder-pairing question.

### 0. Pi player: playback can stall indefinitely while downloads keep succeeding

**Status: not seen for a long while, root cause NEVER found. Kept open, not
closed.** The bounded-decoder work since (below) removes the stall's worst
effect, but nobody has ever explained *why* the decode thread wedged, and this
failure mode is silent — a freeze with downloads still climbing. "We stopped
seeing it" is not "it was fixed", and this file exists to keep that distinction.

**Symptom (as last seen).** On a Pi following a room, `head` and `frames_out`
freeze while `cached`/`downloaded` keep climbing. `fps` near 0, `dropped` flat —
so decode is not happening at all. ALSA xrun warnings (`sound has broken up N
times`) appeared as each stall began.

**Leading hypothesis (never confirmed).** `feed_loop` parked in
`CmafDecoder::push_fragment()`, waiting for queue space a wedged decode thread
never frees.

**Why it may not recur, and why that is not a diagnosis.** `push_fragment` now
waits only while the decoder is still producing — ten seconds with no progress
declares it wedged and rebuilds it — and `stop()` waits a bounded grace period
before detaching rather than joining forever. So the *effect* is bounded: at
worst one lost segment and a rebuilt decoder. Whether that is what stopped the
occurrences, or whether they simply became rare, is unknown — and the code that
bounds the effect is not the code that would explain the cause.

**If it ever reproduces** (box still up, stall ongoing), the thread dump is
still the only thing that settles it:
```sh
gdb -p $(pgrep multisite-player) -batch -ex "thread apply all bt"
```
Save the output into `docs/bugs/00-pi-player-stall.md` before doing anything
else. The status line watches for this shape and logs a `WARN` naming the pid
and this command, so it will not have to be spotted by eye.

**Files:** `src/appliance/player.cpp` (`feed_loop`, `deliver_loop`, `enqueue`),
`src/core/cmaf_decoder.cpp` (`push_fragment`), `src/appliance/alsa_output.cpp`.

**Archive:** `docs/bugs/00-pi-player-stall.md`

---

### 1. AES67 audio: proven into a console over 24 hours

**Status: CLOSED 2026-09-21 — proven into a receiving console over a 24-hour
run.** The last open question was PTP lock accuracy *at the receiver*, and a
console taking the stream and behaving correctly across 24 hours is that read:
the console stays locked only if PTP holds at both ends.

**Settled.** Lip sync was never an AES67 question — audio and video are
scheduled off the same delivery-queue clock and the same first-frame anchor
(`Player::anchor_pts()`), so they cannot drift from each other whatever card the
sound goes out on. The Pi's own PTP jitter is logged per minute (` ptp=locked
12.3ns` in the status line) and shown on the box's page beside the grandmaster.

**Two operational notes, neither a fault:**
- **A PTP master must exist on the network.** The daemon slaves to a clock; it
  does not hand one out. Most likely cause of silence after a clean install.
- **A kernel upgrade means rerunning the installer** — the module is built
  against the running kernel and is not put through DKMS.

**Not measured, and deliberately not chased:** the console's exact jitter figure
recorded beside the Pi's. The behavioural proof is what the entry asked for; the
number would be a nicety, not a gate.

**Archive:** `docs/bugs/01-aes67-ptp-lock.md`

---

### 2. Hold/resume: fix VERIFIED, and a second fault found after it

**Status: the resume fix works — verified on a real hold 2026-09-21. A separate
fault that surfaces after a resume is OPEN and reproduced live.**

**The resume fix is confirmed.** On a real hold: `PAUSED ... on screen 848.067s`
→ `RESUMED ... held from 848.067s` → `playout anchored on first video frame (pts
848.100s)`. **33 ms** — constant, not growing with the hold. No
`playout clock fell Ns behind` line. The old fault is gone; this entry's
original root cause and fix are now proven, not merely reasoned.

**The new, separate fault (OPEN, reproduced 2026-09-21).** Seconds after that
clean resume, delivery stops handing anything to OBS and the queue jams
permanently:

```
17:06:51.683  first 1s after resume — video min lead 395 ms; audio min lead 395 ms   <- healthy
17:06:52.309  dropped an audio frame after waiting 250 ms — delivery last handed over
              1265 ms ago, playing, this stream held 1003 ms (bound 1000 ms)
17:06:54.327  ... delivery last handed over 3283 ms ago, held 1003 ms (bound 1000 ms)
17:06:56.347  ... delivery last handed over 5302 ms ago, held 1003 ms (bound 1000 ms)
```

`frames_out` freezes; the gap since the last handover grows without bound. This
is the entry's own previously-unexplained half — *"22 of 47 drops were audio,
which should never have jammed on capacity"* — now reproduced on demand.

**Two things are visible and confirmed; the cause is NOT.** Do not treat either
as diagnosed:
1. **The queue's structural span is over its own bound.** 48 audio frames of
   ~20.9 ms span **1003 ms**; `kMaxQueuedNs` (the bound the accept test uses) is
   **1000 ms**. A full audio queue is therefore *permanently* "full" — 3 ms over.
   The comment at `kMaxQueuedNs` already warns "a queue whose capacity equals the
   lead is full by construction"; this is the same shape one layer down, where
   the *span* at the count backstop exceeds the *duration* bound.
2. **Delivery stopped draining.** `delivery last handed over` climbing to 5+ s
   is the delivery loop doing nothing, not slow. Whether (1) is what stops it,
   or delivery stops for its own reason and (1) merely prevents recovery, is the
   open question — they are not separable from this log.

**Fixed while capturing this (safe, isolated):** the resume diagnostic had a
format string with a fifth `%.0f` and no fifth argument, so `the bound is`
printed junk (`0`). The very number this investigation rests on was UB. Fixed at
`multisite_source.cpp` (now passes `kMaxQueuedNs`). Worth knowing: `mlog_*` goes
through `plugin_log_line`, which *is* marked `format(printf,…)`, yet neither
clang nor GCC flagged the missing argument on the concatenated literal — the
class-level fix (a compile-time check that actually fires) is not done.

**Next step, in order — read `docs/bugs/02-hold-resume-skips.md` first:**
1. Determine whether `queued_span_ns()`'s count backstop (`n >= hard cap`
   returns `kMaxQueuedNs` *unconditionally*) is the jam: audio hits 48 frames
   long before its span reaches 1000 ms, so the count path can declare full
   while the queue is only ~1003 ms and cannot drain. That is a two-line
   comparison to check, not an arithmetic rewrite.
2. Only then ask why delivery stopped. Instrument the deliver loop's wait
   (line 954) — how long it is parked at each stage — before changing it.

**DO NOT:**
- **Re-apply option (a)** (`resume()` seeking back to `last_out_pts_ns`). Tried,
  measured, reverted. Do not try it again.
- **Re-derive the anchor.** There is ONE anchor, correctly placed. Read
  `playout_clock.h` before touching any of this arithmetic.

**Files:** `src/obs/multisite_source.cpp` (`enqueue_frame`, `deliver_loop`,
`queued_span_ns`, `resume`), `src/core/playout_timeline.h`,
`src/core/playout_clock.h`.

**Archive:** `docs/bugs/02-hold-resume-skips.md` — **read this before changing
timing code.** It carries the measurements, the four rejected fixes, and the
end-to-end account of the original root cause.

---

## Recently landed (context, not action items)

Resolved and released, or closed and waiting to be released. Full detail for
every item in this list is in `docs/bugs/2b-7-resolved.md` (the bugs that were
fixed) and `docs/bugs/landed-record.md` (the released-work prose). One line
each, so the history is findable without being 1,100 lines in the way.

- **#1 — AES67 PTP into a receiver. CLOSED 2026-09-21.** Proven into a
  receiving console over a 24-hour run; the console locks only if PTP holds at
  both ends, which is the read the entry was waiting for. The console's exact
  jitter figure is not recorded and is not being chased. Archive:
  `docs/bugs/01-aes67-ptp-lock.md`.
- **#2b — media→wall mapping position-dependent by ~1.1%.** The cue system rested
  on an estimated segment start; the encoder's own event-start anchor fixed it.
  FIXED.
- **#2c — seeking slow and landed late.** Five faults, all measured; 1816–5355 ms
  to 77–263 ms. FIXED and verified on real content.
- **#3 — clicking the timeline lands on the segment, not the moment.** Slide
  the click remainder; `seek_to_media_ms()` owns the conversion. FIXED.
- **#2d — seek skip used one base for two streams**, picture ~311 ms behind the
  sound. One arm per stream in `seek_skip.h`. FIXED.
- **#4 — Pi timeline/scrubbing died on a zero.** The sentinel trap, sixth time;
  `web/media.js` split out and tested. FIXED.
- **#5 — "29831921 min 43 sec behind".** A media time subtracted from an epoch;
  live edge now held in media time. FIXED.
- **#6 — OBS froze when uploads stalled.** A network PUT under the status lock;
  a publisher thread owns manifest writes now. Pinned by `test_session` case 21
  (1202 ms → 0 ms). FIXED.
- **#7 — Windows crash: `QPointer` as a cross-thread receiver.** Receiver is
  `qApp` now; PDBs staged as a CI artifact so the next dump is readable. FIXED
  (strongly indicated, not proven).
- **AV1/HEVC/RTMP codec gate** — HEVC reaches RTMP via Enhanced RTMP; AV1 over
  SRT refused because ffmpeg cannot put it in MPEG-TS.
- **End Broadcast published nothing** — sticky cancellation switched the
  transport off before the end writes. FIXED.
- **End Broadcast could hang OBS's main thread** — the drain never honoured its
  deadline. FIXED.
- **Storage provider dropdown, LAN delivery, cloud-disable** — ported to both
  docks, the appliance and the relay.
- **Disk cap on the encoder spool, low-disk warnings.**
- **Sound card width/mute/reconcile** — three faults that presented as a silent
  room.
- **Phase 10 tiles**, the AES67 stream controls, the open AES67 stack replacing
  the licensed card, the decoder seek/timeline readout, resume-stale dialog,
  orphaned cache sweep, idle keep-alive fix, and the monitoring heartbeat.

---

## Sweeps and open derivations

Not bugs yet — the shape that produced twelve faults in one stretch, where it is
still present. Each is *one question answered in more than one place*. The
medicine is always the same: expose the authority's answer and delete the second
copy. Full D1–D4 record in `docs/bugs/sweeps-d1-d4.md`.

- **D1 — "is this played as a recording?"** Was five answers; extracted to
  `DecoderSession::plays_as_recording()`, dock and page rewired. **DONE.**
  Lesson: a sweep entry is a lead, not an inventory — grep the pattern before
  trusting the list, and again after.
- **D2 — segment length, two fallbacks. DONE (2026-09-21).** One question, one
  answer: `DecoderSession::segment_duration_s()` is the authority and returns
  the value *with its fallback applied*, so no caller floors it. The dock and
  two internal sites each re-applied `> 0.1 ? : 6.0` over the snapshot; all
  three now read the authority. **A duplication removal, not a live fix** — both
  assignments into the raw value are guarded `> 0.1` and the initial is 6.0, so
  the consumer floors were provably dead code and nothing observable changed.
  No test pins it, because there is no old-behaviour failure to reproduce; the
  guard is the single accessor. Full note in `docs/bugs/sweeps-d1-d4.md`.
- **D3 — values handed out that nothing consumes. DONE (2026-09-21).**
  `start_buffer_s` was written into the dock snapshot and read nowhere — the
  invitation D3 named, for the next person to build a progress readout on stale
  data. Removed from `multisite_ui.h` and its one assignment in
  `multisite_source.cpp`. (`buffered_span_s` is also write-only but carries a
  comment suggesting deliberate later use; left alone, and not this entry's.)
- **D4 — queue caps were counts, not durations.** `kMaxQueuedVideo` at 12 frames
  held 367 ms against a 400 ms gate — a permanent jam. Now `kMaxQueuedNs` with a
  `static_assert` tying it to the gate. Cost: ~93 MB queued video at 1080p30.
  **FIXED**, with one thing not fully explained: 22 of 47 drops were audio, which
  should never have jammed on capacity — the drop path now logs enough to settle
  whether delivery stopped draining.

---

## Design questions raised but not decided

Not bugs — each needs a decision, not a fix. Kept as pointers.

- **An update experience like DistroAV's** (raised 2026-09-20, researched, not
  decided). DistroAV does not install from inside OBS — its button is
  `openUrl`; installation is a separate manual `brew`/`winget` shell-out. We
  already have `update_check.{h,cpp}` (GitHub releases API, off the UI thread,
  no identifier). The delta is one dialog: release notes as Markdown with HTML
  disabled, skip-this-version / remind-me-later. Open question: is a modal
  startup dialog right for a machine that goes live on a schedule? The dock line
  may simply be correct for this audience.
- **Bucket-mediated status/control**: should campus appliances report status the
  way OBS decoders would, and is the relay the only control plane, or should the
  encoder's dock see campuses too? The answer decides whether `desired.json`-per-
  target needs compare-and-swap on its revision from the start.
- **DASH manifest for browser preview/playback**: operator-only preview behind
  the relay's login, vs. public viewing; live vs. "last Sunday, downloadable".
- **Product names** for the three shipped components (OBS plugin pair, Pi
  player, simulcast relay) — versions derive from one `PROJECT_VERSION`; names
  are still whatever the code happens to call them.
