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

### 0. Pi player: playback can stall indefinitely while downloads keep succeeding

**Status: root cause NOT found.** The process now survives it; the *why* is
still open. Needs a live thread-dump on next repro.

**Symptom.** On a Pi following a room, `head` and `frames_out` freeze while
`cached`/`downloaded` keep climbing. `fps` near 0, `dropped` flat — so decode
is not happening at all, not happening-and-dropped. ALSA xrun warnings
(`sound has broken up N times`) appear as each stall begins.

**Leading hypothesis (not a diagnosis).** `feed_loop` parked in
`CmafDecoder::push_fragment()`, waiting for queue space a wedged decode thread
never frees. If so, the thread dump shows exactly which call it is in.

**Bounded, already.** `push_fragment` waits only while the decoder is still
producing (ten seconds of no progress ⇒ declares it wedged and rebuilds it), and
`stop()` waits a bounded grace period before detaching rather than joining
forever. The worst case is now one lost segment and a rebuilt decoder, not a
silent freeze. **What is not bounded is a decoder wedged inside FFmpeg itself** —
that still needs the dump to distinguish.

**Next step, the moment it reproduces** (box still up, stall ongoing):
```sh
gdb -p $(pgrep multisite-player) -batch -ex "thread apply all bt"
```
Save the output into `docs/bugs/00-pi-player-stall.md` before doing anything
else. The status line now watches for this shape and logs a `WARN` naming the
pid and this command, so it will not have to be spotted by eye.

**Files:** `src/appliance/player.cpp` (`feed_loop`, `deliver_loop`, `enqueue`),
`src/core/cmaf_decoder.cpp` (`push_fragment`), `src/appliance/alsa_output.cpp`.

**Archive:** `docs/bugs/00-pi-player-stall.md`

---

### 1. AES67 audio: proven over an event, PTP lock accuracy at a receiver is not

**Status: eight channels clean on the bench, multi-hour run with no drift, PTP
accuracy at the receiver still unmeasured.** The remaining question needs a
console, not a code change.

**Settled.** Lip sync is not an AES67 question: audio and video are scheduled
off the same delivery-queue clock and the same first-frame anchor
(`Player::anchor_pts()`), so they cannot drift apart from each other whatever
card the sound goes out on. A multi-hour run confirmed this under real load.

**Still open — point 2 of the original entry.** How tightly a Pi's network
interface holds PTP without hardware timestamping, measured at a *receiving*
console over a service. AES67 wants both ends within a millisecond. The Pi's own
half is now recorded (` ptp=locked 12.3ns` in the 60-second status line, and the
jitter shown beside the grandmaster on the box's page), so both ends can be
compared after the fact.

**What is left, in order:**
1. **A PTP master must exist on the network.** The daemon slaves to a clock, it
   does not hand one out. Most likely cause of silence after a clean install,
   and a network question rather than a fault.
2. **PTP lock accuracy at the receiver** — the measurement above.
3. **Dante routing is by hand.** Source appears in Dante Controller; connecting
   it is a manual step there.
4. **A kernel upgrade means rerunning the installer** — the module is built
   against the running kernel and is not put through DKMS.

**Next step:** a full-length service on picture and sound together, capturing
the `ptp=` trace from the Pi's journal and the receiving console's own lock
figure over the same run.

**Archive:** `docs/bugs/01-aes67-ptp-lock.md`

---

### 2. Hold/resume: fix applied, NOT YET VERIFIED against a real hold

**Status: root cause found and fixed 2026-09-19, unverified.** The suite does not
reach `multisite_source.cpp` (plugin glue, not core), so only a real hold and
resume proves it.

**What is fixed.** `resume()` clears the queue and bumps the epoch; the pending
frame's *timestamp* was computed before that bump, but the staleness check
compared the epoch against itself and could never fail. The stale frame tripped
the stall resync, which overwrote the 500 ms cushion with a 400 ms lead. The
epoch is now stamped where the timestamp is computed (beside `playout_due_ns`),
not where the frame is built or popped.

**Also fixed.** The media->wall clock origin no longer walks on resume:
`PlayoutTimeline::adopt` takes a playout epoch *and* a media epoch, and a resume
bumps only the playout epoch.

**Residual, expected and accepted:** ~370 ms lost at resume — the queue clear,
now constant rather than growing with hold length. Removing it means keeping and
re-timing the queue, which is what `82b4183` did and why it was reverted.

**What success looks like:** NO `playout clock fell Ns behind (stall?)` line
after a resume, and both min leads in the `first 1s after resume` line staying
near 400 ms instead of video collapsing to 66 ms.

**DO NOT:**
- **Re-apply option (a)** (`resume()` seeking back to `last_out_pts_ns`). Tried,
  measured, reverted the same day. Do not try it again.
- **Re-derive the anchor.** There is ONE anchor, correctly placed. This has been
  the fourth variant of the same misreading. Read `playout_clock.h` before
  touching any of this arithmetic.

**Files:** `src/obs/multisite_source.cpp` (`deliver_video`, `deliver_audio`,
`enqueue_frame`), `src/core/playout_timeline.h`, `src/core/playout_clock.h`.

**Archive:** `docs/bugs/02-hold-resume-skips.md` — **read this before changing
timing code.** It carries the measurements, the four rejected fixes, and the
end-to-end account of the root cause.

---

## Recently landed (context, not action items)

Resolved and released. Full detail for every item in this list is in
`docs/bugs/2b-7-resolved.md` (the seven bugs that were fixed) and
`docs/bugs/landed-record.md` (the released-work prose). One line each, so the
history is findable without being 1,100 lines in the way.

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
  `multisource_source.cpp`. (`buffered_span_s` is also write-only but carries a
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
