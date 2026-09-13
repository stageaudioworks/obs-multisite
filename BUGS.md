# Bugs and short-term build items

Working list, meant to survive a change of machine or a change of agent —
each entry has enough context to act on without anyone having been in the
room when it was written. Delete an entry once it's fixed and released;
this file is not a changelog.

Last updated: 2026-09-12.

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

---

### 1. AES67 audio: works on the bench, unproven over an event

**Status: working on a bench Pi — eight channels of clean AES67 audio, on a card
with a normal buffer. What still needs a real event is lip sync over a full
service, and the PTP accuracy a Pi's network interface can reach.**

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
2. **Watch it over a full event**, on the picture and the sound together — the
   one thing a bench cannot stand in for. That settles lip sync, and it settles
   how well PTP holds: AES67 wants both ends within a millisecond, and a Pi's
   network interface does no hardware timestamping, so the achievable accuracy
   is whatever the software manages. Measure it at the receiver, not on the Pi.
3. **Dante routing is by hand.** A source shows up in Dante Controller, but
   connecting it to a receiver is a manual step in that application.
4. **A kernel upgrade means rerunning the installer.** The module is built from
   source against the running kernel and is not put through DKMS, because its
   build takes a branch of the submodule and a compiler choice a DKMS hook
   cannot reconstruct reliably. Rerunning the script rebuilds it.

**Next step:** a full-length service on the picture and the sound together,
which is point 2 above.

## Recently landed (context, not action items)

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
  it happens, see entry 0 above) and AES67 lip-sync/PTP accuracy (needs an
  actual multi-hour event, not something fixable in code).

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
  stream itself came later, in the entry above. **Still open: lip sync over a
  full event**, and the PTP accuracy a Pi's network interface reaches. Full
  account in point 3 above.

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
