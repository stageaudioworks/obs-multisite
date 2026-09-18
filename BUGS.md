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

**The fix to attempt next.** Anchor BOTH streams on ONE reference at resume — the
position that was being held — rather than letting each take its own first frame.
That is a change to how the playout clock is established (`anchor_pts`,
`first_pts_ns`, `playout_base_ns`, and the delivery loop's stall resync in
`src/obs/multisite_source.cpp`), and it needs a test that asserts the two
streams' anchors agree to within a frame after a resume. A time-balanced queue
(the 12/48 asymmetry) is a separate, smaller question worth settling at the same
time; it is not itself the cause.

**Instrumentation that stays.** `PAUSED` now prints the pts of the last frame
handed to OBS and the queue depth, and `RESUMED` prints what it continued from.
Without that figure the two candidate causes were indistinguishable and cost
three rounds of guessing — keep it, and keep it honest.

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

## Design questions raised but not decided (not bugs — separate from the above)

Kept here only as pointers so they aren't lost; each needs a decision, not
a fix.

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
