# Self-Hosted Multisite Streaming Platform — Project Scope

A free, open-source, self-hosted platform for distributing a live event from a
main campus to any number of satellite campuses **reliably**, over commodity
hardware and unreliable venue internet. It runs as a pair of OBS Studio plugins
and uses nothing but an S3-compatible bucket you control — no central server, no
database, no vendor. Intelligence lives entirely in the edge plugins; the bucket
is a dumb file store.

> **⚠️ Alpha — development build.** This is pre-release software under active
> development. A six-hour continuous soak has been run end to end (see
> the README's Status section), but it has not yet carried a real congregation's event.
> Interfaces, settings and the storage protocol may still change without a
> migration path, and there is no support contract, warranty or uptime
> guarantee of any kind.
>
> Production use comes with caveats. Run it only with a tested fallback in
> place, a technical person on hand, and the assumption that any given event
> may have to go ahead without it. Treat a successful rehearsal as necessary
> rather than sufficient.

---

## 1. Design priorities (ranked)

1. **Reliability above all.** A live event must not drop frames at a campus
   because the main site's internet hiccupped. Every segment is durable,
   retried, and verifiable; nothing is silently lost.
2. **Feature completeness** for how events actually run — production audio
   distribution, markers/cues, pause-and-hold, resume-after-crash, multisite.
3. **Simplicity of operation.** Decentralized and file-based. An operator's whole
   mental model is "hit Go Live" at the main site and "add the source" at a
   campus.
4. **Latency is last.** Minutes of latency are acceptable. We buffer heavily and
   trade latency for resilience at every decision point.

This ranking is the tie-breaker for every design choice.

---

## 2. Headline capabilities

- **Store-and-forward delivery.** The encoder writes each segment to a durable
  local queue first, then uploads with retry. If the network drops, capture keeps
  queuing to disk; on reconnect the queue drains in order. Nothing is lost.
- **Timeslipping (per-campus live-DVR).** Each receiving site can **pause** the
  incoming feed — to hold for its own welcome or announcements — and later
  **resume from exactly where it paused**, while the plugin keeps downloading the
  live feed into a local cache the whole time. Sites can also **jump to live**,
  **scrub** within what's retained, and see **how far behind live** they are.
- **Multi-track production audio.** Delivers up to all 6 OBS audio tracks — main
  mix, ISOs of specific mics, click track — muxed into the same fragment and so
  locked to the picture and to each other. At an OBS satellite each track is a
  separate source, all fed by one decoder, for local mixing, monitoring and
  in-ears. This is the primary mode (§4.3); packed multi-channel (§4.3.1)
  remains available for sites whose output is a single multi-channel device.
- **Markers / cues.** The main site drops named markers into the stream ("Sermon
  Start", "Offering", "Go to local"), manually or on a schedule; satellites see
  them, jump to them, and can trigger local automation.
- **Crash & outage resilience.** OBS crash or power loss at either end is
  recoverable: the encoder resumes the same event and sequence; decoders hold the
  last frame and resume seamlessly when the feed returns.
- **Out to the public, from the same upload.** A small self-hosted event
  reads the segments already in the bucket and pushes them to YouTube, Facebook
  or any RTMP destination, so the main site uploads once whether the event is
  going to two campuses or to two campuses and the internet. It runs a few
  minutes behind on purpose, so a wobble at the main site delays the public
  stream rather than breaking it (§8.2).
- **Bring-your-own storage.** Works with any S3-compatible endpoint — Cloudflare
  R2, AWS S3, Backblaze B2, Wasabi, or self-hosted MinIO. Cost is just storage.
- **Satellites can be appliances.** A campus that only needs to *receive* runs a
  headless Linux decoder box driving SDI/HDMI out, controlled from a phone or
  tablet over the local network. No OBS to learn, nothing to misconfigure, and
  it starts on power-up. Campuses that also mix local cameras or graphics run
  the OBS source plugin instead; both share the same core.
- **The origin is not tied to hardware.** Encoder and decoder ship in one module,
  so any machine running OBS can take either role, and what originates an event
  is a laptop with OBS on it. A broadcast can come from a guest speaker's laptop, a
  conference venue for one week, a campus hosting this week's combined event,
  or a site set up at short notice; adding an origin costs a room name and a key
  that can write to it. Nothing ships, clears customs, or is licensed per
  location. Store-and-forward matters *more* for an occasional origin than a
  permanent one: a speaker on hotel wifi or a phone hotspot has the worst
  connection in the chain and can least afford a dropout mid-sermon, and a
  broadcast written to disk and resent survives a link that would kill a direct
  stream.

---

## 3. Architecture

```
 MAIN CAMPUS (encode)          S3-COMPATIBLE BUCKET (dumb store)        SATELLITE CAMPUSES (decode)
┌─────────────────────┐                                              ┌─────────────────────┐
│ OBS + output plugin │  PUT init.mp4 / .m4s / manifest / markers    │ OBS + source plugin │
│  capture → encode   │ ───────────────────────────────────────────▶│  poll → download →  │
│  → CMAF segment     │        rooms/{room}/live.json                │  verify → cache →   │
│  → checksum         │        events/{ulid}/…                       │  local DVR playout  │
│  → durable queue    │◀── polled + GET + verified by every decoder ─│  → Projector → HDMI │
│  → upload w/ retry  │                                              │  (pause/resume/live)│
└─────────────────────┘                                              └─────────────────────┘
```

- **Two kinds of satellite, one core.** The receive logic (discovery, cache,
  timeslipping, decode) is a library with no dependency on OBS or Qt. It is
  driven either by the **OBS source plugin** (for campuses that mix locally) or
  by a **headless appliance** (for campuses that just play the feed out). The
  appliance is the expected deployment for most sites.
- **Decentralized.** No control plane. The media path and the signaling path are
  the same path: objects in a bucket.
- **Read-only decoders.** Satellites need only `GetObject` + `ListBucket`.
- **Addressable by number.** Deterministic segment names (`{seq:08d}`) let any
  node fetch any segment without a directory event.

---

## 4. Storage protocol

### 4.1 Namespace (single bucket per organization)

```
rooms/{room_id}/
    live.json                     # pointer to the current live event_id (+ heartbeat)
    events/{event_id}.json        # per-room index entry, written once at "Go Live"
events/{event_id}/                # event_id = ULID minted by the encoder at "Go Live"
    event.json                    # static: video + audio-track layout, codecs, first_seq
    init.mp4                      # ONE CMAF init: video + every enabled audio track
    segments/{seq:08d}.m4s        # ONE fragment per seq carrying video + all audio tracks
    manifest.json                 # rolling window of recent segments (live edge)
    markers.json                  # append-only cue/marker list
```

- `room_id` is the broadcast source (e.g. `main-auditorium`).
- The per-room index exists because `events/` is a flat global namespace:
  nothing in an event's key says which room it belongs to, only `event.json`
  does. Without the index, listing one room's events means listing every event
  ever recorded and reading each descriptor to discard most of them. Writing it
  is deliberately non-fatal — an event must not be held off air because an
  index entry failed.
- **The index is a shortcut, not the register.** An event with no entry — one
  recorded before the index existed, or one whose entry failed to write — must
  still list, so discovery is the *union* of the index and a scan of `events/`
  rather than the index when it has anything and the scan when it does not.
  Treating a non-empty index as the whole truth hid every older event the
  moment one indexed event appeared. The scan costs a descriptor read only
  for the ids the index did not already name.
- `event_id` is a ULID minted locally by the encoder at "Go Live".
- Decoders need only read access; the encoder needs write access scoped to its
  own room/event paths.

### 4.2 Segment format & codecs

- **Container: CMAF / fragmented MP4** (`init.mp4` + independent `.m4s`
  fragments). It seeks cleanly (needed for timeslipping), carries video plus
  multiple audio tracks in one fragment, and is the native format for HLS/DASH —
  so the same objects can later feed a browser/mobile simulcast with no
  re-packaging.
- **Muxing uses FFmpeg's fMP4 muxer** for correctness; the decode path uses
  FFmpeg as well.
- **Duration: 6 s default (configurable 2–15 s).** The keyframe interval strictly
  equals the segment duration. Longer segments mean fewer requests, better
  compression, and fewer opportunities to drop a request — all reliability wins.
- **Video codec roadmap:** **H.264** first (universal decode), then **HEVC** for
  bandwidth, then **AV1**. Codec identity lives in `event.json`/`manifest.json`,
  and the muxer wrapper and decoder are codec-agnostic, so adding a codec is a
  capability change rather than a rewrite.

### 4.3 Production audio: multi-track (primary mode)

The primary delivery mode is **one audio stream per enabled OBS track** — main
mix, mic ISOs, click — all muxed into the same fragment and delivered as a unit.

- **Native to how OBS already works.** An operator assigns sources to tracks in
  Advanced Audio Properties, exactly as they would for a multi-track recording.
  Nothing new to learn, and the encoder side needs no special configuration.
- **No global layout requirement.** Each track is its own stream with its own
  channel count: a stereo main mix beside a mono click beside mono ISOs. OBS can
  stay in plain stereo at both ends.
- **Per-track channel layout is preserved** — a click or ISO may be mono while
  the main mix is stereo or wider.
- **Sync is structural, not incidental.** Every track shares one fragment, one
  timeline and one playback clock. They cannot drift from the video or from each
  other, because they are demuxed from the same object and played from one
  playout base.
- **Capacity:** up to 6 tracks (OBS's limit).
- **Codec:** AAC to start; Opus is a later option.
- **Far-site output:** the Multisite Source carries the video plus one chosen
  track — track 1 by default, so a campus that only wants the programme does
  nothing. Each further track is exposed by adding a **Multisite Audio Track**
  source, which attaches to the decoder already following that room rather than
  opening its own. The segment is therefore downloaded once and decoded once
  however many tracks a campus uses, and every track is emitted from the same
  playout base. The local engineer routes those sources to mixer tracks, monitor
  sends or in-ears.

Why this is the primary mode and packed is not: OBS resamples every source to
its **global** layout, so packed multi-channel requires both ends to be set to
7.1, and a satellite that is not silently downmixes — summing the ISOs and the
click into the programme. That failure destroys production audio without anyone
noticing until it is on air, and no amount of warning text makes it a good
default. Multi-track has no such mode.

### 4.3.1 Production audio: packed multi-channel (alternative mode)

A **single multi-channel track** — one 8-channel AAC stream carrying main mix,
ISOs and click in fixed channel positions. Supported for sites with a
multi-channel interface that would rather have one stream than several.

- **Sample-accurate by construction.** One stream, one clock. This is a real
  property, but it is not an advantage over multi-track *here*: our tracks
  already share a fragment and a clock.
- **Channel order is the interface.** Channel 4 must be the click at both ends,
  so the mapping is *published* in `event.json` and `manifest.json` as
  `channel_labels` and a satellite routes by name rather than guessing.
  Positional meanings from the speaker layout (FL/FR/LFE/…) are deliberately
  ignored — the layout is only a channel-count carrier.
- **Prerequisite, and the reason this is not the default:** both encoder and
  satellite OBS must be set to **7.1** in Settings → Audio → Channels. Both
  plugins detect a narrower layout and log an explicit error, because otherwise
  the extra channels are downmixed and destroyed silently.
- **Capacity:** 8 channels total, e.g. stereo main mix + 6 mono ISOs.
- **Audio interfaces:** ASIO or Blackmagic DeckLink devices. Mapping packed
  channels onto device output channels at the satellite is **out of scope**,
  not merely unbuilt: in OBS,
  [atkAudio's plugin suite](https://github.com/atkAudio/PluginForObsRelease)
  already routes audio to ASIO, CoreAudio and Windows Audio devices and hosts
  VST3/AU/LV2 plugins alongside, which is a superset of what a de-interleaver
  of ours would have done. It is a separate install under AGPL-3.0; nothing
  here links against it or requires it. Verified 2026-09-07: an 8-channel /
  7.1 feed carried on one track survives this pipeline with its channel order
  intact, which is the part that is ours to get right.

**Which mode suits which satellite.** The two modes are not competing for the
same sites. An **OBS satellite** wants multi-track: OBS routes sources
independently, so separate tracks land on separate destinations with no
routing plugin needed at all. An **appliance** driving HDMI or SDI wants packed: its output
is one multi-channel device, and eight channels in one stream map straight onto
HDMI's eight (§8.1). The encoder can send either; the choice belongs to the
receiving end, which is why both remain supported.

### 4.4 Manifest (live-edge discovery)

`manifest.json` carries a rolling window of recent segments plus the range
metadata a timeslipping decoder needs:

```json
{
  "event_id": "01J8XG7QK3ZC9F8P6M2R4T5V7W",
  "status": "live",
  "updated_at_ms": 1719484800000,
  "first_available_seq": 300,   // oldest segment still retained (for timeslip)
  "window_start_seq": 412,      // oldest listed in this manifest
  "latest_seq": 462,            // live edge
  "init": "init.mp4",
  "video": { "codec": "h264", "width": 1920, "height": 1080, "fps": 30 },
  "audio_tracks": [             // streams inside each segment
    { "idx": 0, "label": "Main Mix",   "codec": "aac", "channels": 2, "sample_rate": 48000 },
    { "idx": 1, "label": "Sermon ISO", "codec": "aac", "channels": 1, "sample_rate": 48000 },
    { "idx": 2, "label": "Click",      "codec": "aac", "channels": 1, "sample_rate": 48000 }
    // … up to 6, one per enabled OBS track
  ],
  "segments": [ /* last ~50: seq, duration, checksum (one file carries all tracks) */ ]
}
```

Decoders discover the live edge from the window but can address any segment from
`first_available_seq` to `latest_seq` by deterministic key, so playback is not
limited to the manifest window — essential for timeslipping.

### 4.5 Markers

`markers.json` is append-only:

```json
{ "markers": [
  { "seq": 420, "at_ms": 1719484860000, "type": "cue", "label": "Sermon Start", "id": "01J8…" }
] }
```

### 4.6 Lifecycle / retention

- Retention is handled by a **bucket lifecycle rule keyed on prefix and age**:
  delete objects under `events/` older than 7 days. Set-and-forget, configured
  once in the storage provider's console.
- Object *tagging* is deliberately not used. S3 supports it, but Cloudflare R2
  rejects requests carrying `x-amz-tagging`, so tag-driven expiry is not
  portable. Tagging remains available as an option for stores that support it,
  off by default.
- Every `PUT` carries `Cache-Control: max-age=604800` for any CDN in front of
  the bucket.
- Old segments are removed by lifecycle expiry, not active deletion, so a
  paused or behind-live decoder can still fetch older segments for the full
  retention window — enabling deep DVR rather than a short buffer.
- **The rule must cover `rooms/` as well as `events/`.** The per-room index
  entry for an event is a few hundred bytes and outlives nothing on its own, so
  a rule that expires only the media leaves the event list advertising events
  whose segments have gone. The catalog handles it — such an event is counted
  as skipped rather than offered — but the list degrades over time for no
  reason. Same age on both prefixes.
- Retention is the set-and-forget default. The encoder UI can additionally
  delete specific events on demand — one at a time, or everything older than a
  chosen number of days — with a confirmation and a verification pass, but the
  codebase otherwise does not delete as it goes, so a paused or behind-live
  campus can still fetch older segments for the whole retention window. The
  event live.json currently names is never deletable.
  The listing is a two-part operation by design: the events are read from the
  manifests first and each one's size measured afterwards, six at a time, so the window is usable on a bucket holding months of events. A size that
  cannot be measured is reported as unknown rather than as zero, and closing the
  window cancels the work in flight.

### 4.7 Write-ordering invariant

A segment is never listed in the manifest until it is durably in storage:
`write local → checksum → PUT segment (+tag) → await 200 OK → update manifest →
PUT manifest`. If a decoder can see a manifest entry, the segment is guaranteed
to exist.

### 4.8 Protocol version

Every document the encoder writes carries `protocol_version`, a single integer.
`live.json`, `event.json`, the room index entry, `manifest.json` and
`markers.json` all have it, so any one of them can be judged on its own by
whatever reads it first.

One integer rather than a semver, because a reader has exactly one question —
*can I still understand this?* — and one number answers it. It starts at **1**.

**It is bumped only for a change that would make an older reader misread a
bucket.** Never for an addition an older reader can safely ignore, which is what
every field added so far has been: `name`, `layout`, `channel_labels` and the
rest all arrived as optional fields with defaults, and an older decoder meeting
one simply does not see it. Growing the format is not the same as breaking it,
and only breaking it moves this number.

**An absent version is version 1.** Every bucket written before the field
existed goes on working untouched, exactly as an absent tile layout parses as
1x1. There is no migration and no flag day. So does a version that is present
but unclear — a string, a null, a zero, a negative, a float — because this
arrives from a bucket that any encoder version may have written, and the oldest
protocol is the safest assumption under which to read an unclear document.
Parsing never throws on it: a malformed version must not be the thing that stops
an event playing.

**Older is readable; newer is refused.** A decoder goes on reading everything it
once wrote, because a recording from last year is exactly what somebody wants to
play back. A document from a protocol it does not know is refused with a message
that says so — *"this event needs a newer version of the plugin"* — rather than
half-read.

That refusal is the entire point of the field. The failure it prevents is not a
decoder that stops; it is a decoder that carries on: a newer bucket half-read as
an older one shows up as a stutter, a failed checksum, or a clock time that is
wrong by an hour, and every one of those sends an operator to look at the
network when the answer is that the plugin is out of date. The check is made
against `manifest.json`, because that is the document whose misreading does the
damage — segment sequence, checksums and timing all come from it — and because
`live.json` yields only an event id, which being wrong is caught at the manifest
anyway.

The event listing is deliberately **not** filtered by version. An unreadable
event stays in the list and explains itself when someone tries to play it, which
is more use to an operator than an event that silently is not there.

---

## 5. Reliability

- **Durable encoder queue.** Segments are written to disk before upload, bounded
  only by disk. Survives OBS crash and power loss. Plain files with atomic
  write-then-rename, deliberately not a database: that gives the durability
  guarantee needed here and is trivial to reason about and to test.
- **Retry with backoff.** Failed uploads retry with exponential backoff and
  jitter, in strict sequence order, for as long as the event is live. No segment
  is abandoned.
- **Checksums.** Each segment's hash is recorded in the manifest; decoders verify
  after download and re-fetch on mismatch.
- **Resume-after-crash.** Event state (event_id, last sequence, queue) is
  persisted, so a crashed or power-cycled encoder can continue the same event
  rather than starting a new one with a gap in the middle. Who actually
  decides that, and when it is safe to decide it silently, is §5.1.
- **Decoder-side durability.** Downloads are cached locally and verified;
  missing or corrupt segments are re-requested. A gap causes a wait-and-retry,
  never a crash.
- **Stale detection.** If a room's `live.json`/manifest has not updated within a
  threshold (e.g. 10 minutes), decoders treat the room as **Offline** instead of
  polling a dead event forever.
- **Sequence-driven sync.** All ordering and synchronization is by integer `seq`;
  campus wall clocks are never assumed to agree.

---

## 5.1 Resume-after-crash: who decides, and when?

**Status: built.** What follows describes the mechanism actually implemented
(`SpoolState::last_activity_ms`, `SessionConfig::resume_stale_after_ms`,
`peek_resumable()`, `EncoderDock::onGoLive()`), kept in its original,
before-the-fact form rather than rewritten as a changelog entry — the
reasoning here is still the reasoning for why it works the way it does.

Before this, Go Live called `Session::check_resumable()`, and if it found an
unfinished event on disk it resumed it — **silently, unconditionally, every
time** (`multisite_output.cpp`, `out_start`). There was no prompt. The
comment above the call even said `// Offer resume`; it did not offer
anything.

Unconditional auto-resume is the right default for the case it was built for
— a genuine crash, restarted within the same service, continuing the same
recording without a gap. It was the wrong default for a case the old code
could not tell apart from that one: an encoder left with an unfinished event
from **last week**, because nobody happened to press End, silently
swallowing today's broadcast into it. Nothing on screen would have said this
happened; the operator would only have noticed from sequence numbers that
made no sense, if they noticed at all.

There was a second problem underneath it. If an operator could ever choose
"start new" over a resumable event, `SpoolQueue::begin_event()` deletes every
`.seg`/`.meta` file in the spool unconditionally — including segments that
were captured but never confirmed uploaded. "Start new" is not a neutral
choice between two equally-valid options; it is destructive to whatever the
old event hadn't finished sending. Any design here has to say that plainly
before it happens, not leave it implicit in a wipe on disk.

**The mechanism: a staleness cutoff, not a permanent choice.**

- `SpoolState` gains `last_activity_ms` — the wall-clock time of the last
  `enqueue()` or `confirm()` — persisted in `state.json` alongside the fields
  already there.
- `SessionConfig` gains `resume_stale_after_ms` (default 30 minutes), the same
  shape as the decoder's own `stale_after_ms` (10 minutes), which already
  exists for exactly this kind of judgment: telling a genuinely quiet room
  from a dead one.
- `Session::check_resumable()`'s `ResumeInfo` reports whether the resumable
  event is stale: `now_ms() - last_activity_ms > resume_stale_after_ms`. A
  free function, `peek_resumable(spool_dir, threshold)`, answers the same
  question from the spool directory alone — no `Session`, no `Transport` —
  because the dock has to decide whether to ask *before* Go Live creates
  anything, and a deferred-start encoder (VideoToolbox and the like) may not
  construct its `Session` until well after that click.
- **Not stale (the common case — a real crash, minutes old):** behaviour is
  unchanged. Auto-resume, no click, nothing in the way of getting back on air.
  What changes is that it stops being invisible: `Session::Status` carries
  what happened (event id, when it started, how many segments are already
  confirmed), and the dock shows a persistent — not auto-dismissing — line:
  *"Resumed event from 10:42, 340 segment(s) already confirmed"*, with an
  *"End this and start fresh"* button beside it. Seen, not assumed.
- **Stale (hours or days old — the ambiguous case):** auto-resume is refused.
  Go Live instead shows a dialog: *"An interrupted event from [date/time] was
  found. Resume it, or start a new event?"* — and if pending segments would
  be lost by choosing new, the dialog says so by name: *"Starting a new event
  abandons N segment(s) that were never confirmed uploaded."*
- The same "abandons N segments" warning covers both entry points into
  "start new" — the stale Go-Live dialog and the "End this and start fresh"
  button on an already-resumed event — rather than being written twice and
  drifting apart. Mechanically, the operator's choice becomes one boolean
  (`force_new_event`, threaded through `obs_output_create`'s settings blob
  like every other Go-Live setting, but never persisted — it means nothing
  outside the one Go Live it was set for); the encoder's own start-up logic
  just obeys it, deciding nothing itself.

**What this must never do:**

- Never block Go Live with a dialog for the ordinary crash-and-restart case.
  That is the case this feature exists to protect, and costing it a click
  would be solving the rare problem by taxing the common one.
- Never delete anything the operator wasn't told about first. A wipe that
  happens because thirty minutes silently defaulted a choice is exactly the
  kind of failure this write-up exists to rule out.
- Never require the operator to remember what happened. The resumed state is
  shown for as long as it's true, not just logged once and forgotten.

**Open question, not yet decided:** whether 30 minutes is the right cutoff, or
whether it should scale with the event's own `segment_duration_s` /
`manifest_window` the way the decoder's staleness threshold does. Thirty
minutes covers a reboot and a coffee break; it does not obviously cover a
long intermission with the encoder deliberately left running idle. Pick a
number, ship it, and let a real false-positive (or the lack of one) settle it
rather than guessing further here.

---

## 6. Timeslipping (per-campus live-DVR)

Each decoder maintains a **playback head** independent of the **live edge**:

- **Local cache & download-ahead.** The decoder continuously downloads new
  segments into a local cache regardless of where playback currently sits.
  Paused or behind live, it keeps filling.
- **Pause / Resume.** Pause freezes the playback head (holds the last frame); the
  cache keeps filling. Resume continues from the exact paused position.
- **Jump to Live.** Snaps the head to the live edge, with a configurable catch-up
  (hard cut by default, or a gentle speed-up).
- **Scrub / seek.** Move the head anywhere between `first_available_seq` and
  `latest_seq` — i.e. anywhere still retained, up to the full retention window.
- **Behind-live indicator.** Always shows how far behind live the campus is.
- **Restart recovery.** The playback position is persisted; after an OBS restart
  a campus resumes where it was or jumps to live (configurable).

Backed by durable object storage, this is "pause live TV," per campus.

---

## 7. Markers & cues

- **Authoring (main site).** The operator drops markers live (button/hotkey) or
  from a pre-loaded schedule; each is appended to `markers.json` keyed by `seq`.
- **Consumption (satellites).** Decoders display upcoming and passed markers on a
  timeline, can jump to a marker, and can fire local automation from one (e.g. a
  "Go to local" marker triggering a campus scene switch). Markers ride the same
  durable object path as the media.

---

## 7.5 Finished events: video-on-demand

An event that has ended is not a failure state — it is a complete recording,
and a satellite must be able to load and play it exactly like a live feed that
happens not to be advancing.

- **Loading a finished event starts at the beginning**, not at the live edge.
  Treating a completed event as "live" meant loading it and landing seconds
  from the close.
- **Ending a broadcast mid-playback changes nothing for the satellite**: it
  keeps playing through the remaining segments to the end. Nothing is cut off.
- **The reported time never runs past the end of the recording.** Once playback
  passes the last segment the playhead points at a position that does not
  exist; the displayed clock is clamped to the true end.
- **The UI switches vocabulary.** For a live event it reports how far behind
  live the campus is. For a finished one it reports **position out of total
  length** the way a media player does — "24:15 / 1:24:30" — because that is
  what an operator needs when deciding whether a recording fits the slot.
  "Behind live" means nothing once there is no live edge.
- **A finished recording's timeline spans its whole length**, from the moment it
  started to its true end, and does not move during playback. While live the
  right edge is the live edge and necessarily grows; once ended it must not,
  or positions on the bar mean nothing.
- The manifest lists only a rolling window of segments, so neither the length
  nor the timeline bounds may be derived from it. The event's start time and
  last sequence give the true extent.
- Distinguishing a *clean end* from a *lost connection* matters: a clean end is
  reported as a finished recording, while a manifest that simply stops
  advancing is reported as offline after the stale threshold.
- **"Ended" covers two situations that must not read the same.** A broadcast
  that finished *while the satellite was watching* is reported as
  "BROADCAST ENDED" — the event has just closed and the recording is playing
  out. An event that was *already finished when loaded* is reported as
  "RECORDING (not live)" — this is a past event, and nothing has just
  happened. The satellite remembers whether it ever saw the event live, and
  the memory resets when the event changes.

### 7.5.1 Event browsing

Built. The decoder lists a room's events and each entry carries **its own
current state**, not just a date:

- **LIVE** — this event is the one `live.json` points at and its manifest is
  still advancing. At most one event is live at a time.
- **RECORDING** — a finished event, playable as video-on-demand.
- **INTERRUPTED** — the manifest stopped advancing without a clean end, i.e.
  the encoder died. Still playable up to wherever it got to, but the operator
  should know it is incomplete.

A campus will usually see one live event among many recordings, so the state is
what makes the list scannable — the date alone does not say which one is
happening now. Entries are labelled by start date and time, newest first, with
the live one pinned to the top.

Choosing an event pins playback to it. An event starting mid-watch does **not**
steal the playback; the operator is told something is live and offered the
switch, because being pulled out of a recording part-way through is worse than
being told about it.

## 8. User interface

Two Qt docks, plus hotkeys. The core reliability and media path work with no UI
at all, which is what lets the same engine drive the planned appliance.

**Encoder dock (main site)**

- Storage settings, saved as they are edited so credentials are never retyped.
- Video encoder chosen from what the machine actually has (x264, NVENC,
  QuickSync, AMF), hardware first.
- **Go live / End broadcast**, with failures shown in the dock rather than left
  in the log.
- Marker buttons, named by the operator.
- The reliability readout that matters mid-event: how much of the event has
  been sent, how much is waiting, retries, and link health.

**Decoder dock (satellite)**

- **Load** then **Play**: loading fills the buffer, Play puts it to air.
- A timeline in clock time showing what is in storage, what is downloaded here,
  the playhead and markers. Hovering reports the recorded time under the
  cursor; clicking goes there.
- Hold picture / Continue / Catch up to now, jog in ±1 s to ±1 min steps, and
  "stay behind live by N minutes".
- **Lock**, to stop anything being changed by accident during an event.
- Position and state in plain language, switching vocabulary between a live
  event and a finished recording.

**Language.** The interface never mentions segments, buffers in the abstract,
or live edges. It reports times ("Showing 10:41:03"), durations ("Could
broadcast for 12 min") and plain states. The audience is a volunteer, not the
person who wrote it.

**Hotkeys** cover play, stop, hold, resume, catch-up, jog and marker drops, and
work without Qt — useful for an operator running the event from the keyboard,
and the fallback when a build has no docks.

---

## 8.1 Satellite appliance (headless decoder)

The primary satellite deployment: a small Linux box at the campus that receives,
decodes and plays out, with no operator-facing desktop software.

**Why an appliance rather than a workstation.** A receive-only campus gains
nothing from a full OBS install and loses a great deal: scene collections to
corrupt, updates that change the UI, a desktop that can be left in the wrong
state, and a volunteer expected to understand a production tool. An appliance
boots into its job, restarts itself on failure, and presents one simple screen.

**Shape**

ARM64 / Raspberry Pi, HDMI out — for a site that needs the feed on a screen and
into a small console. This is the only receive-appliance tier this project
builds or documents. Larger hardware tiers are a separate Stage Audio Works
product line, planned and built outside this repository.

**ARM64 / Raspberry Pi notes**

- The portable core cross-compiles for ARM64 today, and CI builds and tests it
  on an ARM64 runner so a regression is caught before it reaches hardware.
- **Video decode.** The Pi 5 (BCM2712) has **no H.264 hardware decoder** — only
  HEVC 4K60. Its NEON software H.264 decoder is reportedly faster than the old
  hardware block and handles 1080p comfortably, so a 1080p30 contribution feed
  is well within it. The Pi 4 *does* have H.264 hardware decode, capped at
  1080p. The decoder should therefore prefer a hardware decoder when one
  exists (`h264_v4l2m2m` on Pi 4, HEVC on Pi 5) and fall back to software
  rather than assuming either.
- This also strengthens the HEVC step on the codec roadmap: HEVC is exactly
  what the Pi 5 accelerates.
- **Audio.** HDMI carries up to 8 channels of LPCM, which suits the packed
  multi-channel layout: an inexpensive HDMI audio de-embedder recovers the
  individual channels at the campus. That gives a third audio path alongside
  ASIO and DeckLink, and is what makes the low-cost tier viable for production
  audio rather than stereo only. Packed is therefore the appliance's mode even
  though multi-track is the primary one overall (§4.3): an appliance has one
  output device, not a mixer. Fed a multi-track event it plays **one chosen
  track** — the first by default, selectable in Settings for a campus whose
  origin puts the house mix elsewhere. Distributing several tracks across
  output channels is not built, and playing all of them is not a fallback: six
  tracks into a device that accepts one is six times real time of audio, and
  the back-pressure starves the video sharing that thread.
- **Audio over the network (AES67).** A fourth audio path, for a campus that
  wants the sound on its own console rather than only inside the HDMI picture.
  The appliance registers a virtual sound card — Merging's open RAVENNA kernel
  module — and the GPL `aes67-daemon` publishes what the player writes to it as
  an eight-channel AES67 stream, so it appears in Dante Controller and routes to
  anything that takes AES67 directly. Installed by
  `scripts/player/merging-aes67.sh`; the stream is created by that install and
  switched afterwards from the player's own page, which also reports the
  multicast address, the port and the clock state. Switching it on moves the
  player's sound onto the AES67 card — that card being what the daemon publishes
  — and switching it off puts the sound back on the chosen output device; there
  is one output device here, not two. The daemon is a PTP slave, so a master has
  to exist on the network or nothing flows.
- **Storage.** The segment cache writes roughly 3 GB per hour at 6 Mbps. That
  will wear out an SD card, so a USB SSD is required rather than recommended,
  and the cache location must be configurable.
- **Thermals.** Sustained decode needs active cooling; a passively cooled case
  will throttle during a long event.
- **Output:** DeckLink (video plus embedded multichannel audio, which suits the
  packed channel layout), or DRM/KMS for direct display. Audio to ALSA/JACK or
  embedded in SDI. Packed channels go out in the order they arrived, which is
  what an eight-channel HDMI de-embedder or an SDI de-embedder expects — there
  is nothing to de-interleave when the output device is itself
  multi-channel.
- **Control:** a small built-in web server. Operators use a phone, tablet or any
  browser on the church network — hold, resume, catch up to now, jump to a
  moment, and see what is playing and how far behind. No app to install.
- **Operation:** starts on power-up (systemd), restarts on failure, keeps its
  local cache across reboots so a restart mid-event resumes rather than
  restarting.

**What it reuses.** Everything in the receive path: room and event discovery,
the durable segment cache, checksum verification, timeslipping (the playback
head, pause/resume, catch-up, scrub), markers, and the CMAF decoder. These are
already free of OBS and Qt and are covered by the existing tests, so the
appliance is a new *output and control* layer rather than a second
implementation.

**Web UI.** The decoder dock is already a thin view over a status snapshot; the
same snapshot serialises to JSON, so the browser UI is that view rendered in
HTML with a WebSocket for live updates. It should carry the same plain language
— clock times, "hold picture", "catch up to now" — and never mention segments.

**Settled while building**

- **HDMI/DRM first.** The appliance claims the KMS connector itself and sets
  the output mode, with no desktop involved. That is what gives exact control
  of resolution and frame rate, removes a desktop that can be left in the
  wrong state, and suits Raspberry Pi OS Lite, which is the right image for a
  box that only ever does one thing.
- **Yes to a holding screen, and it is the default.** The first problem with
  an appliance is finding it: until somebody knows its address there is
  nothing to type into a phone. So the first thing it does with the display is
  put its own address, name, room and state on it. Idle behaviour is
  configurable — black, hold the last picture, the identity screen, or a
  campus-supplied slide — because a screen a congregation can see is not
  always best left showing the last frame of an event.
- **A script on a stock distribution, not an image.** One command on stock
  Raspberry Pi OS installs the dependencies, builds, installs the event and
  enables it. It works from day one with no release infrastructure, updates
  are the same command again, and it does not tie the project to particular
  hardware. A prebuilt package can follow once the player has settled.
- **The preview is a copy of the output, not a second head.** The web UI shows
  the picture going out, refreshed a few times a second, so an operator can
  watch from a phone without changing what is on the screen in the room. There
  is one playhead, so the preview mirrors it rather than looking ahead; a
  genuinely decoupled look-ahead preview would need a second decode path and is
  deliberately out of scope for the single-box appliance. Where the crop makes
  the two genuinely different pictures — a `tile_index` set on a composited
  feed — the interface lets the operator pick: the region going out, or the
  whole feed as received (`/preview.jpg?view=feed`). The default is the output.

## 8.2 Public simulcast relay

The same segments that feed the campuses, pushed out to YouTube, Facebook or
any RTMP destination. A separate sub-project in `relay/`, deployed as one
Docker container on a small VPS. It is not part of the plugins and the core
knows nothing about it.

**Why relay from the bucket rather than add a second OBS output.** The main
site uploads once however many places the event goes, which is what makes
this possible at all on a venue connection that will not carry a second
upload. The public stream also inherits the buffering the campus feed already
has: the relay deliberately runs a configurable time behind the event —
three minutes by default — so a dropout at the main site is absorbed instead
of reaching air. It is the same trade as §1, applied to the public stream:
latency spent to buy resilience.

**Two protocols, told apart by the address alone.** RTMP is what every public
streaming site accepts, so one mechanism covers YouTube, Facebook and a
church's own server. SRT is what broadcast partners, hardware decoders and the
better contribution CDNs ask for, and it is what a lossy path between the VPS
and the destination wants: it retransmits lost packets instead of letting them
become a glitch. There is no protocol setting and no radio button — `rtmp://`
and `srt://` are unmistakable, and asking a volunteer to declare which one
they pasted is asking them to get it wrong.

The two differ in exactly one way that matters upward: RTMP means FLV, and FLV
means H.264. SRT means MPEG-TS, which carries HEVC properly — a standardised
stream type decoders have handled for years, not FLV's after-the-fact extension
that half the receiving end has never heard of. So **the codec rule below is
written per protocol**, and an HEVC feed that cannot go to YouTube can go to an
SRT destination unchanged.

That matters more than it sounds. Until SRT existed here, choosing HEVC for the
campuses cost a church its public stream outright, which made a real bandwidth
saving unusable for anyone who also streams. It now costs them the *RTMP*
destinations only.

**SRT settles for a longer latency than ffmpeg's own.** Its `latency` is the
window in which a lost packet can be asked for again; ffmpeg's default of 120ms
is enough only on a path short enough that the answer comes back almost
immediately. The relay sends 2000ms unless told otherwise. That is §1 applied
where it is cheapest — the relay is already sitting three minutes behind the
event, so two seconds is invisible, and it buys recovery across a path many
times longer than the default can manage. It is on the form, under Advanced,
for the case where it is not enough.

**SRT can also be listened for rather than called out to,** for a broadcast
partner or a hardware decoder that pulls from us. It is never the default and
is never inferred from a setting: an address with nothing before the port —
`srt://:9000` — is how one is written down, and writing it that way is how one
is asked for. It does mean opening an inbound port on a machine we have
otherwise been careful to keep closed, which is why it takes a deliberately
odd-looking address to get one.

A listener nobody has attached to yet is the reason §8.2's supervision grew a
second half. The finding it was built on is that ffmpeg says nothing when it
is *starved*; the same is true when the far end stops *reading*. Both have to
be noticed by watching, and they mean opposite things depending on which end
opened the connection. A destination we called that stops taking content has
gone wrong and is dropped and rebuilt like any other lost connection. A
listener that has never carried anything is simply waiting, possibly for the
whole first half of an event, and is neither reported nor acted on as a
failure — it keeps taking up position behind the live edge while it waits, so
whoever finally attaches gets the event as it is now rather than the forty
minutes they missed. Once a listener has carried content, losing it is a fault
like any other: the distinction is whether anything ever went out, not the
mode.

Watching the outbound side at all is new with SRT and fixes a latent hole in
the RTMP path too — before it, a destination that quietly stopped reading was
fed for ever into a pipe nobody was emptying.

**Copy remux, never a silent transcode.** Segments are pushed on unchanged: no
decode, no encode, no quality loss, and little enough CPU that the cheapest VPS
tier is the target rather than a stretch. What cannot be sent that way is
refused rather than adapted, in the two cases where adapting it silently would
put the wrong thing on air:

- **HEVC over RTMP, and AV1 over either.** RTMP wants H.264. ffmpeg will mux
  either of the others into FLV and report success, producing a well-formed
  stream the destination then rejects — measured, not assumed — so nothing
  downstream can be relied on to notice. The relay refuses and says which
  encoder setting to change, and now also points at the way there is to send
  it on unchanged: an SRT destination, where MPEG-TS carries HEVC properly.

  AV1 is refused on both. MPEG-TS has a mapping for it, but ffmpeg's support
  and the receiving end's support are each patchy enough that the likely
  outcome is the same well-formed-but-rejected stream this rule exists to
  prevent — so it is refused until that stops being true, rather than allowed
  on the strength of the specification.

  H.264 remains the default and the roadmap's first codec precisely because it
  decodes everywhere, a Pi 5 included (§8.1: software decode handles 1080p
  comfortably), so a site that has not gone out of its way to change codec can
  stream publicly with nothing to reconsider. Re-encoding on the way out is
  still the eventual answer for the RTMP case, is not built, and would end the
  $5-a-month claim when it is.
- **Packed multi-channel audio (§4.3.1),** where the mix, the ISOs and the
  click share one track. Selecting a pair out of it is not built, and sending
  it unchanged would put a mic ISO or the click out to the public. Multi-track
  events (§4.3, the primary mode) are handled: each destination carries one
  track, chosen by the label the main site published.

**Supervision is the point, not a refinement.** Most destinations end a
broadcast after roughly a minute without data, so an unattended relay that
cannot recover is worse than none. Each destination has one ffmpeg child and
one thread that owns it; a child that dies is restarted and resumes from the
segment it was on, so nothing is skipped. A silence shorter than 45 seconds is
ridden out without dropping the connection at all — fragment timestamps are
absolute, so content resumes exactly where it stopped and a destination that
tolerates the pause never knows. Beyond that the connection is dropped
deliberately and rebuilt, which splits the recording at the far end and is
reported as such.

Detecting that silence is the relay's own job: ffmpeg given a pipe that stops
producing blocks quietly and holds the socket open indefinitely without
reporting anything, so waiting for the child to complain is waiting for ever.

**What it reuses.** The receive path, unchanged: event discovery, the durable
cache, checksum verification, and the live/ended/interrupted classification of
§7.5. It is the same code a campus runs, so the relay and a campus can never
disagree about whether an event is still running — and an event that ends
cleanly is played out to its last segment and then closed deliberately, rather
than being cut off or left to time out.

**Finished events.** The relay also does two things with events that have
already ended, both gated on the event actually being finished (§7.5.1's
classification, so it and a campus agree on what "finished" means):

- **Download as one MP4**, streamed from storage as it is requested rather than
  assembled on the server, so a two-hour event costs no disk and several
  people can download at once. It carries every audio track, not just the
  streamed one — the ISOs and the click are what a post-production edit needs.
- **Replay to a destination**, playing a finished event out at normal speed
  as though it were live, for a second congregation or an evening repeat. This
  falls out of §7.5 rather than being new machinery: a finished event already
  plays and then ends, which is what a replay is. Proof of concept — one at a
  time, started by hand.

**Access.** The relay can change where a church broadcasts, so unlike the
campus appliance it cannot rely on being on a trusted network. It requires a
login on every endpoint but the sign-in itself, stores the password as
PBKDF2-HMAC-SHA256 over a random salt, and binds to localhost so that exposing
it is a decision. It does not terminate TLS: a proxy in front of it does, and
one is shipped as a working example.

**Not built.** Re-encoding; splitting packed audio; SRT in listener mode being
reachable through anything (the port has to be published, and nothing is
shipped to help); signing in to YouTube (a stream key is pasted, and the
broadcast is still created in YouTube's own page); and starting by itself,
either on a schedule or when the encoder goes live. Scheduling matters most,
because events start late — the intended trigger is `live.json` actually
going live, optionally bounded by a time window, and `markers.json` makes
"start the public stream at Sermon Start" possible.

## 8.3 External control API (planned)

Operators reach for a physical button, not a dock. A volunteer running a
event on a Stream Deck should be able to hold, resume and catch up without
finding a window, and the main site should be able to go live from a button.
The target is **Bitfocus Companion**, which is what churches in this bracket
actually use.

**One surface, two transports.** The appliance already exposes its controls as
HTTP routes (§8.1): `/api/play`, `/api/hold`, `/api/seek`, `/api/status` and the
rest. The OBS plugins will expose *the same command names with the same
payloads* over **obs-websocket vendor requests**. One API to learn and document,
two ways in, and a control surface written against either works against both.

**Why vendor requests rather than a server inside the plugin.** obs-websocket
ships with OBS 28 and later, so there is nothing for a church to install. Its
`obs-websocket-api.h` is header-only and works through OBS's proc handler, so
the plugin gains no link dependency, and if obs-websocket is absent every call
returns false after one log line — control disappears, nothing breaks.
Authentication, the listening socket and TLS are already solved there. A
bespoke HTTP server inside the plugin would duplicate all of it and open a
second port on a machine we have otherwise been careful to keep closed.

**Both ends, equal weight.** The encoder needs go-live, stop, drop-marker and
status; the decoder needs play, stop, hold, resume, catch-up, seek, jog, delay,
load-event, follow-live, audio-track selection, status and the event list. The
command set already exists as `EncoderControls` and `DecoderControls`, so the
API layer is an adapter over what the docks and hotkeys already call — no new
control logic, and no second path to keep in step.

**Two phases, because Companion needs more than actions.**

1. **The vendor API.** Every command registered as a vendor request, plus vendor
   events on state change and a `status` request for polling. Companion can
   drive all of it immediately through its OBS module's *Custom Vendor Request*
   action, and so can any obs-websocket client — scripts, Stream Deck plugins,
   another automation system.
2. **A Companion module.** *Custom Vendor Request* is an action only: it cannot
   light a button red while an event is live, or show "12 s behind" on a
   display. Feedbacks, variables and presets need a purpose-built Companion
   module (Node.js, submitted to Bitfocus) subscribing to the vendor events.
   That is a separate deliverable in a separate repository, and it depends on
   phase 1 existing first.

**Available before any of this:** the plugins already register thirteen named
hotkeys, and Companion's OBS module can trigger hotkeys by id. Play, stop, hold,
resume, catch-up, jog and drop-marker are therefore controllable from a Stream
Deck today, without parameters or feedback. Worth wiring up before building
anything, both because it is free and because it will show which commands
operators actually reach for.

**Built: sub-phase 1, the vendor API.** Every command of both halves is
registered as a vendor request under `obs-multisite` — `encoder/go-live`,
`decoder/hold`, `decoder/jog` and the rest — with vendor events
(`encoder/state`, `decoder/state`) emitted on change and a `status` request for
polling. It is an adapter: each request calls the same function the docks, the
hotkeys and the pages already call, so there is no second code path to keep in
step. The names live in one list in the portable core (`src/core/control_api.h`)
that the HTTP routes read too, which is what makes "the same command names" true
by construction rather than by discipline, and which a core test pins. It
registers from `obs_module_post_load()` — the header's requirement, and the
reason a vendor registered in `obs_module_load()` is never seen. With
obs-websocket absent the plugin logs one line and everything else carries on.

**Built: sub-phase 2, the Companion module.** A Bitfocus Companion module lives
at [stageaudioworks/companion-module-obs-multisite](https://github.com/stageaudioworks/companion-module-obs-multisite).
It is an adapter in the same sense as sub-phase 1: every action is one vendor
request and every feedback is one field of the status document, so a Stream Deck
button and a keypress on the desk cannot disagree. It offers Go live, End and the
marker buttons for a main site, the transport controls and timeslipping for a
campus, feedbacks that light a button while an event is on air or a campus is
held, behind live or offline, variables for both halves, and two preset banks
with the feedbacks already attached. It also carries a pass-through action that
calls any vendor request by name, so a command this plugin adds later is
reachable without waiting for a module release.

**And it drives either end of the system.** The same module talks to a campus
player appliance as well as to OBS: the appliance serves the same controls on its
own HTTP API (§8.1) and has no OBS in it at all, so which end is a choice on the
connection while the actions, feedbacks, variables and presets are written once.
The two places the appliance names a command differently — `follow-live` for
`return-to-live`, `load` for `load-event` — are a route table in one file of the
module, which is what keeps that difference from ever reaching an operator.

Two things about it are worth recording, because both were forced rather than
chosen. It opens **its own obs-websocket connection**, so the operator types the
host, port and password a second time — Companion modules each own their
connection, and there is no way around it. And its **source is MIT** while the
module is *distributed* under `GPL-3.0-only`: the Companion module store
requires module source to be MIT, so the plugin's `-or-later` cannot be carried
across. The manifest states the distribution licence, which is what an installer
sees.

Since then both ends have been driven for real: the plugin against an OBS, and
the module against an OBS and a campus player. The module is tagged **v0.2.0**.
Two things are still open, and neither is code: nothing has yet run a whole
event, and the module is not in the Companion store — until it is, Companion
loads it as a *developer module* (a directory set in the launcher's Developer
section; each folder in it with a `companion/manifest.json` is one module), and
that needs **Companion 4.0 or later**.

**Two names still differ from the appliance's** — the plugin's
`decoder/return-to-live` and `decoder/load-event` against the appliance's
`/api/follow-live` and `/api/load`. The Companion module maps them, so nothing
an operator sees depends on the two being brought together any more: it is
tidiness now, not a gap. The plugin's names are already served to the v0.1.8
pages, so unifying them is a change to both surfaces at once rather than a
rename in one place.

---

## 8.4 Control pages served from the plugin

Both halves of the plugin serve an operator page on the church network, out of
OBS itself: the same interface the appliance has (§8.1), so somebody who has
learned one does not have to learn the other. It is the appliance's page on
purpose, and the words on it are the words of an event rather than of a video
pipeline.

**What it is for.** The desk is not always where the event is. A marker has to be
pressable from the back of the room, the sending queue watched from the foyer,
and the reason nothing is happening read by somebody holding a phone and no
access to the machine.

**Shape.**

- **Sending side** — Go live and End the broadcast, the editable event name, the
  four markers, and the reliability readout an operator watches mid-event:
  confirmed pieces, what is waiting to send, retries, bytes sent, the measured
  upload rate, the colo serving the bucket, and the last error.
- **Receiving side** — play, hold picture, catch up to now, jog, stay behind
  live, a clickable timeline, the recordings list, and how long this campus could
  keep playing through an outage.
- **Both** — the log, for somebody who cannot reach Help → Log Files, and a
  **Lock** that refuses anything which would change what is on air.

**Which pages exist follows the machine's role**, as the docks already do: a main
site serves the encoder page and has no decoder routes at all, a satellite the
other way round, and a machine set to Both serves both and links them.

**One server, three users.** The pages run on the core's own HTTP server
(`src/core/http_server.cpp`), shared with the relay and the appliance rather than
reimplemented three times. That is why the server is tested on every platform CI
builds (`tests/test_http_server.cpp`) rather than only where a POSIX socket is
available, and why a routing mistake is treated as a defect rather than a
cosmetic one: it is an operator's click doing nothing, or somebody reaching a
page they should not.

**Trust, stated plainly.** There is no password and no TLS. It binds every
interface, for the same reason the appliance's page does — a page that answers
only `localhost` cannot be reached from the tablet it exists for — and the
building's own network is the guard. It is on by default on port 8080 and can be
switched off or moved in **Settings → Remote control** in either dock, which also
shows the address to type into a phone. Editing storage from the page is allowed;
retyping a secret key is not required, and a stored key is never sent to a
browser. **Lock** is deliberately not remembered across a restart: a lock that
survived one would leave a campus unable to broadcast with no obvious reason why,
and the tablet that set it is long since charged and put away.

---

## 8.5 Storage credentials: direct or brokered (planned)

Setting this up asks a volunteer to create a cloud account, mint an API token
with exactly the right scope — `s3:ListBucket` included, which the obvious
object-scoped token omits — and write a lifecycle rule that is also, without
saying so, the DVR depth. Those three steps are the wall. Everything else in
QUICKSTART is copying files.

So there should be a second way to answer "which bucket, and with what keys",
without removing the first.

**Two providers, one `S3Config`.** Today one thing builds the `S3Config` the
transport takes: the fields an operator typed. A second producer is added
beside it, and nothing downstream learns which one it got.

- **Direct** — endpoint, bucket, key, secret, region. What exists now, unchanged
  and never deprecated. Somebody running MinIO in their own rack is a first-class
  user of this project, not a legacy case.
- **Brokered** — the plugin holds a device identity and a service URL, and
  fetches short-lived credentials from a broker that manages the bucket on the
  operator's behalf.

**Pairing is a device-code flow**, the one a television uses to sign into a
video service — not a password typed into OBS, and not a key pasted from an
email:

```
1. First run, offline. The plugin mints a device id locally. No network.
2. The operator presses "Connect to a storage service".
3. The plugin asks the broker for a code and shows it:   JNB-4K7M
4. The operator opens the broker's page on any device and enters the code.
5. The plugin polls, receives credentials, and caches them.
```

The same flow serves the appliance, which is the reason to prefer it over
anything bespoke: a headless player with no keyboard shows the code on its own
screen and is paired from a phone, using the code path the plugin already has.

**What holds this honest.** A brokered mode is a place where a plugin could
quietly start working for somebody other than the person running it, so the
constraints matter more than the mechanism and belong here rather than in a
commit message:

- **Inert until asked.** Nothing contacts anything but the configured bucket
  until an operator presses Connect. No registration on first run, no version
  check riding along, no telemetry. A fresh install that is never paired must
  produce no traffic a packet capture would surprise anyone with.
- **The broker URL is a field, not a constant.** It may ship with a default,
  and it must be editable. Anyone can run a broker — an integrator looking
  after a dozen churches has better reason to than most — and the protocol
  between plugin and broker is documented here for that reason.
- **Cached credentials are never a precondition.** If the broker cannot be
  reached, the last good credentials are used and the event goes live. A
  service that can stop a Sunday is not one this project will depend on.
- **Always visible.** Mode, bucket, endpoint and expiry are shown in the dock.
  Brokered must not come to mean opaque.
- **Disconnect is a real button**, and it offers the underlying bucket details
  on the way out. The promise that nothing here can be taken away is worth
  little if leaving is undocumented, and the code path that proves it should
  exist whether or not it is ever used.

One side effect worth stating, because it runs against the intuition that
managed means less safe: a brokered credential is short-lived, where today a
long-lived key sits in the settings in plain text. The managed path is the more
defensible of the two at rest, not the less.

**Not settled.** What a broker owes a plugin when a subscription lapses — the
answer must not be "the event stops" — and whether a decoder pairs
independently or inherits from the encoder that already knows the room. Both are
design questions, not details, and neither should be answered by the first
implementation that happens to work.

---

## 8.6 Storage provider selection

**Status: built — the two OBS docks first, the Raspberry Pi appliance's web
settings page after an operator noticed it was missing there.** What follows
is kept in its original, before-the-fact form, with deviations from it noted
where they happen — the reasoning here is still the reasoning for why it
works the way it does.

Setting up storage used to mean an operator typing six fields — account ID or
endpoint, bucket, key, secret, region — into the encoder dock's Storage tab,
having first worked out which of those six their provider actually needs and
what shape the endpoint hostname has to be in. Cloudflare R2 needs an account
ID and no region. AWS S3, Backblaze B2 and Wasabi each need a region and
derive their hostname from it, in three slightly different ways. A church
volunteer configuring this for the first time had no way to know any of
that, and the dock didn't tell them either — it showed the same six blank
fields regardless of where the bucket actually lived.

**A provider dropdown, not a new field shape.** `S3Config` — bucket, key,
secret, and either an account id or an endpoint host plus region — does not
change. What changes is what builds it. A `Provider` choice
(*Cloudflare R2 · AWS S3 · Backblaze B2 · Wasabi · Custom / other
S3-compatible*) selects which of a small set of templates derives the
endpoint from the fields that provider actually needs, and the dock shows
only those fields:

- **Cloudflare R2** — Account ID, Bucket, Key, Secret. Region is fixed
  (`auto`); the endpoint is `https://<account>.r2.cloudflarestorage.com`.
- **AWS S3 / Backblaze B2 / Wasabi** — Region, Bucket, Key, Secret. Each
  derives its own hostname template from the region (`s3.<region>.amazonaws.com`
  and so on).
- **Custom / other S3-compatible** — today's full form, unchanged: raw
  endpoint, bucket, key, secret, region. This is not a legacy fallback to be
  deprecated later — someone running MinIO in their own rack, or a provider
  not in the list, is a first-class user of this option, exactly as §8.5
  already says of "Direct" credentials generally.

The templates themselves live in one place, `src/core/storage_providers.h/.cpp`
— read by every settings surface that needs them, not duplicated per surface,
the same "decided in one place" instinct behind `audio_plan.h` and
`disk_health.h` elsewhere in this codebase, and fully unit-tested with no Qt
and no OBS (`tests/test_storage_providers.cpp`). That portability is what
made the appliance's own dropdown a small job once it was noticed missing:
`src/appliance/api.cpp` derives the same way `onSaveSettings()` does in both
docks (a new `/api/storage/providers` route lists the choices; `apply_edit()`
derives `endpoint_host`/`r2_account_id`/`region` from whichever one field the
operator typed, exactly the OBS logic, just in C++ building JSON instead of
setting `S3Config` members directly), and `Config` gained the matching
`storage_provider` field with the identical empty-means-guess-from-
`detect_provider()` fallback for a config saved before this existed.
**One deviation from the original plan here:** this was designed as a bundled `data/providers.json`, editable
without a rebuild, the same way locale strings and web pages are. Built as a
compiled table instead — these five providers' hostname conventions are a
technical fact that essentially never changes, not operator-facing content
an integrator would want to edit on a live machine, so the file-loading
machinery (`obs_module_file()` path resolution, one more thing to get wrong
on three platforms) bought less than it cost. Adding a provider is still a
small, self-contained change; it just needs a rebuild, the same as any other
fix in this codebase. A network-fetched manifest was considered too, for the
same "add a provider without a release" property, and rejected for the same
reason as the file: this project's general reluctance to add a network
dependency where none was truly needed. Phase 11's update-check manifest
(§10) is the place that pattern already belongs, if it's ever needed here.

**Where "Multisite Cloud" fits.** §8.5 already frames the future brokered
option as a second producer of the same `S3Config` — "two providers, one
`S3Config`." This dropdown is the literal UI expression of that sentence,
generalized to N providers instead of two: *Multisite Cloud* becomes one
more entry, greyed out until built, and when selected shows §8.5's
device-code pairing flow (a *"Connect to a storage service"* button, a
status line, a Disconnect button) instead of key/secret fields. No new
settings surface is built for it later — it slots into the one that
already exists for R2, AWS, Backblaze and Wasabi.

**Not settled.** Whether region-based providers (AWS/Backblaze/Wasabi) offer
a dropdown of common regions, free text, or both — free text alone repeats
today's "which exact string does the hostname want" problem; a dropdown
needs to be honest about the fact that not every provider's regions are the
same list.

---

## 8.7 LAN / direct delivery

**Status: built, both halves, and every kind of receiver.** A receiver —
the OBS decoder plugin, the Raspberry Pi appliance, or the simulcast relay —
on the same network as the encoder, or reachable over an existing
site-to-site VPN, downloads directly from it: manifest, init segment, media
fragments and markers, instead of from the bucket, automatically preferring
that path when it answers and falling back to cloud, per request, when it
doesn't. Cloud delivery can also be turned off entirely for an operator who
wants everything to stay on one network and never touch a bucket at all. The
appliance's `Config`/`Player` (`src/appliance/config.h`, `player.h/.cpp`)
and the relay's `ConfigStore`/`RoomFeeder` (`relay/src/config_store.h`,
`room_feeder.h/.cpp`) both gained the identical `lan_host`/`lan_port`/
`lan_auth_token` fields and the same `LanTransport`/`FallbackTransport`
wiring as the OBS decoder — one codebase (`src/core/`), three independent
settings surfaces, no divergence in behaviour. The relay's own past-events
browsing, download and rebroadcast stay cloud-only regardless of LAN
settings: they are `list()`-based, which `LanObjectServer` does not serve —
it only ever holds the one event currently in progress, the same reason a
satellite's event browser is cloud-only too. What follows is kept in its
original, before-the-fact form, with
corrections noted in place where building it changed something — the
reasoning here is still the reasoning for why it works the way it does.

Every campus today reaches the main site the same way, and only that way:
through the bucket, over whatever internet connection each site has. That is
correct and stays correct — it is the whole reason this project works on
mobile data and LEO satellite links that would defeat a direct stream. But a
campus on the same building network as the main site, or reachable over a
VPN the church already runs between sites, has a faster and cheaper path
sitting unused: the encoder machine itself.

**What this adds, and what it deliberately does not replace — unless told
to.** A satellite that can reach the encoder directly — over the LAN, or over
an existing site-to-site VPN — downloads from it instead of from the bucket,
while the encoder, by default, keeps uploading to the bucket exactly as it
does today, unconditionally. Nothing about §3's "decentralized, no control
plane" holds any less true for this: the encoder serves the *identical*
object shape (`manifest.json`, `event.json`, `init.mp4`,
`segments/{seq}.m4s`, and — for LAN satellites following the room rather
than a pinned event — `live.json`) a cloud decoder already reads. The media
path and the signaling path are still the same path; there is just a second,
local way to walk it.

Cloud upload staying on by default is what makes LAN mode safe to *attempt*
in the first place: a cloud-only decoder, a LAN decoder whose link just
dropped, and the archival recording all still depend on it running
regardless of who else is connected directly. But an operator who has no use
for a cloud copy at all — a single building, no remote viewers, no interest
in an off-site archive — can turn cloud delivery off entirely for an event.
Doing so hands `Session` a `NullTransport` (`src/core/null_transport.h`) in
place of the real `S3Transport`: every PUT reports instant success, so the
spool → retry-uploader → manifest pipeline runs exactly as it always has —
segments confirm immediately, the LAN hooks fire on schedule — and nothing
ever actually leaves the machine. `Session` cannot tell the difference,
which is the point: cloud-off is not a separate code path, it is the same
one pointed at a transport that keeps nothing. The dock refuses to go live
with both cloud and LAN off at once (there would be nowhere for anything to
go), and the checkbox for it only appears once LAN delivery is turned on.

**One correction from the original plan, found while building the encoder
half:** this was going to serve straight from "the same durable spool" a
cloud decoder's segments pass through. It cannot. The spool's entire job is
to hold a segment only until the bucket confirms it, then delete it (see
`spool_queue.h`) — which means the segment a LAN decoder is most likely to
actually want (recent, ordinary programme, already confirmed) is by design
the segment the spool no longer has. LAN serving keeps its own bounded
retention window instead — a `SegmentCache`, the exact same class a decoder
already uses for its own cache, fed via three new `Session` hooks
(`set_event_started_callback`, `set_segment_confirmed_callback`,
`set_manifest_published_callback`) fired at exactly the moments `begin_common()`,
`on_confirmed()` and `publish_manifest_locked()` already have the relevant
bytes or JSON in hand. `Session` itself stays completely unaware that LAN
delivery exists — the hooks cost nothing when unset, and it never holds a
reference to the class that uses them.

**Shape, as built.** The encoder's HTTP server (`src/core/http_server.h`)
gained `route_prefix()` — a "starts with", not "equals", route, matched
longest-prefix-first, needed because a segment's path names a sequence
number that cannot be registered as one exact route per possible value. A
new `LanObjectServer` (`src/core/lan_object_server.h`) combines that with a
`SegmentCache` and five `Session` hooks (`set_event_started_callback`,
`set_segment_confirmed_callback`, `set_manifest_published_callback`,
`set_live_published_callback`, `set_markers_published_callback`) into the
actual object server: `GET .../manifest.json`, `.../event.json`,
`.../init.mp4`, `.../segments/{seq}.m4s`, `.../markers.json`, and
`.../rooms/{room}/live.json` — all proven end to end over a real loopback
socket in `tests/test_lan_object_server.cpp`, including the retention cap,
an event switch discarding the previous event's window and markers, and
auth enforcement. `markers.json` earned its own hook rather than riding
along with the manifest: found live, once cloud delivery could actually be
turned off — without it, a marker dropped mid-event never reached a
LAN-only satellite at all, since there is no cloud copy to fall back to for
just that one object.

On the decoder side, `LanTransport` (`src/core/lan_transport.h`) implements
the same `Transport` interface a decoder already downloads through, as a
plain HTTP client against exactly those routes — proven against a real
`LanObjectServer` in `tests/test_lan_transport.cpp`, including what a
genuine miss (404, LAN path alive) looks like next to a connection failure
(LAN path itself down), which is what tells `FallbackTransport`
(`src/core/fallback_transport.h`) which one happened. That class is the
whole of "preference and fallback": it holds a LAN `Transport&` and a cloud
`Transport&` and, per `get()` call, tries LAN first and only reaches for
cloud if LAN didn't answer — so a segment that aged out of the LAN's bounded
retention window falls back to cloud for *that segment alone*, without
flipping the whole session to cloud over one old fragment. `DecoderSession`
is handed whichever of the two — or, for a LAN-only satellite with no cloud
credentials at all, `LanTransport` alone — it never learns which, the same
boundary `Session`'s hooks keep on the encoder side.

The Raspberry Pi appliance (`src/appliance/`) is the second satellite this
applies to, wired the same way: `Player::rebuild_session()` builds the same
LAN/cloud/fallback choice `multisite_source.cpp` does, `Config` carries the
matching three fields, and the web settings page (`web/index.html`,
`api.cpp`'s `/api/config`) is the appliance's equivalent of the decoder
dock's settings dialog. `Player::storage_health()` (`/api/storage`) reports
`lan_configured`/`lan_active` the same way the OBS decoder's status JSON
does, including for a LAN-only box with no cloud transport at all to ask
about — proven with a real `multisite-player` process pointed at a real
`LanObjectServer` (an OBS encoder with LAN on), which correctly reported
"room is LIVE" and served segments with no bucket involved at any point.

- **Discovery — built, and deliberately manual.** A host (and port, and an
  optional shared token) typed into the decoder dock's settings, the same
  place cloud credentials go — not auto-discovered. mDNS was considered and
  set aside: it does nothing for the VPN case, where the two ends are rarely
  on the same broadcast domain, and it is one more thing to fail silently on
  a locked-down church network. A satellite with a LAN host configured but no
  cloud credentials at all now works LAN-only — `DecoderSettings::configured()`
  accepts either, not just cloud — and one following the room (not a pinned
  past event) discovers the live event id from the LAN server's own
  `live.json`, needing no bucket at all when the encoder also has cloud
  delivery turned off.
- **Auth — built, still a shared secret, not yet paired.**
  `LanServerConfig::auth_token` / `LanTransportConfig::auth_token` and the
  `Authorization: Bearer <token>` check are real and enforced end to end;
  what generates that token and gets it onto a decoder is still typing the
  same string into both docks, not yet the device-code pairing flow §8.5
  designs for cloud credentials — the eventual goal is still one pairing flow
  an operator learns once, used for both. A plain LAN inside one building is
  already treated as the trust boundary elsewhere in this project (the
  remote-control pages have no password and no TLS, deliberately); a token
  matters more once the path crosses a VPN.
- **Preference and fallback — built, per request rather than per session.**
  See `FallbackTransport` above. Deliberately simpler than tracking
  `LinkHealth` hysteresis per transport and switching on a threshold: a
  per-request decision cannot get "stuck" preferring the wrong path, and it
  needs no timer, no state machine, and no operator-visible mode to explain.
- **Visibility — built.** The decoder dock's storage-link line names which
  path the most recent fetch actually took — *"via LAN"* or *"via cloud"* —
  next to the existing colo/throughput readout, and only appears at all once
  a LAN host is actually configured.

**Deferred rather than decided against.** Whether LAN mode serves a segment
the moment it's spooled (lower latency than cloud, since it skips waiting for
upload confirmation) or only once the bucket has confirmed it (identical
consistency guarantee to cloud, simpler to reason about) is left for a later,
explicitly opt-in mode. A first build serves only what's already confirmed —
the same manifest a cloud decoder would eventually see, just sooner.

---

## 9. Capability overview

What this project does, and where each piece stands. Status is against the
codebase, not against anything else on the market — where a commercial platform
is the better answer for a given church, section 12 says so plainly.

| Capability | Status |
|---|---|
| Resilient store-and-forward upload | built |
| Multisite to any number of campuses (storage cost only) | built |
| Markers / event cues | built |
| Pause & hold at a campus, resuming exactly where it stopped | built |
| Per-campus independent DVR position | built |
| Multi-track production audio (main / ISOs / click), up to 6 tracks | built — one source per track at the satellite (§4.3) |
| Event browsing with live / recording / interrupted state | built |
| Storage management from the encoder dock (list a room's events with sizes, delete one or older-than-N) | built (§4.6) |
| Operator page served from the plugin to a phone or tablet, following the machine's role | built (§8.4) |
| Video-on-demand playback of past and interrupted events | built |
| Any OBS machine can originate a broadcast | built |
| Dedicated receive appliance (Raspberry Pi) | built and proven on a Pi 5; not yet run through an event (§10 Phase 6) |
| Self-hosted, on storage you own | built |
| Open protocol, no vendor lock-in | by design — the whole protocol is §4 |
| Public simulcast to YouTube / Facebook / RTMP or SRT | built and pushing live to YouTube; not yet through a full event. H.264 over either; HEVC over SRT only, and not yet from real encoder output (§8.2) |
| SRT output, caller or listener | built and receiving on a real client; not yet run through a full event (§8.2) |
| HEVC out over SRT | built, and the remux verified against ffmpeg — but not yet carried from a real HEVC encoder (§8.2) |
| Download a finished event as an MP4, all audio tracks | built (§8.2) |
| Replay a finished event to a destination | proof of concept — one at a time, by hand (§8.2) |
| Per-channel routing of packed audio at an OBS satellite | out of scope — use [atkAudio's OBS plugins](https://github.com/atkAudio/PluginForObsRelease) (§4.3.1) |
| Re-encoding an HEVC feed for a streaming site | not built; an SRT destination carries HEVC unchanged instead (§8.2) |
| External control API (obs-websocket vendor requests, §8.3) | built — every command of both halves, with vendor events |
| Bitfocus Companion module (buttons, feedbacks, variables) | built — [companion-module-obs-multisite](https://github.com/stageaudioworks/companion-module-obs-multisite), driving OBS or a campus player; not yet in the store |
| Control from a Stream Deck via OBS hotkey triggers | available now, no parameters or feedback |
| Web / mobile simulcast from the same files | planned; CMAF makes it feasible |
| Scheduling / auto-go-live | planned — for the relay as well as the encoder |
| Redundant storage: two independent S3 targets, active/active or active/passive | planned (§10 Phase 9) |
| Tile layout: a 2x1 or 2x2 feed split into discrete sources, owned by the decoder | built — OBS plugin sources it; campus player shows one chosen tile on its single screen (§10 Phase 10) |
| Fullscreen / SDI output assignment driven by the decoder plugin | planned (§10 Phase 10) |
| ABR transcoder ("relay plus"): a ladder written to a bucket that is its own HLS/DASH origin | **not part of this project** — moved to a separate hosted service (§10) |
| End-to-end low latency over ZeroTier, with WebRTC or SRT | **dropped** — use SRT, already in OBS (§10) |
| Knowing a newer build exists, and applying it without a manual reinstall | planned — notification first; whether an update applies itself is undecided (§10 Phase 11) |
| Connecting a bucket by pairing rather than by pasting keys, against a broker anyone can run | planned (§8.5, §10 Phase 12) |
| Choosing a storage provider from a list instead of typing raw endpoint fields | built (§8.6, §10 Phase 13) |
| Satellite receiving directly from the encoder over a LAN or existing VPN, cloud as automatic fallback | built, both sides (§8.7, §10 Phase 14) |
| Lossless high-quality mode: FLAC audio + ~10 Mbps HEVC, players only, no relay/web path | planned (§10 Phase 15) |

Further directions to explore: web/mobile simulcast served directly from the
bucket (which needs no relay at all — the CMAF objects are already the right
shape for it), and local insertion windows for campus announcements.
Multi-bucket mirroring has graduated from a direction to explore into Phase 9
(§10).

---

## 10. Delivery phases

Each phase leaves the project in a testable, usable state. Phases 1–5 are
built and have been run end to end. Phase 6 is built, proven on a Pi 5 and
carrying `v0.1.12-alpha`; the hardware beyond it — SDI output, larger signal
paths — is a separate Stage Audio Works product line now, not built here.
Phase 7 is built but has not yet carried an event. Phase 8 is built — the vendor
API and the Companion module — and both have been driven against a real OBS, the
module also against a real campus player, though nothing has yet run a whole
event. Phase 13 is built. Phase 14 is built.
Phases 9, 10, 12 and 15 have not been started.

**Phases 11, 12 and 13 carry weight together.** Between them they are most
of the distance between a project a technician can deploy and one an
ordinary church can — knowing a new build exists and installing it without a
manual reinstall, connecting a bucket without minting a token by hand, and
choosing a provider from a list rather than typing a hostname convention
nobody outside this project has memorized. 13 was deliberately the smaller
half of what 12 needs anyway, and is done first for that reason — "Multisite
Cloud" is already sitting in the dropdown, greyed out, waiting for 12 to make
it real. Phase 14 answers a different question — cost and reliability for a
campus already on the same network or VPN as the main site — and depends on
none of the others. None of the four depends on 9 or 10, and all were
written as late phases when the list
assumed the hardest problem was features rather than deployment.

This project's scope is now the OBS plugin pair and the Raspberry Pi
appliance — nothing wider. Three things that used to be on this list are not
any more. They are written up after the phases, with the reasoning, rather
than deleted.

- **Phase 1 — Reliability core.** ✅ Durable upload queue, retry/backoff, checksums,
  resume-after-crash, decoder cache with verification, and stale detection. This
  is format-agnostic and lands before the media format work. Resume-after-crash
  now also asks rather than deciding silently, when it matters — see §5.1.
- **Phase 2 — Format, namespace & audio.** ✅ FFmpeg CMAF muxing (`init.mp4` +
  `.m4s`), codec-agnostic wrapper (H.264 and HEVC both tested end to end; AV1
  carried but less exercised), packed multi-channel production audio, the
  `rooms/live.json` + `events/{ulid}` model, keyframe-aligned segments,
  prefix/age lifecycle, and generalized S3 endpoint configuration.
- **Phase 3 — Timeslipping.** ✅ Decoder DVR: playback head vs live edge, deep local
  cache, pause/resume/jump-to-live/scrub, behind-live indicator, restart
  recovery.
- **Phase 4 — Markers & cues.** ✅ Authoring from the encoder, consumption and
  jump-to-marker at the satellite. Decoder-side cue authoring is not built.
- **Phase 5 — User interface.** ✅ Encoder and decoder Qt docks, hotkeys, and
  plain-language status, plus event browsing (section 7.5.1), which needs
  bucket listing.
- **Phase 6 — Satellite appliance.** ✅ The low-cost tier: a headless Linux decoder
  with HDMI output and a browser-based operator UI (section 8.1), built on the
  existing receive core. Built: the player engine, DRM/KMS display output with
  its own modesetting, ALSA multichannel audio, the splash and idle screens, the
  web control surface (decoder controls, event list, storage and system
  settings, decoupled preview), and the systemd/install path that makes it start
  on power-up, **proven on a Pi 5 on 2026-09-07** and carrying `v0.1.12-alpha`,
  though not yet through a congregation's event. Complete **for the tier it
  defines**, in section 8.1's terms: the ARM64/HDMI appliance is what this phase
  set out to build. One item remains within this same phase rather than moved
  elsewhere, since it needs no wider hardware tier to answer: hardware-decoder
  selection on Pi 4 (`h264_v4l2m2m`), alongside the software path Pi 5 already
  uses — prefer a hardware decoder when one exists, fall back to software.
  DeckLink SDI output belongs to hardware beyond the Pi, which is out of this
  project's scope — see "Appliance hardware beyond the Pi" near the end of
  this section. The channel de-interleaver has been dropped from scope rather
  than deferred (§4.3.1).
- **Phase 7 — Extensions.** 🟨 Built: the public simulcast relay (§8.2), as a
  separate container in `relay/` — copy remux to one or more RTMP **or SRT**
  destinations, per-destination audio selection, a delay buffer, and
  supervised reconnection, with its own browser UI. Like the appliance it has
  not yet carried an event. HEVC now has a route out, over SRT, where RTMP
  can carry only H.264; that remux is verified against ffmpeg but has not yet
  carried real encoder output. Not started: re-encoding, web/mobile simulcast
  served from the bucket, scheduling and auto-go-live, redundancy, and local
  insertion.

- **Phase 8 — External control API.** ✅ Both halves. The command surface of each
  plugin is an obs-websocket vendor request, mirroring the control pages name for
  name and driven through one shared command layer; and a Bitfocus Companion
  module provides the buttons, feedbacks, variables and presets (§8.3). What is
  left is not code: it has been driven against a real OBS and a real campus
  player but has not run a whole event, and the module is not in the Bitfocus
  store yet. Hotkey-based control from Companion works as well, and needs
  nothing.

- **Phase 9 — Redundant storage.** ⬜ Upload to two independent S3 targets, so a
  provider outage, a regional failure, an account lockout or an accident in one
  console stops being a single point of failure for every campus at once. Two
  modes, chosen per room: **active/active**, where all media goes to both, and
  **active/passive**, where the manifests and `live.json` go to both but the
  media only to the primary until a failover.
  The question that needs answering before any code is what *confirmed* means
  with two targets. The whole reliability claim rests on a segment appearing in
  the manifest only after storage has acknowledged it (§4.7); requiring both
  acknowledgements doubles the exposure to the slower link and lets one bad
  provider stall the feed. The proposal is to keep the invariant per target —
  manifest on primary confirmation, secondary mirrored best-effort, each
  target's state carried in the manifest — and to add a failover state machine
  over the top of the link health that already exists. The decoder side follows
  from `live.json` naming both targets, and the checksums already in the
  protocol are the proof that a mirror is a true copy rather than a hopeful one.
  Costs to state plainly: double storage and double origin writes, lifecycle
  rules needed on **both** buckets, and *Manage storage…* extended to say which
  target an event is in and to delete from both.
- **Phase 10 — Tile layout and assigned outputs.** ⬜ A room that needs two or
  four discrete pictures composites them at the main site today and pulls them
  apart at the satellite with OBS filters by hand ([Choosing a
  satellite](docs/SATELLITE.md)). This makes the split a property of the
  decoder instead: the encoder declares a layout (`1x1`, `2x1`, `2x2`) in
  `event.json`, and each region becomes its own video source, already cropped,
  assignable to a fullscreen output, a DeckLink or AJA output, or a video wall.
  The shape is deliberately the one audio already uses — one decoder, many
  sources, no extra download and no extra decode. Assignment is the part to
  design: OBS can be asked for a fullscreen projector on a chosen monitor, so
  tile-to-output mapping is the plugin driving OBS's own outputs rather than a
  new output of its own. Questions recorded here rather than answered: what a
  satellite does when it has fewer outputs than tiles, whether the mapping
  survives a layout change mid-event, and whether audio follows the tile.
  Built so far: the layout in `event.json` and the crop geometry live in
  `src/core/` (`TileLayout::tile_rect` and `tile_view()`), the OBS plugin
  exposes each region as its own source, and the headless campus player can put
  one chosen region on its single screen — a `tile_index` setting (`-1` whole
  picture, `0..3` in reading order) crops in the present path as a non-owning
  view, so no frame is copied and the audio is left where it is. The web
  preview offers both views of the same instant — the region going out and the
  whole feed — because once a tile is selected those are no longer the same
  picture, and an operator checking the crop needs to see the edges. Assigning
  tiles to several outputs from one box needs hardware beyond the Pi, which is
  out of this project's scope — see "Appliance hardware beyond the Pi" near
  the end of this section.
- **Phase 11 — Keeping installations current.** ⬜ Installing the plugin is a
  manual act — unzip, move files, restart OBS — and the only way to learn that a
  newer build exists is to go and look at the releases page. Two jobs live here
  and they are not the same size.

  **Telling the operator** is the small one. The plugin already speaks HTTPS
  through the libcurl it links for uploads and already bundles on Windows, so a
  version check costs one request and a comparison against `PLUGIN_VERSION`; it
  belongs off the render thread, cached, and stated in the dock the operator
  already has open rather than in a dialog nobody reads. OBS offers nothing to
  build on: there is no update or upgrade entry point in `libobs` or in
  `obs-frontend-api`, so this is written once, for all three platforms. The
  decision to settle first is whether the check runs by default, because it is
  the one request this project makes that tells a third party that OBS with this
  plugin is running — and a project whose deployments are otherwise entirely
  inside a church's own network should say that plainly rather than bury it in a
  settings page nobody opens. Publishing a small manifest beside the release —
  one request, one number, which build is for which OBS — also keeps the check
  off GitHub's API and its rate limit, and is where the platform and OBS-version
  matching has to live anyway, since OBS refuses a plugin built for a different
  major version and offering the wrong asset is worse than offering none.

  **Applying the update** is the part to be decided rather than assumed. Windows
  cannot overwrite a DLL OBS has loaded: it can be renamed, or a helper can be
  left behind to wait for OBS to exit, but either way the new version appears on
  the *next* start, and the install directory needs elevation to write into.
  macOS is the opposite — the per-user plugin directory is writable without
  privileges, so the swap itself is easy — but these builds are neither signed
  nor notarised, so an update arrives quarantined and OBS then loads nothing at
  all and logs nothing, which is exactly the failure OPERATOR.md already warns
  about; an updater would have to clear the flag itself. Linux is the easiest of
  the three and the Flatpak case the most awkward, because a plugin that cannot
  write its own directory should not pretend otherwise. OBS does not do this
  in-process for itself either: on macOS it hands updates to Sparkle.

  **The prerequisite is done.** Our Windows artifacts and the operator guide
  used to use the legacy layout — files merged into the OBS install directory
  under `C:\Program Files\obs-studio\` — which OBS's own plugins guide warns
  will stop working in a future version. Both now use the recommended
  `C:\ProgramData\obs-studio\plugins\obs-multisite\`, one self-contained
  directory holding `bin\64bit\` and `data\` (see `.github/workflows/obs-plugin.yml`
  and `docs/OPERATOR.md`). That was the actual blocker for a Windows update
  mechanism: the files an updater would replace are now data it can own,
  rather than files inside `Program Files`.

  Nothing here depends on phases 9–12, and it is small enough to pull forward if
  reinstalling by hand on each release is costing more than those phases are
  worth.

- **Phase 12 — Storage credentials and pairing.** ⬜ A second way to answer
  "which bucket, and with what keys" — a device-code pairing against a
  credential broker, beside the typed keys that exist now and never instead of
  them. Designed in §8.5, including the constraints that keep it honest: inert
  until an operator asks for it, a broker URL that is a field rather than a
  constant, cached credentials that never gate going live, and a disconnect
  that hands back the bucket details.

  This is on the list because of what §8.5 opens with. Creating a cloud
  account, scoping a token correctly and writing a lifecycle rule are the three
  steps that decide whether a church can deploy this at all, and they are the
  three a broker removes. Whatever the eventual split between what a church
  does itself and what it pays somebody for, the plugin needs the seam — and
  the seam is small, because `S3Config` is already a plain value struct that
  something builds rather than something the transport reaches out for.

  Depends on nothing else here. Along with Phase 11, the most useful work
  available once the plugins are finished.

- **Phase 13 — Storage provider selection.** ✅ A provider dropdown
  (Cloudflare R2, AWS S3, Backblaze B2, Wasabi, Custom / other
  S3-compatible) that shows only the fields each one actually needs and
  derives the rest, instead of six blank fields regardless of where the
  bucket lives. Built per §8.6, in both OBS docks and — added in later
  passes, once its absence there was noticed — the Raspberry Pi appliance's
  and the simulcast relay's web settings pages too. `S3Config` itself did
  not change; this is a UI-layer derivation in front of it
  (`src/core/storage_providers.h/.cpp`), read by all four settings surfaces
  rather than reimplemented per surface, backward compatible with every
  saved setup — a configuration saved before
  this existed reads back as whichever provider its endpoint actually
  matches, or "Custom" if none does, never misrepresented as something it
  isn't. Also the seam Phase 12's brokered credentials will slot into later —
  "Multisite Cloud" is already a greyed-out entry in the same dropdown, not a
  second settings surface waiting to be built.

  Depended on nothing else here, and nothing here depends on it.

- **Phase 14 — LAN / direct delivery.** ✅ A satellite on the same network as
  the main site, or reachable over a VPN the church already runs, downloads
  directly from the encoder instead of from the bucket — automatically
  preferred when it answers, falling back to cloud per request the instant it
  doesn't. Designed and built in §8.7. Cloud upload keeps running by default
  throughout — this adds a second path to reach the same objects, it does
  not remove the first or introduce a control plane §3 doesn't already have
  — but can now be turned off entirely for an operator with no use for a
  cloud copy at all.

  **Built, both sides:** the encoder's `HttpServer::route_prefix()`, a
  `LanObjectServer` serving `manifest.json`/`event.json`/`init.mp4`/segments/
  `markers.json`/`live.json` from a bounded retention window fed by five
  `Session` hooks, an auth-token check; the decoder's `LanTransport` (the
  same `Transport` interface a decoder already downloads through, against
  those exact routes)
  and `FallbackTransport` (LAN preferred, cloud per request otherwise) — all
  proven over real loopback sockets
  (`tests/test_lan_object_server.cpp`, `tests/test_lan_transport.cpp`,
  `tests/test_fallback_transport.cpp`). A `NullTransport`
  (`tests/test_null_transport.cpp`) lets cloud delivery be switched off
  without Session knowing anything changed. The Raspberry Pi appliance
  carries the identical decoder-side wiring (`Config`, `Player`, the web
  settings page) — proven with a real `multisite-player` process against a
  real encoder, receiving an entire event over LAN with no bucket involved.
  The simulcast relay carries the same wiring for the live feed it reads
  before pushing onward (`RoomFeeder`, `ConfigStore`, the web settings
  page) — a relay in the same building as the encoder can now read the
  feed straight from it instead of round-tripping through the bucket, LAN-
  preferred with cloud fallback or LAN alone. Past events stay cloud-only
  on the relay, exactly as they do everywhere else this pattern appears:
  browsing them is a `list()` operation, and `LanObjectServer` only ever
  holds the event currently in progress. **Still not built:** anything
  beyond manual host:port discovery (no mDNS —
  see §8.7 for why), and the device-code pairing flow §8.5 designs for cloud
  credentials, which LAN's shared-token auth is meant to eventually share
  rather than duplicate.

  Depends on nothing else here. The pairing step designed for LAN auth is
  meant to eventually match §8.5's device-code flow, but does not require
  §8.5 or Phase 12 to be built first — the plain pre-shared-token exchange
  already built is enough on its own until brokered pairing exists to share.

- **Phase 15 — Lossless high-quality mode.** ⬜ FLAC in place of AAC on every
  audio track, alongside roughly 10 Mbps HEVC video, as an opt-in mode for an
  event where bandwidth genuinely is not the constraint — a from-the-source
  archival/monitoring copy rather than the bandwidth-tuned broadcast one this
  project is otherwise built around. Switched per event, not per track: an
  event runs either the normal delivery or this ceiling, not a mix.

  **Researched, not yet built** (2026-09-14). OBS already ships what this
  needs: `obs-ffmpeg` registers a FLAC encoder (`ffmpeg_flac`) through the
  identical machinery its AAC encoder uses — same lossless-aware creation
  path, same packet shape — and the mux/decode core here is already
  codec-agnostic: `CmafMuxer`/`CmafDecoder` (`src/core/cmaf_muxer.*`,
  `cmaf_decoder.cpp`) carry whatever `AVCodecID` and extradata they are
  given, which is why this is a plugin-layer change and not a core one. What
  would actually move: `BroadcastController`'s hardcoded
  `obs_audio_encoder_create("ffmpeg_aac", …)`, `multisite_output.cpp`'s
  hardcoded `AV_CODEC_ID_AAC`/`"aac"` track metadata (and its `frame_size`,
  which is never read off the real encoder today and silently defaults to
  AAC's 1024), and the `encoded_audio_codecs` compatibility string OBS
  checks (semicolon-separated — `"aac;flac"`). The Raspberry Pi appliance
  needs no changes at all: it decodes through this same `CmafDecoder`, and
  `pcm_convert.h` already works from however many samples a decoded frame
  actually carries rather than assuming AAC's frame size — FLAC decode is
  also native ffmpeg, no extra dependency, and lighter than the video decode
  the Pi already does.

  **The real constraint is downstream, not technical feasibility.** FLAC
  audio has no home on the public web at all — no streaming site's ingest or
  browser player takes it — and 10 Mbps HEVC already needs SRT rather than
  RTMP (§8.2). So high-quality mode is a dead end for the simulcast relay and
  anything reached through it: choosing it is choosing "campuses and
  archival only", not "also simulcast this to YouTube/Facebook or watch it
  in a browser". Only the two purpose-built players (the OBS decoder plugin,
  the Pi appliance) decode through `CmafDecoder` and can play it; nothing
  else in this project plays media back at all — the plugin's own web pages
  and the relay's own web page are control/status surfaces, never an
  in-browser player. Whatever UI this gets has to say so up front, before an
  operator discovers it by the relay quietly refusing the event instead.

  Depends on nothing else here.


### Three things that were on this list

None of the three was built, so nothing anyone has is affected. The reasoning
is kept rather than the entries deleted: the people who read a roadmap are the
ones who would notice things quietly disappearing from it.

### The ABR transcoder, "relay plus" — no longer part of this project

It answers a different question from the rest of this document. Everything
else here carries an event between sites a church runs; a rendition ladder
exists to serve an audience on the open internet, which is a delivery
business with its own cost curve, its own failure modes and its own
competitors. Building it here would have quietly changed what this
project is, and would have set an expectation of hosted delivery that a
repository of two OBS plugins cannot honour.

It is not cancelled, it has moved: it is the foundation of a hosted service
Stage Audio Works intends to build separately, fed by the encoder plugin or
by Stage Audio Works' own appliance hardware. The existing relay stays exactly as it is —
free, in this repository, and not degraded to make room for anything.

The engineering notes are kept below because they were dearly bought and
the next person to attempt this should not start from nothing.

The relay copies; this
re-encodes. Decode the incoming feed once, produce a ladder of renditions
(1080p, 720p, 480p), package it as CMAF for **both HLS and DASH**, and write
it to a bucket that is then the origin — so a browser or a phone plays ABR
straight from object storage with no origin server running anywhere. Encoders:
NVENC, VAAPI and x264, behind a capability probe, the way the relay already
probes ffmpeg for SRT. Input stays the bucket as now, but SRT and RTMP input
as well, which is what makes the container useful outside this project.
The hard part is not the encode. It is ladder *alignment* — closed GOPs, one
IDR per segment, identical boundaries across renditions — or ABR switching
glitches at every switch. That should be proven before anything else is built.
Note that the target hardware changes with the job: a $5 VPS has no encoder,
so this wants a small box with a usable iGPU, and the documentation should say
so rather than inherit the relay's VPS story. Built as a library first, the way
`relay_logic` is, so the same engine can later become an in-OBS multi-bitrate
encoder uploading resiliently from the machine that already has the picture.

### End-to-end low latency — dropped

The concept was a second delivery path in fractions of a second rather than
tens — ZeroTier to avoid port forwarding, WebRTC or SRT above it — running on
the same plugins and appliances.

It is dropped for three reasons, none of which were going to improve. It
inverts §1: everything that makes this project worth running comes from
being allowed to buffer, and a sub-second path gives that up on exactly the
venue connections this project exists to survive. Timeslipping does not
survive it — hold, resume and scrub are most of why a campus wants this, and
they are not features you can offer on one path and withdraw on the other
without confusing every operator who has to choose. And it would be a
support liability out of proportion to its use: a path whose quality tracks
the connection minute by minute generates calls that no amount of
documentation prevents.

Where a site genuinely needs conversational latency — a two-way interview, a
campus pastor taking questions — SRT already exists in OBS, there is good
hardware for it, and Stage Audio Works can engineer it per project on the SRT
infrastructure it already runs. That is a better answer than a second delivery
path in this repository, and it is available today rather than after a phase.

### Appliance hardware beyond the Pi — moved, not dropped

Unlike the two above, this isn't a rejected idea — it's a scope decision.
This project's receive and encode hardware is the Raspberry Pi appliance
(Phase 6) and nothing wider: no x86/DeckLink production tier, no RK3588
headless encoder, no rackmount box. Both were sketched here at one point —
an x86 satellite with SDI output and genlock, and a headless encoder on a
Mekotronics R58 (RK3588) using its `rk_hdmirx` capture path and
`librockchip_mpp` hardware encoder — and both are real, still being built,
just not in this repository.

Stage Audio Works builds and sells that hardware, and the software that runs
on it, under its own name (**MultisiteOS**), as a product line planned and
documented separately from this project. Where it reuses this project's core
— the CMAF muxer, the durable queue, the storage protocol — that code stays
exactly what it already is: GPLv3, in this repository, available to anyone
who wants to build their own version of that hardware rather than buy ours.
What moved out is the *planning and positioning* of the hardware itself, not
the license on the software underneath it.

Practically, this is why Phase 6's own text no longer defers Pi 4's
hardware-decoder selection to a later phase, and why Phase 10's tile-to-output
assignment no longer promises a numbered phase to finish it in: both were
waiting on a hardware tier that isn't part of this project's roadmap any more.

---

## 11. Baseline configuration

- **Retention window:** 7 days (sets how far back campuses can timeslip).
- **Segment length:** 6 seconds default, configurable.
- **Video codecs:** H.264 first; HEVC then AV1 on the roadmap; codec-agnostic
  pipeline.
- **Audio:** every enabled OBS output track is delivered (up to 6), each labelled
  once by the operator; multichannel/surround allowed per track; AAC to start.
  A satellite plays track 1 unless told otherwise, so a stereo-only site needs
  no audio configuration at either end.
- **Pause behaviour:** hold the last frame.
- **First storage target:** Cloudflare R2 (any S3-compatible store supported).
- **Platform order:** Windows first, macOS second.
- **Security:** encoders use write access scoped to their room/event paths;
  decoders use read-only access to the bucket.

---

## 12. Why this exists

This project is developed by the projects team at **Stage Audio Works**, a
worship AVL integrator working across Africa, to support churches that are
growing into multiple locations. Multisite streaming is a solved problem for a
large church in a well-connected part of the world; the commercial platforms
that solve it are good, and largely unavailable or unaffordable where these
churches are.

The full argument — what this is and is not, why it is not a managed event,
why latency is traded for resilience, what it asks of a venue's network, and
the clean-room and licensing position — is in the README, under
[Why this exists](README.md#why-this-exists). It was duplicated here word for
word, which meant two places to keep in step and one of them silently going
stale. This document covers the design; the README covers the case for it.
